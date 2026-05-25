#include "system_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "system_status";

static status_entry_t   s_entries[MAX_STATUS_ENTRIES];
static int              s_entry_count;
static SemaphoreHandle_t s_mtx;

static struct {
    system_status_cb_t cb;
    void *user;
} s_subscribers[MAX_STATUS_SUBSCRIBERS];
static int s_subscriber_count;

const char *system_status_severity_str(severity_t s)
{
    switch (s) { case STATUS_OK: return "OK"; case STATUS_WARN: return "WARN"; default: return "ERROR"; }
}

esp_err_t system_status_init(void)
{
    if (s_mtx) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    return s_mtx ? ESP_OK : ESP_ERR_NO_MEM;
}

static int find_or_alloc(const char *id)
{
    for (int i = 0; i < s_entry_count; i++) {
        if (strncmp(s_entries[i].id, id, sizeof(s_entries[i].id)) == 0) return i;
    }
    if (s_entry_count >= MAX_STATUS_ENTRIES) return -1;
    int idx = s_entry_count++;
    strncpy(s_entries[idx].id, id, sizeof(s_entries[idx].id) - 1);
    s_entries[idx].id[sizeof(s_entries[idx].id) - 1] = 0;
    return idx;
}

void system_status_set(const char *id, severity_t sev, const char *fmt, ...)
{
    if (!id || !s_mtx) return;
    char msg[80];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
    } else {
        msg[0] = 0;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int i = find_or_alloc(id);
    if (i < 0) { xSemaphoreGive(s_mtx); return; }
    bool changed = (s_entries[i].severity != sev) ||
                   (strncmp(s_entries[i].message, msg, sizeof(s_entries[i].message)) != 0);
    s_entries[i].severity = sev;
    strncpy(s_entries[i].message, msg, sizeof(s_entries[i].message) - 1);
    s_entries[i].message[sizeof(s_entries[i].message) - 1] = 0;
    status_entry_t snapshot = s_entries[i];
    xSemaphoreGive(s_mtx);

    if (changed) {
        ESP_LOGI(TAG, "%s: %s — %s", id, system_status_severity_str(sev), snapshot.message);
        for (int s = 0; s < s_subscriber_count; s++) {
            s_subscribers[s].cb(&snapshot, s_subscribers[s].user);
        }
    }
}

size_t system_status_get_all(status_entry_t *out, size_t max)
{
    if (!out || !s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    size_t n = (size_t)s_entry_count < max ? (size_t)s_entry_count : max;
    memcpy(out, s_entries, n * sizeof(status_entry_t));
    xSemaphoreGive(s_mtx);
    return n;
}

severity_t system_status_worst(void)
{
    severity_t worst = STATUS_OK;
    if (!s_mtx) return worst;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < s_entry_count; i++) {
        if (s_entries[i].severity > worst) worst = s_entries[i].severity;
    }
    xSemaphoreGive(s_mtx);
    return worst;
}

void system_status_subscribe(system_status_cb_t cb, void *user)
{
    if (!cb || s_subscriber_count >= MAX_STATUS_SUBSCRIBERS) return;
    s_subscribers[s_subscriber_count].cb = cb;
    s_subscribers[s_subscriber_count].user = user;
    s_subscriber_count++;
}
