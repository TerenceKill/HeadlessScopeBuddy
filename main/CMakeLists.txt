idf_component_register(
    SRCS "main.c"
         "scopebuddy_lessons.c"
         "scopebuddy_output.c"
         "scopebuddy_game.c"
         "web_server.c"
         "vendor/cjson/cJSON.c"
    INCLUDE_DIRS "include" "vendor/cjson"
    REQUIRES driver bsp_extra esp_timer esp_app_format nvs_flash
             esp_http_server esp_wifi esp_netif esp_event mdns
    EMBED_FILES "web/index.html" "web/lang/de.json" "web/lang/en.json"
)
