/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "dialog_ft8.h"

#include "ft8/worker.h"
#include "ft8/qso.h"
#include "ft8/utils.h"
#include "lvgl/lvgl.h"
#include "dialog.h"
#include "styles.h"
#include "params/params.h"
#include "cfg/digital_modes.h"
#include "radio.h"
#include "audio.h"
#include "dsp.h"
#include "keyboard.h"
#include "events.h"
#include "buttons.h"
#include "main_screen.h"
#include "qth/qth.h"
#include "msg.h"
#include "util.h"
#include "recorder.h"
#include "textarea_window.h"
#include "dsp.h"

#include <ft8lib/message.h>

#include "ft8/audio_worker.h"
#include "ft8/auto_dnf.h"
#include "ft8/cq_scheduler.h"
#include "ft8/table_view.h"
#include "ft8/tx_worker.h"
#include "ft8/ft8_log.h"
#include "ft8/ft8_remote.h"
#include "widgets/lv_waterfall.h"
#include "widgets/lv_finder.h"

#include "adif.h"
#include "qso_log.h"
#include "scheduler.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>

#define SAMPLE_RATE     (AUDIO_CAPTURE_RATE / AUDIO_DECIM)

#define WIDTH           771

#define UNKNOWN_SNR     99

#define MAX_PWR         10.0f

#define FT8_WIDTH_HZ    50
#define FT4_WIDTH_HZ    83

#define MAX_TX_START_DELAY     5.0f
#define MAX_TX_START_DELAY_FT4 1.5f

#define FT8_FREETEXT_FILE        "/mnt/ft8_freetext.txt"
#define FT8_FREETEXT_MAX_LEN     13
#define FT8_FREETEXT_ACCEPTED_CHARS " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))

/* Last decoded message meta, set by add_rx_text() and available to
 * on_message_cb extension code. Worker thread only. */
static ftx_msg_meta_t     last_rx_meta;
typedef enum {
    RX_PROCESS,
    TX_PROCESS,
} ft8_state_t;

/* ft8_cell_type_t and cell_data_t live in ft8/table_view.h */

/* slot_info_t lives in ft8/audio_worker.h */

static ft8_state_t state = RX_PROCESS;
static Subject    *tx_enabled;

/* TX CQ button tri-state: Off, or an explicit TX slot parity picked by
 * the user (no clock guessing). Non-zero means the CQ loop is running;
 * the value doubles as the parity anchor the loop resumes to after an
 * interleaved QSO. Session-local: every dialog run starts at Off.
 * cq_state_t is declared in dialog_ft8.h. */
static Subject    *cq_enabled;

/* Auto behaviour level (ftx_qso_auto_t). Session-local, starts at Off;
 * only the Auto Mode tie-break (cfg.ft8_auto_mode) persists. */
static Subject    *auto_level;

/* QSO processor profile (ftx_qso_proc_t). Session-local, always back to
 * Normal on dialog construct — contest mode must not stick after the event. */
static Subject    *qso_proc;

static bool        tx_time_slot;

static ftx_tx_msg_t tx_msg;
/* True when tx_msg came from the QSO engine: valid for exactly one slot of
 * tx_time_slot parity, dropped if the TX start window is missed. CQ messages
 * (engine-external) keep the classic repeat-until-count behaviour instead. */
static bool         tx_msg_oneshot;

/* Auto TX2 confirm UI (Full/Pre first initiate). Armed from slot end;
 * accepted via PTT / main VFO rotary; timed out from on_tick. */
static bool     ui_confirm_pending = false;
static bool     confirm_tx_odd     = false;
static uint64_t confirm_deadline   = 0;

#define DECODED_SLOT_MSG_MAX 100
static ftx_decoded_msg_t decoded_slot_msgs[DECODED_SLOT_MSG_MAX];
static char              decoded_slot_texts[DECODED_SLOT_MSG_MAX][64];
static size_t            decoded_slot_msg_count = 0;

/* The lv_table widget is owned by ft8/table_view; expose its handle as
 * `table` so existing fade/group/anim call sites need no rename. */
#define table (table_view_obj())

static lv_timer_t *timer = NULL;
static lv_anim_t   fade;
static bool        fade_run        = false;
static bool        disable_buttons = false;

static lv_obj_t *finder;
static lv_obj_t *waterfall;

/* Audio capture, decimation, FFT, decoder thread and stop-flag plumbing
 * all live in ft8/audio_worker.c. The dialog just creates/destroys the
 * worker and exposes a handful of callbacks. */
static audio_worker_t *audio_worker = NULL;

static adif_log         ft8_log;

static double cur_lat, cur_lon;

static int32_t filter_low, filter_high;

static float base_gain_offset;

/* PSD staging for coalesced waterfall updates — written by worker thread,
 * drained by scheduler callback on LVGL task. */
#define PSD_STAGING_MAX     1024
static float             psd_staging[PSD_STAGING_MAX];
static size_t            psd_staging_len    = 0;
static struct timespec   psd_staging_ts     = {0, 0};
static bool              psd_flush_pending  = false;
static pthread_mutex_t   psd_mutex          = PTHREAD_MUTEX_INITIALIZER;

/* Protects the audio_worker pointer: the PulseAudio capture thread feeds
 * samples through audio_cb while the LVGL thread may tear the worker down
 * (band change, FT4/FT8 switch, dialog close). */
static pthread_mutex_t   audio_worker_mutex = PTHREAD_MUTEX_INITIALIZER;

static void construct_cb(lv_obj_t *parent);
static void key_cb(lv_event_t * e);
static void destruct_cb();
static void audio_cb(unsigned int n, float *samples);
static void rotary_cb(int32_t diff);

/* audio_worker callbacks (run on worker thread). Direct LVGL widget
 * access must go through scheduler_put() to land on the LVGL task.
 * Subject writes (subject_set_int) are thread-safe and run synchronously. */
static void on_message_cb(const char *text, int snr, float freq_hz, float time_sec,
                          const slot_info_t *info, void *ctx);
static void on_psd_cb(const float *psd, uint16_t nfft,
                      struct timespec frame_ts,
                      float sec_since_slot_start,
                      const slot_info_t *info, void *ctx);
static void on_slot_end_cb(const slot_info_t *info, void *ctx);
static void on_tick_cb(const slot_info_t *info, bool new_slot,
                       float sec_since_slot_start, void *ctx);

static const char * cq_all_label_getter();
static const char * protocol_label_getter();
static const char * tx_cq_label_getter();
static const char * tx_call_label_getter();
static const char * hold_freq_label_getter();
static const char * auto_label_getter();
static const char * auto_mode_label_getter();
static const char * processor_label_getter();

static void show_cq_all_cb(struct button_data_t *btn_data);
static void mode_ft4_ft8_cb(struct button_data_t *btn_data);
static void tx_cq_en_dis_cb(struct button_data_t *btn_data);
static void cq_rearm(void);
static void tx_call_en_dis_cb(struct button_data_t *btn_data);

static void hold_tx_freq_cb(struct button_data_t *btn_data);
static void mode_auto_cb(struct button_data_t *btn_data);
static void mode_auto_sel_cb(struct button_data_t *btn_data);
static void mode_processor_cb(struct button_data_t *btn_data);
static void cq_modifier_cb(struct button_data_t *btn_data);
static void time_sync(struct button_data_t *btn_data);

static void force_save_qso(struct button_data_t *btn_data);
static void free_msg_cb(struct button_data_t *btn_data);
static void free_msg_open(void);
static void free_msg_close(void);
static bool free_msg_cancel_cb(void);
static bool free_msg_ok_cb(void);
static void save_qso_record(const ftx_qso_record_t *rec);
static size_t flush_unfinished_qsos(void);
static ftx_qso_context_t qso_context(void);
static void apply_qso_response(const ftx_qso_response_t *response, bool async_ui);
static void confirm_dismiss(void);
static void confirm_arm(const char *call, bool tx_odd, float tx_max_delay);
static void confirm_accept(void);
static void confirm_dismiss_cb(void *arg);
static void confirm_dismiss_async(void);

const char *auto_dnf_label_getter(void);
static void auto_dnf_cb(struct button_data_t *btn_data);

static void on_table_press(const cell_data_t *cell_data);
static void on_table_close(void);
static void on_table_vol_change(int32_t delta);

static void keyboard_open();
static bool keyboard_cancel_cb();
static bool keyboard_ok_cb();
static void keyboard_close();

static void add_slot_info(ft8_cell_type_t cell_type, const char *direction);
static void add_tx_text(const char * text);
static bool get_time_slot(struct timespec now, float *time_since_start);


// button label is current state, press action and name - next state

static buttons_page_t btn_page_1;
static buttons_page_t btn_page_2;
static buttons_page_t btn_page_3;
static buttons_page_t btn_page_4;

static button_data_t button_page_1 = { .type=BTN_TEXT, .label = "(Page: 1:4)", .press = button_next_page_cb, .next=&btn_page_2};
static button_data_t button_free_msg = { .type=BTN_TEXT, .label = "Free\nMSG", .press = free_msg_cb };
static button_data_t button_tx_cq_en_dis = { .type=BTN_TEXT_FN, .label_fn = tx_cq_label_getter, .press = tx_cq_en_dis_cb };
static button_data_t button_auto_en_dis = { .type=BTN_TEXT_FN, .label_fn = auto_label_getter, .press = mode_auto_cb };
static button_data_t button_auto_mode = { .type=BTN_TEXT_FN, .label_fn = auto_mode_label_getter, .press = mode_auto_sel_cb, .subj=&cfg.ft8_auto_mode.val };

static button_data_t button_page_2 = { .type=BTN_TEXT, .label = "(Page: 2:4)", .press = button_next_page_cb, .next=&btn_page_3};
static button_data_t button_show_cq_all = { .type=BTN_TEXT_FN, .label_fn = cq_all_label_getter, .press = show_cq_all_cb, .subj=&cfg.ft8_show_all.val};
static button_data_t button_mode_ft4_ft8 = { .type=BTN_TEXT_FN, .label_fn = protocol_label_getter, .press = mode_ft4_ft8_cb, .subj=&cfg.ft8_protocol.val };
static button_data_t button_hold_freq = { .type=BTN_TEXT_FN, .label_fn = hold_freq_label_getter, .press = hold_tx_freq_cb, .subj=&cfg.ft8_hold_freq.val };
static button_data_t button_tx_call_en_dis = { .type=BTN_TEXT_FN, .label_fn = tx_call_label_getter, .press = tx_call_en_dis_cb};

static button_data_t button_page_3 = { .type=BTN_TEXT, .label = "(Page: 3:4)", .press = button_next_page_cb, .next=&btn_page_4};
static button_data_t button_force_save = { .type=BTN_TEXT, .label = "Force QSO\nsave", .press = force_save_qso };
static button_data_t button_cq_mod = { .type=BTN_TEXT, .label = "CQ\nModifier", .press = cq_modifier_cb };
static button_data_t button_time_sync = { .type=BTN_TEXT, .label = "Time\nSync", .press = time_sync };
static button_data_t button_auto_dnf = { .type=BTN_TEXT_FN, .label_fn = auto_dnf_label_getter, .press = auto_dnf_cb, .subj=&cfg.ft8_auto_dnf.val };

static button_data_t button_page_4 = { .type=BTN_TEXT, .label = "(Page: 4:4)", .press = button_next_page_cb, .next=&btn_page_1};
static button_data_t button_processor = { .type=BTN_TEXT_FN, .label_fn = processor_label_getter, .press = mode_processor_cb };

static buttons_page_t btn_page_1 = {
    {&button_page_1, &button_free_msg, &button_tx_cq_en_dis, &button_auto_en_dis, &button_auto_mode}
};

static buttons_page_t btn_page_2 = {
    {&button_page_2, &button_show_cq_all, &button_mode_ft4_ft8, &button_hold_freq, &button_tx_call_en_dis}
};

static buttons_page_t btn_page_3 = {
    {&button_page_3, &button_force_save, &button_cq_mod, &button_time_sync, &button_auto_dnf}
};

static buttons_page_t btn_page_4 = {
    {&button_page_4, &button_processor}
};

static dialog_t dialog = {
    .run = false,
    .construct_cb = construct_cb,
    .destruct_cb = destruct_cb,
    .audio_cb = audio_cb,
    .rotary_cb = rotary_cb,
    .key_cb = key_cb,
};

dialog_t *dialog_ft8 = &dialog;

/* ---- async LVGL helpers — safe to call from audio worker thread -------- */

static void set_freq(uint32_t freq);

static void finder_set_cursor_cb(void *data) {
    /* The dialog may have been destroyed between scheduling and execution;
     * finder is NULLed in destruct_cb, so a stale item must not touch it. */
    if (!finder) return;
    int16_t freq = *(int16_t *)data;
    lv_finder_set_cursor(finder, freq);
}

static void finder_set_cursor_async(int16_t freq_hz) {
    scheduler_put(finder_set_cursor_cb, &freq_hz, sizeof(freq_hz));
}

static void finder_clear_cursor_cb(void *data) {
    (void)data;
    if (!finder) return;
    lv_finder_clear_cursor(finder);
}

static void finder_clear_cursor_async(void) {
    scheduler_put_noargs(finder_clear_cursor_cb);
}

static void set_freq_async_cb(void *data) {
    if (!finder) return;   /* dialog already destroyed */
    uint32_t freq = *(uint32_t *)data;
    set_freq(freq);
}

static void set_freq_async(uint32_t freq_hz) {
    scheduler_put(set_freq_async_cb, &freq_hz, sizeof(freq_hz));
}

static void worker_init() {
    ftx_qso_reset();

    audio_worker_cb_t cb = {
        .on_message  = on_message_cb,
        .on_psd      = on_psd_cb,
        .on_slot_end = on_slot_end_cb,
        .on_tick     = on_tick_cb,
        .ctx         = NULL,
    };
    audio_worker_t *w = audio_worker_create(
        SAMPLE_RATE,
        subject_get_int(cfg.ft8_protocol.val),
        filter_low, filter_high,
        &cb);
    pthread_mutex_lock(&audio_worker_mutex);
    audio_worker = w;
    pthread_mutex_unlock(&audio_worker_mutex);
    if (w) {
        audio_worker_start(w);
    }
}

static void worker_done() {
    /* The engine state is about to be dropped (dialog close, FT4/FT8 or
     * band switch re-inits the worker): silently log any QSO that has both
     * reports but never got its final RR73/73. */
    flush_unfinished_qsos();

    state = RX_PROCESS;

    /* Detach the pointer under the mutex first: once audio_worker is NULL,
     * the PulseAudio capture thread (audio_cb) can no longer reach the
     * worker, and any in-flight feed has finished before the lock was
     * granted. Only then is it safe to join and free it. */
    pthread_mutex_lock(&audio_worker_mutex);
    audio_worker_t *w = audio_worker;
    audio_worker = NULL;
    pthread_mutex_unlock(&audio_worker_mutex);
    if (w) {
        audio_worker_destroy(w);
    }
    radio_set_modem(false);

    lv_finder_clear_cursor(finder);
    tx_msg.msg[0] = '\0';
    tx_msg.force_free_text = false;
    tx_msg_oneshot = false;
    decoded_slot_msg_count = 0;
    ftx_qso_abort_pending();
    ui_confirm_pending = false;
    confirm_deadline = 0;
}

/* Table widget lifecycle, draw, scroll and message insertion all live
 * in ft8/table_view.c. The dialog only owns the surrounding state. */

static void key_cb(lv_event_t * e) {
    uint32_t key = *((uint32_t *) lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct(&dialog);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static void destruct_cb() {
    // TODO: check free mem
    keyboard_close();

    /* The 1-shot fade timer from rotary_cb references the table widget;
     * it must not fire after the table is destroyed. */
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    fade_run = false;

    /* Module extension point: cleanup
     * Thread: LVGL / main (destruct_cb runs on dialog close).
     * Timing: AFTER worker_done() — join the audio worker first so
     * on_message / on_psd / on_slot_end / on_tick callbacks cannot touch
     * module state while it is being torn down (UAF / race; see P0-2).
     * Order: reverse of init extension calls if modules depend on each other.
     * Example:
     *   worker_done();
     *   ft8_log_on_cleanup();
     *   ft8_autodnf_on_cleanup();
     *   autosel_cleanup_state(); */

    subject_set_int(cq_enabled, CQ_OFF);

    worker_done();
    ft8_log_on_cleanup();
    ft8_autodnf_on_cleanup();
    ft8_remote_on_cleanup();
    table_view_destroy();

    /* The LVGL objects themselves are deleted by dialog_destruct() via
     * lv_obj_del(dialog.obj). NULL the static handles so stale scheduler
     * items queued by the (already joined) worker thread become no-ops
     * instead of dereferencing freed widgets. */
    finder    = NULL;
    waterfall = NULL;

    pthread_mutex_lock(&psd_mutex);
    psd_staging_len   = 0;
    psd_flush_pending = false;
    pthread_mutex_unlock(&psd_mutex);

    dsp_set_waterfall_enabled(true);
    dsp_set_spectrum_enabled(true);

    mem_load(MEM_BACKUP_ID);

    main_screen_lock_mode(false);
    main_screen_lock_ab(false);
    main_screen_lock_freq(false);
    main_screen_lock_band(false);

    radio_set_pwr(subject_get_float(cfg.pwr.val));
    adif_log_close(ft8_log);
}

static void load_band(int8_t dir) {
    cfg_digital_type_t type;
    switch (subject_get_int(cfg.ft8_protocol.val)) {
        case FTX_PROTOCOL_FT8:
            type = CFG_DIG_TYPE_FT8;
            lv_finder_set_width(finder, FT8_WIDTH_HZ);
            break;

        case FTX_PROTOCOL_FT4:
            type = CFG_DIG_TYPE_FT4;
            lv_finder_set_width(finder, FT4_WIDTH_HZ);
            break;
    }
    bool res = cfg_digital_load(dir, type);
    if (res) {
        msg_update_text_fmt("%s", cfg_digital_label_get());
    }
}

/// @brief Clean waterfall and table
static void clean_screen() {
    table_view_reset();
    lv_waterfall_clear_data(waterfall);

    int32_t *c = malloc(sizeof(int32_t));
    *c = LV_KEY_UP;

    lv_event_send(table, LV_EVENT_KEY, c);
}

static void band_cb(lv_event_t * e) {
    int8_t dir;

    if (lv_event_get_code(e) == EVENT_BAND_UP) {
        dir = 1;
    } else {
        dir = -1;
    }

    load_band(dir);

    worker_done();
    worker_init();
    clean_screen();
}

static void msg_timer(lv_timer_t *t) {
    lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0), LV_OPA_COVER);
    lv_anim_start(&fade);
    timer = NULL;
}

static void fade_anim(void * obj, int32_t v) {
    lv_obj_set_style_opa_layered(obj, v, 0);
}

static void fade_ready(lv_anim_t * a) {
    fade_run = false;
}

static void set_freq(uint32_t freq) {
    if (freq > filter_high) {
        freq = filter_high;
    }

    if (freq < filter_low) {
        freq = filter_low;
    }

    params_uint16_set(&params.ft8_tx_freq, freq);

    lv_finder_set_value(finder, freq);
    lv_obj_invalidate(finder);
}

static void rotary_cb(int32_t diff) {
    uint32_t abs_diff = abs(diff);
    if (abs_diff > 3) {
        diff *= (abs_diff < 6) ? 5 : 10;
    }
    uint32_t f = params.ft8_tx_freq.x + diff;
    f = limit(f, filter_low, filter_high - (subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8 ? FT8_WIDTH_HZ : FT4_WIDTH_HZ));

    set_freq(f);

    if (!fade_run) {
        fade_run = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa_layered(table, 0), LV_OPA_TRANSP);
        lv_anim_start(&fade);
    }

    if (timer) {
        lv_timer_reset(timer);
    } else {
        timer = lv_timer_create(msg_timer, 1000, NULL);
        lv_timer_set_repeat_count(timer, 1);
    }

}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    /* FT8 dialog owns the full screen + audio pipe; main_screen's spectrum
     * and waterfall are not visible, so skip their DSP cost entirely. The
     * companion lv_obj_invalidate() in tx_info handles the side effect of
     * losing the spectrum redraw that previously refreshed PWR/SWR bars. */
    dsp_set_waterfall_enabled(false);
    dsp_set_spectrum_enabled(false);

    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_UP, NULL);
    lv_obj_add_event_cb(dialog.obj, band_cb, EVENT_BAND_DOWN, NULL);

    if (!cq_enabled) {
        cq_enabled = subject_create_int(CQ_OFF);
        button_tx_cq_en_dis.subj = &cq_enabled;
    } else {
        subject_set_int(cq_enabled, CQ_OFF);
    }
    if (!auto_level) {
        auto_level = subject_create_int(FTX_QSO_AUTO_OFF);
        button_auto_en_dis.subj = &auto_level;
    } else {
        subject_set_int(auto_level, FTX_QSO_AUTO_OFF);
    }
    if (!qso_proc) {
        qso_proc = subject_create_int(FTX_QSO_PROC_NORMAL);
        button_processor.subj = &qso_proc;
    } else {
        subject_set_int(qso_proc, FTX_QSO_PROC_NORMAL);
    }
    if (!tx_enabled) {
        tx_enabled = subject_create_int(true);
        button_tx_call_en_dis.subj = &tx_enabled;
    }

    buttons_load_page(&btn_page_1);

    /* Audio pipeline (cbuffer/firdecim/spgramcf/decoder thread) is created
     * lazily inside worker_init() -> audio_worker_create(). */

    /* Waterfall */

    waterfall = lv_waterfall_create(dialog.obj);

    lv_obj_add_style(waterfall, &waterfall_style, 0);
    lv_obj_clear_flag(waterfall, LV_OBJ_FLAG_SCROLLABLE);

    lv_waterfall_set_palette(waterfall, (lv_color_t*)wf_palette, 256);
    lv_waterfall_set_size(waterfall, WIDTH, 325);
    lv_waterfall_set_min(waterfall, -60);

    lv_obj_set_pos(waterfall, 13, 13);
    ft8_autodnf_set_waterfall(waterfall);

    /* Freq finder */

    finder = lv_finder_create(waterfall);

    lv_finder_set_width(finder, 50);
    lv_finder_set_value(finder, params.ft8_tx_freq.x);

    lv_obj_set_size(finder, WIDTH, 325);
    lv_obj_set_pos(finder, 0, 0);

    lv_obj_set_style_radius(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(finder, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(finder, LV_OPA_0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(finder, bg_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    lv_obj_set_style_border_width(finder, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(finder, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(finder, LV_OPA_50, LV_PART_INDICATOR);

    /* Table */

    table_view_build(dialog.obj, 13, 13 + 55, WIDTH, 325 - 55);
    table_view_set_press_cb(on_table_press);
    table_view_actions_t tv_actions = {
        .on_close      = on_table_close,
        .on_vol_change = on_table_vol_change,
    };
    table_view_set_actions(&tv_actions);

    /* Fade */

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, table);
    lv_anim_set_time(&fade, 250);
    lv_anim_set_exec_cb(&fade, fade_anim);
    lv_anim_set_ready_cb(&fade, fade_ready);

    /* * */

    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);

    mem_save(MEM_BACKUP_ID);
    load_band(0);

    filter_low = subject_get_int(cfg_cur.filter.low);
    filter_high = subject_get_int(cfg_cur.filter.high);

    lv_finder_set_range(finder, filter_low, filter_high);

    qth_str_to_pos(params.qth.x, &cur_lat, &cur_lon);

    main_screen_lock_ab(true);
    main_screen_lock_mode(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);

    worker_init();

    /* Logger */
    ft8_log = adif_log_init("/mnt/ft_log.adi");

    if (subject_get_float(cfg.pwr.val) > MAX_PWR) {
        radio_set_pwr(MAX_PWR);
        msg_schedule_text_fmt("Power was limited to %0.0fW", MAX_PWR);
    }

    // setup gain offset
    float target_pwr = LV_MIN(subject_get_float(cfg.pwr.val), MAX_PWR);
    if (x6100_control_get_base_ver().rev >= 3) {
        // patched firmware has a true power control
        base_gain_offset = -9.4f;
    } else {
        base_gain_offset = -16.4f + log10f(target_pwr) * 10.0f;
    }

    /* Module extension point: init
     * Thread: LVGL / main (construct_cb runs on dialog open).
     * Timing: after worker_init() and base gain setup — audio worker and
     * worker/table state is ready; modules may register buttons or load files.
     * Example: ft8_log_on_init(); ft8_autodnf_on_init(); */
    ft8_log_on_init();
    ft8_autodnf_on_init(dialog.obj);
    ft8_remote_on_init();
    /* Overlay is a waterfall child; keep the decode table on top. */
    lv_obj_move_foreground(table);
}

/* Buttons */

const char *cq_all_label_getter() {
    static char buf[32];
    sprintf(buf, "Show:\n%s", subject_get_int(cfg.ft8_show_all.val) ? "All": "CQ");
    return buf;
}

const char *protocol_label_getter() {
    static char buf[32];
    sprintf(buf, "Mode:\n%s", subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8 ? "FT8": "FT4");
    return buf;
}

const char *tx_cq_label_getter() {
    static const char *cq_names[] = {"Off", "Even", "Odd"};
    static char buf[32];
    int cq = subject_get_int(cq_enabled);
    if ((cq < CQ_OFF) || (cq > CQ_ODD)) {
        cq = CQ_OFF;
    }
    sprintf(buf, "TX CQ:\n%s", cq_names[cq]);
    return buf;
}

const char *tx_call_label_getter() {
    static char buf[32];
    sprintf(buf, "TX Call:\n%s", subject_get_int(tx_enabled) ? "Enabled": "Disabled");
    return buf;
}

const char *hold_freq_label_getter() {
    static char buf[32];
    sprintf(buf, "Hold Freq:\n%s", subject_get_int(cfg.ft8_hold_freq.val) ? "Enabled": "Disabled");
    return buf;
}

const char *auto_label_getter() {
    static const char *level_names[] = {"Off", "Res", "Full", "Pre"};
    static char buf[32];
    int level = subject_get_int(auto_level);
    if ((level < FTX_QSO_AUTO_OFF) || (level > FTX_QSO_AUTO_PRE)) {
        level = FTX_QSO_AUTO_OFF;
    }
    sprintf(buf, "Auto:\n%s", level_names[level]);
    return buf;
}

const char *auto_mode_label_getter() {
    static const char *sel_names[] = {"SNR", "Dist", "Rnd", "Grid"};
    static char buf[32];
    int sel = subject_get_int(cfg.ft8_auto_mode.val);
    if ((sel < FTX_QSO_SEL_SNR) || (sel > FTX_QSO_SEL_NEW_GRID)) {
        sel = FTX_QSO_SEL_SNR;
    }
    sprintf(buf, "Auto Mode:\n%s", sel_names[sel]);
    return buf;
}

const char *processor_label_getter() {
    static char buf[32];
    int proc = subject_get_int(qso_proc);
    if ((proc < FTX_QSO_PROC_NORMAL) || (proc > FTX_QSO_PROC_NA_VHF)) {
        proc = FTX_QSO_PROC_NORMAL;
    }
    sprintf(buf, "Processor:\n%s",
            (proc == FTX_QSO_PROC_NA_VHF) ? "NA VHF" : "Normal");
    return buf;
}

const char *auto_dnf_label_getter(void) {
    static char buf[32];
    sprintf(buf, "Auto DNF:\n%s", subject_get_int(cfg.ft8_auto_dnf.val) ? "On" : "Off");
    return buf;
}

static void auto_dnf_cb(struct button_data_t *btn_data) {
    (void)btn_data;
    if (disable_buttons) return;
    ft8_set_auto_dnf(!subject_get_int(cfg.ft8_auto_dnf.val));
}

static void show_cq_all_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    ft8_set_show_all(!subject_get_int(cfg.ft8_show_all.val));
}

static void mode_ft4_ft8_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;

    ftx_protocol_t proto = subject_get_int(cfg.ft8_protocol.val);
    if (proto == FTX_PROTOCOL_FT8)
        proto = FTX_PROTOCOL_FT4;
    else {
        proto = FTX_PROTOCOL_FT8;
    }
    ft8_set_protocol(proto);
}

/* Any Auto / Auto Mode / click change ends the CQ loop and restarts the
 * engine decision state; a pending engine one-shot belongs to the old
 * setting and is dropped. The peers ledger survives (see qso.h). */
static void qso_setting_changed(void) {
    if (subject_get_int(cq_enabled) != CQ_OFF) {
        subject_set_int(cq_enabled, CQ_OFF);
        tx_msg.msg[0] = '\0';
        tx_msg.force_free_text = false;
    }
    if (tx_msg_oneshot) {
        tx_msg.msg[0] = '\0';
        tx_msg.force_free_text = false;
        tx_msg_oneshot = false;
    }
    ftx_qso_clear_decision_state();
    confirm_dismiss();
}

static void mode_auto_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    int level = subject_get_int(auto_level);
    if ((level < FTX_QSO_AUTO_OFF) || (level >= FTX_QSO_AUTO_PRE)) {
        level = FTX_QSO_AUTO_OFF;
    } else {
        level++;
    }
    ft8_set_auto_level((ftx_qso_auto_t)level);
}

static void mode_auto_sel_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    int sel = subject_get_int(cfg.ft8_auto_mode.val);
    if ((sel < FTX_QSO_SEL_SNR) || (sel >= FTX_QSO_SEL_NEW_GRID)) {
        sel = FTX_QSO_SEL_SNR;
    } else {
        sel++;
    }
    ft8_set_auto_mode((ftx_qso_sel_t)sel);
}

static void mode_processor_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    int proc = subject_get_int(qso_proc);
    if (proc == FTX_QSO_PROC_NORMAL) {
        proc = FTX_QSO_PROC_NA_VHF;
    } else {
        proc = FTX_QSO_PROC_NORMAL;
    }
    ft8_set_processor((ftx_qso_proc_t)proc);
}

static void hold_tx_freq_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    ft8_set_hold_freq(!subject_get_int(cfg.ft8_hold_freq.val));
}

/* (Re)load the CQ text into tx_msg with a fresh repeats budget, aimed at
 * the parity held in cq_enabled. Called on CQ enable and whenever the
 * engine reply that displaced the CQ freed the TX slot again — a served
 * responder just proved propagation, so the countdown restarts. */
static void cq_rearm(void) {
    const char *mod = params.ft8_cq_modifier.x;
    if (subject_get_int(qso_proc) == FTX_QSO_PROC_NA_VHF) {
        mod = "TEST";
    }
    cq_make_message(params.callsign.x, params.qth.x, mod, tx_msg.msg);
    tx_msg.repeats = subject_get_int(cfg.ft8_max_repeats.val);
    tx_msg.force_free_text = false;
    tx_msg_oneshot = false;
    tx_time_slot = (subject_get_int(cq_enabled) == CQ_ODD);
}

void ft8_set_auto_dnf(bool enable) {
    if (disable_buttons) return;
    subject_set_int(cfg.ft8_auto_dnf.val, enable);
    if (!enable) {
        ft8_autodnf_restore_entry();
    }
}

void ft8_set_show_all(bool show_all) {
    if (disable_buttons) return;
    subject_set_int(cfg.ft8_show_all.val, show_all);
}

void ft8_set_protocol(ftx_protocol_t proto) {
    if (disable_buttons) return;
    subject_set_int(cfg.ft8_protocol.val, proto);
    subject_set_int(cq_enabled, CQ_OFF);

    worker_done();
    worker_init();
    clean_screen();
    load_band(0);
}

void ft8_set_auto_level(ftx_qso_auto_t level) {
    if (disable_buttons) return;
    subject_set_int(auto_level, level);
    qso_setting_changed();
}

void ft8_set_auto_mode(ftx_qso_sel_t sel) {
    if (disable_buttons) return;
    subject_set_int(cfg.ft8_auto_mode.val, sel);
    qso_setting_changed();
}

void ft8_set_processor(ftx_qso_proc_t proc) {
    if (disable_buttons) return;
    subject_set_int(qso_proc, proc);
    qso_setting_changed();
}

void ft8_set_hold_freq(bool hold) {
    if (disable_buttons) return;
    subject_set_int(cfg.ft8_hold_freq.val, hold);
}

void ft8_set_cq(cq_state_t st) {
    if (disable_buttons) return;

    if (st == CQ_OFF) {
        if (state == TX_PROCESS) {
            state = RX_PROCESS;
        }
        subject_set_int(cq_enabled, CQ_OFF);
        tx_msg.msg[0] = '\0';
        tx_msg.force_free_text = false;
        return;
    }

    if (strlen(params.callsign.x) == 0) {
        msg_schedule_text_fmt("Call sign required");
        ft8_remote_set_status("Call sign required");
        return;
    }

    subject_set_int(cq_enabled, st);
    subject_set_int(tx_enabled, true);
    subject_set_int(auto_level, FTX_QSO_AUTO_RES);
    ftx_qso_clear_decision_state();
    cq_rearm();

    if (tx_msg.msg[2] == '_') {
        msg_schedule_text_fmt("Next TX: CQ %s", tx_msg.msg + 3);
        char status[64];
        snprintf(status, sizeof(status), "Next TX: CQ %s", tx_msg.msg + 3);
        ft8_remote_set_status(status);
    } else {
        msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
        char status[64];
        snprintf(status, sizeof(status), "Next TX: %s", tx_msg.msg);
        ft8_remote_set_status(status);
    }
    if (finder) {
        lv_finder_clear_cursor(finder);
    }
}

void ft8_set_tx_call(bool enable) {
    if (disable_buttons) return;

    if (enable) {
        if (strlen(params.callsign.x) == 0) {
            msg_schedule_text_fmt("Call sign required");
            ft8_remote_set_status("Call sign required");
            return;
        }
        subject_set_int(tx_enabled, true);
    } else {
        if (state == TX_PROCESS) {
            state = RX_PROCESS;
        }
        subject_set_int(tx_enabled, false);
    }
}

static void tx_cq_en_dis_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;

    int cq = subject_get_int(cq_enabled);
    cq = ((cq < CQ_OFF) || (cq >= CQ_ODD)) ? CQ_OFF : (cq + 1);
    ft8_set_cq((cq_state_t)cq);
}

static void tx_call_en_dis_cb(struct button_data_t *btn_data) {
    if (disable_buttons)
        return;

    ft8_set_tx_call(!subject_get_int(tx_enabled));
}

static void tx_call_off() {
    state = RX_PROCESS;
    subject_set_int(tx_enabled, false);
}

static void cq_modifier_cb(struct button_data_t *btn_data) {
    if (disable_buttons) return;
    keyboard_open();
}

void ft8_time_sync(void) {
    time_t now = time(NULL);
    uint8_t sec = now % 60;
    float drift, slot_time;
    switch (subject_get_int(cfg.ft8_protocol.val)) {
        case FTX_PROTOCOL_FT4:
            slot_time = FT4_SLOT_TIME;
            break;

        case FTX_PROTOCOL_FT8:
            slot_time = FT8_SLOT_TIME;
            break;
        default:
            slot_time = FT8_SLOT_TIME;
            break;
    }
    drift = fmodf(sec + slot_time / 2, slot_time) - slot_time / 2;
    struct timespec tp;

    now -= (int) drift;
    tp.tv_sec = now;
    tp.tv_nsec = 0;

    int res = clock_settime(CLOCK_REALTIME, &tp);
    if (res != 0)
    {
        LV_LOG_ERROR("Can't set system time: %s\n", strerror(errno));
        return;
    }
}

static void time_sync(struct button_data_t *btn_data) {
    (void)btn_data;
    ft8_time_sync();
}

void ft8_force_save(void) {
    size_t n = flush_unfinished_qsos();
    if (n > 0) {
        msg_schedule_text_fmt("Saved %u QSO%s", (unsigned)n, (n > 1) ? "s" : "");
        char status[64];
        snprintf(status, sizeof(status), "Saved %u QSO%s", (unsigned)n, (n > 1) ? "s" : "");
        ft8_remote_set_status(status);
        finder_clear_cursor_async();
    } else {
        msg_schedule_text_fmt("No QSO to save");
        ft8_remote_set_status("No QSO to save");
    }
}

static void force_save_qso(struct button_data_t *btn_data) {
    (void)btn_data;
    ft8_force_save();
}

static void on_table_press(const cell_data_t *cell_data) {
    if (state == TX_PROCESS) {
        tx_call_off();
        return;
    }

    if ((cell_data == NULL) ||
        (cell_data->cell_type == CELL_TX_MSG) ||
        (cell_data->cell_type == CELL_RX_INFO) ||
        (cell_data->cell_type == CELL_TX_INFO) ||
        (cell_data->cell_type == CELL_START_QSO)
    ) {
        msg_schedule_text_fmt("What should I do about it?");
        return;
    }

    /* A click is the freshest user intent: it ends the CQ loop, drops the
     * Auto knob to Off and restarts the engine as a pure manual QSO. */
    subject_set_int(auto_level, FTX_QSO_AUTO_OFF);
    qso_setting_changed();

    ftx_qso_context_t ctx = qso_context();
    ftx_qso_response_t response;
    ftx_qso_on_user_message(&ctx,
                            cell_data->text,
                            cell_data->meta.local_snr,
                            cell_data->meta.freq_hz,
                            cell_data->odd,
                            &response);
    if (response.save) {
        save_qso_record(&response.qso);
    }
    if (response.action == FTX_QSO_ACTION_TX) {
        apply_qso_response(&response, false);
        {
            cell_data_t cd = {0};
            cd.cell_type = CELL_START_QSO;
            cd.time = time(NULL);
            snprintf(cd.text, sizeof(cd.text), "Start QSO with %s", cell_data->meta.call_de);
            scheduler_put(table_view_add_msg_cb, &cd, sizeof(cell_data_t));
        }
    } else {
        msg_schedule_text_fmt("Invalid message");
        ft8_remote_set_status("Invalid message");
        tx_call_off();
    }
}

static void on_table_close(void) {
    dialog_destruct(&dialog);
}

static void on_table_vol_change(int32_t delta) {
    radio_change_vol(delta);
}

static void keyboard_open() {
    lv_group_remove_obj(table);
    textarea_window_open(keyboard_ok_cb, keyboard_cancel_cb);
    lv_obj_t *text = textarea_window_text();

    lv_textarea_set_accepted_chars(text,
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    );

    if (strlen(params.ft8_cq_modifier.x) > 0) {
        textarea_window_set(params.ft8_cq_modifier.x);
    } else {
        lv_obj_t *text = textarea_window_text();
        lv_textarea_set_placeholder_text(text, " CQ modifier");
    }
    disable_buttons = true;
}

static void keyboard_close() {
    textarea_window_close();
    lv_group_add_obj(keyboard_group, table);
    lv_group_set_editing(keyboard_group, true);
    disable_buttons = false;
}

static bool keyboard_cancel_cb() {
    keyboard_close();
    return true;
}

static bool keyboard_ok_cb() {
    char *cq_mod = (char *)textarea_window_get();
    if ((strlen(cq_mod) > 0) && !is_cq_modifier(cq_mod)) {
        msg_schedule_text_fmt("Unsupported CQ modifier");
        ft8_remote_set_status("Unsupported CQ modifier");
        return false;
    }
    ft8_set_cq_modifier(cq_mod);
    keyboard_close();
    return true;
}

static void audio_cb(unsigned int n, float *samples) {
    pthread_mutex_lock(&audio_worker_mutex);
    if ((state == RX_PROCESS) && audio_worker) {
        audio_worker_feed(audio_worker, n, samples);
    }
    pthread_mutex_unlock(&audio_worker_mutex);
}

static bool get_time_slot(struct timespec now, float *sec_since_start) {
    bool cur_odd;
    float sec = (now.tv_sec % 60) + now.tv_nsec / 1.0e9f;

    switch (subject_get_int(cfg.ft8_protocol.val)) {
    case FTX_PROTOCOL_FT4:
        cur_odd = (int)(sec / FT4_SLOT_TIME) % 2;
        *sec_since_start = fmodf(sec, FT4_SLOT_TIME);
        break;

        case FTX_PROTOCOL_FT8:
        cur_odd = (int)(sec / FT8_SLOT_TIME) % 2;
        *sec_since_start = fmodf(sec, FT8_SLOT_TIME);
        break;
    }
    return cur_odd;
}


/* CQ message composition has moved to ft8/cq_scheduler.c
 * (see cq_make_message()). */

/* TX waveform synthesis + per-block ALC gain correction live in
 * ft8/tx_worker.c. The dialog only owns the per-session abort flag. */
static bool tx_should_abort_cb(void *ctx) {
    (void)ctx;
    return state != TX_PROCESS;
}

/**
 * Add RX/TX slot header to the table
 */
static void add_slot_info(ft8_cell_type_t cell_type, const char *direction) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm *ts = localtime(&now.tv_sec);
    if (!ts) return;

    cell_data_t cell_data = {0};
    cell_data.cell_type = cell_type;
    snprintf(cell_data.text, sizeof(cell_data.text), "%s %s %02i:%02i:%02i",
             direction, cfg_digital_label_get(), ts->tm_hour, ts->tm_min, ts->tm_sec);

    scheduler_put(table_view_add_msg_cb, &cell_data, sizeof(cell_data_t));
}

/**
 * Add TX message to the table
 */
static void add_tx_text(const char * text) {
    cell_data_t  cell_data = {0};
    cell_data.cell_type = CELL_TX_MSG;
    cell_data.time = time(NULL);

    strncpy(cell_data.text, text, sizeof(cell_data.text) - 1);
    if (strncmp(cell_data.text, "CQ_", 3) == 0) {
        cell_data.text[2] = ' ';
    }
    scheduler_put(table_view_add_msg_cb, &cell_data, sizeof(cell_data_t));
}

static ftx_qso_context_t qso_context(void) {
    int level = subject_get_int(auto_level);
    if ((level < FTX_QSO_AUTO_OFF) || (level > FTX_QSO_AUTO_PRE)) {
        level = FTX_QSO_AUTO_OFF;
    }
    int sel = subject_get_int(cfg.ft8_auto_mode.val);
    if ((sel < FTX_QSO_SEL_SNR) || (sel > FTX_QSO_SEL_NEW_GRID)) {
        sel = FTX_QSO_SEL_SNR;
    }
    int proc = subject_get_int(qso_proc);
    if ((proc < FTX_QSO_PROC_NORMAL) || (proc > FTX_QSO_PROC_NA_VHF)) {
        proc = FTX_QSO_PROC_NORMAL;
    }
    ftx_qso_context_t ctx = {
        .local_callsign = params.callsign.x,
        .local_qth      = params.qth.x,
        .auto_level     = (ftx_qso_auto_t)level,
        .sel            = (ftx_qso_sel_t)sel,
        .proc           = (ftx_qso_proc_t)proc,
        .now            = time(NULL),
    };
    return ctx;
}

/* Persist a QSO record produced by the engine (ADIF file + sqlite log),
 * no UI. Safe from both the LVGL and the audio worker threads. */
static void save_qso_record_db(const ftx_qso_record_t *rec) {
    char *canonized_call = util_canonize_callsign(rec->call, false);
    uint64_t dial_hz = (uint64_t)subject_get_int(cfg_cur.fg_freq);
    uint64_t freq_hz = dial_hz;
    /* ADIF FREQ = dial + RX audio offset (tone-0 base), same idea as ft8d. */
    if (rec->freq_hz > 0.0f) {
        freq_hz = dial_hz + (uint64_t)llroundf(rec->freq_hz);
    }
    qso_log_record_t qso = qso_log_record_create(
        params.callsign.x,
        canonized_call,
        rec->end_time,
        subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8 ? MODE_FT8 : MODE_FT4,
        rec->rst_sent, rec->rst_rcvd, freq_hz, NULL, NULL,
        params.qth.x, rec->grid
    );
    free(canonized_call);

    bool contest = (subject_get_int(qso_proc) == FTX_QSO_PROC_NA_VHF);
    adif_add_qso(ft8_log, qso, contest ? "NA VHF contest" : NULL);
    if (contest) {
        qso_log_record_save_contest(qso);
    } else {
        qso_log_record_save(qso);
    }
}

/* db save + UI feedback (popup goes through the async helpers, so this
 * one is also fine from the audio worker thread). */
static void save_qso_record(const ftx_qso_record_t *rec) {
    save_qso_record_db(rec);

    if (strlen(rec->grid) >= 4) {
        double lat, lon, dist;
        qth_str_to_pos(rec->grid, &lat, &lon);
        dist = qth_pos_dist(lat, lon, cur_lat, cur_lon);
        msg_schedule_long_text_fmt("Saved QSO de %s %d %d (%.0f km)",
                                   rec->call, rec->rst_sent, rec->rst_rcvd, dist);
    } else {
        msg_schedule_long_text_fmt("Saved QSO de %s %d %d",
                                   rec->call, rec->rst_sent, rec->rst_rcvd);
    }

    finder_clear_cursor_async();
}

/* Log every QSO that has both reports but never got its final RR73/73
 * (peer vanished mid-QSO). Quiet — the callers own the UI feedback. */
static size_t flush_unfinished_qsos(void) {
    ftx_qso_record_t records[16];
    size_t total = 0;
    size_t n;

    while ((n = ftx_qso_flush_complete(records, 16)) > 0) {
        for (size_t i = 0; i < n; i++) {
            save_qso_record_db(&records[i]);
        }
        total += n;
    }
    return total;
}

/* Fill the logbook flags for one decoded message (remove_worked and the
 * NEW_GRID tie-break run on these; the engine never queries the db).
 * The sender's grid falls back to "worked" when unknown. */
static void worked_flags(const ftx_msg_meta_t *meta,
                         bool *worked, bool *grid_worked) {
    *worked      = false;
    *grid_worked = true;
    if (meta->call_de[0] == '\0') return;

    qso_log_mode_t mode = subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8
        ? MODE_FT8 : MODE_FT4;
    qso_log_band_t band = qso_log_freq_to_band(subject_get_int(cfg_cur.fg_freq));

    char local_grid4[5] = "";
    strncpy(local_grid4, params.qth.x, sizeof(local_grid4) - 1);
    char remote_grid4[5] = "";
    strncpy(remote_grid4, meta->grid, sizeof(remote_grid4) - 1);

    bool contest = (subject_get_int(qso_proc) == FTX_QSO_PROC_NA_VHF);
    if (contest) {
        *worked = qso_log_worked_pair_contest(params.callsign.x, local_grid4, mode, band,
                                              meta->call_de, remote_grid4);
        if (remote_grid4[0] != '\0') {
            *grid_worked = qso_log_worked_grid_contest(params.callsign.x, local_grid4,
                                                       mode, band, remote_grid4);
        }
    } else {
        *worked = qso_log_worked_pair(params.callsign.x, local_grid4, mode, band,
                                      meta->call_de, remote_grid4);
        if (remote_grid4[0] != '\0') {
            *grid_worked = qso_log_worked_grid(params.callsign.x, local_grid4,
                                               mode, band, remote_grid4);
        }
    }
}

static void decoded_slot_push(const char *text, int snr,
                              float freq_hz, float time_sec,
                              bool odd, const ftx_msg_meta_t *meta) {
    if (decoded_slot_msg_count >= DECODED_SLOT_MSG_MAX) return;

    size_t idx = decoded_slot_msg_count++;
    strncpy(decoded_slot_texts[idx], text, sizeof(decoded_slot_texts[idx]) - 1);
    decoded_slot_texts[idx][sizeof(decoded_slot_texts[idx]) - 1] = '\0';

    decoded_slot_msgs[idx].text     = decoded_slot_texts[idx];
    decoded_slot_msgs[idx].snr      = snr;
    decoded_slot_msgs[idx].freq_hz  = freq_hz;
    decoded_slot_msgs[idx].time_sec = time_sec;
    decoded_slot_msgs[idx].odd      = odd;
    worked_flags(meta,
                 &decoded_slot_msgs[idx].worked,
                 &decoded_slot_msgs[idx].grid_worked);
}

static void confirm_dismiss(void) {
    ui_confirm_pending = false;
    confirm_deadline   = 0;
}

static void confirm_dismiss_cb(void *arg) {
    LV_UNUSED(arg);
    confirm_dismiss();
}

static void confirm_dismiss_async(void) {
    scheduler_put_noargs(confirm_dismiss_cb);
}

static void confirm_arm(const char *call, bool tx_odd, float tx_max_delay) {
    ui_confirm_pending = true;
    confirm_tx_odd     = tx_odd;
    confirm_deadline   = get_time() + (uint64_t)(tx_max_delay * 1000.0f);
    msg_schedule_long_text_fmt("TX2 %s? PTT/VFO=OK",
                               (call && call[0]) ? call : "?");
}

static void confirm_accept(void) {
    if (!ui_confirm_pending) return;

    /* Same gate as arming: a TX Call pause flipped during the window must
     * not be overridden by apply_qso_response re-enabling tx_enabled. */
    if ((get_time() > confirm_deadline) || !subject_get_int(tx_enabled)) {
        ftx_qso_abort_pending();
        confirm_dismiss();
        return;
    }

    ftx_qso_context_t  qctx = qso_context();
    ftx_qso_response_t response;
    if (!ftx_qso_commit_pending(&qctx, &response)) {
        confirm_dismiss();
        return;
    }
    confirm_dismiss();
    apply_qso_response(&response, false);
}

static void apply_qso_response(const ftx_qso_response_t *response,
                               bool async_ui) {
    if (!response || response->action != FTX_QSO_ACTION_TX || response->tx_msg[0] == '\0') return;

    /* One-shot: transmit exactly once in the requested parity slot, then
     * wait for the next engine response. Retry policy is engine-internal. */
    strncpy(tx_msg.msg, response->tx_msg, sizeof(tx_msg.msg) - 1);
    tx_msg.msg[sizeof(tx_msg.msg) - 1] = '\0';
    tx_msg.repeats = 1;
    tx_msg_oneshot = true;
    tx_msg.force_free_text = false;
    tx_time_slot = response->tx_odd;
    subject_set_int(tx_enabled, true);
    /* The CQ loop (if any) stays enabled: an engine reply just takes over
     * the slot; on_slot_end_cb re-arms the CQ once the QSO is over. */

    float freq_hz = response->freq_hz;
    if (freq_hz > 0.0f) {
        if (async_ui) {
            finder_set_cursor_async(freq_hz);
            if (!subject_get_int(cfg.ft8_hold_freq.val)) {
                set_freq_async(freq_hz);
            }
        } else {
            lv_finder_set_cursor(finder, freq_hz);
            if (!subject_get_int(cfg.ft8_hold_freq.val)) {
                set_freq(freq_hz);
            }
        }
    }

    msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
    {
        char status[64];
        snprintf(status, sizeof(status), "Next TX: %s", tx_msg.msg);
        ft8_remote_set_status(status);
    }
}

/**
 * Parse and add RX messages to the table
 */
static void add_rx_text(int16_t snr, const char * text, slot_info_t *s_info, float freq_hz, float time_sec) {

    ftx_msg_meta_t *meta = &last_rx_meta;
    ftx_qso_context_t ctx = qso_context();
    ftx_qso_parse_rx_text(&ctx, text, snr, freq_hz, time_sec, meta);
    decoded_slot_push(text, snr, freq_hz, time_sec, s_info->odd, meta);

    ft8_cell_type_t cell_type;
    if (meta->to_me) {
        cell_type = CELL_RX_TO_ME;
    } else if (meta->type == FTX_MSG_TYPE_CQ) {
        cell_type = CELL_RX_CQ;
    } else if (!subject_get_int(cfg.ft8_show_all.val)) {
        return;
    } else {
        cell_type = CELL_RX_MSG;
    }

    cell_data_t  cell_data = {0};
    cell_data.time = time(NULL);

    qso_log_mode_t mode = subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8
        ? MODE_FT8 : MODE_FT4;
    qso_log_band_t band = qso_log_freq_to_band(subject_get_int(cfg_cur.fg_freq));
    bool contest = (subject_get_int(qso_proc) == FTX_QSO_PROC_NA_VHF);

    qso_log_search_worked_t sw = contest
        ? qso_log_search_worked_contest(meta->call_de, mode, band)
        : qso_log_search_worked(meta->call_de, mode, band);
    cell_data.call_worked = (sw != SEARCH_WORKED_NO);
    if (meta->type == FTX_MSG_TYPE_CQ) {
        cell_data.worked_type = sw;
    }

    {
        bool pair = false;
        bool grid = true;
        worked_flags(meta, &pair, &grid);
        cell_data.call_grid_worked = pair;
        cell_data.grid_worked = grid;
    }

    cell_data.cell_type = cell_type;
    strncpy(cell_data.text, text, sizeof(cell_data.text) - 1);
    cell_data.meta = *meta;
    cell_data.odd = s_info->odd;
    if (params.qth.x[0] != 0) {
        if (strlen(meta->grid) > 0) {
            double lat, lon;
            qth_str_to_pos(meta->grid, &lat, &lon);
            cell_data.dist = qth_pos_dist(lat, lon, cur_lat, cur_lon);
        } else {
            cell_data.dist = 0;
        }
    } else {
        cell_data.dist = 0;
    }
    scheduler_put(table_view_add_msg_cb, (void*)&cell_data, sizeof(cell_data_t));
}

/* ---- audio_worker callbacks (run on the worker thread) ---------------- */

static void on_message_cb(const char *text, int snr, float freq_hz, float time_sec,
                          const slot_info_t *info, void *ctx) {
    (void)ctx;
    add_rx_text((int16_t)snr, text, (slot_info_t *)info, freq_hz, time_sec);

    /* Module extension point: rx_msg
     * Thread: audio worker (same as this callback).
     * Timing: immediately after add_rx_text() — last_rx_meta and info are
     * valid; decoded_slot_msgs has the raw message for slot-end processing.
     * Constraint: no direct lv_* calls; use scheduler_put / *_async helpers.
     * Example: ft8_log_on_rx_msg(text, snr, freq_hz, time_sec, &last_rx_meta, info); */
    ft8_log_on_rx_msg(text, snr, freq_hz, time_sec, &last_rx_meta, info);
}

/*
 * Coalesced waterfall flush — runs on the LVGL task via scheduler.
 * Drains psd_staging[] and emits a single lv_waterfall_add_data call.
 */
static void flush_ft8_waterfall_cb(void *arg) {
    (void)arg;

    float           local_psd[PSD_STAGING_MAX];
    size_t          local_len;
    struct timespec local_ts;

    pthread_mutex_lock(&psd_mutex);
    local_len = psd_staging_len;
    if (local_len > 0) {
        memcpy(local_psd, psd_staging, local_len * sizeof(float));
    }
    local_ts = psd_staging_ts;
    psd_staging_len   = 0;
    psd_flush_pending = false;
    pthread_mutex_unlock(&psd_mutex);

    if ((local_len > 0) && waterfall) {
        lv_waterfall_add_data_with_ts(waterfall, local_psd, local_len, local_ts);
    }
}

static void on_psd_cb(const float *psd, uint16_t nfft,
                      struct timespec frame_ts,
                      float sec_since_slot_start,
                      const slot_info_t *info, void *ctx) {
    (void)ctx;
    (void)sec_since_slot_start;
    if (!psd || !nfft) return;

    uint32_t low_bin  = (uint32_t)nfft / 2u + (uint32_t)nfft * filter_low  / SAMPLE_RATE;
    uint32_t high_bin = (uint32_t)nfft / 2u + (uint32_t)nfft * filter_high / SAMPLE_RATE;
    if (high_bin > nfft) high_bin = nfft;
    if (low_bin >= high_bin) return;

    size_t len = high_bin - low_bin;
    if (len > PSD_STAGING_MAX) len = PSD_STAGING_MAX;

    /* Write latest PSD row into staging; schedule exactly one flush
     * when none is pending.  The LVGL task drains staging later. */
    pthread_mutex_lock(&psd_mutex);
    memcpy(psd_staging, &psd[low_bin], len * sizeof(float));
    psd_staging_len = len;
    psd_staging_ts = frame_ts;

    bool need_flush = !psd_flush_pending;
    if (need_flush) {
        psd_flush_pending = true;
    }
    pthread_mutex_unlock(&psd_mutex);

    /* Module extension point: psd
     * Thread: audio worker (same as this callback).
     * Timing: after core waterfall staging is queued — psd[] and filter bins
     * are valid; runs once per emitted PSD frame (~10 Hz).
     * Constraint: no direct lv_* / lv_waterfall_*; use scheduler_put only.
     * Marker is scheduled before the waterfall flush below. */
    ft8_autodnf_on_psd(psd, nfft, frame_ts, info);

    if (need_flush && !scheduler_put_noargs(flush_ft8_waterfall_cb)) {
        /* Flush item dropped (queue overflow): roll the flag back so a
         * later PSD frame can retry, otherwise the waterfall would freeze
         * with psd_flush_pending stuck at true. */
        pthread_mutex_lock(&psd_mutex);
        psd_flush_pending = false;
        pthread_mutex_unlock(&psd_mutex);
    }
}

static void on_slot_end_cb(const slot_info_t *info, void *ctx) {
    (void)ctx;

    /* Always consult the engine, even for a slot with zero decodes: an
     * empty slot is itself a signal (e.g. the peer stopped sending). */
    {
        ftx_qso_context_t qctx = qso_context();
        ftx_qso_response_t response;
        ftx_qso_on_decoded_messages(&qctx,
                                    decoded_slot_msgs,
                                    decoded_slot_msg_count,
                                    info->odd,
                                    &response);
        /* A QSO can complete with no TX scheduled (received final 73),
         * so the save flag is handled independently of the action. */
        if (response.save) {
            save_qso_record(&response.qso);
        }
        /* Respect the TX Call switch: the user can pause engine-driven
         * transmissions without leaving the mode.
         * A pending Free MSG wins its slot: the engine still ran above
         * (peers/save updated); only TX / CQ re-arm are deferred. */
        bool free_msg_pending = tx_msg.force_free_text && (tx_msg.msg[0] != '\0');
        float tx_max_delay = (subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8)
                             ? MAX_TX_START_DELAY : MAX_TX_START_DELAY_FT4;
        if (response.need_confirm) {
            /* Free MSG owns the slot; TX Call Off must not be overridden by
             * a later confirm_accept → apply_qso_response. */
            if (free_msg_pending || !subject_get_int(tx_enabled)) {
                ftx_qso_abort_pending();
            } else {
                confirm_arm(response.confirm_call, response.tx_odd, tx_max_delay);
                if ((subject_get_int(cq_enabled) != CQ_OFF) &&
                    (strncmp(tx_msg.msg, "CQ", 2) != 0)) {
                    cq_rearm();
                }
            }
        } else if ((response.action == FTX_QSO_ACTION_TX) && !free_msg_pending &&
            subject_get_int(tx_enabled)) {
            apply_qso_response(&response, true);
        } else if ((subject_get_int(cq_enabled) != CQ_OFF) && !free_msg_pending &&
                   (strncmp(tx_msg.msg, "CQ", 2) != 0)) {
            /* The TX slot is free again: the engine has nothing to send and
             * the reply that displaced the CQ was consumed (or its one-shot
             * window missed). Refill the slot with the CQ at a fresh repeats
             * budget — serving a responder just proved propagation. This is
             * not a "QSO over" call: if the peer was merely fading, its next
             * retry makes the engine displace the CQ again. A CQ still
             * sitting in tx_msg keeps its running countdown instead. */
            cq_rearm();
        }
        decoded_slot_msg_count = 0;
    }

    /* Module extension point: slot_end
     * Thread: audio worker (same as this callback).
     * Timing: at FT8/FT4 slot boundary, after final decode flush — info
     * describes the slot that just ended.
     * Constraint: no direct lv_* calls; use scheduler_put / *_async helpers.
     * Example: ft8_log_on_slot_end(info); ft8_autosel_on_slot_end(info); */
    ft8_log_on_slot_end(info);
}

static void on_tick_cb(const slot_info_t *info, bool new_slot,
                       float sec_since_slot_start, void *ctx) {
    (void)ctx;

    if (new_slot) {
        /* Workaround: BASE can key RF TX without any GUI TX_ATTEMPT (phantom
         * carrier). radio_thread only calls x6100_control_idle() while
         * state==RADIO_RX; stuck flow.flag.tx keeps RADIO_TX and disables that
         * 3s idle for hours. Re-push the full register shadow at each slot
         * boundary from here — still runs in RADIO_TX. Harmless on normal RX
         * (redundant with the periodic idle) and on TX slots (tx_worker keys
         * modem on immediately below if this slot transmits). */
        radio_idle();
    }

    bool have_tx_msg = tx_msg.msg[0] != '\0';
    bool tx_enabled_now = subject_get_int(tx_enabled);
    bool tx_slot_pending = have_tx_msg && tx_enabled_now && (tx_time_slot == info->odd);

    float tx_max_delay = (subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8)
                         ? MAX_TX_START_DELAY : MAX_TX_START_DELAY_FT4;

    /* TX2 confirm window: same clock as oneshot over-window. */
    if (ui_confirm_pending && (info->odd == confirm_tx_odd) &&
        (sec_since_slot_start >= tx_max_delay)) {
        if (ftx_qso_pending_active()) {
            ftx_qso_abort_pending();
        }
        confirm_dismiss_async();
    }

    if ((sec_since_slot_start < tx_max_delay) && tx_slot_pending) {
        /* Module extension point: pre_tx
         * Thread: audio worker (on_tick_cb).
         * Timing: sec_since_slot_start < max delay, tx_time_slot
         * matches info->odd, TX enabled, and tx_msg non-empty — immediately
         * before state = TX_PROCESS and tx_worker_run_with_config().
         * Use for: TX file log open, DNF marker clear, grid-swap on tx_msg.
         * Cannot defer TX from here without modifying core flow below.
         * Example: ft8_log_on_pre_tx(info, tx_text); */
        state = TX_PROCESS;

        /* Snapshot the message: the UI thread (on_table_press) may overwrite
         * tx_msg while the blocking TX below is in flight. */
        char tx_text[sizeof(tx_msg.msg)];
        strncpy(tx_text, tx_msg.msg, sizeof(tx_text) - 1);
        tx_text[sizeof(tx_text) - 1] = '\0';

        ft8_autodnf_on_pre_tx(info);
        ft8_log_on_pre_tx(info, tx_text);

        ft8_tx_config_t tx_cfg = {
            .tx_text              = tx_text,
            .base_gain_offset     = base_gain_offset,
            .force_free_text      = tx_msg.force_free_text,
            .sec_since_slot_start = sec_since_slot_start,
            .abort_check          = tx_should_abort_cb,
            .abort_check_ctx      = NULL,
        };
        add_slot_info(CELL_TX_INFO, "TX");
        add_tx_text(tx_text);
        tx_worker_run_with_config(&tx_cfg);
        state = RX_PROCESS;

        /* Module extension point: post_tx
         * Thread: audio worker (on_tick_cb).
         * Timing: immediately after tx_worker_run_with_config() returns —
         * TX slot finished; CQ repeats have not yet been decremented.
         * Example: ft8_autosel_on_post_tx(info); */

        /* Skip repeats bookkeeping if the user queued a different message
         * during the TX above; it has its own fresh counter. */
        if (strcmp(tx_text, tx_msg.msg) == 0) {
            if (tx_msg.repeats > 0) {
                tx_msg.repeats--;
            }
            if (tx_msg.repeats == 0) {
                if (strncmp(tx_msg.msg, "CQ", 2) == 0) {
                    /* CQ exhausted with no takers. Setting the subject
                     * from the worker is fine here: the button label
                     * observer runs delayed on the LVGL thread. */
                    subject_set_int(cq_enabled, CQ_OFF);
                }
                tx_msg.msg[0] = '\0';
                tx_msg.force_free_text = false;
            }
        }
        return;
    }

    /* Engine responses are one-shot: once the start window of their target
     * slot has passed, drop the message instead of deferring it. The engine
     * re-decides at the next slot end (sticky retry regenerates it). */
    if (tx_msg_oneshot && have_tx_msg && (tx_time_slot == info->odd) &&
        (sec_since_slot_start >= tx_max_delay)) {
        tx_msg.msg[0] = '\0';
        tx_msg.force_free_text = false;
        tx_msg_oneshot = false;
    }

    if (new_slot) {
        state = RX_PROCESS;
        if (!tx_slot_pending) {
            add_slot_info(CELL_RX_INFO, "RX");
        }
    }
}

/* ---- Free MSG helpers (save/load/sanitize) --------------------------- */

static void ft8_freetext_sanitize(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t j = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        char c = in[i];
        if (c == '\n' || c == '\r') {
            break;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (strchr(FT8_FREETEXT_ACCEPTED_CHARS, c) == NULL) {
            continue;
        }
        if (j == 0 && c == ' ') {
            continue;
        }
        if (j + 1 >= out_size) {
            break;
        }
        if (j >= FT8_FREETEXT_MAX_LEN) {
            break;
        }
        out[j++] = c;
    }
    while (j > 0 && out[j - 1] == ' ') {
        j--;
    }
    out[j] = '\0';
}

static void ft8_freetext_load(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    FILE *fp = fopen(FT8_FREETEXT_FILE, "r");
    if (!fp) return;

    char buf[128];
    buf[0] = '\0';
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        buf[0] = '\0';
    }
    fclose(fp);

    ft8_freetext_sanitize(buf, out, out_size);
}

static bool ft8_freetext_save(const char *text) {
    FILE *fp = fopen(FT8_FREETEXT_FILE, "w");
    if (!fp) return false;
    if (text && text[0] != '\0') {
        fputs(text, fp);
    }
    fputc('\n', fp);
    fclose(fp);
    return true;
}

/* ---- Free MSG button / dialog ---------------------------------------- */

static void free_msg_cb(struct button_data_t *btn_data) {
    (void)btn_data;
    free_msg_open();
}

static void free_msg_open(void) {
    if (!table) {
        return;
    }

    lv_group_remove_obj(table);
    textarea_window_open_w_label(free_msg_ok_cb, free_msg_cancel_cb, "Free MSG");
    lv_obj_t *text = textarea_window_text();

    lv_textarea_set_one_line(text, true);
    lv_textarea_set_max_length(text, FT8_FREETEXT_MAX_LEN);
    lv_textarea_set_accepted_chars(text, FT8_FREETEXT_ACCEPTED_CHARS);

    char def[FT8_FREETEXT_MAX_LEN + 1];
    ft8_freetext_load(def, sizeof(def));
    if (def[0] != '\0') {
        textarea_window_set(def);
    } else {
        lv_textarea_set_placeholder_text(text, " FREE TEXT");
    }
    disable_buttons = true;
}

static void free_msg_close(void) {
    textarea_window_close();
    if (table) {
        lv_group_add_obj(keyboard_group, table);
        lv_group_set_editing(keyboard_group, true);
    }
    disable_buttons = false;
}

static bool free_msg_cancel_cb(void) {
    free_msg_close();
    return true;
}

static bool free_msg_ok_cb(void) {
    const char *raw = textarea_window_get();
    char clean[FT8_FREETEXT_MAX_LEN + 1];
    ft8_freetext_sanitize(raw, clean, sizeof(clean));
    if (clean[0] == '\0') {
        msg_schedule_text_fmt("Empty Free MSG");
        ft8_remote_set_status("Empty Free MSG");
        return false;
    }

    {
        ftx_message_t    tmp_msg;
        ftx_message_rc_t rc = ftx_message_encode_free(&tmp_msg, clean);
        if (rc != FTX_MESSAGE_RC_OK) {
            msg_schedule_text_fmt("Free MSG too long for FT8");
            ft8_remote_set_status("Free MSG too long for FT8");
            return false;
        }
    }

    free_msg_close();
    ft8_apply_free_msg(clean);
    return true;
}

void ft8_set_cq_modifier(const char *cq_mod) {
    if (!cq_mod) {
        cq_mod = "";
    }
    if ((strlen(cq_mod) > 0) && !is_cq_modifier(cq_mod)) {
        msg_schedule_text_fmt("Unsupported CQ modifier");
        ft8_remote_set_status("Unsupported CQ modifier");
        return;
    }
    params_str_set(&params.ft8_cq_modifier, cq_mod);
}

void ft8_apply_free_msg(const char *text) {
    if (disable_buttons) return;

    char clean[FT8_FREETEXT_MAX_LEN + 1];
    ft8_freetext_sanitize(text ? text : "", clean, sizeof(clean));
    if (clean[0] == '\0') {
        msg_schedule_text_fmt("Empty Free MSG");
        ft8_remote_set_status("Empty Free MSG");
        return;
    }

    if (!ft8_freetext_save(clean)) {
        msg_schedule_text_fmt("Save Free MSG failed");
        ft8_remote_set_status("Save Free MSG failed");
    }

    {
        ftx_message_t    tmp_msg;
        ftx_message_rc_t rc = ftx_message_encode_free(&tmp_msg, clean);
        if (rc != FTX_MESSAGE_RC_OK) {
            msg_schedule_text_fmt("Free MSG too long for FT8");
            ft8_remote_set_status("Free MSG too long for FT8");
            return;
        }
    }

    subject_set_int(cq_enabled, CQ_OFF);

    strncpy(tx_msg.msg, clean, sizeof(tx_msg.msg) - 1);
    tx_msg.msg[sizeof(tx_msg.msg) - 1] = '\0';
    tx_msg.repeats = 1;
    tx_msg.force_free_text = true;
    tx_msg_oneshot = false;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    float time_since_slot_start = 0.0f;
    tx_time_slot = !get_time_slot(now, &time_since_slot_start);
    float max_delay = (subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8)
                      ? MAX_TX_START_DELAY : MAX_TX_START_DELAY_FT4;
    if (time_since_slot_start < max_delay) {
        tx_time_slot = !tx_time_slot;
    }

    subject_set_int(tx_enabled, true);
    msg_schedule_text_fmt("Next TX: %s", tx_msg.msg);
    {
        char status[64];
        snprintf(status, sizeof(status), "Next TX: %s", tx_msg.msg);
        ft8_remote_set_status(status);
    }
    ft8_remote_note_free_msg(clean);
}

void ft8_remote_click(uint32_t row_id) {
    if (disable_buttons) return;
    const cell_data_t *cell = table_view_find_by_id(row_id);
    if (!cell) {
        ft8_remote_set_status("Row not found");
        return;
    }
    on_table_press(cell);
}

void ft8_set_tx_delta(int hz) {
    if (disable_buttons) return;
    if (!ft8_remote_active()) return;
    set_freq((uint32_t)hz);
}

void ft8_band(int dir) {
    if (disable_buttons) return;
    if (!dialog.obj) return;
    if (dir > 0) {
        lv_event_send(dialog.obj, EVENT_BAND_UP, NULL);
    } else if (dir < 0) {
        lv_event_send(dialog.obj, EVENT_BAND_DOWN, NULL);
    }
}

bool ft8_consume_ptt(keypad_state_t state) {
    if (!dialog_ft8 || !dialog_ft8->run) return false;
    /* FT8 dialog open: swallow PTT entirely (no radio_set_ptt). */
    if (ui_confirm_pending && (state == KEYPAD_PRESS)) {
        confirm_accept();
    }
    return true;
}

bool ft8_confirm_consume_rotary(void) {
    if (!dialog_ft8 || !dialog_ft8->run || !ui_confirm_pending) return false;
    confirm_accept();
    return true;
}

void ft8_get_filter_range(int *low_hz, int *high_hz) {
    if (low_hz)  *low_hz  = filter_low;
    if (high_hz) *high_hz = filter_high;
}

bool ft8_is_our_tx_slot(const slot_info_t *info) {
    if (!info) return false;
    return (tx_msg.msg[0] != '\0') && subject_get_int(tx_enabled) &&
           (tx_time_slot == info->odd);
}

bool ft8_remote_active(void) {
    return dialog_ft8 && dialog_ft8->run;
}

int ft8_remote_protocol(void) {
    return subject_get_int(cfg.ft8_protocol.val);
}

int ft8_remote_cq_state(void) {
    if (!cq_enabled) return CQ_OFF;
    return subject_get_int(cq_enabled);
}

int ft8_remote_auto_level(void) {
    if (!auto_level) return FTX_QSO_AUTO_OFF;
    return subject_get_int(auto_level);
}

int ft8_remote_auto_mode(void) {
    return subject_get_int(cfg.ft8_auto_mode.val);
}

int ft8_remote_processor(void) {
    if (!qso_proc) return FTX_QSO_PROC_NORMAL;
    return subject_get_int(qso_proc);
}

bool ft8_remote_show_all(void) {
    return subject_get_int(cfg.ft8_show_all.val) != 0;
}

bool ft8_remote_hold_freq(void) {
    return subject_get_int(cfg.ft8_hold_freq.val) != 0;
}

bool ft8_remote_tx_call(void) {
    if (!tx_enabled) return false;
    return subject_get_int(tx_enabled) != 0;
}

bool ft8_remote_auto_dnf(void) {
    return subject_get_int(cfg.ft8_auto_dnf.val) != 0;
}

bool ft8_remote_tx_active(void) {
    return state == TX_PROCESS;
}

int ft8_remote_tx_delta(void) {
    return params.ft8_tx_freq.x;
}

void ft8_remote_filter_range(int *lo, int *hi) {
    ft8_get_filter_range(lo, hi);
}

const char *ft8_remote_band_label(void) {
    return cfg_digital_label_get();
}

const char *ft8_remote_cq_modifier(void) {
    return params.ft8_cq_modifier.x;
}

const char *ft8_remote_next_tx(void) {
    return tx_msg.msg;
}

const char *ft8_remote_de_call(void) {
    return params.callsign.x;
}

const char *ft8_remote_de_grid(void) {
    return params.qth.x;
}
