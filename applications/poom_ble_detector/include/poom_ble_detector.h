// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_DETECTOR_H
#define POOM_BLE_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_BLE_DETECTOR_NAME_LEN (24U)
#define POOM_BLE_DETECTOR_TEXT_LEN (24U)
#define POOM_BLE_DETECTOR_ADV_LEN  (62U)

typedef enum
{
    POOM_BLE_DETECTOR_FILTER_DEVICES = 0,
    POOM_BLE_DETECTOR_FILTER_TRACKERS,
    POOM_BLE_DETECTOR_FILTER_WEARABLES,
    POOM_BLE_DETECTOR_FILTER_COUNT,
} poom_ble_detector_filter_t;

typedef enum
{
    POOM_BLE_DETECTOR_CLASS_UNKNOWN = 0,
    POOM_BLE_DETECTOR_CLASS_AIRTAG,
    POOM_BLE_DETECTOR_CLASS_SMARTTAG,
    POOM_BLE_DETECTOR_CLASS_TILE,
    POOM_BLE_DETECTOR_CLASS_OTHER_TRACKER,
    POOM_BLE_DETECTOR_CLASS_SMART_GLASSES,
    POOM_BLE_DETECTOR_CLASS_BODY_CAMERA,
} poom_ble_detector_class_t;

typedef enum
{
    POOM_DETECT_CONFIDENCE_INFO = 0,
    POOM_DETECT_CONFIDENCE_POSSIBLE,
    POOM_DETECT_CONFIDENCE_PROBABLE,
    POOM_DETECT_CONFIDENCE_HIGH,
} poom_detect_confidence_t;

typedef struct
{
    char name[POOM_BLE_DETECTOR_NAME_LEN];
    char vendor[POOM_BLE_DETECTOR_TEXT_LEN];
    uint8_t mac_address[6];
    int rssi;
    float distance_m;
    poom_ble_detector_class_t device_class;
    poom_detect_confidence_t confidence;
    uint8_t adv_data[POOM_BLE_DETECTOR_ADV_LEN];
    uint8_t adv_data_length;
} poom_ble_detector_record_t;

typedef void (*poom_ble_detector_cb_t)(const poom_ble_detector_record_t *record);

void poom_ble_detector_register_cb(poom_ble_detector_cb_t callback);
esp_err_t poom_ble_detector_start(poom_ble_detector_filter_t filter);
esp_err_t poom_ble_detector_stop(void);

bool poom_ble_detector_parse(const uint8_t *adv_data,
                             size_t adv_len,
                             const uint8_t mac_address[6],
                             int rssi,
                             poom_ble_detector_record_t *out_record);

bool poom_ble_detector_record_matches_filter(const poom_ble_detector_record_t *record,
                                             poom_ble_detector_filter_t filter);

const char *poom_ble_detector_filter_label(poom_ble_detector_filter_t filter);
const char *poom_ble_detector_class_label(poom_ble_detector_class_t device_class);
const char *poom_detect_confidence_label(poom_detect_confidence_t confidence);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BLE_DETECTOR_H */
