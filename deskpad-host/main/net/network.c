#include "network.h"

#include <stdio.h>
#include <string.h>

#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "system_status.h"

static const char *TAG = "network";

// Waveshare ESP32-P4-NANO Ethernet pinout (IP101GRI on RMII).
// Sources: Waveshare wiki / schematic. Verify against your board if the
// PHY never links up.
//
// Clock direction: the PHY (with its own 25 MHz crystal) generates the
// 50 MHz RMII reference clock and drives it INTO the ESP on GPIO50.
// We don't generate it ourselves because the P4's MPLL is owned by
// PSRAM at 400 MHz (CONFIG_SPIRAM_SPEED_200M) — see espressif/esp-idf
// issue #18377.
#define ETH_PHY_ADDR     1
#define ETH_PHY_RST_GPIO 51
#define ETH_MDC_GPIO     31
#define ETH_MDIO_GPIO    52
#define ETH_RMII_CLK_GPIO 50  // 50 MHz input from PHY's REF_CLK

#define MDNS_HOSTNAME    "deskpad"
#define MDNS_INSTANCE    "Deskpad"

#define IP_BIT  BIT0

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint8_t mac[6];
    esp_eth_handle_t h = *(esp_eth_handle_t *)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(h, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "link up — MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        system_status_set("network", STATUS_WARN, "link up, awaiting IP");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        system_status_set("network", STATUS_ERROR, "link down");
        xEventGroupClearBits(s_events, IP_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "driver started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "driver stopped");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_ETH_GOT_IP) return;
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got IP " IPSTR " mask " IPSTR " gw " IPSTR,
             IP2STR(&evt->ip_info.ip),
             IP2STR(&evt->ip_info.netmask),
             IP2STR(&evt->ip_info.gw));
    char ipbuf[24];
    snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&evt->ip_info.ip));
    system_status_set("network", STATUS_OK, "%s", ipbuf);
    xEventGroupSetBits(s_events, IP_BIT);
}

static esp_err_t mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    err = mdns_hostname_set(MDNS_HOSTNAME);
    if (err != ESP_OK) return err;
    err = mdns_instance_name_set(MDNS_INSTANCE);
    if (err != ESP_OK) return err;
    // HTTP service announce — actual server is started by http_server_start().
    return mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t network_init(void)
{
    if (s_events) return ESP_OK;
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&cfg);

    // MAC: stock ESP32-P4 EMAC. PHY provides the 50 MHz RMII clock; the ESP
    // receives it on GPIO50. This avoids a conflict with PSRAM over MPLL.
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &handle));

    // Attach driver to netif glue layer.
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL));

    esp_err_t err = mdns_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "mdns init: %s — continuing without", esp_err_to_name(err));

    return esp_eth_start(handle);
}

esp_err_t network_wait_for_ip(uint32_t timeout_ms)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    EventBits_t got = xEventGroupWaitBits(s_events, IP_BIT, pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(timeout_ms));
    return (got & IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
