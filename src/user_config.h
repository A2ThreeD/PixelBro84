#ifndef A2THREED_PIXELBRO84_USER_CONFIG_H
#define A2THREED_PIXELBRO84_USER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USER_CONFIG_VERSION 2u
#define CAKE_LED_COUNT_MAX 64u
#define CAKE_ROTATION_MS_MIN 100u
#define CAKE_ROTATION_MS_MAX 10000u

typedef enum {
    CAKE_LED_TYPE_WS2812B,
    CAKE_LED_TYPE_WS2811,
} cake_led_type_t;

typedef enum {
    CAKE_COLOR_ORDER_GRB,
    CAKE_COLOR_ORDER_RGB,
} cake_color_order_t;

typedef enum {
    CAKE_TIMING_SYNCED,
    CAKE_TIMING_FREE,
} cake_timing_mode_t;

typedef enum {
    CAKE_EFFECT_SOLID,
    CAKE_EFFECT_FADE,
    CAKE_EFFECT_TRAIL,
    CAKE_EFFECT_COLOR_SHIFT,
} cake_effect_t;

typedef struct {
    uint16_t version;
    uint16_t cake_led_count;
    uint16_t cake_bit_rate_khz;
    uint16_t cake_start_offset;
    uint16_t cake_rotation_ms;
    uint8_t outer_color_index;
    uint8_t cake_led_type;
    uint8_t cake_color_order;
    uint8_t cake_red;
    uint8_t cake_green;
    uint8_t cake_blue;
    uint8_t cake_reverse;
    uint8_t lid_bypass;
    uint8_t cake_timing_mode;
    uint8_t cake_speed_multiplier;
    uint8_t cake_effect;
    uint8_t reserved[3];
} user_config_t;

void user_config_set_defaults(user_config_t *config);

bool user_config_validate(const user_config_t *config,
                          uint8_t outer_color_count,
                          char *error,
                          size_t error_size);

bool user_config_parse_update(const char *settings,
                              const user_config_t *base,
                              uint8_t outer_color_count,
                              user_config_t *result,
                              char *error,
                              size_t error_size);

const char *cake_led_type_name(uint8_t type);
const char *cake_color_order_name(uint8_t order);
const char *cake_timing_mode_name(uint8_t mode);
const char *cake_effect_name(uint8_t effect);

#endif
