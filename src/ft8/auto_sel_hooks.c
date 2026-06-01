/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 auto_sel hook adapters (C, not C++)
 *
 *  Copyright (c) 2026
 */

#include "ft8_hooks.h"
#include "auto_sel.h"

#include "../cfg/subjects.h"
#include "../cfg/cfg.h"
#include "../params/params.h"
#include "../qth/qth.h"
#include "../msg.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ===================================================================
 *  AutoSel state machine (moved from dialog_ft8.c per §8.3)
 * =================================================================== */

static auto_sel_mode_t  s_mode                 = AUTO_SEL_OFF;
static char             s_qso_active_call[16]  = {0};
static bool             s_cq_paused_for_qso    = false;
static bool             s_recover_mode         = false;
static bool             s_qso_active           = false;
static uint8_t          s_grid_tx_count        = 0;
static bool             s_pending_to_me_valid   = false;
static bool             s_pending_to_me_odd     = false;
static ftx_msg_meta_t   s_pending_to_me_meta;
static bool             s_pending_grid_valid    = false;
static bool             s_pending_grid_odd      = false;
static ftx_msg_meta_t   s_pending_grid_meta;
static bool             s_local_qth_valid       = false;
static bool             s_hooks_registered      = false;

/* ---- Helpers -------------------------------------------------------- */

static bool is_grid_exchange_msg(const char *msg) {
    if (!msg || msg[0] == '\0') return false;
    char t1[16] = {0}, t2[16] = {0}, t3[16] = {0};
    if (sscanf(msg, "%15s %15s %15s", t1, t2, t3) != 3) return false;
    return qth_grid_check(t3);
}

static const char *mode_text(auto_sel_mode_t m) {
    switch (m) {
        case AUTO_SEL_OFF:         return "Off";
        case AUTO_SEL_FIRST:       return "First";
        case AUTO_SEL_FARTHEST:    return "Farthest";
        case AUTO_SEL_HIGHEST_SNR: return "Best SNR";
        case AUTO_SEL_NEW_GRID:    return "New Grid";
        default:                   return "?";
    }
}

static void reset_qso_state(void) {
    s_recover_mode        = false;
    s_pending_to_me_valid = false;
    s_pending_grid_valid  = false;
    s_qso_active          = false;
    s_grid_tx_count       = 0;
    s_qso_active_call[0]  = '\0';
}

static void schedule_cq_if_idle(void) {
    if (s_cq_paused_for_qso) return;
    ftx_tx_msg_t *txm = ft8_get_tx_msg();
    if (!txm || txm->msg[0] != '\0') return;
    FTxQsoProcessor *qso = ft8_get_qso_processor();
    if (qso && ftx_qso_processor_has_current(qso)) return;
    if (!ft8_is_tx_enabled()) return;
    ft8_schedule_cq_tx();
    s_cq_paused_for_qso = false;
}

static int compute_dist(const ftx_msg_meta_t *meta) {
    if (!meta || meta->grid[0] == '\0' || !s_local_qth_valid) return 0;
    double qth_lat = 0.0, qth_lon = 0.0;
    ft8_get_qth(&qth_lat, &qth_lon);
    double grid_lat = 0.0, grid_lon = 0.0;
    qth_str_to_pos(meta->grid, &grid_lat, &grid_lon);
    return (int)qth_pos_dist(grid_lat, grid_lon, qth_lat, qth_lon);
}

/* ===================================================================
 *  Hook adapters
 * =================================================================== */

static void autosel_rx_hook(const char *text, int snr,
                            float freq_hz, float time_sec,
                            ftx_msg_meta_t *meta,
                            const slot_info_t *info) {
    (void)text;
    (void)freq_hz;
    (void)time_sec;
    (void)info;

    if (s_mode == AUTO_SEL_OFF) return;
    if (!meta) return;

    if (meta->type == FTX_MSG_TYPE_CQ) {
        int dist = compute_dist(meta);
        autosel_add_candidate(meta, dist, snr);
    } else if (s_mode == AUTO_SEL_NEW_GRID && meta->type == FTX_MSG_TYPE_GRID) {
        int dist = compute_dist(meta);
        autosel_add_candidate(meta, dist, snr);
    }
}

static void autosel_slot_end_hook(const slot_info_t *info) {
    (void)info;

    if (s_mode == AUTO_SEL_OFF) return;

    FTxQsoProcessor *qso = ft8_get_qso_processor();
    ftx_tx_msg_t    *txm = ft8_get_tx_msg();
    if (!qso || !txm) return;

    if (!ftx_qso_processor_has_current(qso) && s_qso_active) {
        reset_qso_state();
        schedule_cq_if_idle();
        autosel_clear_unseen();
        return;
    }

    if (ftx_qso_processor_has_current(qso)) {
        autosel_clear_unseen();
        return;
    }

    if (s_pending_to_me_valid && (s_pending_to_me_odd != info->odd)) {
        s_pending_to_me_valid = false;
    }

    autosel_candidate_t cand;
    if (!autosel_pick(s_mode, &cand)) {
        autosel_clear_unseen();
        if (s_cq_paused_for_qso) {
            s_cq_paused_for_qso = false;
            schedule_cq_if_idle();
        }
        return;
    }

    if (cand.meta.call_de[0] == '\0') {
        autosel_clear_unseen();
        return;
    }

    strncpy(txm->msg, cand.meta.call_de, sizeof(txm->msg) - 1);
    txm->msg[sizeof(txm->msg) - 1] = '\0';
    txm->repeats = 1;

    bool *tx_slot = ft8_get_tx_time_slot();
    *tx_slot = !info->odd;

    strncpy(s_qso_active_call, cand.meta.call_de, sizeof(s_qso_active_call) - 1);
    s_qso_active_call[sizeof(s_qso_active_call) - 1] = '\0';
    s_qso_active       = true;
    s_grid_tx_count    = 0;
    s_recover_mode     = true;
    s_pending_grid_valid = false;

    autosel_clear_unseen();
    autosel_blacklist_mark_seen(cand.meta.call_de);
}

static void autosel_init_state(void) {
    s_mode                 = AUTO_SEL_OFF;
    s_local_qth_valid      = false;
    s_cq_paused_for_qso    = false;
    reset_qso_state();

    autosel_init();
    autosel_user_blacklist_load();

    if (params.qth.x[0] != '\0' && qth_grid_check(params.qth.x)) {
        s_local_qth_valid = true;
    }
}

static void autosel_cleanup_state(void) {
    autosel_deinit();
    reset_qso_state();
    s_cq_paused_for_qso = false;
    s_mode = AUTO_SEL_OFF;
}

/* ---- Explicit call points (called from dialog_ft8.c on_tick_cb) ----- */

bool autosel_grid_swap_on_tick(void) {
    if (!s_pending_grid_valid) return false;
    if (!s_qso_active) return false;

    s_pending_grid_valid = false;

    FTxQsoProcessor *qso = ft8_get_qso_processor();
    if (!qso || ftx_qso_processor_has_current(qso)) return false;

    ftx_tx_msg_t *txm = ft8_get_tx_msg();
    if (!txm) return false;

    strncpy(txm->msg, s_pending_grid_meta.call_de, sizeof(txm->msg) - 1);
    txm->msg[sizeof(txm->msg) - 1] = '\0';
    txm->repeats = 1;

    strncpy(s_qso_active_call, s_pending_grid_meta.call_de,
            sizeof(s_qso_active_call) - 1);
    s_grid_tx_count    = 0;
    s_qso_active       = true;

    return true;
}

void autosel_post_tx(void) {
    ftx_tx_msg_t *txm = ft8_get_tx_msg();
    if (!txm) return;

    FTxQsoProcessor *qso = ft8_get_qso_processor();
    if (!qso) return;

    if (s_qso_active && is_grid_exchange_msg(txm->msg)) {
        s_grid_tx_count++;
    }

    if (!ftx_qso_processor_has_current(qso) && txm->msg[0] == '\0') {
        reset_qso_state();
    }
}

/* ---- Getter for dialog label / mode toggle --------------------------- */

int autosel_get_mode(void) {
    return (int)s_mode;
}

const char *autosel_get_mode_text(void) {
    return mode_text(s_mode);
}

void autosel_cycle_mode(void) {
    s_mode = (auto_sel_mode_t)((s_mode + 1) % 5);
    if (s_mode == AUTO_SEL_OFF) {
        autosel_clear_unseen();
        autosel_blacklist_clear_all();
        reset_qso_state();
    }
}

/* ---- Registration --------------------------------------------------- */

void autosel_register_hooks(void) {
    if (s_hooks_registered) return;
    s_hooks_registered = true;

    autosel_init_state();
    ft8_register_init_hook(autosel_init_state);
    ft8_register_cleanup_hook(autosel_cleanup_state);
    ft8_register_rx_msg_hook(autosel_rx_hook);
    ft8_register_slot_end_hook(autosel_slot_end_hook);
}
