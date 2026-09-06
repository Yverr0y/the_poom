// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#include "poom_ble_detector.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_gap_ble_api.h"
#include "poom_ble_scan.h"

#define POOM_BLE_DETECTOR_RSSI_AT_1M_DBM (-65.0f)
#define POOM_BLE_DETECTOR_PATH_LOSS       (2.2f)
static poom_ble_detector_cb_t volatile s_callback;
static poom_ble_detector_filter_t s_filter = POOM_BLE_DETECTOR_FILTER_DEVICES;
static volatile bool s_active;

static void poom_ble_detector_copy_text_(char *out, size_t out_len, const char *text)
{
    size_t copy_len;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }

    if(text == NULL)
    {
        out[0] = '\0';
        return;
    }

    copy_len = strnlen(text, out_len - 1U);
    (void)memcpy(out, text, copy_len);
    out[copy_len] = '\0';
}

static bool poom_ble_detector_contains_ci_(const char *text, const char *needle)
{
    size_t i;

    if((text == NULL) || (needle == NULL) || (needle[0] == '\0'))
    {
        return false;
    }

    for(i = 0U; text[i] != '\0'; ++i)
    {
        size_t j = 0U;
        while((needle[j] != '\0') && (text[i + j] != '\0') &&
              (tolower((unsigned char)text[i + j]) == tolower((unsigned char)needle[j])))
        {
            ++j;
        }
        if(needle[j] == '\0')
        {
            return true;
        }
    }

    return false;
}

static void poom_ble_detector_set_match_(poom_ble_detector_record_t *record,
                                         poom_ble_detector_class_t device_class,
                                         poom_detect_confidence_t confidence,
                                         const char *canonical_name,
                                         const char *vendor)
{
    if((record == NULL) || (confidence < record->confidence))
    {
        return;
    }

    if((confidence == record->confidence) &&
       (record->device_class != POOM_BLE_DETECTOR_CLASS_UNKNOWN))
    {
        return;
    }

    record->device_class = device_class;
    record->confidence = confidence;
    if((record->name[0] == '\0') && (canonical_name != NULL))
    {
        poom_ble_detector_copy_text_(record->name, sizeof(record->name), canonical_name);
    }
    poom_ble_detector_copy_text_(record->vendor, sizeof(record->vendor), vendor);
}

static void poom_ble_detector_match_uuid_(poom_ble_detector_record_t *record, uint16_t uuid)
{
    switch(uuid)
    {
        case 0xFD5AU:
            poom_ble_detector_set_match_(record,
                                         POOM_BLE_DETECTOR_CLASS_SMARTTAG,
                                         POOM_DETECT_CONFIDENCE_HIGH,
                                         "SmartTag",
                                         "Samsung");
            break;
        case 0xFEEDU:
        case 0xFEECU:
            poom_ble_detector_set_match_(record,
                                         POOM_BLE_DETECTOR_CLASS_TILE,
                                         POOM_DETECT_CONFIDENCE_HIGH,
                                         "Tile",
                                         "Tile");
            break;
        case 0xFE2CU:
            poom_ble_detector_set_match_(record,
                                         POOM_BLE_DETECTOR_CLASS_OTHER_TRACKER,
                                         POOM_DETECT_CONFIDENCE_PROBABLE,
                                         "Find My tag",
                                         "Google network");
            break;
        case 0xFD5FU:
            poom_ble_detector_set_match_(record,
                                         POOM_BLE_DETECTOR_CLASS_SMART_GLASSES,
                                         POOM_DETECT_CONFIDENCE_HIGH,
                                         "Ray-Ban Meta",
                                         "Meta");
            break;
        case 0xFE07U:
            poom_ble_detector_set_match_(record,
                                         POOM_BLE_DETECTOR_CLASS_SMART_GLASSES,
                                         POOM_DETECT_CONFIDENCE_PROBABLE,
                                         "Snap Spectacles",
                                         "Snap");
            break;
        default:
            break;
    }
}

static void poom_ble_detector_match_manufacturer_(poom_ble_detector_record_t *record,
                                                   const uint8_t *data,
                                                   size_t data_len)
{
    uint16_t company;

    if((record == NULL) || (data == NULL) || (data_len < 2U))
    {
        return;
    }

    company = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
    if((company == 0x004CU) && (data_len >= 3U) &&
       ((data[2] == 0x12U) || (data[2] == 0x1EU)))
    {
        poom_ble_detector_set_match_(record,
                                     POOM_BLE_DETECTOR_CLASS_AIRTAG,
                                     POOM_DETECT_CONFIDENCE_HIGH,
                                     "AirTag",
                                     "Apple");
    }
    else if(company == 0x0075U)
    {
        poom_ble_detector_set_match_(record,
                                     POOM_BLE_DETECTOR_CLASS_SMARTTAG,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     "Samsung device",
                                     "Samsung");
    }
    else if(company == 0x00C7U)
    {
        poom_ble_detector_set_match_(record,
                                     POOM_BLE_DETECTOR_CLASS_TILE,
                                     POOM_DETECT_CONFIDENCE_PROBABLE,
                                     "Tile",
                                     "Tile");
    }
}

static void poom_ble_detector_match_name_(poom_ble_detector_record_t *record)
{
    const char *name;

    if(record == NULL)
    {
        return;
    }

    name = record->name;
    if(poom_ble_detector_contains_ci_(name, "airtag"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_AIRTAG,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Apple");
    }
    else if(poom_ble_detector_contains_ci_(name, "smarttag"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_SMARTTAG,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Samsung");
    }
    else if(poom_ble_detector_contains_ci_(name, "tile"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_TILE,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Tile");
    }
    else if(poom_ble_detector_contains_ci_(name, "chipolo") ||
            poom_ble_detector_contains_ci_(name, "pebblebee") ||
            poom_ble_detector_contains_ci_(name, "smarttrack"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_OTHER_TRACKER,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Find My tag");
    }

    if(poom_ble_detector_contains_ci_(name, "ray-ban") ||
       poom_ble_detector_contains_ci_(name, "rayban") ||
       poom_ble_detector_contains_ci_(name, "stories"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_SMART_GLASSES,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Meta");
    }
    else if(poom_ble_detector_contains_ci_(name, "spectacles"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_SMART_GLASSES,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Snap");
    }

    if(poom_ble_detector_contains_ci_(name, "axon") ||
       poom_ble_detector_contains_ci_(name, "body 2") ||
       poom_ble_detector_contains_ci_(name, "body 3"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_BODY_CAMERA,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Axon");
    }
    else if(poom_ble_detector_contains_ci_(name, "v300") ||
            poom_ble_detector_contains_ci_(name, "watchguard"))
    {
        poom_ble_detector_set_match_(record, POOM_BLE_DETECTOR_CLASS_BODY_CAMERA,
                                     POOM_DETECT_CONFIDENCE_POSSIBLE,
                                     NULL, "Body camera");
    }

}

bool poom_ble_detector_parse(const uint8_t *adv_data,
                             size_t adv_len,
                             const uint8_t mac_address[6],
                             int rssi,
                             poom_ble_detector_record_t *out_record)
{
    size_t offset = 0U;

    if((adv_data == NULL) || (mac_address == NULL) || (out_record == NULL))
    {
        return false;
    }

    (void)memset(out_record, 0, sizeof(*out_record));
    (void)memcpy(out_record->mac_address, mac_address, sizeof(out_record->mac_address));
    out_record->rssi = rssi;
    out_record->distance_m = powf(10.0f,
                                  (POOM_BLE_DETECTOR_RSSI_AT_1M_DBM - (float)rssi) /
                                  (10.0f * POOM_BLE_DETECTOR_PATH_LOSS));
    out_record->device_class = POOM_BLE_DETECTOR_CLASS_UNKNOWN;
    out_record->confidence = POOM_DETECT_CONFIDENCE_INFO;
    poom_ble_detector_copy_text_(out_record->vendor, sizeof(out_record->vendor), "Unknown");

    if(adv_len > sizeof(out_record->adv_data))
    {
        adv_len = sizeof(out_record->adv_data);
    }
    out_record->adv_data_length = (uint8_t)adv_len;
    (void)memcpy(out_record->adv_data, adv_data, adv_len);

    while(offset < adv_len)
    {
        uint8_t field_len = adv_data[offset];
        uint8_t ad_type;
        const uint8_t *data;
        size_t data_len;
        size_t next;

        if(field_len == 0U)
        {
            break;
        }

        next = offset + (size_t)field_len + 1U;
        if((field_len < 1U) || (next > adv_len))
        {
            break;
        }

        ad_type = adv_data[offset + 1U];
        data = &adv_data[offset + 2U];
        data_len = (size_t)field_len - 1U;

        if((ad_type == ESP_BLE_AD_TYPE_NAME_SHORT) || (ad_type == ESP_BLE_AD_TYPE_NAME_CMPL))
        {
            size_t copy_len = data_len;
            if(copy_len >= sizeof(out_record->name))
            {
                copy_len = sizeof(out_record->name) - 1U;
            }
            (void)memcpy(out_record->name, data, copy_len);
            out_record->name[copy_len] = '\0';
        }
        else if(ad_type == ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE)
        {
            poom_ble_detector_match_manufacturer_(out_record, data, data_len);
        }
        else if((ad_type == ESP_BLE_AD_TYPE_16SRV_PART) ||
                (ad_type == ESP_BLE_AD_TYPE_16SRV_CMPL))
        {
            size_t uuid_offset;
            for(uuid_offset = 0U; (uuid_offset + 1U) < data_len; uuid_offset += 2U)
            {
                uint16_t uuid = (uint16_t)data[uuid_offset] |
                                ((uint16_t)data[uuid_offset + 1U] << 8U);
                poom_ble_detector_match_uuid_(out_record, uuid);
            }
        }
        else if((ad_type == ESP_BLE_AD_TYPE_SERVICE_DATA) && (data_len >= 2U))
        {
            uint16_t uuid = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
            poom_ble_detector_match_uuid_(out_record, uuid);
        }

        offset = next;
    }

    if(out_record->name[0] == '\0')
    {
        poom_ble_detector_copy_text_(out_record->name, sizeof(out_record->name), "UNKNOWN");
    }

    poom_ble_detector_match_name_(out_record);

    if((out_record->mac_address[0] == 0x00U) &&
       (out_record->mac_address[1] == 0x25U) &&
       (out_record->mac_address[2] == 0xDFU))
    {
        poom_ble_detector_set_match_(out_record,
                                     POOM_BLE_DETECTOR_CLASS_BODY_CAMERA,
                                     POOM_DETECT_CONFIDENCE_PROBABLE,
                                     "Axon device",
                                     "Axon");
    }

    return true;
}

bool poom_ble_detector_record_matches_filter(const poom_ble_detector_record_t *record,
                                             poom_ble_detector_filter_t filter)
{
    if(record == NULL)
    {
        return false;
    }

    switch(filter)
    {
        case POOM_BLE_DETECTOR_FILTER_DEVICES:
            return true;
        case POOM_BLE_DETECTOR_FILTER_TRACKERS:
            return (record->device_class == POOM_BLE_DETECTOR_CLASS_AIRTAG) ||
                   (record->device_class == POOM_BLE_DETECTOR_CLASS_SMARTTAG) ||
                   (record->device_class == POOM_BLE_DETECTOR_CLASS_TILE) ||
                   (record->device_class == POOM_BLE_DETECTOR_CLASS_OTHER_TRACKER);
        case POOM_BLE_DETECTOR_FILTER_WEARABLES:
            return (record->device_class == POOM_BLE_DETECTOR_CLASS_SMART_GLASSES) ||
                   (record->device_class == POOM_BLE_DETECTOR_CLASS_BODY_CAMERA);
        default:
            return false;
    }
}

static void poom_ble_detector_scan_cb_(const esp_ble_gap_cb_param_t *scan_result)
{
    poom_ble_detector_cb_t callback = s_callback;
    poom_ble_detector_record_t record;
    size_t adv_len;

    if((scan_result == NULL) || !s_active || (callback == NULL))
    {
        return;
    }

    adv_len = (size_t)scan_result->scan_rst.adv_data_len +
              (size_t)scan_result->scan_rst.scan_rsp_len;
    if(adv_len > sizeof(scan_result->scan_rst.ble_adv))
    {
        adv_len = sizeof(scan_result->scan_rst.ble_adv);
    }

    if(poom_ble_detector_parse(scan_result->scan_rst.ble_adv,
                               adv_len,
                               scan_result->scan_rst.bda,
                               scan_result->scan_rst.rssi,
                               &record) &&
       poom_ble_detector_record_matches_filter(&record, s_filter))
    {
        callback(&record);
    }
}

void poom_ble_detector_register_cb(poom_ble_detector_cb_t callback)
{
    s_callback = callback;
}

esp_err_t poom_ble_detector_start(poom_ble_detector_filter_t filter)
{
    esp_err_t status;

    if(filter >= POOM_BLE_DETECTOR_FILTER_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(s_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_filter = filter;
    poom_ble_scan_set_scan_type(BLE_SCAN_TYPE_PASSIVE);
    poom_ble_scan_set_filter_type(BLE_SCAN_FILTER_ALLOW_ALL);
    poom_ble_scan_set_uart_forward_enabled(false);
    poom_ble_scan_register_cb(poom_ble_detector_scan_cb_);
    s_active = true;

    status = poom_ble_scan_start();
    if(status != ESP_OK)
    {
        s_active = false;
        poom_ble_scan_register_cb(NULL);
    }

    return status;
}

esp_err_t poom_ble_detector_stop(void)
{
    esp_err_t status;

    s_active = false;
    poom_ble_scan_register_cb(NULL);
    status = poom_ble_scan_stop();
    return status;
}

const char *poom_ble_detector_filter_label(poom_ble_detector_filter_t filter)
{
    switch(filter)
    {
        case POOM_BLE_DETECTOR_FILTER_DEVICES: return "BLE DEVICES";
        case POOM_BLE_DETECTOR_FILTER_TRACKERS: return "TRACKERS";
        case POOM_BLE_DETECTOR_FILTER_WEARABLES: return "WEARABLES";
        default: return "BLE DETECT";
    }
}

const char *poom_ble_detector_class_label(poom_ble_detector_class_t device_class)
{
    switch(device_class)
    {
        case POOM_BLE_DETECTOR_CLASS_AIRTAG: return "AirTag";
        case POOM_BLE_DETECTOR_CLASS_SMARTTAG: return "SmartTag";
        case POOM_BLE_DETECTOR_CLASS_TILE: return "Tile";
        case POOM_BLE_DETECTOR_CLASS_OTHER_TRACKER: return "Tracker";
        case POOM_BLE_DETECTOR_CLASS_SMART_GLASSES: return "Smart glasses";
        case POOM_BLE_DETECTOR_CLASS_BODY_CAMERA: return "Body camera";
        default: return "BLE device";
    }
}

const char *poom_detect_confidence_label(poom_detect_confidence_t confidence)
{
    switch(confidence)
    {
        case POOM_DETECT_CONFIDENCE_POSSIBLE: return "POSSIBLE";
        case POOM_DETECT_CONFIDENCE_PROBABLE: return "PROBABLE";
        case POOM_DETECT_CONFIDENCE_HIGH: return "HIGH";
        default: return "INFO";
    }
}
