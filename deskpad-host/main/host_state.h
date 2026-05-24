#pragma once

#include "config.h"

// Shared "which Host is currently selected on the KVM" state, factored out
// of actions.c so renderers (e.g. monitor) can react to changes.
//
// Today the value is only updated when deskpad itself initiates a switch
// (kvm_select / kvm_toggle in actions.c). Phase 6 will add external-switch
// detection via the RP2350 USB-re-enumeration signal + DDC poll, which
// will also call host_state_set_active().

typedef void (*host_state_change_cb_t)(host_t new_active, void *user);

// Returns 0 (no valid host_t) until the first switch happens.
host_t host_state_get_active(void);

// Set the Active Host. No-op if already that value. Fires subscribers
// only on a true transition. Safe to call from any task.
void host_state_set_active(host_t h);

// Subscribe to active-host changes. Callback is invoked on the caller of
// host_state_set_active (typically the actions worker task), so keep it
// cheap — defer expensive work to your own task / queue.
// Up to HOST_STATE_MAX_SUBSCRIBERS callbacks; further calls silently drop.
#define HOST_STATE_MAX_SUBSCRIBERS 4
void host_state_subscribe(host_state_change_cb_t cb, void *user);
