/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 auto_sel hook adapters (C)
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
#include "../lv_finder.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ===================================================================
 *  AutoSel state machine (moved from dialog_ft8.c per §8.3)
 * =================================================================== */

/* --- State variables (all static to this module) --------------------- */

static auto_sel_mode_t  s_mode                 = AUTO_SEL_OFF;
static char             s_qso_active_call[16]  = {0};
static bool             s_cq_paused_for_qso    = false;
static bool             s_recover_mode         = false;
static bool             s_qso_active           = false;
static uint8_t          s_grid_tx_count        = 0;
static bool             s_qso_tx_exhausted     = false;
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
    s_qso_tx_exhausted    = false;
}

/* Start a QSO with the given meta: call the QSO processor, set finder,
 * frequency, slot, and pause CQ.  Used by pick / to-me swap / grid swap. */
static void start_qso_from_meta(const ftx_msg_meta_t *meta,
                                bool target_odd_slot) {
    FTxQsoProcessor *qso = ft8_get_qso_processor();
    ftx_tx_msg_t    *txm = ft8_get_tx_msg();
    if (!qso || !txm || !meta) return;

    ftx_qso_processor_start_qso(qso, meta, txm);
    if (txm->msg[0] == '\0') return;

    bool *tx_slot = ft8_get_tx_time_slot();
    *tx_slot = target_odd_slot;

    lv_finder_set_cursor(ft8_get_finder(), meta->freq_hz);
    if (!subject_get_int(cfg.ft8_hold_freq.val)) {
        ft8_set_dial_freq(meta->freq_hz);
    }
    strncpy(s_qso_active_call, meta->call_de, sizeof(s_qso_active_call) - 1);
    s_qso_active_call[sizeof(s_qso_active_call) - 1] = '\0';

    msg_schedule_text_fmt("Next TX: %s", txm->msg);
}

/* Pause CQ for an active QSO. */
static void pause_cq_for_qso(void) {
    if (subject_get_int(cq_enabled)) {
        s_cq_paused_for_qso = true;
        subject_set_int(cq_enabled, false);
    }
}

/* Resume CQ after a QSO completes. */
static void resume_cq_if_qso_gone(void) {
    FTxQsoProcessor *qso = ft8_get_qso_processor();
    ftx_tx_msg_t    *txm = ft8_get_tx_msg();
    bool qso_gone = (!qso || !ftx_qso_processor_has_current(qso));

    if (s_cq_paused_for_qso && qso_gone && (!txm || txm->msg[0] == '\0')) {
        subject_set_int(cq_enabled, true);
        s_cq_paused_for_qso = false;
        ft8_schedule_cq_tx();
    }
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

/* --- rx_msg hook ------------------------------------------------------
 * Collect CQ callers + grid responses into AutoSel scratch.
 * Also maintain blacklist seen-marking, to-me stash, grid stash. */

static void autosel_rx_hook(const char *text, int snr,
                            float freq_hz, float time_sec,
                            ftx_msg_meta_t *meta,
                            const slot_info_t *info) {
    (void)text;
    (void)freq_hz;
    (void)time_sec;

    if (!meta) return;

    /* --- to-me stash (priority 2: exhaustion swap) --- */
    if (s_qso_tx_exhausted && meta->to_me && meta->call_de[0] != '\0') {
        if ((s_qso_active_call[0] == '\0') ||
            (strcmp(s_qso_active_call, meta->call_de) != 0)) {
            if (!s_pending_to_me_valid) {
                s_pending_to_me_meta  = *meta;
                s_pending_to_me_odd   = info->odd;
                s_pending_to_me_valid = true;
            }
        }
    }

    /* --- grid-stage stash (priority 3: grid swap) --- */
    FTxQsoProcessor *qso = ft8_get_qso_processor();
    ftx_tx_msg_t    *txm = ft8_get_tx_msg();
    if (s_qso_active && txm && is_grid_exchange_msg(txm->msg) &&
        meta->to_me && (meta->type == FTX_MSG_TYPE_GRID) && meta->call_de[0] != '\0') {
        if ((s_qso_active_call[0] == '\0') ||
            (strcmp(s_qso_active_call, meta->call_de) != 0)) {
            if (!s_pending_grid_valid) {
                s_pending_grid_meta  = *meta;
                s_pending_grid_odd   = info->odd;
                s_pending_grid_valid = true;
            }
        }
    }

    /* --- CQ candidate collection --- */
    if (meta->type == FTX_MSG_TYPE_CQ && !meta->to_me) {
        autosel_blacklist_mark_seen(meta->call_de);
        bool no_current  = !qso || !ftx_qso_processor_has_current(qso);
        bool may_collect = (s_mode != AUTO_SEL_OFF) &&
                           (no_current || s_recover_mode);
        if (may_collect) {
            int dist = compute_dist(meta);
            autosel_add_candidate(meta, dist, snr);
        }
    }
}

/* --- slot_end hook -----------------------------------------------------
 * Cycle blacklist advance, stale-QSO cleanup, to-me priority swap,
 * AutoSel pick + start QSO, CQ restore. */

static void autosel_slot_end_hook(const slot_info_t *info) {
    FTxQsoProcessor *qso = ft8_get_qso_processor();
    ftx_tx_msg_t    *txm = ft8_get_tx_msg();

    /* Cycle blacklist must advance BEFORE start_new_slot. */
    autosel_blacklist_advance_cycle();

    if (qso) {
        ftx_qso_processor_start_new_slot(qso);
    }

    /* No active QSO: drop all per-QSO bookkeeping. */
    if (!qso || !ftx_qso_processor_has_current(qso)) {
        reset_qso_state();
    }

    /* CQ was paused for a QSO that's now gone + nothing queued: resume. */
    resume_cq_if_qso_gone();

    /* Priority (2): exhaustion → to-me caller arrived. Reset + start QSO. */
    if (s_qso_tx_exhausted && (!txm || txm->msg[0] == '\0') && s_pending_to_me_valid) {
        if (qso && ftx_qso_processor_has_current(qso)) {
            ftx_qso_processor_reset(qso);
        }
        start_qso_from_meta(&s_pending_to_me_meta, !s_pending_to_me_odd);
        pause_cq_for_qso();
        s_pending_to_me_valid  = false;
        s_qso_tx_exhausted     = false;
        s_recover_mode         = false;
        s_qso_active           = false;
        s_grid_tx_count        = 0;
        s_pending_grid_valid   = false;
    }

    /* AutoSel pick: no QSO (or stuck in recover mode), no pending TX,
     * mode enabled -> pick from unseen CQ callers. */
    if ((!txm || txm->msg[0] == '\0') && (s_mode != AUTO_SEL_OFF) &&
        (!qso || !ftx_qso_processor_has_current(qso) || s_recover_mode)) {
        autosel_candidate_t pick;
        if (autosel_pick(s_mode, &pick)) {
            if (s_recover_mode && qso && ftx_qso_processor_has_current(qso)) {
                ftx_qso_processor_reset(qso);
            }
            start_qso_from_meta(&pick.meta, !info->odd);
            s_qso_active        = true;
            s_grid_tx_count     = 0;
            s_pending_grid_valid = false;
            pause_cq_for_qso();
            s_recover_mode      = false;
            s_qso_tx_exhausted  = false;
            s_pending_to_me_valid = false;
        }
    }

    /* Consume the per-slot scratch. */
    autosel_clear_unseen();
}

/* ===================================================================
 *  Explicit call points (called from dialog_ft8.c on_tick_cb)
 * =================================================================== */

/* --- grid_swap_on_tick ------------------------------------------------ */

bool autosel_grid_swap_on_tick(void) {
    FTxQsoProcessor *qso = ft8_get_qso_processor();
    ftx_tx_msg_t    *txm = ft8_get_tx_msg();
    if (!qso || !txm) return false;
    if (!is_grid_exchange_msg(txm->msg)) return false;
    if (!s_qso_active) return false;
    if (s_grid_tx_count < 2) return false;
    if (!s_pending_grid_valid) return false;

    /* Reset current QSO, start new one with grid caller. */
    if (ftx_qso_processor_has_current(qso)) {
        ftx_qso_processor_reset(qso);
    }
    start_qso_from_meta(&s_pending_grid_meta, !s_pending_grid_odd);

    s_pending_grid_valid  = false;
    s_qso_active          = false;
    s_grid_tx_count       = 0;

    /* If the swap re-targeted the opposite slot, signal dialog to defer. */
    bool *tx_slot = ft8_get_tx_time_slot();
    const slot_info_t *info = NULL; /* not available here; dialog checks */
    (void)info;
    return (*tx_slot);
}

/* --- post_tx ----------------------------------------------------------
 * Called after TX completes. Tracks grid count, detects exhaustion,
 * blacklists stuck stations, arms recover mode. */

void autosel_post_tx(void) {
    FTxQsoProcessor *qso = ft8_get_qso_processor();
    ftx_tx_msg_t    *txm = ft8_get_tx_msg();
    if (!qso || !txm) return;

    bool sent_grid = is_grid_exchange_msg(txm->msg);

    /* Count grid TXs for grid-stage priority logic (saturate at 255). */
    if (sent_grid && s_qso_active && (s_grid_tx_count < 255)) {
        s_grid_tx_count++;
    }

    /* repeats exhausted at end of TX burst */
    bool repeats_exhausted = (txm->repeats == 0);

    if (repeats_exhausted) {
        bool was_cq = (strncmp(txm->msg, "CQ", 2) == 0);

        if (!was_cq && ftx_qso_processor_has_current(qso)) {
            /* Exhausted mid-QSO: arm priority paths so another to-me
             * caller or AutoSel can take over. */
            s_qso_tx_exhausted = true;

            /* Only blacklist when the failure happened sending our
             * initial grid (reply to CQ). */
            if (sent_grid && s_qso_active_call[0] != '\0') {
                autosel_blacklist_add(s_qso_active_call);
            }
            if ((s_mode != AUTO_SEL_OFF) && !s_cq_paused_for_qso) {
                s_recover_mode = true;
            }
        }
    }

    /* Resume CQ if QSO is gone after exhaustion. */
    if (!ftx_qso_processor_has_current(qso) && txm->msg[0] == '\0') {
        resume_cq_if_qso_gone();
    }
}

/* ===================================================================
 *  Init / cleanup / registration
 * =================================================================== */

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

/* ---- Registration (idempotent) ------------------------------------- */

void autosel_register_hooks(void) {
    if (s_hooks_registered) return;
    s_hooks_registered = true;

    ft8_register_init_hook(autosel_init_state);
    ft8_register_cleanup_hook(autosel_cleanup_state);
    ft8_register_rx_msg_hook(autosel_rx_hook);
    ft8_register_slot_end_hook(autosel_slot_end_hook);
}