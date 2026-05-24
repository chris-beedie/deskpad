#pragma once

#include "esp_err.h"

// Brings up the wired Ethernet (P4-NANO has an IP101GRI PHY on RMII),
// requests an IPv4 address over DHCP, and announces `deskpad.local` via
// mDNS. Non-blocking — returns ESP_OK as soon as the driver is started.
// Use network_wait_for_ip() to block until an address has been assigned.
//
// Must run after nvs_flash_init() (which config_init() handles).
esp_err_t network_init(void);

// Blocks until the Ethernet interface has a routable IPv4 address, or
// the supplied timeout elapses. Returns ESP_OK if connected, ESP_ERR_TIMEOUT
// otherwise.
esp_err_t network_wait_for_ip(uint32_t timeout_ms);
