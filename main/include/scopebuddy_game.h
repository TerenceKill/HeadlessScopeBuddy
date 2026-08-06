#ifndef SCOPEBUDDY_GAME_H
#define SCOPEBUDDY_GAME_H

#include "cJSON.h"
#include "esp_err.h"
#include "scopebuddy_lessons.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GAME_SCREEN_START,
    GAME_SCREEN_RESET,
    GAME_SCREEN_QUESTION,
    GAME_SCREEN_SETTINGS,
    GAME_SCREEN_DIAGNOSTICS,
    GAME_SCREEN_HWTESTS,
} game_screen_t;

/* Loads persisted settings, prepares the signal timer/mutex, and shows the
 * start screen. Must run after the GPIO waveform hardware is initialized. */
void scopebuddy_game_init(void);

void scopebuddy_game_set_hardware_ready(bool ready);

esp_err_t scopebuddy_game_start_lesson(scope_lesson_id_t id);
esp_err_t scopebuddy_game_confirm_ready(void);
esp_err_t scopebuddy_game_next_question(void);
esp_err_t scopebuddy_game_toggle_measurement(uint8_t index);
esp_err_t scopebuddy_game_reveal_selected(void);
esp_err_t scopebuddy_game_reveal_all(void);
esp_err_t scopebuddy_game_return_home(void);
esp_err_t scopebuddy_game_change_page(int8_t direction);

esp_err_t scopebuddy_game_open_settings(void);
esp_err_t scopebuddy_game_open_diagnostics(void);
esp_err_t scopebuddy_game_open_hwtests(void);

/* key: "show_timer" | "reveal_values" | "auto_prep" | "confirm_home" */
esp_err_t scopebuddy_game_set_setting(const char *key, bool value);

esp_err_t scopebuddy_game_hwtest_single(bool enable);
esp_err_t scopebuddy_game_hwtest_sync(bool enable);

/* Builds the full UI snapshot consumed by the web client on every poll. */
void scopebuddy_game_build_state(cJSON *root);

/* Builds the static lesson catalogue (title/summary/etc. for the start screen). */
void scopebuddy_game_build_lessons(cJSON *root);

#endif
