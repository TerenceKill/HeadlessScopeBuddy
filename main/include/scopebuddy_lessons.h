#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCOPE_LESSON_PERIODIC = 0,
    SCOPE_LESSON_PULSE_WIDTH,
    SCOPE_LESSON_BURST,
    SCOPE_LESSON_MISSING_PULSE,
    SCOPE_LESSON_SERVO,
    SCOPE_LESSON_TACHOMETER,
    SCOPE_LESSON_BUTTON_BOUNCE,
    SCOPE_LESSON_UART,
    SCOPE_LESSON_ALTERNATING,
    SCOPE_LESSON_TRIGGER_RESPONSE,
    SCOPE_LESSON_PHASE_SHIFT,
    SCOPE_LESSON_FREQUENCY_DIVIDER,
    SCOPE_LESSON_ULTRASONIC,
    SCOPE_LESSON_GATED_PWM,
    SCOPE_LESSON_QUADRATURE,
    SCOPE_LESSON_GLITCH,
    SCOPE_LESSON_MANCHESTER,
    SCOPE_LESSON_NEOPIXEL,
    SCOPE_LESSON_PWM_DAC,
    SCOPE_LESSON_SPI,
    SCOPE_LESSON_STEPPER,
    SCOPE_LESSON_I2C,
    SCOPE_LESSON_DEADTIME,
    SCOPEBUDDY_LESSON_COUNT
} scope_lesson_id_t;

typedef enum {
    SCOPE_SIGNAL_PWM = 0,
    SCOPE_SIGNAL_SEQUENCE,
    SCOPE_SIGNAL_SEQUENCE_PAIR,
    SCOPE_SIGNAL_ALTERNATING,
} scope_signal_kind_t;

typedef struct {
    uint32_t frequency_hz;
    uint8_t duty_percent;
} scope_pwm_spec_t;

typedef struct {
    bool level;
    uint32_t duration_us;
} scope_signal_segment_t;

#define SCOPEBUDDY_MAX_SEGMENTS 64U
#define SCOPEBUDDY_MAX_MEASUREMENTS 3U
#define SCOPEBUDDY_MAX_CHANNELS 2U

typedef struct {
    scope_signal_segment_t segments[SCOPEBUDDY_MAX_SEGMENTS];
    size_t segment_count;
} scope_timeline_spec_t;

typedef struct {
    scope_timeline_spec_t timeline;
    bool loop;
} scope_sequence_spec_t;

typedef struct {
    scope_timeline_spec_t channels[SCOPEBUDDY_MAX_CHANNELS];
    bool loop;
} scope_sequence_pair_spec_t;

typedef struct {
    scope_pwm_spec_t state_a;
    scope_pwm_spec_t state_b;
    uint16_t state_duration_ms;
} scope_alternating_spec_t;

typedef struct {
    scope_signal_kind_t kind;
    union {
        scope_pwm_spec_t pwm;
        scope_sequence_spec_t sequence;
        scope_sequence_pair_spec_t pair;
        scope_alternating_spec_t alternating;
    } data;
} scope_signal_spec_t;

typedef enum {
    SCOPE_MEASUREMENT_CHANNEL_1 = 0,
    SCOPE_MEASUREMENT_CHANNEL_2,
    SCOPE_MEASUREMENT_CHANNEL_PAIR,
    SCOPE_MEASUREMENT_DERIVED,
} scope_measurement_source_t;

typedef enum {
    SCOPE_MEASUREMENT_NUMERIC = 0,
    SCOPE_MEASUREMENT_TEXT,
} scope_measurement_value_kind_t;

typedef struct {
    const char *label;
    const char *unit;
    const char *calculation;
    scope_measurement_source_t source;
    scope_measurement_value_kind_t value_kind;
    double expected_value;
    double tolerance;
    uint8_t decimals;
    char value[32];
} scope_measurement_t;

typedef struct {
    scope_lesson_id_t id;
    const char *title;
    const char *category;
    const char *summary;
    const char *learning_objective;
    const char *connection_hint;
    const char *trigger_hint;
    uint8_t required_channels;
} scope_lesson_definition_t;

typedef struct {
    uint32_t frequency_hz;
    uint32_t period_us;
    uint32_t high_time_us;
    uint8_t duty_percent;
} scope_realized_channel_t;

typedef struct {
    const scope_lesson_definition_t *lesson;
    uint8_t difficulty;
    scope_signal_spec_t signal;
    scope_realized_channel_t realized[SCOPEBUDDY_MAX_CHANNELS];
    char context[128];
    uint8_t measurement_count;
    scope_measurement_t measurements[SCOPEBUDDY_MAX_MEASUREMENTS];
    uint32_t signature;
    union {
        struct { uint32_t pulse_count; uint32_t pause_ms; } burst;
        struct { uint32_t slot_count; uint32_t missing_index; } missing_pulse;
        struct { uint32_t target_angle_deg; } servo;
        struct { uint32_t pulses_per_revolution; } tachometer;
        struct { uint32_t edge_count; uint32_t duration_us; } button_bounce;
        struct { uint32_t baud; uint8_t data_byte; } uart;
        struct { uint32_t state_duration_ms; } alternating;
        struct { uint32_t delay_us; uint32_t response_width_us; } trigger_response;
        struct { uint32_t delay_us; double phase_degrees; } phase_shift;
        struct { uint32_t ratio; } frequency_divider;
        struct { uint32_t echo_delay_us; uint32_t distance_cm; } ultrasonic;
        struct { uint32_t start_delay_us; uint32_t pulse_count; } gated_pwm;
        struct { bool channel_a_leads; uint32_t quarter_period_us; } quadrature;
        struct { uint32_t glitch_us; uint32_t interval_us; } glitch;
        struct { uint32_t bit_us; uint8_t data_byte; } manchester;
        struct { uint32_t t0h_us; uint32_t t1h_us; uint8_t data_byte; } neopixel;
        struct { uint32_t carrier_hz; uint32_t step_count; } pwm_dac;
        struct { uint32_t clock_hz; uint8_t data_byte; } spi;
        struct { uint32_t step_count; uint32_t dir_setup_us; } stepper;
        struct { uint32_t clock_hz; uint8_t address_7bit; bool is_read; } i2c;
        struct { uint32_t frequency_hz; uint32_t deadtime_us; } deadtime;
    } parameters;
} scope_lesson_instance_t;

size_t scopebuddy_lesson_count(void);
const scope_lesson_definition_t *scopebuddy_lesson_at(size_t index);
esp_err_t scopebuddy_generate_lesson(scope_lesson_id_t id, uint8_t difficulty,
                                     scope_lesson_instance_t *instance);
void scopebuddy_update_effective_pwm(scope_lesson_instance_t *instance,
                                     uint32_t frequency_a_hz, uint8_t duty_a_percent,
                                     uint32_t frequency_b_hz, uint8_t duty_b_percent);

#ifdef __cplusplus
}
#endif
