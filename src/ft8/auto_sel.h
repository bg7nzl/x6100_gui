/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 auto-select (unseen-CQ picker)
 *
 *  Copyright (c) 2026
 */

/*
 *  Tracks CQ callers observed during the current slot window, filters them
 *  by the system cycle blacklist and the user's persistent blacklist, and
 *  picks one for auto-QSO according to auto_sel_mode_t.
 *
 *  Backed by std::deque<UnseenEntry> (per-slot scratch), std::array<> for
 *  the short cycle blacklist, and std::vector<std::string> for the user
 *  blacklist. All accesses are mutex-protected so worker-thread
 *  autosel_add_candidate() and UI-thread autosel_pick() are safe.
 *
 *  C-compatible API so C callers (qso_state, dialog) can use it directly.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "qso.h"   /* ftx_msg_meta_t */

/* Opaque forward-declare slot_info_t (full definition in audio_worker.h).
 * Using pointer avoids pulling in C99 float complex from audio_worker.h. */
typedef struct slot_info_t slot_info_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUTO_SEL_OFF         = 0,
    AUTO_SEL_FIRST       = 1,
    AUTO_SEL_FARTHEST    = 2,
    AUTO_SEL_HIGHEST_SNR = 3,
    AUTO_SEL_NEW_GRID    = 4,
} auto_sel_mode_t;

typedef struct {
    ftx_msg_meta_t meta;
    int            dist;
    int            snr;
} autosel_candidate_t;

void autosel_init(void);
void autosel_deinit(void);

/* Load / persist /mnt/autosel_blacklist.txt. */
void autosel_user_blacklist_load(void);

/* Worker thread: register a CQ caller observed in this slot. */
void autosel_add_candidate(const ftx_msg_meta_t *meta, int dist, int snr);

/* UI thread: pick a candidate per mode. Returns false if nothing suitable. */
bool autosel_pick(auto_sel_mode_t mode, autosel_candidate_t *out);

/* Drop all unseen candidates (end-of-slot hygiene). */
void autosel_clear_unseen(void);

/* System cycle blacklist. Calls normalise callsigns internally. */
void autosel_blacklist_add(const char *call);
void autosel_blacklist_mark_seen(const char *call);
void autosel_blacklist_advance_cycle(void);
void autosel_blacklist_clear_all(void);

/* Blacklist checks. user_blacklisted() reads only the persistent list; 
 * blacklisted() reads either list. */
bool autosel_is_user_blacklisted(const char *call);
bool autosel_is_blacklisted(const char *call);

/* ---- Hook registration and dialog call points (PR-AUTOSEL) ----------- */

/** Register all autosel hook adapters with the dialog's hook chain.
 *  Also initialises the module state. Idempotent. */
void autosel_register_hooks(void);

/** Cycle through AUTO_SEL_OFF → FIRST → FARTHEST → HIGHEST_SNR → NEW_GRID.
 *  OFF clears unseen candidates and the cycle blacklist. */
void autosel_cycle_mode(void);

/** Return current mode as int (for label rendering). */
int  autosel_get_mode(void);

/** Return human-readable mode name. */
const char *autosel_get_mode_text(void);

/** Pre-TX grid swap: if a pending grid caller is stashed and we are
 *  mid-QSO, swap tx_msg to work the grid caller. Returns true if
 *  the tick should be deferred (slot mismatch after swap).
 *  info is the current slot_info from on_tick_cb. */
bool autosel_grid_swap_on_tick(const slot_info_t *info);

/** Post-TX housekeeping: track grid TX count, detect QSO exhaustion.
 *  Call from on_tick_cb after TX completes. */
void autosel_post_tx(void);

/** Called from add_rx_text when the QSO processor updates tx_msg.
 *  Tracks qso_active_call, cq_paused flag, clears exhaustion state. */
void autosel_on_tx_msg_updated(const ftx_msg_meta_t *meta, bool odd_slot);

#ifdef __cplusplus
}
#endif
