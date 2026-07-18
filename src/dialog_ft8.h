/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dialog.h"
#include "events.h"
#include "ft8/qso.h"

#include <ft8lib/message.h>

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_ft8;

/* TX CQ tri-state (session-local Subject). */
typedef enum {
    CQ_OFF = 0,
    CQ_EVEN,
    CQ_ODD,
} cq_state_t;

/* Semantic setters — panel buttons and remote API share these. */
void ft8_set_cq(cq_state_t st);
void ft8_set_auto_level(ftx_qso_auto_t level);
void ft8_set_auto_mode(ftx_qso_sel_t sel);
void ft8_set_processor(ftx_qso_proc_t proc);
void ft8_set_show_all(bool show_all);
void ft8_set_protocol(ftx_protocol_t proto);
void ft8_set_hold_freq(bool hold);
void ft8_set_tx_call(bool enable);
void ft8_set_auto_dnf(bool enable);
void ft8_set_cq_modifier(const char *cq_mod);
void ft8_apply_free_msg(const char *text);
void ft8_force_save(void);
void ft8_time_sync(void);

void ft8_remote_click(uint32_t row_id);
void ft8_set_tx_delta(int hz);
void ft8_band(int dir);

/* FT8 dialog open → always true (swallow PTT / no radio_set_ptt).
 * When a TX2 confirm is armed, KEYPAD_PRESS also accepts it. */
bool ft8_consume_ptt(keypad_state_t state);

/* Pending TX2 confirm + dialog open → accept confirm and swallow this
 * VFO step (no freq_shift). Otherwise false. */
bool ft8_confirm_consume_rotary(void);

/* State getters for shm publish (main thread). */
bool        ft8_remote_active(void);
int         ft8_remote_protocol(void);
int         ft8_remote_cq_state(void);
int         ft8_remote_auto_level(void);
int         ft8_remote_auto_mode(void);
int         ft8_remote_processor(void);
bool        ft8_remote_show_all(void);
bool        ft8_remote_hold_freq(void);
bool        ft8_remote_tx_call(void);
bool        ft8_remote_auto_dnf(void);
bool        ft8_remote_tx_active(void);
int         ft8_remote_tx_delta(void);
void        ft8_remote_filter_range(int *lo, int *hi);
const char *ft8_remote_band_label(void);
const char *ft8_remote_cq_modifier(void);
const char *ft8_remote_next_tx(void);
const char *ft8_remote_de_call(void);
const char *ft8_remote_de_grid(void);

#ifdef __cplusplus
}
#endif
