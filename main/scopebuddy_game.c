#include "scopebuddy_game.h"

#include "bsp_extra.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "scopebuddy_output.h"
#include <string.h>

#define GAME_TAG "SCOPEBUDDY_GAME"
#define SCOPEBUDDY_FIRMWARE_VERSION "0.6.0"
#define SETTINGS_NAMESPACE "scopebuddy"
#define SETTINGS_KEY "ui_flags"
#define SETTING_FLAG_TIMER       (1U << 0)
#define SETTING_FLAG_VALUES      (1U << 1)
#define SETTING_FLAG_SCOPE_RESET (1U << 2)
#define SETTING_FLAG_SKIP_CONFIRM (1U << 4)
#define LESSONS_PER_PAGE 3U

static bool hardware_ready;
static game_screen_t current_screen = GAME_SCREEN_START;

static bool lesson_selected;
static scope_lesson_id_t selected_lesson;
static scope_lesson_instance_t challenge;
static uint32_t question_number;
static uint8_t lesson_page;

static bool measurement_selected[SCOPEBUDDY_MAX_MEASUREMENTS];
static bool measurement_revealed[SCOPEBUDDY_MAX_MEASUREMENTS];

static bool setting_show_timer = true;
static bool setting_reveal_values;
static bool setting_scope_reset = true;
static bool setting_confirm_home = true;
static bool settings_loaded;

static bool single_test_running;
static bool sync_test_running;
static esp_err_t single_test_error = ESP_OK;
static esp_err_t sync_test_error = ESP_OK;

static uint32_t recent_signatures[10];
static uint8_t recent_count;

static esp_timer_handle_t pattern_timer;
static StaticSemaphore_t pattern_mutex_buffer;
static SemaphoreHandle_t pattern_mutex;
static bool pattern_enabled;
static bool alternating_state_b;

static int64_t question_start_time_us;

static void log_operation_error(const char *operation, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(GAME_TAG, "%s failed: %s", operation, esp_err_to_name(err));
    }
}

static uint8_t settings_flags(void)
{
    return (setting_show_timer ? SETTING_FLAG_TIMER : 0U) |
           (setting_reveal_values ? SETTING_FLAG_VALUES : 0U) |
           (setting_scope_reset ? SETTING_FLAG_SCOPE_RESET : 0U) |
           (!setting_confirm_home ? SETTING_FLAG_SKIP_CONFIRM : 0U);
}

static void load_settings(void)
{
    if (settings_loaded) return;
    settings_loaded = true;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) log_operation_error("Opening stored settings", err);
        return;
    }

    uint8_t flags = 0;
    esp_err_t flags_err = nvs_get_u8(handle, SETTINGS_KEY, &flags);
    if (flags_err == ESP_OK) {
        setting_show_timer = (flags & SETTING_FLAG_TIMER) != 0;
        setting_reveal_values = (flags & SETTING_FLAG_VALUES) != 0;
        setting_scope_reset = (flags & SETTING_FLAG_SCOPE_RESET) != 0;
        setting_confirm_home = (flags & SETTING_FLAG_SKIP_CONFIRM) == 0;
    } else if (flags_err != ESP_ERR_NVS_NOT_FOUND) {
        log_operation_error("Reading stored settings", flags_err);
    }
    nvs_close(handle);
}

static void save_settings(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, SETTINGS_KEY, settings_flags());
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    log_operation_error("Saving settings", err);
}

static uint32_t challenge_signature(void) { return challenge.signature; }

static bool signature_seen(uint32_t signature)
{
    for (uint8_t i = 0; i < recent_count; ++i) {
        if (recent_signatures[i] == signature) return true;
    }
    return false;
}

static void remember_signature(uint32_t signature)
{
    if (recent_count < (sizeof(recent_signatures) / sizeof(recent_signatures[0]))) {
        recent_signatures[recent_count++] = signature;
        return;
    }
    for (uint8_t i = 1; i < recent_count; ++i) recent_signatures[i - 1] = recent_signatures[i];
    recent_signatures[recent_count - 1] = signature;
}

static void stop_pattern_locked(void)
{
    pattern_enabled = false;
    if (pattern_timer && esp_timer_is_active(pattern_timer)) {
        esp_err_t err = esp_timer_stop(pattern_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            log_operation_error("Stopping signal timer", err);
        }
    }
    log_operation_error("Stopping challenge output", scopebuddy_output_stop());
}

static void stop_pattern(void)
{
    if (pattern_mutex == NULL) {
        log_operation_error("Stopping challenge output", scopebuddy_output_stop());
        return;
    }
    xSemaphoreTake(pattern_mutex, portMAX_DELAY);
    stop_pattern_locked();
    xSemaphoreGive(pattern_mutex);
}

static bool schedule_pattern_locked(uint64_t delay_us)
{
    if (pattern_timer == NULL || !pattern_enabled) return false;
    esp_err_t err = esp_timer_start_once(pattern_timer, delay_us);
    if (err != ESP_OK) {
        pattern_enabled = false;
        log_operation_error("Starting signal timer", err);
        return false;
    }
    return true;
}

static void pattern_callback(void *arg)
{
    (void)arg;
    if (pattern_mutex == NULL) return;
    xSemaphoreTake(pattern_mutex, portMAX_DELAY);
    if (!pattern_enabled) {
        xSemaphoreGive(pattern_mutex);
        return;
    }
    if (challenge.signal.kind == SCOPE_SIGNAL_ALTERNATING) {
        const scope_alternating_spec_t *alternating = &challenge.signal.data.alternating;
        alternating_state_b = !alternating_state_b;
        esp_err_t err = gpio_wave_set_frequency(
            alternating_state_b ? alternating->state_b.frequency_hz : alternating->state_a.frequency_hz);
        if (err == ESP_OK) {
            err = gpio_wave_set_duty(alternating_state_b ? alternating->state_b.duty_percent :
                                                            alternating->state_a.duty_percent);
        }
        if (err == ESP_OK) {
            schedule_pattern_locked((uint64_t)alternating->state_duration_ms * 1000ULL);
        } else {
            pattern_enabled = false;
            log_operation_error("Switching alternating output", err);
        }
    }
    xSemaphoreGive(pattern_mutex);
}

static void ensure_pattern_timer(void)
{
    if (pattern_timer) return;
    if (pattern_mutex == NULL) {
        pattern_mutex = xSemaphoreCreateMutexStatic(&pattern_mutex_buffer);
        if (pattern_mutex == NULL) {
            ESP_LOGE(GAME_TAG, "Creating signal mutex failed");
            return;
        }
    }
    const esp_timer_create_args_t args = { .callback = pattern_callback, .name = "measurement_game" };
    esp_err_t err = esp_timer_create(&args, &pattern_timer);
    if (err != ESP_OK) {
        pattern_timer = NULL;
        log_operation_error("Creating signal timer", err);
    }
}

static void generate_challenge(void)
{
    esp_err_t err;
    uint8_t attempts = 0;
    do {
        uint8_t parameter_profile = (uint8_t)(esp_random() % 3U) + 1U;
        err = scopebuddy_generate_lesson(selected_lesson, parameter_profile, &challenge);
        if (err != ESP_OK) {
            log_operation_error("Generating lesson", err);
            return;
        }
        ++attempts;
    } while (signature_seen(challenge_signature()) && attempts < 20U);
    remember_signature(challenge_signature());
}

static void start_challenge_signal(void)
{
    if (pattern_mutex == NULL) {
        ESP_LOGE(GAME_TAG, "Cannot start challenge: signal mutex unavailable");
        return;
    }
    xSemaphoreTake(pattern_mutex, portMAX_DELAY);
    stop_pattern_locked();

    esp_err_t err = scopebuddy_output_start(&challenge.signal);
    if (err != ESP_OK) {
        log_operation_error("Starting challenge output", err);
        xSemaphoreGive(pattern_mutex);
        return;
    }

    if (challenge.signal.kind == SCOPE_SIGNAL_PWM) {
        uint32_t actual_frequency = 0;
        uint8_t actual_duty = 0;
        if (gpio_wave_get_effective(&actual_frequency, &actual_duty) == ESP_OK) {
            scopebuddy_update_effective_pwm(&challenge, actual_frequency, actual_duty, 0, 0);
        }
    } else if (challenge.signal.kind == SCOPE_SIGNAL_ALTERNATING) {
        const scope_alternating_spec_t *alternating = &challenge.signal.data.alternating;
        uint32_t actual_a = 0, actual_b = 0;
        uint8_t duty_a = 0, duty_b = 0;
        if (gpio_wave_get_effective(&actual_a, &duty_a) == ESP_OK &&
            gpio_wave_set_frequency(alternating->state_b.frequency_hz) == ESP_OK &&
            gpio_wave_set_duty(alternating->state_b.duty_percent) == ESP_OK &&
            gpio_wave_get_effective(&actual_b, &duty_b) == ESP_OK &&
            gpio_wave_set_frequency(alternating->state_a.frequency_hz) == ESP_OK &&
            gpio_wave_set_duty(alternating->state_a.duty_percent) == ESP_OK) {
            scopebuddy_update_effective_pwm(&challenge, actual_a, duty_a, actual_b, duty_b);
        }
        pattern_enabled = true;
        alternating_state_b = false;
        schedule_pattern_locked((uint64_t)alternating->state_duration_ms * 1000ULL);
    }
    xSemaphoreGive(pattern_mutex);
}

static void start_scope_reset_signal(void)
{
    if (pattern_mutex == NULL) {
        ESP_LOGE(GAME_TAG, "Cannot start reset signal: signal mutex unavailable");
        return;
    }
    xSemaphoreTake(pattern_mutex, portMAX_DELAY);
    stop_pattern_locked();

    if (challenge.lesson && challenge.lesson->required_channels == 2) {
        scope_signal_spec_t reset_signal = { .kind = SCOPE_SIGNAL_SEQUENCE_PAIR };
        reset_signal.data.pair.loop = true;
        reset_signal.data.pair.channels[0].segments[0] = (scope_signal_segment_t){ .level = true, .duration_us = 500 };
        reset_signal.data.pair.channels[0].segments[1] = (scope_signal_segment_t){ .level = false, .duration_us = 500 };
        reset_signal.data.pair.channels[0].segment_count = 2;
        reset_signal.data.pair.channels[1].segments[0] = (scope_signal_segment_t){ .level = false, .duration_us = 250 };
        reset_signal.data.pair.channels[1].segments[1] = (scope_signal_segment_t){ .level = true, .duration_us = 500 };
        reset_signal.data.pair.channels[1].segments[2] = (scope_signal_segment_t){ .level = false, .duration_us = 250 };
        reset_signal.data.pair.channels[1].segment_count = 3;
        esp_err_t pair_err = scopebuddy_output_start(&reset_signal);
        log_operation_error("Starting two-channel oscilloscope reset output", pair_err);
        xSemaphoreGive(pattern_mutex);
        return;
    }

    uint32_t challenge_frequency = challenge.realized[0].frequency_hz;
    if (challenge_frequency == 0 && challenge.realized[0].period_us > 0) {
        challenge_frequency = 1000000U / challenge.realized[0].period_us;
    }
    /* 100 Hz, not 10 Hz: classic ESP32's LEDC divider can't reach 10 Hz at our
     * 10-bit duty resolution with the clock source auto-selected at init
     * (minimum is ~78 Hz on the 80 MHz APB clock); 100 Hz keeps clear margin. */
    uint32_t reset_frequency = (challenge_frequency < 448U) ? 20000U : 100U;
    esp_err_t err = gpio_wave_set_frequency(reset_frequency);
    if (err == ESP_OK) err = gpio_wave_set_duty(50);
    if (err == ESP_OK) err = gpio_wave_start();
    log_operation_error("Starting oscilloscope reset output", err);
    xSemaphoreGive(pattern_mutex);
}

static void reset_measurement_state(void)
{
    for (uint8_t i = 0; i < SCOPEBUDDY_MAX_MEASUREMENTS; ++i) {
        measurement_selected[i] = false;
        measurement_revealed[i] = false;
    }
}

static void apply_direct_value_setting(void)
{
    if (!setting_reveal_values) return;
    for (uint8_t i = 0; i < challenge.measurement_count; ++i) measurement_revealed[i] = true;
}

static void enter_question_or_reset(void)
{
    stop_pattern();
    generate_challenge();
    reset_measurement_state();
    question_start_time_us = esp_timer_get_time();
    if (setting_scope_reset) {
        current_screen = GAME_SCREEN_RESET;
        start_scope_reset_signal();
    } else {
        current_screen = GAME_SCREEN_QUESTION;
        apply_direct_value_setting();
        if (hardware_ready) start_challenge_signal();
    }
}

void scopebuddy_game_init(void)
{
    load_settings();
    ensure_pattern_timer();
    current_screen = GAME_SCREEN_START;
}

void scopebuddy_game_set_hardware_ready(bool ready) { hardware_ready = ready; }

esp_err_t scopebuddy_game_start_lesson(scope_lesson_id_t id)
{
    const scope_lesson_definition_t *lesson = scopebuddy_lesson_at(id);
    if (lesson == NULL) return ESP_ERR_INVALID_ARG;
    selected_lesson = id;
    lesson_selected = true;
    question_number = 1;
    enter_question_or_reset();
    return ESP_OK;
}

esp_err_t scopebuddy_game_confirm_ready(void)
{
    if (current_screen != GAME_SCREEN_RESET) return ESP_ERR_INVALID_STATE;
    current_screen = GAME_SCREEN_QUESTION;
    question_start_time_us = esp_timer_get_time();
    apply_direct_value_setting();
    if (hardware_ready) start_challenge_signal();
    return ESP_OK;
}

esp_err_t scopebuddy_game_next_question(void)
{
    if (!lesson_selected) return ESP_ERR_INVALID_STATE;
    question_number = question_number == UINT32_MAX ? 1U : question_number + 1U;
    enter_question_or_reset();
    return ESP_OK;
}

esp_err_t scopebuddy_game_toggle_measurement(uint8_t index)
{
    if (index >= challenge.measurement_count) return ESP_ERR_INVALID_ARG;
    measurement_selected[index] = !measurement_selected[index];
    return ESP_OK;
}

esp_err_t scopebuddy_game_reveal_selected(void)
{
    bool hide_selected = true;
    for (uint8_t i = 0; i < challenge.measurement_count; ++i) {
        if (measurement_selected[i] && !measurement_revealed[i]) hide_selected = false;
    }
    for (uint8_t i = 0; i < challenge.measurement_count; ++i) {
        if (!measurement_selected[i]) continue;
        measurement_revealed[i] = !hide_selected;
    }
    return ESP_OK;
}

esp_err_t scopebuddy_game_reveal_all(void)
{
    bool hide_all = true;
    for (uint8_t i = 0; i < challenge.measurement_count; ++i) {
        if (!measurement_revealed[i]) hide_all = false;
    }
    for (uint8_t i = 0; i < challenge.measurement_count; ++i) measurement_revealed[i] = !hide_all;
    return ESP_OK;
}

esp_err_t scopebuddy_game_return_home(void)
{
    stop_pattern();
    lesson_selected = false;
    current_screen = GAME_SCREEN_START;
    return ESP_OK;
}

esp_err_t scopebuddy_game_change_page(int8_t direction)
{
    size_t page_count = (scopebuddy_lesson_count() + LESSONS_PER_PAGE - 1U) / LESSONS_PER_PAGE;
    if (direction == 0 || page_count < 2U) return ESP_OK;
    if (direction < 0) {
        lesson_page = lesson_page > 0 ? lesson_page - 1U : (uint8_t)(page_count - 1U);
    } else {
        lesson_page = ((size_t)lesson_page + 1U < page_count) ? lesson_page + 1U : 0U;
    }
    return ESP_OK;
}

esp_err_t scopebuddy_game_open_settings(void)
{
    stop_pattern();
    lesson_selected = false;
    current_screen = GAME_SCREEN_SETTINGS;
    return ESP_OK;
}

esp_err_t scopebuddy_game_open_diagnostics(void)
{
    stop_pattern();
    lesson_selected = false;
    single_test_running = false;
    sync_test_running = false;
    current_screen = GAME_SCREEN_DIAGNOSTICS;
    return ESP_OK;
}

esp_err_t scopebuddy_game_open_hwtests(void)
{
    stop_pattern();
    lesson_selected = false;
    single_test_running = false;
    sync_test_running = false;
    current_screen = GAME_SCREEN_HWTESTS;
    return ESP_OK;
}

esp_err_t scopebuddy_game_set_setting(const char *key, bool value)
{
    if (strcmp(key, "show_timer") == 0) setting_show_timer = value;
    else if (strcmp(key, "reveal_values") == 0) setting_reveal_values = value;
    else if (strcmp(key, "auto_prep") == 0) setting_scope_reset = value;
    else if (strcmp(key, "confirm_home") == 0) setting_confirm_home = value;
    else return ESP_ERR_INVALID_ARG;
    save_settings();
    return ESP_OK;
}

esp_err_t scopebuddy_game_hwtest_single(bool enable)
{
    esp_err_t err;
    if (enable) {
        stop_pattern();
        sync_test_running = false;
        err = gpio_wave_set_frequency(1000);
        if (err == ESP_OK) err = gpio_wave_set_duty(50);
        if (err == ESP_OK) err = gpio_wave_start();
        single_test_running = (err == ESP_OK);
    } else {
        err = gpio_wave_stop();
        if (err == ESP_OK) single_test_running = false;
    }
    single_test_error = err;
    log_operation_error(enable ? "Starting one-channel test" : "Stopping one-channel test", err);
    return err;
}

esp_err_t scopebuddy_game_hwtest_sync(bool enable)
{
    esp_err_t err;
    if (enable) {
        stop_pattern();
        single_test_running = false;
        err = gpio_sync_test_start();
        sync_test_running = (err == ESP_OK);
    } else {
        err = gpio_sync_test_stop();
        if (err == ESP_OK) sync_test_running = false;
    }
    sync_test_error = err;
    log_operation_error(enable ? "Starting two-channel test" : "Stopping two-channel test", err);
    return err;
}

static const char *screen_name(game_screen_t screen)
{
    switch (screen) {
    case GAME_SCREEN_START: return "start";
    case GAME_SCREEN_RESET: return "reset";
    case GAME_SCREEN_QUESTION: return "question";
    case GAME_SCREEN_SETTINGS: return "settings";
    case GAME_SCREEN_DIAGNOSTICS: return "diagnostics";
    case GAME_SCREEN_HWTESTS: return "hwtests";
    default: return "start";
    }
}

static const char *measurement_source_name(scope_measurement_source_t source)
{
    switch (source) {
    case SCOPE_MEASUREMENT_CHANNEL_1: return "CH1";
    case SCOPE_MEASUREMENT_CHANNEL_2: return "CH2";
    case SCOPE_MEASUREMENT_CHANNEL_PAIR: return "CH1/CH2";
    case SCOPE_MEASUREMENT_DERIVED: return "BERECHNET";
    default: return "";
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "POWER-ON";
    case ESP_RST_EXT: return "EXTERNER RESET";
    case ESP_RST_SW: return "SOFTWARE-RESET";
    case ESP_RST_PANIC: return "PANIC/ASSERT";
    case ESP_RST_INT_WDT: return "INTERRUPT-WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK-WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP-SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE-FEHLER";
    case ESP_RST_PWR_GLITCH: return "POWER-GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU-LOCKUP";
    default: return "UNBEKANNT";
    }
}

void scopebuddy_game_build_lessons(cJSON *root)
{
    cJSON *lessons = cJSON_AddArrayToObject(root, "lessons");
    for (size_t i = 0; i < scopebuddy_lesson_count(); ++i) {
        const scope_lesson_definition_t *lesson = scopebuddy_lesson_at(i);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", (double)lesson->id);
        cJSON_AddStringToObject(item, "title", lesson->title);
        cJSON_AddStringToObject(item, "category", lesson->category);
        cJSON_AddStringToObject(item, "summary", lesson->summary);
        cJSON_AddStringToObject(item, "learning_objective", lesson->learning_objective);
        cJSON_AddStringToObject(item, "connection_hint", lesson->connection_hint);
        cJSON_AddStringToObject(item, "trigger_hint", lesson->trigger_hint);
        cJSON_AddNumberToObject(item, "required_channels", lesson->required_channels);
        cJSON_AddItemToArray(lessons, item);
    }
}

static void build_start_json(cJSON *root)
{
    size_t page_count = (scopebuddy_lesson_count() + LESSONS_PER_PAGE - 1U) / LESSONS_PER_PAGE;
    cJSON *start = cJSON_AddObjectToObject(root, "start");
    cJSON_AddNumberToObject(start, "page", lesson_page);
    cJSON_AddNumberToObject(start, "page_count", (double)page_count);
    cJSON_AddNumberToObject(start, "lessons_per_page", LESSONS_PER_PAGE);
}

static void build_question_json(cJSON *root)
{
    cJSON *question = cJSON_AddObjectToObject(root, "question");
    cJSON_AddNumberToObject(question, "number", question_number);
    cJSON_AddBoolToObject(question, "two_channel", challenge.lesson && challenge.lesson->required_channels == 2);
    if (challenge.lesson == NULL) return;
    cJSON_AddStringToObject(question, "mode_name", challenge.lesson->title);
    cJSON_AddStringToObject(question, "context", challenge.context);
    cJSON_AddStringToObject(question, "connection_hint", challenge.lesson->connection_hint);
    cJSON_AddStringToObject(question, "trigger_hint", challenge.lesson->trigger_hint);

    int64_t elapsed = current_screen == GAME_SCREEN_QUESTION ?
        (esp_timer_get_time() - question_start_time_us) / 1000000LL : 0;
    cJSON_AddNumberToObject(question, "elapsed_seconds", (double)elapsed);

    cJSON *measurements = cJSON_AddArrayToObject(question, "measurements");
    bool any_selected = false, all_selected_visible = true, all_visible = true;
    for (uint8_t i = 0; i < challenge.measurement_count; ++i) {
        if (measurement_selected[i]) {
            any_selected = true;
            if (!measurement_revealed[i]) all_selected_visible = false;
        }
        if (!measurement_revealed[i]) all_visible = false;

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", challenge.measurements[i].label);
        cJSON_AddStringToObject(item, "unit", challenge.measurements[i].unit);
        cJSON_AddStringToObject(item, "source", measurement_source_name(challenge.measurements[i].source));
        cJSON_AddStringToObject(item, "calculation", challenge.measurements[i].calculation);
        cJSON_AddBoolToObject(item, "selected", measurement_selected[i]);
        cJSON_AddBoolToObject(item, "revealed", measurement_revealed[i]);
        cJSON_AddStringToObject(item, "value", measurement_revealed[i] ? challenge.measurements[i].value : "---");
        cJSON_AddItemToArray(measurements, item);
    }
    cJSON_AddBoolToObject(question, "any_selected", any_selected);
    cJSON_AddBoolToObject(question, "all_selected_visible", all_selected_visible);
    cJSON_AddBoolToObject(question, "all_visible", all_visible);
}

static void build_diagnostics_json(cJSON *root)
{
    cJSON *diagnostics = cJSON_AddObjectToObject(root, "diagnostics");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(diagnostics, "firmware_version", SCOPEBUDDY_FIRMWARE_VERSION);
    char build[80];
    snprintf(build, sizeof(build), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(diagnostics, "build", build);
    cJSON_AddStringToObject(diagnostics, "idf_version", app->idf_ver);
    cJSON_AddStringToObject(diagnostics, "target", CONFIG_IDF_TARGET);
    cJSON_AddBoolToObject(diagnostics, "hardware_ready", hardware_ready);

    char reset_text[40];
    snprintf(reset_text, sizeof(reset_text), "%s (%d)", reset_reason_name(esp_reset_reason()), (int)esp_reset_reason());
    cJSON_AddStringToObject(diagnostics, "reset_reason", reset_text);

    uint64_t uptime_seconds = (uint64_t)esp_timer_get_time() / 1000000ULL;
    char uptime_text[32];
    snprintf(uptime_text, sizeof(uptime_text), "%02llu:%02llu:%02llu",
             (unsigned long long)(uptime_seconds / 3600ULL),
             (unsigned long long)((uptime_seconds / 60ULL) % 60ULL),
             (unsigned long long)(uptime_seconds % 60ULL));
    cJSON_AddStringToObject(diagnostics, "uptime", uptime_text);

    cJSON_AddNumberToObject(diagnostics, "min_heap_kb", esp_get_minimum_free_heap_size() / 1024U);
    cJSON_AddNumberToObject(diagnostics, "free_heap_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U);
    cJSON_AddNumberToObject(diagnostics, "stack_reserve", uxTaskGetStackHighWaterMark(NULL));
}

static void build_hwtests_json(cJSON *root)
{
    cJSON *hwtests = cJSON_AddObjectToObject(root, "hwtests");
    cJSON_AddBoolToObject(hwtests, "single_running", single_test_running);
    cJSON_AddBoolToObject(hwtests, "single_error", single_test_error != ESP_OK);
    cJSON_AddBoolToObject(hwtests, "sync_running", sync_test_running);
    cJSON_AddBoolToObject(hwtests, "sync_error", sync_test_error != ESP_OK);
}

void scopebuddy_game_build_state(cJSON *root)
{
    cJSON_AddStringToObject(root, "screen", screen_name(current_screen));
    cJSON_AddBoolToObject(root, "hardware_ready", hardware_ready);

    cJSON *settings = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddBoolToObject(settings, "show_timer", setting_show_timer);
    cJSON_AddBoolToObject(settings, "reveal_values", setting_reveal_values);
    cJSON_AddBoolToObject(settings, "auto_prep", setting_scope_reset);
    cJSON_AddBoolToObject(settings, "confirm_home", setting_confirm_home);

    build_start_json(root);
    build_question_json(root);
    build_diagnostics_json(root);
    build_hwtests_json(root);
}
