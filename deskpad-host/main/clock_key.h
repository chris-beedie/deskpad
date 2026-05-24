#pragma once

#include "esp_err.h"

// Test live-key: shows "MM:SS" of minutes-since-boot on page 2, key 5.
// Will become a real wall-clock once NTP / Wi-Fi is in place.
esp_err_t clock_key_init(void);
