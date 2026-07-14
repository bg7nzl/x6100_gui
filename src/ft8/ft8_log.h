/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 File-Log Module (RX/TX/DNF)
 *
 *  Copyright (c) 2026
 *
 *  Writes RX, TX and DNF lines to /mnt/ft8_logs/x6100_log_<ts>.txt
 *  during FT8 sessions via direct calls from dialog_ft8.c / auto_dnf.c.
 */

#pragma once

#include <stdint.h>
#include <time.h>

#include "audio_worker.h"
#include "qso.h"

void ft8_log_on_init(void);
void ft8_log_on_cleanup(void);
void ft8_log_on_rx_msg(const char *text, int snr, float freq_hz, float time_sec,
                       const ftx_msg_meta_t *meta, const slot_info_t *info);
void ft8_log_on_slot_end(const slot_info_t *info);
void ft8_log_on_pre_tx(const slot_info_t *info, const char *tx_text);
void ft8_log_dnf(struct timespec slot_start, uint16_t center_hz, float delta_db);
