#include "bsp_extra.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "scopebuddy_game.h"
#include "web_server.h"
#include <string.h>

#define MAIN_TAG "MAIN"

/* Fallback-Standardwerte, falls im NVS noch nichts hinterlegt ist */
#ifndef CONFIG_SCOPEBUDDY_WIFI_SSID
#define CONFIG_SCOPEBUDDY_WIFI_SSID ""
#endif
#ifndef CONFIG_SCOPEBUDDY_WIFI_PASSWORD
#define CONFIG_SCOPEBUDDY_WIFI_PASSWORD ""
#endif

/* Fallback-Einstellungen für den eigenen Access Point */
#define FALLBACK_AP_SSID        "HeadlessScopeBuddy"
#define FALLBACK_AP_PASSWORD    "scopebuddy"
#ifndef CONFIG_SCOPEBUDDY_WIFI_CHANNEL
#define CONFIG_SCOPEBUDDY_WIFI_CHANNEL 6
#endif

#define NVS_WIFI_NAMESPACE      "wifi_cfg"
#define WIFI_STA_MAX_RETRY      5
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_fallback_active = false;
static char s_sta_ssid[33] = {0};
static char s_sta_password[65] = {0};

static void init_fail_handler(const char *module_name, esp_err_t err)
{
    while (1) {
        ESP_LOGE(MAIN_TAG, "[%s] init failed: %s", module_name, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Lädt SSID und Passwort aus NVS (oder nutzt menuconfig-Werte als Standard) */
static void wifi_load_credentials(void)
{
    strncpy(s_sta_ssid, CONFIG_SCOPEBUDDY_WIFI_SSID, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    strncpy(s_sta_password, CONFIG_SCOPEBUDDY_WIFI_PASSWORD, sizeof(s_sta_password) - 1);
    s_sta_password[sizeof(s_sta_password) - 1] = '\0';

    nvs_handle_t nvs;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_len = sizeof(s_sta_ssid);
        size_t pass_len = sizeof(s_sta_password);
        nvs_get_str(nvs, "ssid", s_sta_ssid, &ssid_len);
        nvs_get_str(nvs, "pass", s_sta_password, &pass_len);
        nvs_close(nvs);
        ESP_LOGI(MAIN_TAG, "WLAN-Daten aus NVS geladen (SSID: '%s')", s_sta_ssid);
    }
}

/* Wird von web_server.c aufgerufen, um die aktuelle SSID im Formular anzuzeigen */
void wifi_get_configured_ssid(char *out_ssid, size_t max_len)
{
    if (out_ssid == NULL || max_len == 0) return;
    strncpy(out_ssid, s_sta_ssid, max_len - 1);
    out_ssid[max_len - 1] = '\0';
}

/* Wird von web_server.c aufgerufen, um neue WLAN-Daten zu speichern und neu zu starten */
esp_err_t wifi_save_credentials_and_restart(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs),
                        MAIN_TAG, "NVS open failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "ssid", ssid), MAIN_TAG, "NVS set ssid failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "pass", password ? password : ""),
                        MAIN_TAG, "NVS set pass failed");
    ESP_RETURN_ON_ERROR(nvs_commit(nvs), MAIN_TAG, "NVS commit failed");
    nvs_close(nvs);

    ESP_LOGI(MAIN_TAG, "WLAN-Einstellungen aktualisiert ('%s'). Starte ESP32 neu...", ssid);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (s_fallback_active) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_STA_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(MAIN_TAG, "Verbindung zu '%s' fehlgeschlagen, Versuch %d/%d...",
                     s_sta_ssid, s_retry_num, WIFI_STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_num = 0;
        ESP_LOGI(MAIN_TAG, "Verbunden! DHCP IP-Adresse: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(MAIN_TAG, "Erreichbar unter http://headlessscopebuddy.local und http://" IPSTR "/",
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_fallback_to_ap(void)
{
    s_fallback_active = true;
    ESP_LOGW(MAIN_TAG, "Starte Fallback-AP '%s'...", FALLBACK_AP_SSID);

    esp_wifi_stop();

    wifi_config_t ap_config = {
        .ap = {
            .ssid = FALLBACK_AP_SSID,
            .ssid_len = strlen(FALLBACK_AP_SSID),
            .channel = CONFIG_SCOPEBUDDY_WIFI_CHANNEL,
            .password = FALLBACK_AP_PASSWORD,
            .max_connection = 4,
            .authmode = strlen(FALLBACK_AP_PASSWORD) == 0 ?
                WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), MAIN_TAG, "wifi ap mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), MAIN_TAG, "wifi ap config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), MAIN_TAG, "wifi ap start failed");

    ESP_LOGI(MAIN_TAG, "Fallback-AP '%s' aktiv (Kanal %d) -> http://headlessscopebuddy.local und http://192.168.4.1",
             FALLBACK_AP_SSID, CONFIG_SCOPEBUDDY_WIFI_CHANNEL);
    return ESP_OK;
}

static esp_err_t wifi_start_with_fallback(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), MAIN_TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), MAIN_TAG, "event loop failed");

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), MAIN_TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        MAIN_TAG, "wifi event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        MAIN_TAG, "ip event register failed");

    wifi_load_credentials();

    /* Wenn noch keine STA-SSID konfiguriert ist (oder noch der AP-Standardname drin steht), direkt AP starten */
    if (strlen(s_sta_ssid) == 0 || strcmp(s_sta_ssid, FALLBACK_AP_SSID) == 0) {
        return wifi_fallback_to_ap();
    }

    wifi_config_t sta_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strncpy((char *)sta_config.sta.ssid, s_sta_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, s_sta_password, sizeof(sta_config.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), MAIN_TAG, "wifi sta mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), MAIN_TAG, "wifi sta config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), MAIN_TAG, "wifi start failed");

    ESP_LOGI(MAIN_TAG, "Verbinde mit WLAN '%s' (WPA2/DHCP, automatische Kanalwahl)...", s_sta_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    return wifi_fallback_to_ap();
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

    esp_err_t err = wifi_start_with_fallback();
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

    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    err = web_server_start();
    if (err != ESP_OK) init_fail_handler("HTTP-Server", err);

    ESP_LOGI(MAIN_TAG, "HeadlessScopeBuddy bereit.");
}
