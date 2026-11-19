#include "web_server.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "scopebuddy_game.h"
#include <stdbool.h>
#include <string.h>

#define WEB_TAG "SCOPEBUDDY_WEB"
#define MAX_BODY_SIZE 512

extern const uint8_t web_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t lang_de_start[] asm("_binary_de_json_start");
extern const uint8_t lang_de_end[]   asm("_binary_de_json_end");
extern const uint8_t lang_en_start[] asm("_binary_en_json_start");
extern const uint8_t lang_en_end[]   asm("_binary_en_json_end");
extern const uint8_t favicon_svg_start[] asm("_binary_favicon_svg_start");
extern const uint8_t favicon_svg_end[]   asm("_binary_favicon_svg_end");

static esp_err_t favicon_handler(httpd_req_t *req)
{
    size_t len = favicon_svg_end - favicon_svg_start;
    while (len > 0 && favicon_svg_start[len - 1] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, "image/svg+xml");
    return httpd_resp_send(req, (const char *)favicon_svg_start, len);
}

/* Funktionen aus main.c zum Lesen/Speichern der WLAN-Daten im NVS */
extern esp_err_t wifi_save_credentials_and_restart(const char *ssid, const char *password);
extern void wifi_get_configured_ssid(char *out_ssid, size_t max_len);

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t send_state(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    scopebuddy_game_build_state(root);
    return send_json(req, root);
}

static esp_err_t read_body_json(httpd_req_t *req, cJSON **out_json)
{
    *out_json = NULL;
    if (req->content_len == 0 || req->content_len >= MAX_BODY_SIZE) return ESP_OK;

    char buffer[MAX_BODY_SIZE];
    int received = httpd_req_recv(req, buffer, req->content_len);
    if (received <= 0) return ESP_FAIL;
    buffer[received] = '\0';

    *out_json = cJSON_Parse(buffer);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)web_index_html_start,
                            web_index_html_end - web_index_html_start);
}

static esp_err_t lang_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (strcmp(req->uri, "/lang/en.json") == 0) {
        return httpd_resp_send(req, (const char *)lang_en_start, lang_en_end - lang_en_start);
    }
    return httpd_resp_send(req, (const char *)lang_de_start, lang_de_end - lang_de_start);
}

static esp_err_t lessons_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    scopebuddy_game_build_lessons(root);
    return send_json(req, root);
}

static esp_err_t state_handler(httpd_req_t *req) { return send_state(req); }

static int json_int(cJSON *body, const char *key, int fallback)
{
    cJSON *item = body ? cJSON_GetObjectItem(body, key) : NULL;
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

static bool json_bool(cJSON *body, const char *key, bool fallback)
{
    cJSON *item = body ? cJSON_GetObjectItem(body, key) : NULL;
    return item ? cJSON_IsTrue(item) : fallback;
}

static const char *json_string(cJSON *body, const char *key, const char *fallback)
{
    cJSON *item = body ? cJSON_GetObjectItem(body, key) : NULL;
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

static esp_err_t lesson_start_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    read_body_json(req, &body);
    int id = json_int(body, "id", -1);
    if (body) cJSON_Delete(body);
    if (id < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
    scopebuddy_game_start_lesson((scope_lesson_id_t)id);
    return send_state(req);
}

static esp_err_t question_ready_handler(httpd_req_t *req)
{
    scopebuddy_game_confirm_ready();
    return send_state(req);
}

static esp_err_t question_next_handler(httpd_req_t *req)
{
    scopebuddy_game_next_question();
    return send_state(req);
}

static esp_err_t measurement_toggle_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    read_body_json(req, &body);
    int index = json_int(body, "index", -1);
    if (body) cJSON_Delete(body);
    if (index >= 0) scopebuddy_game_toggle_measurement((uint8_t)index);
    return send_state(req);
}

static esp_err_t measurement_reveal_selected_handler(httpd_req_t *req)
{
    scopebuddy_game_reveal_selected();
    return send_state(req);
}

static esp_err_t measurement_reveal_all_handler(httpd_req_t *req)
{
    scopebuddy_game_reveal_all();
    return send_state(req);
}

static esp_err_t home_handler(httpd_req_t *req)
{
    scopebuddy_game_return_home();
    return send_state(req);
}

static esp_err_t page_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    read_body_json(req, &body);
    int direction = json_int(body, "direction", 0);
    if (body) cJSON_Delete(body);
    scopebuddy_game_change_page((int8_t)direction);
    return send_state(req);
}

static esp_err_t nav_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    read_body_json(req, &body);
    const char *screen = json_string(body, "screen", "");
    if (strcmp(screen, "settings") == 0) scopebuddy_game_open_settings();
    else if (strcmp(screen, "diagnostics") == 0) scopebuddy_game_open_diagnostics();
    else if (strcmp(screen, "hwtests") == 0) scopebuddy_game_open_hwtests();
    else if (strcmp(screen, "start") == 0) scopebuddy_game_return_home();
    if (body) cJSON_Delete(body);
    return send_state(req);
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    read_body_json(req, &body);
    const char *key = json_string(body, "key", NULL);
    bool value = json_bool(body, "value", false);
    if (key != NULL) scopebuddy_game_set_setting(key, value);
    if (body) cJSON_Delete(body);
    return send_state(req);
}

static esp_err_t hwtest_single_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    read_body_json(req, &body);
    bool enable = json_bool(body, "enable", false);
    if (body) cJSON_Delete(body);
    scopebuddy_game_hwtest_single(enable);
    return send_state(req);
}

static esp_err_t hwtest_sync_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    read_body_json(req, &body);
    bool enable = json_bool(body, "enable", false);
    if (body) cJSON_Delete(body);
    scopebuddy_game_hwtest_sync(enable);
    return send_state(req);
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    char ssid[33] = {0};
    wifi_get_configured_ssid(ssid, sizeof(ssid));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", ssid);
    return send_json(req, root);
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (read_body_json(req, &body) != ESP_OK || body == NULL) {
        if (body) cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const char *ssid = json_string(body, "ssid", "");
    const char *password = json_string(body, "password", "");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    esp_err_t send_err = send_json(req, resp);

    /* Speichert Daten im NVS (leere SSID setzt auf AP-Modus zurück) und startet neu */
    esp_err_t save_err = wifi_save_credentials_and_restart(ssid, password);
    cJSON_Delete(body);

    if (save_err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "saving wifi failed");
    }
    return send_err;
}

static const httpd_uri_t routes[] = {
    { .uri = "/", .method = HTTP_GET, .handler = index_handler },
    { .uri = "/api/lessons", .method = HTTP_GET, .handler = lessons_handler },
    { .uri = "/api/state", .method = HTTP_GET, .handler = state_handler },
    { .uri = "/api/lesson/start", .method = HTTP_POST, .handler = lesson_start_handler },
    { .uri = "/api/question/ready", .method = HTTP_POST, .handler = question_ready_handler },
    { .uri = "/api/question/next", .method = HTTP_POST, .handler = question_next_handler },
    { .uri = "/api/measurement/toggle", .method = HTTP_POST, .handler = measurement_toggle_handler },
    { .uri = "/api/measurement/reveal_selected", .method = HTTP_POST, .handler = measurement_reveal_selected_handler },
    { .uri = "/api/measurement/reveal_all", .method = HTTP_POST, .handler = measurement_reveal_all_handler },
    { .uri = "/api/home", .method = HTTP_POST, .handler = home_handler },
    { .uri = "/api/page", .method = HTTP_POST, .handler = page_handler },
    { .uri = "/api/nav", .method = HTTP_POST, .handler = nav_handler },
    { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_handler },
    { .uri = "/api/hwtest/single", .method = HTTP_POST, .handler = hwtest_single_handler },
    { .uri = "/api/hwtest/sync", .method = HTTP_POST, .handler = hwtest_sync_handler },
    { .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get_handler },
    { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler },
	{ .uri = "/lang/*", .method = HTTP_GET, .handler = lang_handler },
	{ .uri = "/favicon.svg", .method = HTTP_GET, .handler = favicon_handler },
    { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler },

};

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = sizeof(routes) / sizeof(routes[0]) + 2;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(WEB_TAG, "Starting HTTP server failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(WEB_TAG, "Registering route '%s' failed: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(WEB_TAG, "HTTP server ready");
    return ESP_OK;
}
