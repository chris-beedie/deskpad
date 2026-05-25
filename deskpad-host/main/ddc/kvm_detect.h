#pragma once

#include "esp_err.h"

// Externally-initiated KVM switch detection. Triggers:
//   1. RP2350 USB re-enumeration  — fired from hid_link USB-status callback
//      when the U38 swaps input and the downstream USB hub re-attaches to
//      a different upstream host.
//   2. Slow DDC poll on the U38   — every 5 s, read VCP 0x60 and compare
//      to config-known inputs. Belt-and-braces for switches that didn't
//      cleanly cycle the USB hub.
//
// On detect, looks up which configured Host owns the new U38 input,
// updates host_state, and drives the Secondary Monitor to follow.
//
// Must run after hid_link_init(), config_init(), and ddc_init().

esp_err_t kvm_detect_init(void);
