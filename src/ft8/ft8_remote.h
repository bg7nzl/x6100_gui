/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 remote structured state (shm) + command API
 *
 *  Copyright (c) 2026
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FT8_REMOTE_SHM_PATH   "/dev/shm/x6100_ft8_state"
#define FT8_REMOTE_MAGIC      0x46543852u   /* 'F''T''8''R' */
#define FT8_REMOTE_VERSION    2
#define FT8_REMOTE_MAX_ROWS   512

typedef struct {
    uint32_t id;
    uint32_t time_utc;
    uint8_t  type;
    uint8_t  odd;
    uint8_t  call_worked;
    uint8_t  grid_worked;
    uint8_t  call_grid_worked;
    uint8_t  _pad0[3];
    int16_t  snr_db;
    int16_t  delta_hz;
    int16_t  dist_km;
    int16_t  _pad1;
    char     text[40];
    char     call[16];
    char     grid[8];
} ft8_remote_row_t;

typedef struct {
    uint32_t          magic;
    uint16_t          version;
    uint16_t          _pad0;
    volatile uint32_t seq;

    uint8_t  active;
    uint8_t  protocol;

    uint8_t  cq_state;
    uint8_t  auto_level;
    uint8_t  auto_mode;
    uint8_t  processor;
    uint8_t  show_all;
    uint8_t  hold_freq;
    uint8_t  tx_call;
    uint8_t  auto_dnf;
    uint8_t  tx_active;

    int16_t  tx_delta_hz;
    int16_t  filter_low_hz;
    int16_t  filter_high_hz;
    int16_t  _pad1;

    char     band_label[24];
    char     cq_modifier[16];
    char     free_msg[16];
    char     next_tx[32];
    char     de_call[16];
    char     de_grid[8];
    char     status[64];

    uint16_t row_count;
    uint16_t row_capacity;
    ft8_remote_row_t rows[FT8_REMOTE_MAX_ROWS];
} ft8_remote_state_t;

void ft8_remote_init(void);
void ft8_remote_tick(void);
void ft8_remote_command(const char *rest);

void ft8_remote_on_init(void);
void ft8_remote_on_cleanup(void);

void ft8_remote_set_status(const char *text);
void ft8_remote_note_free_msg(const char *text);
void ft8_remote_request_publish(void);

#ifdef __cplusplus
}
#endif
