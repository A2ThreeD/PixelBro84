#include "user_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PARSE_BUFFER_SIZE 1024u

// Copy a parser or validation failure into the caller's optional buffer.
static void set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

// Parse an unsigned decimal field without accepting overflow or trailing text.
static bool parse_u16(const char *value, uint16_t *result) {
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *result = (uint16_t)parsed;
    return true;
}

// Parse a byte-sized decimal field using the stricter 16-bit parser above.
static bool parse_u8(const char *value, uint8_t *result) {
    uint16_t parsed = 0;
    if (!parse_u16(value, &parsed) || parsed > UINT8_MAX) {
        return false;
    }
    *result = (uint8_t)parsed;
    return true;
}

// Accept the textual and numeric boolean forms used by protocol clients.
static bool parse_bool(const char *value, uint8_t *result) {
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *result = 1;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *result = 0;
        return true;
    }
    return false;
}

// Populate the complete factory baseline used for first boot and migrations.
void user_config_set_defaults(user_config_t *config) {
    *config = (user_config_t){
        .version = USER_CONFIG_VERSION,
        .cake_led_count = 12,
        .cake_bit_rate_khz = 800,
        .cake_start_offset = 0,
        .cake_rotation_ms = 1000,
        .outer_color_index = 0,
        .cake_led_type = CAKE_LED_TYPE_WS2812B,
        .cake_color_order = CAKE_COLOR_ORDER_GRB,
        .cake_red = 255,
        .cake_green = 0,
        .cake_blue = 0,
        .cake_reverse = 0,
        .lid_bypass = 0,
        .cake_timing_mode = CAKE_TIMING_SYNCED,
        .cake_speed_multiplier = 1,
        .cake_effect = CAKE_EFFECT_SOLID,
        .reserved = {0, 0, 0},
        .cyclotron_led_count = 4,
        .cyclotron_bit_rate_khz = 800,
        .cyclotron_rotation_ms = 1000,
        .cyclotron_led_type = CAKE_LED_TYPE_WS2812B,
        .cyclotron_color_order = CAKE_COLOR_ORDER_GRB,
        .cyclotron_reverse = 0,
        .cyclotron_timing_mode = CAKE_TIMING_SYNCED,
        .cyclotron_speed_multiplier = 1,
        .cyclotron_effect = CAKE_EFFECT_SOLID,
        .cyclotron_led_style = CYCLOTRON_STYLE_SINGLE,
        .cyclotron_reserved = {0, 0, 0},
    };
}

// Convert the stored LED-type enumeration to its protocol spelling.
const char *cake_led_type_name(uint8_t type) {
    return type == CAKE_LED_TYPE_WS2811 ? "WS2811" : "WS2812B";
}

// Convert the stored wire-order enumeration to its protocol spelling.
const char *cake_color_order_name(uint8_t order) {
    return order == CAKE_COLOR_ORDER_RGB ? "RGB" : "GRB";
}

// Convert the timing-mode enumeration to the value returned over USB.
const char *cake_timing_mode_name(uint8_t mode) {
    return mode == CAKE_TIMING_FREE ? "FREE" : "SYNCED";
}

// Convert the effect enumeration to the value returned over USB.
const char *cake_effect_name(uint8_t effect) {
    static const char *const names[] = {
        "SOLID", "FADE", "TRAIL", "COLOR_SHIFT",
    };
    return effect <= CAKE_EFFECT_COLOR_SHIFT ? names[effect] : "UNKNOWN";
}

// Return the total physical pixel count required by a cyclotron style.
uint16_t cyclotron_led_count_for_style(uint8_t style) {
    static const uint16_t counts[] = {4, 12, 20, 36};
    return style < (sizeof(counts) / sizeof(counts[0])) ? counts[style] : 0;
}

// Convert the cyclotron layout enumeration to its protocol spelling.
const char *cyclotron_led_style_name(uint8_t style) {
    static const char *const names[] = {
        "SINGLE", "PUCK3", "PUCK5", "PUCK9",
    };
    return style < (sizeof(names) / sizeof(names[0])) ? names[style]
                                                      : "UNKNOWN";
}

// Infer a protocol-4 style from a legacy explicit physical pixel count.
static uint8_t cyclotron_led_style_from_count(uint16_t count) {
    if (count > 0 && count <= CYCLOTRON_WINDOW_COUNT) {
        return CYCLOTRON_STYLE_SINGLE;
    }
    switch (count) {
        case 12:
            return CYCLOTRON_STYLE_PUCK_3;
        case 20:
            return CYCLOTRON_STYLE_PUCK_5;
        case 36:
            return CYCLOTRON_STYLE_PUCK_9;
        default:
            return UINT8_MAX;
    }
}

// Reject configurations that cannot be safely rendered or persisted.
bool user_config_validate(const user_config_t *config,
                          uint8_t outer_color_count,
                          char *error,
                          size_t error_size) {
    if (config->version != USER_CONFIG_VERSION) {
        set_error(error, error_size, "unsupported config version");
        return false;
    }
    if (config->cake_led_count == 0 ||
        config->cake_led_count > CAKE_LED_COUNT_MAX) {
        set_error(error, error_size, "cake_led_count must be 1 through 64");
        return false;
    }
    if (config->cyclotron_led_style > CYCLOTRON_STYLE_PUCK_9) {
        set_error(error, error_size, "unsupported cyclotron_led_style");
        return false;
    }
    if (config->cyclotron_led_count !=
        cyclotron_led_count_for_style(config->cyclotron_led_style)) {
        set_error(error, error_size,
                  "cyclotron_led_count does not match cyclotron_led_style");
        return false;
    }
    if (config->cyclotron_bit_rate_khz != 400 &&
        config->cyclotron_bit_rate_khz != 800) {
        set_error(error, error_size,
                  "cyclotron_bit_rate_khz must be 400 or 800");
        return false;
    }
    if (config->cyclotron_rotation_ms < CAKE_ROTATION_MS_MIN ||
        config->cyclotron_rotation_ms > CAKE_ROTATION_MS_MAX) {
        set_error(error, error_size,
                  "cyclotron_rotation_ms must be 100 through 10000");
        return false;
    }
    if (config->cake_bit_rate_khz != 400 &&
        config->cake_bit_rate_khz != 800) {
        set_error(error, error_size, "cake_bit_rate_khz must be 400 or 800");
        return false;
    }
    if (config->cake_start_offset >= config->cake_led_count) {
        set_error(error, error_size,
                  "cake_start_offset must be less than cake_led_count");
        return false;
    }
    if (config->cake_rotation_ms < CAKE_ROTATION_MS_MIN ||
        config->cake_rotation_ms > CAKE_ROTATION_MS_MAX) {
        set_error(error, error_size,
                  "cake_rotation_ms must be 100 through 10000");
        return false;
    }
    if (config->outer_color_index >= outer_color_count) {
        set_error(error, error_size, "outer_color_index is out of range");
        return false;
    }
    if (config->cake_led_type > CAKE_LED_TYPE_WS2811) {
        set_error(error, error_size, "unsupported cake_led_type");
        return false;
    }
    if (config->cake_color_order > CAKE_COLOR_ORDER_RGB) {
        set_error(error, error_size, "unsupported cake_color_order");
        return false;
    }
    if (config->cake_timing_mode > CAKE_TIMING_FREE) {
        set_error(error, error_size, "unsupported cake_timing_mode");
        return false;
    }
    if (!((config->cake_speed_multiplier >= 1 &&
           config->cake_speed_multiplier <= 5) ||
          config->cake_speed_multiplier == 10 ||
          config->cake_speed_multiplier == 20)) {
        set_error(error, error_size,
                  "cake_speed_multiplier must be 1-5, 10, or 20");
        return false;
    }
    if (config->cake_effect > CAKE_EFFECT_COLOR_SHIFT) {
        set_error(error, error_size, "unsupported cake_effect");
        return false;
    }
    if (config->cyclotron_led_type > CAKE_LED_TYPE_WS2811 ||
        config->cyclotron_color_order > CAKE_COLOR_ORDER_RGB) {
        set_error(error, error_size, "unsupported cyclotron LED format");
        return false;
    }
    if (config->cyclotron_timing_mode > CAKE_TIMING_FREE) {
        set_error(error, error_size, "unsupported cyclotron_timing_mode");
        return false;
    }
    if (!((config->cyclotron_speed_multiplier >= 1 &&
           config->cyclotron_speed_multiplier <= 5) ||
          config->cyclotron_speed_multiplier == 10 ||
          config->cyclotron_speed_multiplier == 20)) {
        set_error(error, error_size,
                  "cyclotron_speed_multiplier must be 1-5, 10, or 20");
        return false;
    }
    if (config->cyclotron_effect > CAKE_EFFECT_COLOR_SHIFT) {
        set_error(error, error_size, "unsupported cyclotron_effect");
        return false;
    }
    if (config->cake_reverse > 1 || config->cyclotron_reverse > 1 ||
        config->lid_bypass > 1) {
        set_error(error, error_size, "boolean setting must be true or false");
        return false;
    }
    return true;
}

// Apply whitespace-separated key=value updates to a base configuration, then
// normalize legacy request versions and validate the complete result.
bool user_config_parse_update(const char *settings,
                              const user_config_t *base,
                              uint8_t outer_color_count,
                              user_config_t *result,
                              char *error,
                              size_t error_size) {
    char buffer[CONFIG_PARSE_BUFFER_SIZE];
    if (strlen(settings) >= sizeof(buffer)) {
        set_error(error, error_size, "configuration command is too long");
        return false;
    }

    snprintf(buffer, sizeof(buffer), "%s", settings);
    *result = *base;

    char *save = NULL;
    for (char *token = strtok_r(buffer, " \t", &save);
         token != NULL;
         token = strtok_r(NULL, " \t", &save)) {
        char *separator = strchr(token, '=');
        if (separator == NULL || separator == token ||
            separator[1] == '\0') {
            set_error(error, error_size, "expected key=value settings");
            return false;
        }

        *separator = '\0';
        const char *key = token;
        const char *value = separator + 1;
        bool parsed = true;

        if (strcmp(key, "version") == 0) {
            parsed = parse_u16(value, &result->version);
        } else if (strcmp(key, "cake_led_count") == 0) {
            parsed = parse_u16(value, &result->cake_led_count);
        } else if (strcmp(key, "cake_bit_rate_khz") == 0) {
            parsed = parse_u16(value, &result->cake_bit_rate_khz);
        } else if (strcmp(key, "cake_start_offset") == 0) {
            parsed = parse_u16(value, &result->cake_start_offset);
        } else if (strcmp(key, "cyclotron_led_count") == 0) {
            uint16_t count = 0;
            parsed = parse_u16(value, &count);
            if (parsed) {
                result->cyclotron_led_style =
                    cyclotron_led_style_from_count(count);
                result->cyclotron_led_count = count;
            }
        } else if (strcmp(key, "cyclotron_bit_rate_khz") == 0) {
            parsed = parse_u16(value, &result->cyclotron_bit_rate_khz);
        } else if (strcmp(key, "cyclotron_rotation_ms") == 0) {
            parsed = parse_u16(value, &result->cyclotron_rotation_ms);
        } else if (strcmp(key, "cake_rotation_ms") == 0) {
            parsed = parse_u16(value, &result->cake_rotation_ms);
        } else if (strcmp(key, "outer_color_index") == 0) {
            parsed = parse_u8(value, &result->outer_color_index);
        } else if (strcmp(key, "cake_red") == 0) {
            parsed = parse_u8(value, &result->cake_red);
        } else if (strcmp(key, "cake_green") == 0) {
            parsed = parse_u8(value, &result->cake_green);
        } else if (strcmp(key, "cake_blue") == 0) {
            parsed = parse_u8(value, &result->cake_blue);
        } else if (strcmp(key, "cake_reverse") == 0) {
            parsed = parse_bool(value, &result->cake_reverse);
        } else if (strcmp(key, "lid_bypass") == 0) {
            parsed = parse_bool(value, &result->lid_bypass);
        } else if (strcmp(key, "cake_led_type") == 0) {
            if (strcmp(value, "WS2812B") == 0) {
                result->cake_led_type = CAKE_LED_TYPE_WS2812B;
            } else if (strcmp(value, "WS2811") == 0) {
                result->cake_led_type = CAKE_LED_TYPE_WS2811;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cake_color_order") == 0) {
            if (strcmp(value, "GRB") == 0) {
                result->cake_color_order = CAKE_COLOR_ORDER_GRB;
            } else if (strcmp(value, "RGB") == 0) {
                result->cake_color_order = CAKE_COLOR_ORDER_RGB;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cake_timing_mode") == 0) {
            if (strcmp(value, "SYNCED") == 0) {
                result->cake_timing_mode = CAKE_TIMING_SYNCED;
            } else if (strcmp(value, "FREE") == 0) {
                result->cake_timing_mode = CAKE_TIMING_FREE;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cake_speed_multiplier") == 0) {
            parsed = parse_u8(value, &result->cake_speed_multiplier);
        } else if (strcmp(key, "cake_effect") == 0) {
            if (strcmp(value, "SOLID") == 0) {
                result->cake_effect = CAKE_EFFECT_SOLID;
            } else if (strcmp(value, "FADE") == 0) {
                result->cake_effect = CAKE_EFFECT_FADE;
            } else if (strcmp(value, "TRAIL") == 0) {
                result->cake_effect = CAKE_EFFECT_TRAIL;
            } else if (strcmp(value, "COLOR_SHIFT") == 0) {
                result->cake_effect = CAKE_EFFECT_COLOR_SHIFT;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cyclotron_led_type") == 0) {
            if (strcmp(value, "WS2812B") == 0) {
                result->cyclotron_led_type = CAKE_LED_TYPE_WS2812B;
            } else if (strcmp(value, "WS2811") == 0) {
                result->cyclotron_led_type = CAKE_LED_TYPE_WS2811;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cyclotron_color_order") == 0) {
            if (strcmp(value, "GRB") == 0) {
                result->cyclotron_color_order = CAKE_COLOR_ORDER_GRB;
            } else if (strcmp(value, "RGB") == 0) {
                result->cyclotron_color_order = CAKE_COLOR_ORDER_RGB;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cyclotron_reverse") == 0) {
            parsed = parse_bool(value, &result->cyclotron_reverse);
        } else if (strcmp(key, "cyclotron_timing_mode") == 0) {
            if (strcmp(value, "SYNCED") == 0) {
                result->cyclotron_timing_mode = CAKE_TIMING_SYNCED;
            } else if (strcmp(value, "FREE") == 0) {
                result->cyclotron_timing_mode = CAKE_TIMING_FREE;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cyclotron_speed_multiplier") == 0) {
            parsed = parse_u8(value, &result->cyclotron_speed_multiplier);
        } else if (strcmp(key, "cyclotron_effect") == 0) {
            if (strcmp(value, "SOLID") == 0) {
                result->cyclotron_effect = CAKE_EFFECT_SOLID;
            } else if (strcmp(value, "FADE") == 0) {
                result->cyclotron_effect = CAKE_EFFECT_FADE;
            } else if (strcmp(value, "TRAIL") == 0) {
                result->cyclotron_effect = CAKE_EFFECT_TRAIL;
            } else if (strcmp(value, "COLOR_SHIFT") == 0) {
                result->cyclotron_effect = CAKE_EFFECT_COLOR_SHIFT;
            } else {
                parsed = false;
            }
        } else if (strcmp(key, "cyclotron_led_style") == 0) {
            if (strcmp(value, "SINGLE") == 0) {
                result->cyclotron_led_style = CYCLOTRON_STYLE_SINGLE;
            } else if (strcmp(value, "PUCK3") == 0) {
                result->cyclotron_led_style = CYCLOTRON_STYLE_PUCK_3;
            } else if (strcmp(value, "PUCK5") == 0) {
                result->cyclotron_led_style = CYCLOTRON_STYLE_PUCK_5;
            } else if (strcmp(value, "PUCK9") == 0) {
                result->cyclotron_led_style = CYCLOTRON_STYLE_PUCK_9;
            } else {
                parsed = false;
            }
        } else {
            snprintf(error, error_size, "unknown setting: %s", key);
            return false;
        }

        if (!parsed) {
            snprintf(error, error_size, "invalid value for %s", key);
            return false;
        }
    }

    // Older configurators may send a complete version 1, 2, or 3 update. Keep
    // the new cyclotron fields from the base configuration and normalize the
    // record to the current schema before validation and persistence.
    if (result->version >= 1 && result->version < USER_CONFIG_VERSION) {
        result->version = USER_CONFIG_VERSION;
    }
    result->cyclotron_led_count =
        cyclotron_led_count_for_style(result->cyclotron_led_style);
    return user_config_validate(result, outer_color_count, error, error_size);
}
