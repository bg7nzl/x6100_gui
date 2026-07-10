/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "adif.h"

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <regex.h>

#define MHZ 1000000
#define KHZ 1000

#define COPY_STR(dst, src, len) (copy_str(dst, src, len, sizeof(dst)))

struct adif_log_s {
    FILE *fd;
};

static void write_header(FILE *fd);

static void write_str(FILE *fd, const char * key, const char * val);
static void write_int(FILE *fd, const char * key, int val);

static void write_date_time(FILE *fd, time_t time);
static void write_freq(FILE *fd, float freq_mhz);
static void write_band(FILE *fd, qso_log_band_t band);
static void write_mode(FILE *fd, qso_log_mode_t mode);

static void copy_str(char * dst, char * src, size_t val_len, size_t dst_len);
static char * extract_str(const char * src, size_t src_len);

static qso_log_band_t str_to_band(const char * s);
static qso_log_mode_t create_mode(const char * mode, const char * submode);

static bool field_is(const char *field, size_t field_len, const char *name);
static bool line_has_eor(const char *line, ssize_t read);


adif_log adif_log_init(const char * path) {
    adif_log log = (adif_log) malloc(sizeof(struct adif_log_s));
    bool new_file = false;
    if (access(path, F_OK) != 0) {
        new_file = true;
    }
    FILE *log_fd = fopen(path, "a");
    if (log_fd == NULL) {
        perror("Unable to open log file:");
        free(log);
        return NULL;
    } else {
        log->fd = log_fd;
        if (new_file) {
            write_header(log->fd);
        }
    }
    return log;
}

void adif_log_close(adif_log l) {
    fclose(l->fd);
}

void adif_add_qso(adif_log l, qso_log_record_t qso)
{
    write_str(l->fd, "STATION_CALLSIGN", qso.local_call);
    write_str(l->fd, "OPERATOR", qso.local_call);
    write_str(l->fd, "CALL", qso.remote_call);
    write_date_time(l->fd, qso.time);
    write_mode(l->fd, qso.mode);
    write_str(l->fd, "NAME", NULL);
    write_str(l->fd, "QTH", NULL);
    write_int(l->fd, "RST_SENT", qso.rsts);
    write_str(l->fd, "STX", NULL);
    write_int(l->fd, "RST_RCVD", qso.rstr);
    write_band(l->fd, qso.band);
    write_freq(l->fd, qso.freq_mhz);
    write_str(l->fd, "GRIDSQUARE", qso.remote_grid);
    write_str(l->fd, "MY_GRIDSQUARE", qso.local_grid);
    fprintf(l->fd, "<EOR>\r\n");
    fflush(l->fd);
}

int adif_read(const char * path, qso_log_record_t ** records) {
    char * line = NULL;
    size_t len = 0;
    ssize_t read;

    FILE *fp = fopen(path, "r");

    if (fp == NULL) {
        perror("Unable to open log file:");
        return 0;
    }

    static regex_t  regex;
    static const    char re[] = "<([A-Za-z_]+):([0-9]+)>";
    regmatch_t      pmatch[3];
    regoff_t        off, r_len;
    char            *s;

    if (regcomp(&regex, re, REG_NEWLINE | REG_EXTENDED)) {
        printf("Can't compile regexp");
        return -2;
    }

    size_t arr_size = 128;
    *records = malloc(arr_size * sizeof(qso_log_record_t));

    qso_log_record_t *cur_record;
    ssize_t cur_record_id = 0;
    struct tm qso_ts;
    size_t val_len;

    char *rec = NULL;
    size_t rec_len = 0;
    size_t rec_cap = 0;

    while ((read = getline(&line, &len, fp)) != -1) {
        size_t append_len = (size_t) read;
        while (append_len > 0 && (line[append_len - 1] == '\n' || line[append_len - 1] == '\r')) {
            append_len--;
        }
        if (rec_len + append_len + 1 > rec_cap) {
            rec_cap = (rec_len + append_len + 1) * 2;
            rec = realloc(rec, rec_cap);
        }
        memcpy(rec + rec_len, line, append_len);
        rec_len += append_len;
        rec[rec_len] = '\0';

        if (!line_has_eor(line, read)) continue;

        s = rec;
        cur_record = &(*records)[cur_record_id];
        memset(cur_record, 0, sizeof(*cur_record));
        memset(&qso_ts, 0, sizeof(qso_ts));
        char * mode = NULL;
        char * submode = NULL;
        for (unsigned int i = 0; ; i++) {
            if (regexec(&regex, s, ARRAY_SIZE(pmatch), pmatch, 0))
                break;
            val_len = atoi(s + pmatch[2].rm_so);
            if (val_len > 0) {
                const char *field = s + pmatch[1].rm_so;
                size_t field_len = pmatch[1].rm_eo - pmatch[1].rm_so;
                const char *val = s + pmatch[0].rm_eo;

                if (field_is(field, field_len, "STATION_CALLSIGN")) {
                    if (cur_record->local_call[0] == '\0') {
                        COPY_STR(cur_record->local_call, (char *) val, val_len);
                    }
                } else if (field_is(field, field_len, "OPERATOR")) {
                    COPY_STR(cur_record->local_call, (char *) val, val_len);
                } else if (field_is(field, field_len, "CALL")) {
                    COPY_STR(cur_record->remote_call, (char *) val, val_len);
                } else if (field_is(field, field_len, "QSO_DATE")) {
                    strptime(val, "%Y%m%d", &qso_ts);
                } else if (field_is(field, field_len, "TIME_ON")) {
                    if (val_len >= 6) {
                        strptime(val, "%H%M%S", &qso_ts);
                    } else {
                        strptime(val, "%H%M", &qso_ts);
                    }
                } else if (field_is(field, field_len, "MODE")) {
                    mode = extract_str(val, val_len);
                } else if (field_is(field, field_len, "SUBMODE")) {
                    submode = extract_str(val, val_len);
                } else if (field_is(field, field_len, "NAME")) {
                    COPY_STR(cur_record->name, (char *) val, val_len);
                } else if (field_is(field, field_len, "QTH")) {
                    COPY_STR(cur_record->qth, (char *) val, val_len);
                } else if (field_is(field, field_len, "RST_SENT")) {
                    cur_record->rsts = atoi(val);
                } else if (field_is(field, field_len, "RST_RCVD")) {
                    cur_record->rstr = atoi(val);
                } else if (field_is(field, field_len, "BAND")) {
                    cur_record->band = str_to_band(val);
                } else if (field_is(field, field_len, "FREQ")) {
                    cur_record->freq_mhz = strtof(val, NULL);
                } else if (field_is(field, field_len, "MY_GRIDSQUARE")) {
                    COPY_STR(cur_record->local_grid, (char *) val, val_len);
                } else if (field_is(field, field_len, "GRIDSQUARE")) {
                    COPY_STR(cur_record->remote_grid, (char *) val, val_len);
                }
            }

            s += pmatch[0].rm_eo;
        }

        cur_record->time = mktime(&qso_ts);
        cur_record->mode = create_mode(mode, submode);
        if (mode) free(mode);
        if (submode) free(submode);
        if ((qso_log_freq_to_band(cur_record->freq_mhz * MHZ) != cur_record->band) &&
            (qso_log_freq_to_band(cur_record->freq_mhz * KHZ) == cur_record->band)) {
                cur_record->freq_mhz /= 1000;
        }
        cur_record_id++;
        if (cur_record_id >= arr_size) {
            arr_size *= 2;
            (*records) = realloc((*records), arr_size * sizeof(qso_log_record_t));
        }
        rec_len = 0;
    }
    free(rec);
    return cur_record_id--;
}

static void write_header(FILE *fd) {
    fprintf(fd, "<PROGRAMID:5>X6100\r\n");
    fprintf(fd, "<PROGRAMVERSION:5>1.0.0\r\n");
    fprintf(fd, "<ADIF_VER:4>3.14\r\n");
    fprintf(fd, "<EOH>\r\n");
}

static void write_str(FILE *fd, const char * key, const char * val) {
    if (val == NULL) {
        fprintf(fd, "<%s:0>", key);
    } else {
        size_t l = strlen(val);
        fprintf(fd, "<%s:%i>%s", key, l, val);
    }
}

static void write_int(FILE *fd, const char * key, int val) {
    char str_val[8];
    sprintf(str_val, "%i", val);
    write_str(fd, key, str_val);
}

static void write_date_time(FILE *fd, time_t time) {
    struct tm *ts = gmtime(&time);
    fprintf(fd, "<QSO_DATE:8>%04i%02i%02i", ts->tm_year + 1900, ts->tm_mon + 1, ts->tm_mday);
    fprintf(fd, "<QSO_DATE_OFF:8>%04i%02i%02i", ts->tm_year + 1900, ts->tm_mon + 1, ts->tm_mday);
    fprintf(fd, "<TIME_ON:4>%02i%02i", ts->tm_hour, ts->tm_min);
    fprintf(fd, "<TIME_OFF:4>%02i%02i", ts->tm_hour, ts->tm_min);
}

static void write_freq(FILE *fd, float freq_mhz) {
    char str_freq[8];
    sprintf(str_freq, "%0.4f", freq_mhz);
    write_str(fd, "FREQ", str_freq);
}

static void write_band(FILE *fd, qso_log_band_t band) {
    if (band == BAND_OTHER) {
        write_str(fd, "BAND", "");
    } else {
        char str_band[8];
        sprintf(str_band, "%dM", band);
        write_str(fd, "BAND", str_band);
    }
}

static void write_mode(FILE *fd, qso_log_mode_t mode) {
    char * mode_str;
    char * submode_str = NULL;
    switch (mode) {
        case MODE_SSB:
            mode_str = "SSB";
            // submode_str = "USB";
        case MODE_AM:
            mode_str = "AM";
            break;
        case MODE_FM:
            mode_str = "FM";
            break;
        case MODE_CW:
            mode_str = "CW";
            // submode_str = "PCW";
            break;
        case MODE_FT8:
            mode_str = "FT8";
            break;
        case MODE_FT4:
            mode_str = "MFSK";
            submode_str = "FT4";
            break;
        case MODE_RTTY:
            mode_str = "RTTY";
            break;
    }
    write_str(fd, "MODE", mode_str);
    write_str(fd, "SUBMODE", submode_str);
}


static void copy_str(char * dst, char * src, size_t val_len, size_t dst_len) {
    if (val_len > (dst_len - 1)) {
        val_len = dst_len - 1;
    }
    strncpy(dst, src, val_len);
    dst[val_len] = 0;
}

static char * extract_str(const char * src, size_t src_len) {
    char * dst;
    dst = malloc(src_len + 1);
    memcpy(dst, src, src_len);
    dst[src_len] = 0;
    return dst;
}

static qso_log_band_t str_to_band(const char * s) {
    return atoi(s);
}


static qso_log_mode_t create_mode(const char * mode, const char * submode) {
    if (!mode) return MODE_OTHER;
    if (strcasecmp(mode, "SSB") == 0) return MODE_SSB;
    if (strcasecmp(mode, "AM") == 0) return MODE_AM;
    if (strcasecmp(mode, "FM") == 0) return MODE_FM;
    if (strcasecmp(mode, "CW") == 0) return MODE_CW;
    if (strcasecmp(mode, "FT8") == 0) return MODE_FT8;
    if (strcasecmp(mode, "RTTY") == 0) return MODE_RTTY;
    if (!submode) return MODE_OTHER;
    if ((strcasecmp(mode, "MFSK") == 0) && (strcasecmp(submode, "FT4") == 0)) return MODE_FT4;
    return MODE_OTHER;
}

static bool field_is(const char *field, size_t field_len, const char *name)
{
    size_t name_len = strlen(name);
    return field_len == name_len && strncasecmp(field, name, field_len) == 0;
}

static bool line_has_eor(const char *line, ssize_t read)
{
    ssize_t end = read;

    if (end < 5) return false;

    while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r')) {
        end--;
    }

    return end >= 5 && strncasecmp(line + end - 5, "<EOR>", 5) == 0;
}
