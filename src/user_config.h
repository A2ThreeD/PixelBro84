#ifndef A2THREED_PIXELBRO84_USER_CONFIG_H
#define A2THREED_PIXELBRO84_USER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USER_CONFIG_VERSION 1u
#define CAKE_LED_COUNT_MAX 64u

typedef enum {
    CAKE_LED_TYPE_WS2812B,
    CAKE_LED_TYPE_WS2811,
} cake_led_type_t;

typedef enum {
    CAKE_COLOR_ORDER_GRB,
    CAKE_COLOR_ORDER_RGB,
} cake_color_order_t;

typedef struct {
    uint16_t version;
    uint16_t cake_led_count;
    uint16_t cake_bit_rate_khz;
    uint16_t cake_start_offset;
    uint8_t outer_color_index;
    uint8_t cake_led_type;
    uint8_t cake_color_order;
    uint8_t cake_red;
    uint8_t cake_green;
    uint8_t cake_blue;
    uint8_t cake_reverse;
    uint8_t lid_bypass;
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

#endif
