/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 auto-select (unseen-CQ picker)
 *
 *  Copyright (c) 2026
 */

#include "auto_sel.h"

/* subjects.h and cfg.h already gate their C++/C contents internally; do
 * NOT wrap them in extern "C" or their own STL includes get C linkage. */
#include "../cfg/subjects.h"
#include "../cfg/cfg.h"

#include <ft8lib/constants.h>

extern "C" {
#include "../qso_log.h"
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t   MAX_UNSEEN                   = 100;
constexpr size_t   CYCLE_BL_SIZE                = 8;
constexpr uint8_t  CYCLE_BL_CLEAR_MISS          = 2;
constexpr size_t   USER_BL_CALL_MAX_LEN         = 15;   /* matches 16-1 in old code */
constexpr const char *USER_BL_PATH              = "/mnt/autosel_blacklist.txt";

struct UnseenEntry {
    ftx_msg_meta_t meta;
    int            dist;
    int            snr;
};

struct CycleEntry {
    char    call[16];
    uint8_t miss_cycles;
    bool    seen_this_cycle;
};

std::mutex                                  g_mutex;
std::deque<UnseenEntry>                     g_unseen;
std::array<CycleEntry, CYCLE_BL_SIZE>       g_cycle_bl{};
std::vector<std::string>                    g_user_bl;

/* ---- helpers (hold g_mutex while calling these) ----------------------- */

void trim_in_place(std::string &s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

void normalize_call(const char *in, std::string &out) {
    out.clear();
    if (!in) return;
    out.assign(in);

    /* Strip UTF-8 BOM if present. */
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    trim_in_place(out);

    for (auto &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (out.size() > USER_BL_CALL_MAX_LEN) out.resize(USER_BL_CALL_MAX_LEN);
}

bool user_blacklisted_norm(const std::string &norm) {
    if (norm.empty()) return false;
    for (const auto &s : g_user_bl) {
        if (s == norm) return true;
    }
    return false;
}

bool cycle_blacklisted_norm(const std::string &norm) {
    if (norm.empty()) return false;
    for (const auto &e : g_cycle_bl) {
        if (e.call[0] != '\0' && (norm == e.call)) return true;
    }
    return false;
}

bool any_blacklisted_norm(const std::string &norm) {
    return user_blacklisted_norm(norm) || cycle_blacklisted_norm(norm);
}

void save_user_bl_locked(bool use_crlf) {
    const char *eol      = use_crlf ? "\r\n" : "\n";
    std::string tmp_path = std::string(USER_BL_PATH) + ".tmp";
    FILE *fp = std::fopen(tmp_path.c_str(), "wb");
    if (!fp) return;
    for (const auto &s : g_user_bl) {
        if (s.empty()) continue;
        std::fputs(s.c_str(), fp);
        std::fputs(eol, fp);
    }
    std::fflush(fp);
    std::fclose(fp);
    if (std::rename(tmp_path.c_str(), USER_BL_PATH) != 0) {
        std::remove(tmp_path.c_str());
    }
}

} /* namespace */

/* -------------------- public C API ------------------------------------- */

extern "C" void autosel_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_unseen.clear();
    for (auto &e : g_cycle_bl) {
        e.call[0]          = '\0';
        e.miss_cycles      = 0;
        e.seen_this_cycle  = false;
    }
    g_user_bl.clear();
}

extern "C" void autosel_deinit(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_unseen.clear();
    for (auto &e : g_cycle_bl) {
        e.call[0]          = '\0';
        e.miss_cycles      = 0;
        e.seen_this_cycle  = false;
    }
    g_user_bl.clear();
}

extern "C" void autosel_user_blacklist_load(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_user_bl.clear();

    bool dirty    = false;
    bool use_crlf = false;

    FILE *fp = std::fopen(USER_BL_PATH, "rb");
    if (!fp) {
        if (errno == ENOENT) {
            FILE *nfp = std::fopen(USER_BL_PATH, "wb");
            if (nfp) std::fclose(nfp);
        }
        return;
    }

    char line[128];
    while (std::fgets(line, sizeof(line), fp)) {
        if (std::strchr(line, '\r')) use_crlf = true;

        std::string orig(line);
        trim_in_place(orig);

        std::string call;
        normalize_call(line, call);
        if (call.empty())            continue;
        if (call[0] == '#' || call[0] == ';') continue;

        if (!(orig.empty() || orig[0] == '#' || orig[0] == ';')) {
            if (orig != call) dirty = true;
        }

        bool exists = false;
        for (const auto &s : g_user_bl) {
            if (s == call) { exists = true; break; }
        }
        if (exists) {
            dirty = true;
            continue;
        }
        g_user_bl.push_back(call);
    }
    std::fclose(fp);

    if (dirty) save_user_bl_locked(use_crlf);
}

extern "C" void autosel_add_candidate(const ftx_msg_meta_t *meta, int dist, int snr) {
    if (!meta) return;

    /* Already worked (per qso_log)? skip. These calls take their own
     * qso_log mutex internally so we run them outside our lock. */
    qso_log_search_worked_t worked = qso_log_search_worked(
        meta->call_de,
        subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8 ? MODE_FT8 : MODE_FT4,
        qso_log_freq_to_band(subject_get_int(cfg_cur.fg_freq)));
    if (worked != SEARCH_WORKED_NO) return;

    std::string norm;
    normalize_call(meta->call_de, norm);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (norm.empty() || any_blacklisted_norm(norm)) return;
    for (const auto &u : g_unseen) {
        if (std::strcmp(u.meta.call_de, meta->call_de) == 0) return;
    }
    if (g_unseen.size() >= MAX_UNSEEN) return;

    UnseenEntry e;
    e.meta = *meta;
    e.dist = dist;
    e.snr  = snr;
    g_unseen.push_back(e);
}

extern "C" void autosel_clear_unseen(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_unseen.clear();
}

extern "C" bool autosel_pick(auto_sel_mode_t mode, autosel_candidate_t *out) {
    if (!out || mode == AUTO_SEL_OFF) return false;

    std::lock_guard<std::mutex> lock(g_mutex);

    /* Purge blacklisted entries first; they may have been added before the
     * blacklist was loaded/updated. */
    g_unseen.erase(
        std::remove_if(g_unseen.begin(), g_unseen.end(), [](const UnseenEntry &u) {
            std::string norm;
            normalize_call(u.meta.call_de, norm);
            return any_blacklisted_norm(norm);
        }),
        g_unseen.end());

    if (g_unseen.empty()) return false;

    auto it_best = g_unseen.end();

    if (mode == AUTO_SEL_FIRST) {
        it_best = g_unseen.begin();
    } else if (mode == AUTO_SEL_NEW_GRID) {
        qso_log_mode_t qlog_mode = subject_get_int(cfg.ft8_protocol.val) == FTX_PROTOCOL_FT8 ? MODE_FT8 : MODE_FT4;
        qso_log_band_t qlog_band = qso_log_freq_to_band(subject_get_int(cfg_cur.fg_freq));
        for (auto it = g_unseen.begin(); it != g_unseen.end(); ++it) {
            if (it->meta.grid[0] == '\0') continue;
            if (!qso_log_search_worked_grid(it->meta.grid, qlog_mode, qlog_band)) {
                it_best = it;
                break;
            }
        }
        if (it_best == g_unseen.end()) it_best = g_unseen.begin();
    } else if (mode == AUTO_SEL_FARTHEST) {
        int best_dist = 0;
        for (auto it = g_unseen.begin(); it != g_unseen.end(); ++it) {
            if (it->dist > best_dist) {
                best_dist = it->dist;
                it_best   = it;
            }
        }
        if (it_best == g_unseen.end()) it_best = g_unseen.begin();
    } else { /* AUTO_SEL_HIGHEST_SNR (and any other non-OFF value) */
        it_best = g_unseen.begin();
        for (auto it = std::next(g_unseen.begin()); it != g_unseen.end(); ++it) {
            if (it->snr > it_best->snr) it_best = it;
        }
    }

    if (it_best == g_unseen.end()) return false;

    out->meta = it_best->meta;
    out->dist = it_best->dist;
    out->snr  = it_best->snr;
    g_unseen.erase(it_best);
    return true;
}

extern "C" void autosel_blacklist_add(const char *call) {
    if (!call || call[0] == '\0') return;
    std::string norm;
    normalize_call(call, norm);
    if (norm.empty()) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    /* Refresh existing */
    for (auto &e : g_cycle_bl) {
        if (e.call[0] != '\0' && norm == e.call) {
            e.miss_cycles     = 0;
            e.seen_this_cycle = true;
            return;
        }
    }
    /* Free slot */
    for (auto &e : g_cycle_bl) {
        if (e.call[0] == '\0') {
            std::snprintf(e.call, sizeof(e.call), "%s", norm.c_str());
            e.miss_cycles     = 0;
            e.seen_this_cycle = true;
            return;
        }
    }
    /* Evict entry with the largest miss count. */
    size_t worst = 0;
    for (size_t i = 1; i < g_cycle_bl.size(); i++) {
        if (g_cycle_bl[i].miss_cycles > g_cycle_bl[worst].miss_cycles) worst = i;
    }
    std::snprintf(g_cycle_bl[worst].call, sizeof(g_cycle_bl[worst].call), "%s", norm.c_str());
    g_cycle_bl[worst].miss_cycles     = 0;
    g_cycle_bl[worst].seen_this_cycle = true;
}

extern "C" void autosel_blacklist_mark_seen(const char *call) {
    if (!call || call[0] == '\0') return;
    std::string norm;
    normalize_call(call, norm);
    if (norm.empty()) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &e : g_cycle_bl) {
        if (e.call[0] != '\0' && norm == e.call) {
            e.seen_this_cycle = true;
            return;
        }
    }
}

extern "C" void autosel_blacklist_advance_cycle(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &e : g_cycle_bl) {
        if (e.call[0] == '\0') continue;
        if (e.seen_this_cycle) {
            e.miss_cycles = 0;
        } else {
            if (e.miss_cycles < 255) e.miss_cycles++;
        }
        e.seen_this_cycle = false;
        if (e.miss_cycles >= CYCLE_BL_CLEAR_MISS) {
            e.call[0]         = '\0';
            e.miss_cycles     = 0;
            e.seen_this_cycle = false;
        }
    }
}

extern "C" void autosel_blacklist_clear_all(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &e : g_cycle_bl) {
        e.call[0]         = '\0';
        e.miss_cycles     = 0;
        e.seen_this_cycle = false;
    }
}

extern "C" bool autosel_is_user_blacklisted(const char *call) {
    if (!call || call[0] == '\0') return false;
    std::string norm;
    normalize_call(call, norm);
    std::lock_guard<std::mutex> lock(g_mutex);
    return user_blacklisted_norm(norm);
}

extern "C" bool autosel_is_blacklisted(const char *call) {
    if (!call || call[0] == '\0') return false;
    std::string norm;
    normalize_call(call, norm);
    std::lock_guard<std::mutex> lock(g_mutex);
    return any_blacklisted_norm(norm);
}
/* ---- Hook adapters and registration (PR-AUTOSEL) --------------------- */

/* audio_worker.h uses C99 float complex which is invalid C++.
 * Avoid including it: forward-declare slot_info_t instead. */
struct slot_info_t;

extern "C" {
#include "../qth/qth.h"
#include "../cfg/subjects.h"
#include "../cfg/cfg.h"
#include "../params/params.h"
#include "../msg.h"

/* Hook registration API (ft8_hooks.h subset, without audio_worker.h) */
typedef void (*ft8_lifecycle_fn_t)(void);
typedef void (*ft8_rx_msg_fn_t)(const char *text, int snr,
                                float freq_hz, float time_sec,
                                struct ftx_msg_meta_t *meta,
                                const struct slot_info_t *info);
typedef void (*ft8_psd_fn_t)(const float *psd, uint16_t nfft,
                             float sec_since_slot_start,
                             const struct slot_info_t *info);
typedef void (*ft8_slot_end_fn_t)(const struct slot_info_t *info);
typedef void (*ft8_pre_tx_fn_t)(const struct slot_info_t *info);

void ft8_register_init_hook(ft8_lifecycle_fn_t fn);
void ft8_register_cleanup_hook(ft8_lifecycle_fn_t fn);
void ft8_register_rx_msg_hook(ft8_rx_msg_fn_t fn);
void ft8_register_slot_end_hook(ft8_slot_end_fn_t fn);
void ft8_register_psd_hook(ft8_psd_fn_t fn);
void ft8_register_pre_tx_hook(ft8_pre_tx_fn_t fn);

/* Dialog getters used by the state machine */
struct FTxQsoProcessor;
bool ftx_qso_processor_has_current(struct FTxQsoProcessor*);
struct ftx_tx_msg_t;
struct ftx_tx_msg_t *ft8_get_tx_msg(void);
bool *ft8_get_tx_time_slot(void);
struct FTxQsoProcessor *ft8_get_qso_processor(void);
bool ft8_is_tx_enabled(void);
void ft8_schedule_cq_tx(void);
void ft8_get_qth(double *lat, double *lon);
}

#include <cstdio>

/* ===================================================================
 *  AutoSel state machine (moved from dialog_ft8.c per §8.3)
 * =================================================================== */

namespace {
    auto_sel_mode_t  s_mode                 = AUTO_SEL_OFF;
    char             s_qso_active_call[16]  = {0};
    bool             s_cq_paused_for_qso    = false;
    bool             s_recover_mode         = false;
    bool             s_qso_active           = false;
    uint8_t          s_grid_tx_count        = 0;
    bool             s_pending_to_me_valid   = false;
    bool             s_pending_to_me_odd     = false;
    ftx_msg_meta_t   s_pending_to_me_meta;
    bool             s_pending_grid_valid    = false;
    bool             s_pending_grid_odd      = false;
    ftx_msg_meta_t   s_pending_grid_meta;
    bool             s_local_qth_valid       = false;
    bool             s_hooks_registered      = false;

    /* ---- Helpers ported from dialog_ft8.c ----------------------------- */

    bool is_grid_exchange_msg(const char *msg) {
        if (!msg || msg[0] == '\0') return false;
        char t1[16] = {0}, t2[16] = {0}, t3[16] = {0};
        if (sscanf(msg, "%15s %15s %15s", t1, t2, t3) != 3) return false;
        return qth_grid_check(t3);
    }

    const char *mode_text(auto_sel_mode_t m) {
        switch (m) {
            case AUTO_SEL_OFF:         return "Off";
            case AUTO_SEL_FIRST:       return "First";
            case AUTO_SEL_FARTHEST:    return "Farthest";
            case AUTO_SEL_HIGHEST_SNR: return "Best SNR";
            case AUTO_SEL_NEW_GRID:    return "New Grid";
            default:                   return "?";
        }
    }

    void reset_qso_state(void) {
        s_recover_mode        = false;
        s_pending_to_me_valid = false;
        s_pending_grid_valid  = false;
        s_qso_active          = false;
        s_grid_tx_count       = 0;
        s_qso_active_call[0]  = '\0';
    }

    /* Schedule a fresh CQ if preconditions are met (ported from
     * schedule_cq_tx_if_needed in source branch). */
    void schedule_cq_if_idle(void) {
        if (s_cq_paused_for_qso) return; /* still in QSO */
        ftx_tx_msg_t *txm = ft8_get_tx_msg();
        if (!txm || txm->msg[0] != '\0') return;
        FTxQsoProcessor *qso = ft8_get_qso_processor();
        if (qso && ftx_qso_processor_has_current(qso)) return;
        if (!ft8_is_tx_enabled()) return;
        ft8_schedule_cq_tx();
        s_cq_paused_for_qso = false;
    }

    int compute_dist(const ftx_msg_meta_t *meta) {
        if (!meta || meta->grid[0] == '\0' || !s_local_qth_valid) return 0;
        double qth_lat = 0.0, qth_lon = 0.0;
        ft8_get_qth(&qth_lat, &qth_lon);
        double grid_lat = 0.0, grid_lon = 0.0;
        qth_str_to_pos(meta->grid, &grid_lat, &grid_lon);
        return (int)qth_pos_dist(grid_lat, grid_lon, qth_lat, qth_lon);
    }
} // anonymous namespace

/* ===================================================================
 *  Hook adapters (extern "C" — registered with dialog hook chain)
 * =================================================================== */

extern "C" {

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

    /* Collect CQ callers (all modes) and grid responses (NEW_GRID mode). */
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

    /* Recover from stuck QSO: if the processor has nothing current
     * but we still think we're in a QSO, reset. */
    if (!ftx_qso_processor_has_current(qso) && s_qso_active) {
        reset_qso_state();
        schedule_cq_if_idle();
        autosel_clear_unseen();
        return;
    }

    /* If we're already in a QSO, let it continue. Don't interrupt. */
    if (ftx_qso_processor_has_current(qso)) {
        autosel_clear_unseen();
        return;
    }

    /* Priority swap: a to-me caller arrived while we were exhausted. */
    if (s_pending_to_me_valid && (s_pending_to_me_odd != info->odd)) {
        s_pending_to_me_valid = false;
        /* Let the QSO processor handle the to-me via existing mechanism. */
    }

    /* No current QSO — try AutoSel pick. */
    autosel_candidate_t cand;
    if (!autosel_pick(s_mode, &cand)) {
        autosel_clear_unseen();
        /* Resume CQ if we paused it for a QSO that never materialised. */
        if (s_cq_paused_for_qso) {
            s_cq_paused_for_qso = false;
            schedule_cq_if_idle();
        }
        return;
    }

    /* Start QSO with the picked candidate. */
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
    s_recover_mode     = true;  /* allow next pick to reset if stuck */
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

    /* Validate local QTH for distance computation. */
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

    /* Wait for next odd slot to match the stashed meta. */
    /* The dialog will call this before TX; if returns true, it modifies
     * tx_msg to swap to the pending grid caller. */
    s_pending_grid_valid = false;  /* consumed */

    FTxQsoProcessor *qso = ft8_get_qso_processor();
    if (!qso || ftx_qso_processor_has_current(qso)) return false;

    ftx_tx_msg_t *txm = ft8_get_tx_msg();
    if (!txm) return false;

    /* Swap to the grid caller. */
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

    /* Track grid TX count for priority swap timing. */
    if (s_qso_active && is_grid_exchange_msg(txm->msg)) {
        s_grid_tx_count++;
    }

    /* If QSO is exhausted (processor has nothing, msg cleared), reset. */
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

} // extern "C"