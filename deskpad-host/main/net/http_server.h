#pragma once

#include "esp_err.h"

// Starts the on-device HTTP server (port 80). Endpoints:
//   GET  /                — embedded SPA (deskpad config UI)
//   GET  /api/config      — current binding config as JSON
//   PUT  /api/config      — replace binding config with request body
//   GET  /api/enums       — vocabularies for SPA dropdowns
//                            (scopes, hosts, renderers, binding_types)
//   POST /api/ota         — stream new firmware into the inactive OTA slot
//   POST /api/reboot      — schedule an esp_restart() ~500 ms in the future
//
// Must run after network_init() — needs the netif up and the default event
// loop created. config_init() must also have run.
esp_err_t http_server_start(void);
