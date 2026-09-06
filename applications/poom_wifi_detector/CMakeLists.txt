# SPDX-License-Identifier: GPL-3.0-or-later

set(srcs "poom_wifi_detector.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support esp_timer esp_wifi poom_wifi_ctrl poom_wifi_scanner
)
