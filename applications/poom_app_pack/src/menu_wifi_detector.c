// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#include "menu_wifi_detector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button_driver.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "menu_detector_view.h"
#include "poom_sbus.h"
#include "poom_secrets_store.h"
#include "poom_ui_keyboard.h"
#include "poom_wifi_detector.h"

#define POOM_MENU_RESUME_TOPIC          "poom/menu/resume"
#define MENU_WIFI_DETECTOR_SCAN_MS      (350U)
#define MENU_WIFI_DETECTOR_DETAIL_SCAN_MS (100U)
#define MENU_WIFI_DETECTOR_MONITOR_REFRESH_MS (100U)
#define MENU_WIFI_DETECTOR_TASK_STACK   (4608U)
#define MENU_WIFI_DETECTOR_TASK_PRIO    (5U)
#define MENU_WIFI_DETECTOR_SCAN_STACK   (4096U)
#define MENU_WIFI_DETECTOR_SCAN_PRIO    (4U)
#define MENU_WIFI_DETECTOR_ALIAS_LEN    (24U)
#define MENU_WIFI_DETECTOR_NAME_LEN     POOM_WIFI_DETECTOR_SSID_LEN
#define MENU_WIFI_DETECTOR_LIST_CHARS   (13U)
#define MENU_WIFI_DETECTOR_SCROLL_MS    (350U)
#define MENU_WIFI_DETECTOR_SCROLL_GAP   (3U)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_WIFI_DETECTOR_SCREEN_SELECT = 0,
    MENU_WIFI_DETECTOR_SCREEN_SCAN,
    MENU_WIFI_DETECTOR_SCREEN_DETAIL,
    MENU_WIFI_DETECTOR_SCREEN_ALIAS,
} menu_wifi_detector_screen_t;

typedef struct
{
    bool used;
    uint8_t bssid[6];
    char alias[MENU_WIFI_DETECTOR_ALIAS_LEN];
} menu_wifi_detector_alias_cache_t;

static const char *const s_filter_labels[] = {
    "WIFI DEVICES",
    "AP CLIENTS",
    "FLOCK/ALPR",
    "IP CAMERAS",
};

static poom_wifi_detector_record_t *s_records[MENU_DETECTOR_VIEW_MAX_ITEMS];
static poom_wifi_detector_record_t *s_scan_records[MENU_DETECTOR_VIEW_MAX_ITEMS];
static poom_wifi_detector_record_t *s_record_storage;
static poom_wifi_detector_record_t *s_scan_record_storage;
static char (*s_names)[MENU_WIFI_DETECTOR_NAME_LEN];
static menu_wifi_detector_alias_cache_t *s_alias_cache;
static int s_record_count;
static int s_selected_index;
static int s_scroll;
static int s_filter_index;
static int s_detail_index = -1;
static menu_wifi_detector_screen_t s_screen = MENU_WIFI_DETECTOR_SCREEN_SELECT;
static volatile bool s_active;
static volatile bool s_exit_requested;
static bool s_scan_requested;
static volatile bool s_scan_in_progress;
static volatile bool s_scan_complete;
static bool s_monitor_active;
static esp_err_t s_scan_status;
static size_t s_scan_count;
static uint8_t s_scan_channel;
static uint8_t s_scan_target_bssid[6];
static bool s_input_dirty;
static bool s_buttons_subscribed;
static bool s_secrets_ready;
static TaskHandle_t s_task;
static TaskHandle_t s_scan_task;
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_selection_tick_ms;
static uint32_t s_last_scroll_slot;
static TickType_t s_last_monitor_refresh;
static uint8_t s_monitor_channel;
static bool s_ap_clients_selected;
static poom_ui_keyboard_t s_alias_keyboard;
static char s_alias_buffer[MENU_WIFI_DETECTOR_ALIAS_LEN];

static void menu_wifi_detector_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_wifi_detector_free_(void);
static void menu_wifi_detector_scan_task_(void *arg);
static void menu_wifi_detector_normalize_selection_(void);
static void menu_wifi_detector_apply_names_(void);

static bool menu_wifi_detector_uses_monitor_(void)
{
    return (s_filter_index == POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS) ||
           (s_filter_index == POOM_WIFI_DETECTOR_FILTER_FLOCK_ALPR) ||
           (s_filter_index == POOM_WIFI_DETECTOR_FILTER_IP_CAMERAS);
}

static bool menu_wifi_detector_is_ap_clients_(void)
{
    return s_filter_index == POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS;
}

static uint32_t menu_wifi_detector_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void menu_wifi_detector_reset_text_scroll_(void)
{
    s_selection_tick_ms = menu_wifi_detector_now_ms_();
    s_last_scroll_slot = UINT32_MAX;
}

static bool menu_wifi_detector_text_scroll_active_(void)
{
    return (s_screen == MENU_WIFI_DETECTOR_SCREEN_SCAN) &&
           (s_selected_index >= 0) &&
           (s_selected_index < s_record_count) &&
           (strnlen(s_names[s_selected_index], MENU_WIFI_DETECTOR_NAME_LEN) >
            MENU_WIFI_DETECTOR_LIST_CHARS);
}

static void menu_wifi_detector_format_row_(char *out,
                                            size_t out_len,
                                            const char *label,
                                            bool selected)
{
    size_t label_len;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }

    out[0] = '\0';
    if(label == NULL)
    {
        return;
    }

    label_len = strnlen(label, MENU_WIFI_DETECTOR_NAME_LEN);
    if(!selected || (label_len <= MENU_WIFI_DETECTOR_LIST_CHARS))
    {
        (void)snprintf(out, out_len, "%.13s", label);
        return;
    }

    size_t cycle_width = label_len + MENU_WIFI_DETECTOR_SCROLL_GAP;
    size_t phase = (size_t)(((menu_wifi_detector_now_ms_() - s_selection_tick_ms) /
                              MENU_WIFI_DETECTOR_SCROLL_MS) % cycle_width);

    for(size_t out_index = 0U;
        (out_index < MENU_WIFI_DETECTOR_LIST_CHARS) && (out_index + 1U < out_len);
        ++out_index)
    {
        size_t source_index = (phase + out_index) % cycle_width;
        out[out_index] = (source_index < label_len) ? label[source_index] : ' ';
        out[out_index + 1U] = '\0';
    }
}

static void menu_wifi_detector_format_signal_(char *out,
                                               size_t out_len,
                                               int rssi,
                                               bool rssi_known)
{
    int bars;
    size_t used;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }

    bars = rssi_known ? ((rssi + 100) / 6) : 0;
    if(rssi_known && (bars < 1))
    {
        bars = 1;
    }
    else if(bars > 8)
    {
        bars = 8;
    }

    (void)snprintf(out, out_len, "SIGNAL [");
    used = strnlen(out, out_len);
    for(int position = 0; (position < 8) && ((used + 2U) < out_len); ++position)
    {
        out[used++] = (position < bars) ? '#' : ' ';
    }
    if((used + 1U) < out_len)
    {
        out[used++] = ']';
    }
    out[used] = '\0';
}

static void menu_wifi_detector_cancel_scan_(void)
{
    esp_err_t status;

    if(!s_scan_in_progress)
    {
        return;
    }

    status = esp_wifi_scan_stop();
    if((status != ESP_OK) &&
       (status != ESP_ERR_WIFI_NOT_INIT) &&
       (status != ESP_ERR_WIFI_NOT_STARTED) &&
       (status != ESP_ERR_WIFI_STATE))
    {
        /* The worker still observes s_exit_requested and performs cleanup. */
    }
}

static void menu_wifi_detector_alias_id_(char *out,
                                         size_t out_len,
                                         const uint8_t bssid[6])
{
    if((out == NULL) || (out_len == 0U) || (bssid == NULL))
    {
        return;
    }
    (void)snprintf(out,
                   out_len,
                   "wifi_det/%02X%02X%02X%02X%02X%02X",
                   bssid[0], bssid[1], bssid[2],
                   bssid[3], bssid[4], bssid[5]);
}

static int menu_wifi_detector_alias_cache_find_(const uint8_t bssid[6])
{
    int i;
    for(i = 0; i < MENU_DETECTOR_VIEW_MAX_ITEMS; ++i)
    {
        if(s_alias_cache[i].used && (memcmp(s_alias_cache[i].bssid, bssid, 6U) == 0))
        {
            return i;
        }
    }
    return -1;
}

static int menu_wifi_detector_alias_cache_add_(const uint8_t bssid[6])
{
    int i = menu_wifi_detector_alias_cache_find_(bssid);
    if(i >= 0)
    {
        return i;
    }
    for(i = 0; i < MENU_DETECTOR_VIEW_MAX_ITEMS; ++i)
    {
        if(!s_alias_cache[i].used)
        {
            s_alias_cache[i].used = true;
            (void)memcpy(s_alias_cache[i].bssid, bssid, 6U);
            s_alias_cache[i].alias[0] = '\0';
            return i;
        }
    }
    return -1;
}

static bool menu_wifi_detector_alias_load_(const uint8_t bssid[6],
                                            char *out,
                                            size_t out_len)
{
    char id[64];
    void *blob = NULL;
    size_t blob_len = 0U;
    size_t copy_len;

    if((bssid == NULL) || (out == NULL) || (out_len == 0U) || !s_secrets_ready)
    {
        return false;
    }

    out[0] = '\0';
    menu_wifi_detector_alias_id_(id, sizeof(id), bssid);
    if(poom_secrets_get_record_blob_alloc(id, &blob, &blob_len) != ESP_OK)
    {
        return false;
    }
    if((blob == NULL) || (blob_len == 0U))
    {
        free(blob);
        return false;
    }

    copy_len = (blob_len < out_len) ? blob_len : (out_len - 1U);
    (void)memcpy(out, blob, copy_len);
    out[copy_len] = '\0';
    free(blob);
    return out[0] != '\0';
}

static void menu_wifi_detector_alias_save_(const uint8_t bssid[6], const char *alias)
{
    char id[64];
    size_t len;

    if((bssid == NULL) || (alias == NULL) || !s_secrets_ready)
    {
        return;
    }
    menu_wifi_detector_alias_id_(id, sizeof(id), bssid);
    if(alias[0] == '\0')
    {
        (void)poom_secrets_erase_record(id);
        return;
    }
    len = strnlen(alias, sizeof(s_alias_buffer) - 1U);
    (void)poom_secrets_set_record_blob(id, alias, len + 1U);
}

static bool menu_wifi_detector_alloc_(void)
{
    int i;

    (void)memset(s_records, 0, sizeof(s_records));
    (void)memset(s_scan_records, 0, sizeof(s_scan_records));
    s_record_storage = heap_caps_malloc(
        MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_record_storage),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_scan_record_storage = heap_caps_malloc(
        MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_scan_record_storage),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if((s_record_storage == NULL) || (s_scan_record_storage == NULL))
    {
        menu_wifi_detector_free_();
        return false;
    }
    (void)memset(s_record_storage,
                 0,
                 MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_record_storage));
    (void)memset(s_scan_record_storage,
                 0,
                 MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_scan_record_storage));
    for(i = 0; i < MENU_DETECTOR_VIEW_MAX_ITEMS; ++i)
    {
        s_records[i] = &s_record_storage[i];
        s_scan_records[i] = &s_scan_record_storage[i];
    }

    s_names = heap_caps_malloc(MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_names),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_alias_cache = heap_caps_malloc(
        MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_alias_cache),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if((s_names == NULL) || (s_alias_cache == NULL))
    {
        menu_wifi_detector_free_();
        return false;
    }
    (void)memset(s_names,
                 0,
                 MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_names));
    (void)memset(s_alias_cache,
                 0,
                 MENU_DETECTOR_VIEW_MAX_ITEMS * sizeof(*s_alias_cache));
    return true;
}

static void menu_wifi_detector_free_(void)
{
    free(s_names);
    s_names = NULL;
    free(s_alias_cache);
    s_alias_cache = NULL;
    free(s_record_storage);
    s_record_storage = NULL;
    free(s_scan_record_storage);
    s_scan_record_storage = NULL;
    (void)memset(s_records, 0, sizeof(s_records));
    (void)memset(s_scan_records, 0, sizeof(s_scan_records));
}

static esp_err_t menu_wifi_detector_start_scan_(uint8_t channel,
                                                 const uint8_t target_bssid[6])
{
    if(s_scan_in_progress || s_scan_complete)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_channel = channel;
    if(target_bssid != NULL)
    {
        (void)memcpy(s_scan_target_bssid, target_bssid, sizeof(s_scan_target_bssid));
    }
    else
    {
        (void)memset(s_scan_target_bssid, 0, sizeof(s_scan_target_bssid));
    }
    s_scan_in_progress = true;
    if(xTaskCreate(menu_wifi_detector_scan_task_,
                   "wifi_detector_scan",
                   MENU_WIFI_DETECTOR_SCAN_STACK,
                   NULL,
                   MENU_WIFI_DETECTOR_SCAN_PRIO,
                   &s_scan_task) != pdPASS)
    {
        s_scan_in_progress = false;
        s_scan_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void menu_wifi_detector_scan_task_(void *arg)
{
    size_t count = 0U;
    esp_err_t status;
    TaskHandle_t menu_task;
    (void)arg;

    status = poom_wifi_detector_scan_channel((poom_wifi_detector_filter_t)s_filter_index,
                                             s_scan_channel,
                                             s_scan_records,
                                             MENU_DETECTOR_VIEW_MAX_ITEMS,
                                             &count);

    portENTER_CRITICAL(&s_scan_lock);
    s_scan_status = status;
    s_scan_count = count;
    s_scan_complete = true;
    s_scan_in_progress = false;
    s_scan_task = NULL;
    menu_task = s_task;
    portEXIT_CRITICAL(&s_scan_lock);

    if(menu_task != NULL)
    {
        (void)xTaskNotifyGive(menu_task);
    }
    vTaskDelete(NULL);
}

static bool menu_wifi_detector_take_scan_(esp_err_t *status,
                                           size_t *count,
                                           uint8_t *channel,
                                           uint8_t target_bssid[6])
{
    bool ready;

    if((status == NULL) || (count == NULL) ||
       (channel == NULL) || (target_bssid == NULL))
    {
        return false;
    }

    portENTER_CRITICAL(&s_scan_lock);
    ready = s_scan_complete;
    if(ready)
    {
        *status = s_scan_status;
        *count = s_scan_count;
        *channel = s_scan_channel;
        (void)memcpy(target_bssid, s_scan_target_bssid, sizeof(s_scan_target_bssid));
        s_scan_complete = false;
    }
    portEXIT_CRITICAL(&s_scan_lock);
    return ready;
}

static void menu_wifi_detector_apply_scan_(size_t count)
{
    uint8_t previous_bssid[6] = {0};
    bool detail_was_open = (s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL) &&
                           (s_detail_index >= 0) &&
                           (s_detail_index < s_record_count);
    int previous_index = detail_was_open ? s_detail_index : s_selected_index;
    bool had_previous = (s_record_count > 0) &&
                        (previous_index >= 0) &&
                        (previous_index < s_record_count);
    int matched_index = -1;
    int i;

    if(had_previous)
    {
        (void)memcpy(previous_bssid,
                     s_records[previous_index]->bssid,
                     sizeof(previous_bssid));
    }

    for(i = 0; i < MENU_DETECTOR_VIEW_MAX_ITEMS; ++i)
    {
        poom_wifi_detector_record_t *temporary = s_records[i];
        s_records[i] = s_scan_records[i];
        s_scan_records[i] = temporary;
    }

    s_record_count = (int)count;
    if(had_previous)
    {
        for(i = 0; i < s_record_count; ++i)
        {
            if(memcmp(previous_bssid,
                      s_records[i]->bssid,
                      sizeof(previous_bssid)) == 0)
            {
                matched_index = i;
                break;
            }
        }
    }

    if(matched_index >= 0)
    {
        s_selected_index = matched_index;
        if(detail_was_open)
        {
            s_detail_index = matched_index;
        }
    }
    else if(detail_was_open)
    {
        s_screen = MENU_WIFI_DETECTOR_SCREEN_SCAN;
        s_detail_index = -1;
    }

    menu_wifi_detector_apply_names_();
    menu_wifi_detector_normalize_selection_();
    if(!had_previous || (matched_index < 0))
    {
        menu_wifi_detector_reset_text_scroll_();
    }
    s_input_dirty = true;
}

static void menu_wifi_detector_apply_target_scan_(size_t count,
                                                   const uint8_t target_bssid[6])
{
    int destination = -1;
    int source = -1;
    int i;

    if(target_bssid == NULL)
    {
        return;
    }

    for(i = 0; i < s_record_count; ++i)
    {
        if(memcmp(s_records[i]->bssid, target_bssid, 6U) == 0)
        {
            destination = i;
            break;
        }
    }
    for(i = 0; i < (int)count; ++i)
    {
        if(memcmp(s_scan_records[i]->bssid, target_bssid, 6U) == 0)
        {
            source = i;
            break;
        }
    }

    if((destination < 0) || (source < 0))
    {
        return;
    }

    *s_records[destination] = *s_scan_records[source];
    if((s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL) &&
       (s_detail_index >= 0) &&
       (s_detail_index < s_record_count) &&
       (memcmp(s_records[s_detail_index]->bssid, target_bssid, 6U) == 0))
    {
        s_detail_index = destination;
        s_selected_index = destination;
    }
    menu_wifi_detector_apply_names_();
    s_input_dirty = true;
}

static void menu_wifi_detector_normalize_selection_(void)
{
    int max_scroll;

    if(s_record_count <= 0)
    {
        s_selected_index = 0;
        s_scroll = 0;
        return;
    }
    if(s_selected_index < 0)
    {
        s_selected_index = 0;
    }
    if(s_selected_index >= s_record_count)
    {
        s_selected_index = s_record_count - 1;
    }
    if(s_selected_index < s_scroll)
    {
        s_scroll = s_selected_index;
    }
    if(s_selected_index >= (s_scroll + MENU_DETECTOR_VIEW_VISIBLE_ROWS))
    {
        s_scroll = s_selected_index - MENU_DETECTOR_VIEW_VISIBLE_ROWS + 1;
    }
    max_scroll = s_record_count - MENU_DETECTOR_VIEW_VISIBLE_ROWS;
    if(max_scroll < 0)
    {
        max_scroll = 0;
    }
    if(s_scroll > max_scroll)
    {
        s_scroll = max_scroll;
    }
    if(s_scroll < 0)
    {
        s_scroll = 0;
    }
}

static void menu_wifi_detector_apply_names_(void)
{
    int i;

    for(i = 0; i < s_record_count; ++i)
    {
        int cache_index = menu_wifi_detector_alias_cache_find_(s_records[i]->bssid);
        if(cache_index < 0)
        {
            char alias[MENU_WIFI_DETECTOR_ALIAS_LEN];
            cache_index = menu_wifi_detector_alias_cache_add_(s_records[i]->bssid);
            if((cache_index >= 0) &&
               menu_wifi_detector_alias_load_(s_records[i]->bssid, alias, sizeof(alias)))
            {
                (void)snprintf(s_alias_cache[cache_index].alias,
                               sizeof(s_alias_cache[cache_index].alias),
                               "%.23s",
                               alias);
            }
        }

        if((cache_index >= 0) && (s_alias_cache[cache_index].alias[0] != '\0'))
        {
            (void)snprintf(s_names[i], sizeof(s_names[i]), "%.32s",
                           s_alias_cache[cache_index].alias);
        }
        else
        {
            (void)snprintf(s_names[i], sizeof(s_names[i]), "%.32s", s_records[i]->ssid);
        }
    }
}

static void menu_wifi_detector_render_scan_(void)
{
    menu_detector_view_row_t rows[MENU_DETECTOR_VIEW_MAX_ITEMS];
    bool ap_clients = menu_wifi_detector_is_ap_clients_();
    int i;

    if(s_record_count == 0)
    {
        if(ap_clients && s_ap_clients_selected)
        {
            menu_detector_view_draw_waiting_actions("AP CLIENTS", "B:APS");
        }
        else
        {
            menu_detector_view_draw_waiting(poom_wifi_detector_filter_label(
                (poom_wifi_detector_filter_t)s_filter_index));
        }
        return;
    }

    menu_wifi_detector_normalize_selection_();
    for(i = 0; i < s_record_count; ++i)
    {
        menu_wifi_detector_format_row_(rows[i].name,
                                       sizeof(rows[i].name),
                                       s_names[i],
                                       i == s_selected_index);
        rows[i].rssi = s_records[i]->rssi;
        rows[i].rssi_known = s_records[i]->rssi_known;
    }
    if(ap_clients)
    {
        menu_detector_view_draw_list_actions("AP CLIENTS",
                                             rows,
                                             s_record_count,
                                             s_selected_index,
                                             s_scroll,
                                             s_ap_clients_selected ? "A:DETAIL" : "A:SELECT",
                                             s_ap_clients_selected ? "B:APS" : "B:EXIT");
    }
    else
    {
        menu_detector_view_draw_list(poom_wifi_detector_filter_label(
                                         (poom_wifi_detector_filter_t)s_filter_index),
                                     rows,
                                     s_record_count,
                                     s_selected_index,
                                     s_scroll);
    }
}

static void menu_wifi_detector_render_detail_(void)
{
    const poom_wifi_detector_record_t *record;
    char line0[22];
    char line1[22];
    char line2[22];
    char line3[22];
    char line4[22];
    const char *lines[5];

    if((s_detail_index < 0) || (s_detail_index >= s_record_count))
    {
        s_screen = MENU_WIFI_DETECTOR_SCREEN_SCAN;
        menu_wifi_detector_render_scan_();
        return;
    }

    record = s_records[s_detail_index];
    (void)snprintf(line0, sizeof(line0), "%.21s", s_names[s_detail_index]);
    menu_wifi_detector_format_signal_(line1,
                                      sizeof(line1),
                                      (int)record->rssi,
                                      record->rssi_known);
    if(record->is_access_point && record->auth_known)
    {
        (void)snprintf(line2,
                       sizeof(line2),
                       "CH:%u %.14s",
                       (unsigned)record->channel,
                       poom_wifi_detector_auth_label(record->auth_mode));
    }
    else
    {
        (void)snprintf(line2,
                       sizeof(line2),
                       "CH:%u %s",
                       (unsigned)record->channel,
                       record->is_access_point ? "AP" : "CLIENT");
    }
    if(record->rssi_known)
    {
        (void)snprintf(line3, sizeof(line3), "RSSI=%ddBm", (int)record->rssi);
    }
    else
    {
        (void)snprintf(line3, sizeof(line3), "RSSI=---");
    }
    (void)snprintf(line4,
                   sizeof(line4),
                   "%s:%02X:%02X:%02X:%02X:%02X:%02X",
                   record->is_access_point ? "AP" : "MAC",
                   record->bssid[0], record->bssid[1], record->bssid[2],
                   record->bssid[3], record->bssid[4], record->bssid[5]);

    if(menu_wifi_detector_is_ap_clients_() && s_ap_clients_selected)
    {
        lines[0] = line1;
        lines[1] = line2;
        lines[2] = line3;
        lines[3] = line4;
        lines[4] = "";
    }
    else
    {
        lines[0] = line0;
        lines[1] = line1;
        lines[2] = line2;
        lines[3] = line3;
        lines[4] = line4;
    }
    menu_detector_view_draw_detail(lines, s_detail_index, s_record_count);
}

static void menu_wifi_detector_render_(void)
{
    if(s_screen == MENU_WIFI_DETECTOR_SCREEN_SELECT)
    {
        menu_detector_view_draw_selector("WIFI DETECT",
                                         s_filter_labels,
                                         POOM_WIFI_DETECTOR_FILTER_COUNT,
                                         s_filter_index);
    }
    else if(s_screen == MENU_WIFI_DETECTOR_SCREEN_ALIAS)
    {
        poom_ui_keyboard_draw(&s_alias_keyboard);
    }
    else if(s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL)
    {
        menu_wifi_detector_render_detail_();
    }
    else
    {
        menu_wifi_detector_render_scan_();
    }
}

static void menu_wifi_detector_enter_alias_(void)
{
    const poom_wifi_detector_record_t *record;
    int cache_index;

    if((s_detail_index < 0) || (s_detail_index >= s_record_count))
    {
        return;
    }
    record = s_records[s_detail_index];
    (void)memset(s_alias_buffer, 0, sizeof(s_alias_buffer));
    cache_index = menu_wifi_detector_alias_cache_find_(record->bssid);
    if((cache_index >= 0) && (s_alias_cache[cache_index].alias[0] != '\0'))
    {
        (void)snprintf(s_alias_buffer, sizeof(s_alias_buffer), "%.23s",
                       s_alias_cache[cache_index].alias);
    }
    poom_ui_keyboard_init(&s_alias_keyboard,
                          s_alias_buffer,
                          sizeof(s_alias_buffer),
                          record->ssid);
    s_screen = MENU_WIFI_DETECTOR_SCREEN_ALIAS;
    poom_ui_keyboard_draw(&s_alias_keyboard);
}

static void menu_wifi_detector_accept_alias_(void)
{
    poom_wifi_detector_record_t *record;
    int cache_index;

    if((s_detail_index >= 0) && (s_detail_index < s_record_count))
    {
        record = s_records[s_detail_index];
        menu_wifi_detector_alias_save_(record->bssid, s_alias_buffer);
        cache_index = menu_wifi_detector_alias_cache_add_(record->bssid);
        if(cache_index >= 0)
        {
            (void)snprintf(s_alias_cache[cache_index].alias,
                           sizeof(s_alias_cache[cache_index].alias),
                           "%.23s",
                           s_alias_buffer);
        }
        (void)snprintf(s_names[s_detail_index],
                       sizeof(s_names[s_detail_index]),
                       "%.32s",
                       (s_alias_buffer[0] != '\0') ? s_alias_buffer : record->ssid);
    }
    s_screen = MENU_WIFI_DETECTOR_SCREEN_DETAIL;
    s_input_dirty = true;
}

static void menu_wifi_detector_return_to_ap_list_(void)
{
    esp_err_t status = poom_wifi_detector_monitor_discover_aps();

    if(status != ESP_OK)
    {
        menu_detector_view_draw_error("AP CLIENTS",
                                      "AP scan failed",
                                      esp_err_to_name(status));
        s_exit_requested = true;
        return;
    }
    s_ap_clients_selected = false;
    s_monitor_channel = 0U;
    s_record_count = 0;
    s_selected_index = 0;
    s_scroll = 0;
    s_detail_index = -1;
    s_last_monitor_refresh = 0U;
    menu_wifi_detector_reset_text_scroll_();
    s_input_dirty = true;
}

static void menu_wifi_detector_select_ap_(void)
{
    uint8_t bssid[6];
    uint8_t channel;
    esp_err_t status;

    if((s_selected_index < 0) || (s_selected_index >= s_record_count))
    {
        return;
    }
    channel = s_records[s_selected_index]->channel;
    (void)memcpy(bssid, s_records[s_selected_index]->bssid, sizeof(bssid));
    status = poom_wifi_detector_monitor_select_ap(channel, bssid);
    if(status != ESP_OK)
    {
        menu_detector_view_draw_error("AP CLIENTS",
                                      "AP select failed",
                                      esp_err_to_name(status));
        return;
    }

    s_ap_clients_selected = true;
    s_monitor_channel = channel;
    s_record_count = 0;
    s_selected_index = 0;
    s_scroll = 0;
    s_detail_index = -1;
    s_last_monitor_refresh = 0U;
    menu_wifi_detector_reset_text_scroll_();
    s_input_dirty = true;
}

static void menu_wifi_detector_cleanup_(void)
{
    const uint8_t token = 1U;

    s_active = false;
    s_scan_requested = false;
    s_scan_complete = false;
    s_scan_task = NULL;
    (void)poom_wifi_detector_stop();
    s_monitor_active = false;
    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button",
                                       menu_wifi_detector_button_cb_,
                                       "menu_wifi_detector");
        s_buttons_subscribed = false;
    }
    menu_wifi_detector_free_();
    s_task = NULL;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

static void menu_wifi_detector_task_(void *arg)
{
    TickType_t last_scan = 0U;
    (void)arg;

    while(s_active)
    {
        if(s_exit_requested)
        {
            if(s_scan_in_progress)
            {
                menu_wifi_detector_cancel_scan_();
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
                continue;
            }
            break;
        }

        if((s_screen == MENU_WIFI_DETECTOR_SCREEN_SCAN) ||
           (s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL))
        {
            size_t count = 0U;
            esp_err_t status;
            uint8_t completed_channel = 0U;
            uint8_t completed_target[6] = {0};
            bool use_monitor = menu_wifi_detector_uses_monitor_();
            TickType_t now = xTaskGetTickCount();
            TickType_t scan_interval =
                (s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL) ?
                pdMS_TO_TICKS(MENU_WIFI_DETECTOR_DETAIL_SCAN_MS) :
                pdMS_TO_TICKS(MENU_WIFI_DETECTOR_SCAN_MS);

            if(use_monitor && menu_wifi_detector_is_ap_clients_() &&
               !s_monitor_active)
            {
                s_scan_requested = false;
                status = poom_wifi_detector_monitor_start(
                    POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS,
                    NULL,
                    0U,
                    MENU_DETECTOR_VIEW_MAX_ITEMS);
                if(status != ESP_OK)
                {
                    menu_detector_view_draw_error("AP CLIENTS",
                                                  "Monitor start",
                                                  esp_err_to_name(status));
                    s_exit_requested = true;
                    continue;
                }
                s_monitor_active = true;
                s_monitor_channel = 0U;
                s_last_monitor_refresh = 0U;
            }

            if(use_monitor && s_monitor_active)
            {
                uint8_t desired_channel = 0U;
                const uint8_t *desired_target = NULL;

                if(!menu_wifi_detector_is_ap_clients_() &&
                   (s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL) &&
                   (s_detail_index >= 0) &&
                   (s_detail_index < s_record_count))
                {
                    desired_channel = s_records[s_detail_index]->channel;
                    desired_target = s_records[s_detail_index]->bssid;
                }
                if(!menu_wifi_detector_is_ap_clients_() &&
                   (desired_channel != s_monitor_channel))
                {
                    status = poom_wifi_detector_monitor_set_channel(desired_channel,
                                                                    desired_target);
                    if(status == ESP_OK)
                    {
                        s_monitor_channel = desired_channel;
                    }
                }

                if((s_last_monitor_refresh == 0U) ||
                   ((now - s_last_monitor_refresh) >=
                    pdMS_TO_TICKS(MENU_WIFI_DETECTOR_MONITOR_REFRESH_MS)))
                {
                    status = poom_wifi_detector_monitor_snapshot(
                        s_scan_records,
                        MENU_DETECTOR_VIEW_MAX_ITEMS,
                        &count);
                    if(status == ESP_OK)
                    {
                        s_last_monitor_refresh = now;
                        menu_wifi_detector_apply_scan_(count);
                    }
                    else
                    {
                        menu_detector_view_draw_error("WIFI DETECT",
                                                      "Monitor failed",
                                                      esp_err_to_name(status));
                        s_exit_requested = true;
                        continue;
                    }
                }
            }
            else if(menu_wifi_detector_take_scan_(&status,
                                                  &count,
                                                  &completed_channel,
                                                  completed_target))
            {
                last_scan = now;
                if(status != ESP_OK)
                {
                    menu_detector_view_draw_error("WIFI DETECT",
                                                  "Scan failed",
                                                  esp_err_to_name(status));
                    vTaskDelay(pdMS_TO_TICKS(1200U));
                    s_exit_requested = true;
                    continue;
                }
                if((completed_channel == 0U) &&
                   (s_screen == MENU_WIFI_DETECTOR_SCREEN_SCAN))
                {
                    menu_wifi_detector_apply_scan_(count);
                    if(use_monitor)
                    {
                        status = poom_wifi_detector_monitor_start(
                            (poom_wifi_detector_filter_t)s_filter_index,
                            s_records,
                            count,
                            MENU_DETECTOR_VIEW_MAX_ITEMS);
                        if(status != ESP_OK)
                        {
                            menu_detector_view_draw_error("WIFI DETECT",
                                                          "Monitor start",
                                                          esp_err_to_name(status));
                            s_exit_requested = true;
                            continue;
                        }
                        s_monitor_active = true;
                        s_monitor_channel = 0U;
                        s_last_monitor_refresh = now;
                    }
                }
                else
                {
                    if((completed_channel == 0U) &&
                       (s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL) &&
                       (s_detail_index >= 0) &&
                       (s_detail_index < s_record_count))
                    {
                        (void)memcpy(completed_target,
                                     s_records[s_detail_index]->bssid,
                                     sizeof(completed_target));
                    }
                    menu_wifi_detector_apply_target_scan_(count, completed_target);
                }
            }

            if(((s_screen == MENU_WIFI_DETECTOR_SCREEN_SCAN) ||
                (s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL)) &&
               (!use_monitor || !s_monitor_active) &&
               !s_scan_in_progress && !s_scan_complete &&
               (s_scan_requested || (last_scan == 0U) ||
                (!use_monitor && ((now - last_scan) >= scan_interval))))
            {
                uint8_t channel = 0U;
                const uint8_t *target_bssid = NULL;

                s_scan_requested = false;
                if((last_scan == 0U) && (s_record_count == 0))
                {
                    menu_detector_view_draw_waiting(poom_wifi_detector_filter_label(
                        (poom_wifi_detector_filter_t)s_filter_index));
                }
                if((s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL) &&
                   (s_detail_index >= 0) &&
                   (s_detail_index < s_record_count))
                {
                    channel = s_records[s_detail_index]->channel;
                    target_bssid = s_records[s_detail_index]->bssid;
                }
                status = menu_wifi_detector_start_scan_(channel, target_bssid);
                if(status != ESP_OK)
                {
                    menu_detector_view_draw_error("WIFI DETECT",
                                                  "Scan task failed",
                                                  esp_err_to_name(status));
                    vTaskDelay(pdMS_TO_TICKS(1200U));
                    s_exit_requested = true;
                    continue;
                }
            }
        }

        if(menu_wifi_detector_text_scroll_active_())
        {
            uint32_t scroll_slot =
                (menu_wifi_detector_now_ms_() - s_selection_tick_ms) /
                MENU_WIFI_DETECTOR_SCROLL_MS;
            if(scroll_slot != s_last_scroll_slot)
            {
                s_last_scroll_slot = scroll_slot;
                s_input_dirty = true;
            }
        }

        if(s_input_dirty)
        {
            s_input_dirty = false;
            menu_wifi_detector_render_();
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
    }

    menu_wifi_detector_cleanup_();
    vTaskDelete(NULL);
}

static void menu_wifi_detector_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    button_event_msg_t event;
    (void)user_ctx;

    if(!s_active || (msg == NULL) || (msg->len < sizeof(event)))
    {
        return;
    }
    (void)memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(s_screen == MENU_WIFI_DETECTOR_SCREEN_ALIAS)
    {
        poom_ui_keyboard_action_t action;
        if(event.button == BUTTON_B)
        {
            s_screen = MENU_WIFI_DETECTOR_SCREEN_DETAIL;
            s_input_dirty = true;
            return;
        }
        action = poom_ui_keyboard_handle_button(&s_alias_keyboard, event.button);
        if(action == POOM_UI_KEYBOARD_ACTION_ACCEPT)
        {
            menu_wifi_detector_accept_alias_();
        }
        else
        {
            poom_ui_keyboard_draw(&s_alias_keyboard);
        }
        return;
    }

    if(s_screen == MENU_WIFI_DETECTOR_SCREEN_SELECT)
    {
        if(event.button == BUTTON_B)
        {
            s_exit_requested = true;
            menu_wifi_detector_cancel_scan_();
        }
        else if((event.button == BUTTON_UP) && (s_filter_index > 0))
        {
            --s_filter_index;
            s_input_dirty = true;
        }
        else if((event.button == BUTTON_DOWN) &&
                (s_filter_index < (POOM_WIFI_DETECTOR_FILTER_COUNT - 1)))
        {
            ++s_filter_index;
            s_input_dirty = true;
        }
        else if(event.button == BUTTON_A)
        {
            s_screen = MENU_WIFI_DETECTOR_SCREEN_SCAN;
            s_ap_clients_selected = false;
            s_scan_requested = true;
            s_input_dirty = true;
        }
        return;
    }

    if(s_screen == MENU_WIFI_DETECTOR_SCREEN_DETAIL)
    {
        if(event.button == BUTTON_B)
        {
            s_screen = MENU_WIFI_DETECTOR_SCREEN_SCAN;
            s_detail_index = -1;
            menu_wifi_detector_reset_text_scroll_();
            s_input_dirty = true;
        }
        else if(event.button == BUTTON_A)
        {
            menu_wifi_detector_enter_alias_();
        }
        return;
    }

    if(s_record_count == 0)
    {
        if(event.button == BUTTON_B)
        {
            if(menu_wifi_detector_is_ap_clients_() && s_ap_clients_selected)
            {
                menu_wifi_detector_return_to_ap_list_();
            }
            else
            {
                s_exit_requested = true;
                menu_wifi_detector_cancel_scan_();
            }
        }
        return;
    }

    if(event.button == BUTTON_B)
    {
        if(menu_wifi_detector_is_ap_clients_() && s_ap_clients_selected)
        {
            menu_wifi_detector_return_to_ap_list_();
        }
        else
        {
            s_exit_requested = true;
            menu_wifi_detector_cancel_scan_();
        }
    }
    else if((event.button == BUTTON_UP) && (s_selected_index > 0))
    {
        --s_selected_index;
        menu_wifi_detector_reset_text_scroll_();
        s_input_dirty = true;
    }
    else if((event.button == BUTTON_DOWN) &&
            (s_selected_index < (s_record_count - 1)))
    {
        ++s_selected_index;
        menu_wifi_detector_reset_text_scroll_();
        s_input_dirty = true;
    }
    else if(event.button == BUTTON_A)
    {
        if(menu_wifi_detector_is_ap_clients_() && !s_ap_clients_selected)
        {
            menu_wifi_detector_select_ap_();
        }
        else
        {
            s_detail_index = s_selected_index;
            s_screen = MENU_WIFI_DETECTOR_SCREEN_DETAIL;
            s_input_dirty = true;
        }
    }
}

void menu_wifi_detector_show(void)
{
    if(s_active)
    {
        return;
    }

    if(!menu_wifi_detector_alloc_())
    {
        const uint8_t token = 1U;
        menu_detector_view_draw_error("WIFI DETECT", "No memory", "Try again");
        vTaskDelay(pdMS_TO_TICKS(1200U));
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }

    s_record_count = 0;
    s_selected_index = 0;
    s_scroll = 0;
    s_filter_index = 0;
    s_detail_index = -1;
    s_screen = MENU_WIFI_DETECTOR_SCREEN_SELECT;
    s_exit_requested = false;
    s_scan_requested = false;
    s_scan_in_progress = false;
    s_scan_complete = false;
    s_monitor_active = false;
    s_scan_status = ESP_OK;
    s_scan_count = 0U;
    s_scan_channel = 0U;
    (void)memset(s_scan_target_bssid, 0, sizeof(s_scan_target_bssid));
    s_scan_task = NULL;
    s_last_monitor_refresh = 0U;
    s_monitor_channel = 0U;
    s_ap_clients_selected = false;
    s_input_dirty = true;
    menu_wifi_detector_reset_text_scroll_();
    s_secrets_ready = (poom_secrets_init() == ESP_OK);

    if(!s_buttons_subscribed)
    {
        if(!poom_sbus_subscribe_cb("input/button",
                                   menu_wifi_detector_button_cb_,
                                   "menu_wifi_detector"))
        {
            const uint8_t token = 1U;
            menu_wifi_detector_free_();
            menu_detector_view_draw_error("WIFI DETECT", "Button setup", "failed");
            vTaskDelay(pdMS_TO_TICKS(1200U));
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
        s_buttons_subscribed = true;
    }

    s_active = true;
    if(xTaskCreate(menu_wifi_detector_task_,
                   "menu_wifi_detector",
                   MENU_WIFI_DETECTOR_TASK_STACK,
                   NULL,
                   MENU_WIFI_DETECTOR_TASK_PRIO,
                   &s_task) != pdPASS)
    {
        const uint8_t token = 1U;
        s_active = false;
        if(s_buttons_subscribed)
        {
            (void)poom_sbus_unsubscribe_cb("input/button",
                                           menu_wifi_detector_button_cb_,
                                           "menu_wifi_detector");
            s_buttons_subscribed = false;
        }
        menu_wifi_detector_free_();
        menu_detector_view_draw_error("WIFI DETECT", "Task create", "failed");
        vTaskDelay(pdMS_TO_TICKS(1200U));
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}
