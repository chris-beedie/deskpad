#include "host_state.h"

#include "esp_log.h"

static const char *TAG = "host_state";

static host_t s_active = 0;

static struct {
    host_state_change_cb_t cb;
    void *user;
} s_subscribers[HOST_STATE_MAX_SUBSCRIBERS];
static int s_subscriber_count;

host_t host_state_get_active(void) { return s_active; }

void host_state_set_active(host_t h)
{
    if (h == s_active) return;
    s_active = h;
    for (int i = 0; i < s_subscriber_count; i++) {
        s_subscribers[i].cb(h, s_subscribers[i].user);
    }
}

void host_state_subscribe(host_state_change_cb_t cb, void *user)
{
    if (!cb) return;
    if (s_subscriber_count >= HOST_STATE_MAX_SUBSCRIBERS) {
        ESP_LOGW(TAG, "subscriber table full (%d) — dropping callback",
                 HOST_STATE_MAX_SUBSCRIBERS);
        return;
    }
    s_subscribers[s_subscriber_count].cb   = cb;
    s_subscribers[s_subscriber_count].user = user;
    s_subscriber_count++;
}
