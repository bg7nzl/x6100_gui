/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 remote structured state (shm) + command API
 *
 *  Copyright (c) 2026
 */

#include "ft8_remote.h"

#include <ctype.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../dialog_ft8.h"
#include "../util.h"
#include "qso.h"
#include "table_view.h"

#define FT8_REMOTE_POLL_MS 200
#define FT8_FREETEXT_FILE  "/mnt/ft8_freetext.txt"

static ft8_remote_state_t *s_state;
static int                 s_fd = -1;
static uint64_t            s_last_publish;
static bool                s_force_publish;
static char                s_free_msg[16];
static char                s_status[64];

static void to_upper_token(char *s) {
    for (; *s; ++s) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static void load_free_msg_file(void) {
    s_free_msg[0] = '\0';
    FILE *fp = fopen(FT8_FREETEXT_FILE, "r");
    if (!fp) {
        return;
    }
    char buf[64];
    if (fgets(buf, sizeof(buf), fp)) {
        size_t n = 0;
        while (buf[n] && buf[n] != '\n' && buf[n] != '\r' && n + 1 < sizeof(s_free_msg)) {
            s_free_msg[n] = buf[n];
            n++;
        }
        s_free_msg[n] = '\0';
    }
    fclose(fp);
}

static void publish(void) {
    if (!s_state) {
        return;
    }

    uint32_t seq = s_state->seq;
    __atomic_store_n(&s_state->seq, seq + 1u, __ATOMIC_RELEASE);

    memset(((uint8_t *)s_state) + offsetof(ft8_remote_state_t, active), 0,
           sizeof(ft8_remote_state_t) - offsetof(ft8_remote_state_t, active));

    s_state->magic   = FT8_REMOTE_MAGIC;
    s_state->version = FT8_REMOTE_VERSION;
    s_state->row_capacity = FT8_REMOTE_MAX_ROWS;

    strncpy(s_state->status, s_status, sizeof(s_state->status) - 1);
    strncpy(s_state->free_msg, s_free_msg, sizeof(s_state->free_msg) - 1);

    if (!ft8_remote_active()) {
        s_state->active = 0;
        __atomic_store_n(&s_state->seq, seq + 2u, __ATOMIC_RELEASE);
        s_last_publish = get_time();
        s_force_publish = false;
        return;
    }

    s_state->active     = 1;
    s_state->protocol   = (uint8_t)ft8_remote_protocol();
    s_state->cq_state   = (uint8_t)ft8_remote_cq_state();
    s_state->auto_level = (uint8_t)ft8_remote_auto_level();
    s_state->auto_mode  = (uint8_t)ft8_remote_auto_mode();
    s_state->processor  = (uint8_t)ft8_remote_processor();
    s_state->show_all   = ft8_remote_show_all() ? 1 : 0;
    s_state->hold_freq  = ft8_remote_hold_freq() ? 1 : 0;
    s_state->tx_call    = ft8_remote_tx_call() ? 1 : 0;
    s_state->auto_dnf   = ft8_remote_auto_dnf() ? 1 : 0;
    s_state->tx_active  = ft8_remote_tx_active() ? 1 : 0;

    s_state->tx_delta_hz = (int16_t)ft8_remote_tx_delta();
    {
        int lo = 0, hi = 0;
        ft8_remote_filter_range(&lo, &hi);
        s_state->filter_low_hz  = (int16_t)lo;
        s_state->filter_high_hz = (int16_t)hi;
    }

    {
        const char *band = ft8_remote_band_label();
        if (band) {
            strncpy(s_state->band_label, band, sizeof(s_state->band_label) - 1);
        }
        const char *cqmod = ft8_remote_cq_modifier();
        if (cqmod) {
            strncpy(s_state->cq_modifier, cqmod, sizeof(s_state->cq_modifier) - 1);
        }
        const char *next = ft8_remote_next_tx();
        if (next) {
            strncpy(s_state->next_tx, next, sizeof(s_state->next_tx) - 1);
        }
        const char *de = ft8_remote_de_call();
        if (de) {
            strncpy(s_state->de_call, de, sizeof(s_state->de_call) - 1);
        }
        const char *grid = ft8_remote_de_grid();
        if (grid) {
            strncpy(s_state->de_grid, grid, sizeof(s_state->de_grid) - 1);
        }
    }

    s_state->row_count = (uint16_t)table_view_snapshot(s_state->rows, FT8_REMOTE_MAX_ROWS);

    __atomic_store_n(&s_state->seq, seq + 2u, __ATOMIC_RELEASE);
    s_last_publish  = get_time();
    s_force_publish = false;
}

void ft8_remote_init(void) {
    if (s_state) {
        return;
    }

    s_fd = open(FT8_REMOTE_SHM_PATH, O_RDWR | O_CREAT, 0644);
    if (s_fd < 0) {
        return;
    }
    if (ftruncate(s_fd, (off_t)sizeof(ft8_remote_state_t)) != 0) {
        close(s_fd);
        s_fd = -1;
        return;
    }
    s_state = mmap(NULL, sizeof(ft8_remote_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, 0);
    if (s_state == MAP_FAILED) {
        s_state = NULL;
        close(s_fd);
        s_fd = -1;
        return;
    }

    memset(s_state, 0, sizeof(*s_state));
    s_state->magic        = FT8_REMOTE_MAGIC;
    s_state->version      = FT8_REMOTE_VERSION;
    s_state->row_capacity = FT8_REMOTE_MAX_ROWS;
    load_free_msg_file();
    strncpy(s_state->free_msg, s_free_msg, sizeof(s_state->free_msg) - 1);
}

void ft8_remote_set_status(const char *text) {
    if (!text) {
        s_status[0] = '\0';
        return;
    }
    strncpy(s_status, text, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

void ft8_remote_note_free_msg(const char *text) {
    if (!text) {
        s_free_msg[0] = '\0';
        return;
    }
    strncpy(s_free_msg, text, sizeof(s_free_msg) - 1);
    s_free_msg[sizeof(s_free_msg) - 1] = '\0';
}

void ft8_remote_request_publish(void) {
    s_force_publish = true;
}

void ft8_remote_on_init(void) {
    load_free_msg_file();
    s_force_publish = true;
    publish();
}

void ft8_remote_on_cleanup(void) {
    s_force_publish = true;
    publish();
}

void ft8_remote_tick(void) {
    if (!s_state) {
        return;
    }
    uint64_t now = get_time();
    if (!s_force_publish && (now - s_last_publish < FT8_REMOTE_POLL_MS)) {
        return;
    }
    publish();
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static bool parse_token(const char **pp, char *out, size_t out_sz) {
    const char *p = skip_ws(*pp);
    if (*p == '\0') {
        return false;
    }
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && n + 1 < out_sz) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    *pp = p;
    return n > 0;
}

static bool eq_token(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

void ft8_remote_command(const char *rest) {
    if (!rest) {
        return;
    }

    const char *p = skip_ws(rest);
    char        cmd[32];
    if (!parse_token(&p, cmd, sizeof(cmd))) {
        return;
    }
    to_upper_token(cmd);

    if (!ft8_remote_active()) {
        ft8_remote_set_status("FT8 inactive");
        ft8_remote_request_publish();
        publish();
        return;
    }

    char arg[64];

    if (eq_token(cmd, "TXCQ")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "OFF")) {
            ft8_set_cq(CQ_OFF);
        } else if (eq_token(arg, "EVEN")) {
            ft8_set_cq(CQ_EVEN);
        } else if (eq_token(arg, "ODD")) {
            ft8_set_cq(CQ_ODD);
        }
    } else if (eq_token(cmd, "AUTO")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "OFF")) {
            ft8_set_auto_level(FTX_QSO_AUTO_OFF);
        } else if (eq_token(arg, "RES")) {
            ft8_set_auto_level(FTX_QSO_AUTO_RES);
        } else if (eq_token(arg, "FULL")) {
            ft8_set_auto_level(FTX_QSO_AUTO_FULL);
        } else if (eq_token(arg, "PRE")) {
            ft8_set_auto_level(FTX_QSO_AUTO_PRE);
        }
    } else if (eq_token(cmd, "AUTOMODE")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "SNR")) {
            ft8_set_auto_mode(FTX_QSO_SEL_SNR);
        } else if (eq_token(arg, "DIST")) {
            ft8_set_auto_mode(FTX_QSO_SEL_DISTANCE);
        } else if (eq_token(arg, "RND")) {
            ft8_set_auto_mode(FTX_QSO_SEL_RANDOM);
        } else if (eq_token(arg, "GRID")) {
            ft8_set_auto_mode(FTX_QSO_SEL_NEW_GRID);
        }
    } else if (eq_token(cmd, "PROCESSOR")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "NORMAL")) {
            ft8_set_processor(FTX_QSO_PROC_NORMAL);
        } else if (eq_token(arg, "NAVHF")) {
            ft8_set_processor(FTX_QSO_PROC_NA_VHF);
        }
    } else if (eq_token(cmd, "SHOW")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "CQ")) {
            ft8_set_show_all(false);
        } else if (eq_token(arg, "ALL")) {
            ft8_set_show_all(true);
        }
    } else if (eq_token(cmd, "MODE")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "FT8")) {
            ft8_set_protocol(FTX_PROTOCOL_FT8);
        } else if (eq_token(arg, "FT4")) {
            ft8_set_protocol(FTX_PROTOCOL_FT4);
        }
    } else if (eq_token(cmd, "HOLDFREQ")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "ON")) {
            ft8_set_hold_freq(true);
        } else if (eq_token(arg, "OFF")) {
            ft8_set_hold_freq(false);
        }
    } else if (eq_token(cmd, "TXCALL")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "ON")) {
            ft8_set_tx_call(true);
        } else if (eq_token(arg, "OFF")) {
            ft8_set_tx_call(false);
        }
    } else if (eq_token(cmd, "AUTODNF")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "ON")) {
            ft8_set_auto_dnf(true);
        } else if (eq_token(arg, "OFF")) {
            ft8_set_auto_dnf(false);
        }
    } else if (eq_token(cmd, "CQMOD")) {
        p = skip_ws(p);
        ft8_set_cq_modifier(p);
    } else if (eq_token(cmd, "FREEMSG")) {
        p = skip_ws(p);
        ft8_apply_free_msg(p);
    } else if (eq_token(cmd, "TIMESYNC")) {
        ft8_time_sync();
    } else if (eq_token(cmd, "FORCESAVE")) {
        ft8_force_save();
    } else if (eq_token(cmd, "CLICK")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        ft8_remote_click((uint32_t)strtoul(arg, NULL, 10));
    } else if (eq_token(cmd, "TXDELTA")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        ft8_set_tx_delta((int)strtol(arg, NULL, 10));
    } else if (eq_token(cmd, "BAND")) {
        if (!parse_token(&p, arg, sizeof(arg))) {
            return;
        }
        to_upper_token(arg);
        if (eq_token(arg, "UP")) {
            ft8_band(+1);
        } else if (eq_token(arg, "DOWN")) {
            ft8_band(-1);
        }
    } else {
        ft8_remote_set_status("Unknown FT8 command");
    }

    ft8_remote_request_publish();
    publish();
}
