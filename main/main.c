#include "bsp_extra.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "scopebuddy_game.h"
#include "web_server.h"
#include <string.h>

#define MAIN_TAG "MAIN"

#ifndef CONFIG_SCOPEBUDDY_WIFI_SSID
#define CONFIG_SCOPEBUDDY_WIFI_SSID "HeadlessScopeBuddy"
#endif
#ifndef CONFIG_SCOPEBUDDY_WIFI_PASSWORD
#define CONFIG_SCOPEBUDDY_WIFI_PASSWORD "scopebuddy"
#endif
#ifndef CONFIG_SCOPEBUDDY_WIFI_CHANNEL
#define CONFIG_SCOPEBUDDY_WIFI_CHANNEL 6
#endif

static void init_fail_handler(const char *module_name, esp_err_t err)
{
    while (1) {
        ESP_LOGE(MAIN_TAG, "[%s] init failed: %s", module_name, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t wifi_ap_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), MAIN_TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), MAIN_TAG, "event loop failed");
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), MAIN_TAG, "wifi init failed");

    wifi_config_t ap_config = {
        .ap = {
            .ssid = CONFIG_SCOPEBUDDY_WIFI_SSID,
            .ssid_len = strlen(CONFIG_SCOPEBUDDY_WIFI_SSID),
            .channel = CONFIG_SCOPEBUDDY_WIFI_CHANNEL,
            .password = CONFIG_SCOPEBUDDY_WIFI_PASSWORD,
            .max_connection = 4,
            .authmode = strlen(CONFIG_SCOPEBUDDY_WIFI_PASSWORD) == 0 ?
                WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), MAIN_TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), MAIN_TAG, "wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), MAIN_TAG, "wifi start failed");

    ESP_LOGI(MAIN_TAG, "WLAN-Zugangspunkt '%s' aktiv (Kanal %d)",
             CONFIG_SCOPEBUDDY_WIFI_SSID, CONFIG_SCOPEBUDDY_WIFI_CHANNEL);
    return ESP_OK;
}

static void mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("headlessscopebuddy");
    mdns_instance_name_set("HeadlessScopeBuddy Oszilloskop-Trainer");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(MAIN_TAG, "Erreichbar unter http://headlessscopebuddy.local/ und http://192.168.4.1/");
}

void app_main(void)
{
    ESP_LOGI(MAIN_TAG, "HeadlessScopeBuddy startet...");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_err = nvs_flash_erase();
        if (nvs_err == ESP_OK) nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "NVS nicht verfügbar, Einstellungen verwenden Standardwerte: %s",
                 esp_err_to_name(nvs_err));
    }

    esp_err_t err = wifi_ap_start();
    if (err != ESP_OK) init_fail_handler("WLAN", err);
    mdns_start();

    err = gpio_wave_init(1000, 50);
    if (err == ESP_OK) err = gpio_wave_stop();
    bool wave_hardware_ready = (err == ESP_OK);
    if (!wave_hardware_ready) {
        ESP_LOGE(MAIN_TAG, "Signalgenerator nicht verfügbar: %s", esp_err_to_name(err));
    }

    scopebuddy_game_init();
    scopebuddy_game_set_hardware_ready(wave_hardware_ready);

    err = web_server_start();
    if (err != ESP_OK) init_fail_handler("HTTP-Server", err);

    ESP_LOGI(MAIN_TAG, "HeadlessScopeBuddy bereit.");
}
