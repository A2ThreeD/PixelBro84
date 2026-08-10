/*
 * PixelBro84
 * Copyright (c) 2026 Aaron Morris / A2ThreeD.
 *
 * Version and release information: src/project_info.h
 * License terms: LICENSE
 * Third-party licenses: THIRD_PARTY_NOTICES.md
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include(<hardware/clocks.h>)
#    include <hardware/clocks.h>
#  else
#    include <stdbool.h>
#    include <stdint.h>
extern bool set_sys_clock_khz(uint32_t sys_clock_khz, bool required);
extern uint32_t clock_get_hz(uint32_t clk);
static const uint32_t clk_sys = 1u;
#  endif
#else
#  include <hardware/clocks.h>
#endif
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/io_qspi.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/binary_info.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "project_info.h"
#include "user_config.h"
#include "ws2812_rx.pio.h"
#include "ws2812_tx.pio.h"

// Compile-time feature switches and firmware metadata exposed to picotool.
#ifndef ENABLE_USB_DIAGNOSTICS
#define ENABLE_USB_DIAGNOSTICS 0
#endif

bi_decl(bi_program_name(PROJECT_NAME));
bi_decl(bi_program_version_string(PROJECT_VERSION_STRING));
bi_decl(bi_program_description(PROJECT_CHANGE_SUMMARY));

// Waveshare RP2040-Zero pins. Change these if your wiring uses other GPIOs.
#define WS2812_INPUT_PIN  29u
#define WS2812_OUTPUT_PIN 3u
#define CAKE_OUTPUT_PIN   27u
#define LID_SENSE_PIN     4u
#define LID_OUTPUT_PIN    28u

// Set true to permanently force bypass regardless of the saved setting.
#define LID_DETECTION_BYPASS false

// WS2812 timing, frame dimensions, and input polling intervals.
#define WS2812_BIT_RATE 800000.0f
#define WS2812_RX_CLOCK 8000000.0f
#define SYSTEM_CLOCK_KHZ 48000u
#define WS2812_RESET_US 80u
#define MAX_FRAME_PIXELS 256u
#define DIAGNOSTIC_QUEUE_DEPTH 2u
#define HASBRO_INPUT_PIXELS 12u
#define OUTPUT_LED_COUNT 4u
#define INPUTS_PER_OUTPUT 3u
#define BUTTON_POLL_MS 10u
#define BUTTON_DEBOUNCE_MS 30u
#define BUTTON_LONG_PRESS_MS 2000u
#define LID_POLL_MS 5u
#define LID_DEBOUNCE_MS 20u
#define CAKE_REFRESH_MS 5u
#define INPUT_IDLE_OFF_MS 250u
#define PREVIEW_SYNC_PHASE_MS 250u
#define COLOR_SAVE_DELAY_MS 1000u
#define CONFIG_FLASH_ON_MS 150u
#define CONFIG_FLASH_OFF_MS 120u

// The final flash sector is used as a wear-leveled settings journal.
#define COLOR_SETTINGS_MAGIC 0x434f4c52u
#define USER_SETTINGS_MAGIC 0x50424346u
#define COLOR_SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define COLOR_SETTINGS_SLOTS (FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE)

// User-selectable output colors retain the source frame's brightness.
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    const char *name;
} output_color_t;

// Each settings record occupies one flash page and includes inverted fields
// plus a checksum so interrupted or corrupt writes can be rejected at boot.
typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint8_t color_index;
    uint8_t color_inverse;
    uint8_t bypass_enabled;
    uint8_t bypass_inverse;
    uint32_t checksum;
    uint8_t padding[FLASH_PAGE_SIZE - 16];
} legacy_settings_record_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    user_config_t config;
    uint32_t checksum;
    uint8_t padding[
        FLASH_PAGE_SIZE - 12 - sizeof(user_config_t)];
} user_settings_record_t;

// Firmware 1.2 used version 3 of the user configuration. Keep its exact
// layout so an upgrade retains the existing four-pixel cyclotron setup.
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
    uint16_t cyclotron_led_count;
    uint16_t cyclotron_bit_rate_khz;
    uint16_t cyclotron_rotation_ms;
    uint8_t cyclotron_led_type;
    uint8_t cyclotron_color_order;
    uint8_t cyclotron_reverse;
    uint8_t cyclotron_timing_mode;
    uint8_t cyclotron_speed_multiplier;
    uint8_t cyclotron_effect;
    uint8_t cyclotron_reserved[4];
} user_config_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    user_config_v3_t config;
    uint32_t checksum;
    uint8_t padding[
        FLASH_PAGE_SIZE - 12 - sizeof(user_config_v3_t)];
} user_settings_record_v3_t;

// Firmware 1.2 used version 2 of the user configuration. Keep its exact
// layout so an upgrade can retain all existing Cake settings.
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
} user_config_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    user_config_v2_t config;
    uint32_t checksum;
    uint8_t padding[
        FLASH_PAGE_SIZE - 12 - sizeof(user_config_v2_t)];
} user_settings_record_v2_t;

// Firmware 1.0/1.1 used version 1 of the user configuration.
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
} user_config_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    user_config_v1_t config;
    uint32_t checksum;
    uint8_t padding[
        FLASH_PAGE_SIZE - 12 - sizeof(user_config_v1_t)];
} user_settings_record_v1_t;

typedef struct {
    uint32_t flash_offset;
    bool erase_sector;
    user_settings_record_t record;
} settings_flash_write_t;

_Static_assert(sizeof(legacy_settings_record_t) == FLASH_PAGE_SIZE,
               "Legacy setting must occupy exactly one flash page");
_Static_assert(sizeof(user_settings_record_t) == FLASH_PAGE_SIZE,
               "User setting must occupy exactly one flash page");
_Static_assert(sizeof(user_settings_record_v3_t) == FLASH_PAGE_SIZE,
               "Version 3 user setting must occupy exactly one flash page");
_Static_assert(sizeof(user_settings_record_v1_t) == FLASH_PAGE_SIZE,
               "Version 1 user setting must occupy exactly one flash page");
_Static_assert(sizeof(user_settings_record_v2_t) == FLASH_PAGE_SIZE,
               "Version 2 user setting must occupy exactly one flash page");

static const output_color_t output_colors[] = {
    {255, 0, 0, "Red"},
    {0, 255, 0, "Green"},
    {0, 0, 255, "Blue"},
    {255, 255, 0, "Yellow"},
    {128, 0, 255, "Purple"},
};

#define OUTPUT_COLOR_COUNT (sizeof(output_colors) / sizeof(output_colors[0]))

// A captured source frame is also the unit passed to optional diagnostics.
typedef struct {
    uint32_t number;
    uint32_t pixels[MAX_FRAME_PIXELS];
    uint16_t pixel_count;
    uint16_t cake_led_index;
    uint8_t color_index;
    uint8_t cake_brightness;
    uint8_t lid_state;
    bool truncated;
} diagnostic_frame_t;

// Shared chase state for both output chains. Hardware-specific renderers own
// separate instances but use the same phase clock and output-cache fields.
typedef struct {
    absolute_time_t phase_started_at;
    absolute_time_t free_started_at;
    absolute_time_t tx_ready_at;
    uint32_t phase_duration_us[OUTPUT_LED_COUNT];
    uint32_t fallback_duration_us;
    uint16_t output_led_index;
    uint32_t sync_cycle_count;
    uint32_t output_rotation;
    uint8_t current_phase;
    uint8_t valid_duration_mask;
    uint8_t brightness;
    uint8_t output_brightness;
    uint8_t output_effect_level;
    bool initialized;
    bool phase_start_known;
    bool output_valid;
} chase_state_t;

typedef struct {
    uint32_t rotation;
    uint16_t logical_index;
    uint8_t step_progress;
} chase_animation_sample_t;

#define CONFIG_PROTOCOL_VERSION 4u
#define CONFIG_COMMAND_MAX 1024u
#define CONFIG_MESSAGE_MAX 1024u
#define CONFIG_QUEUE_DEPTH 4u

typedef enum {
    CONFIG_REQUEST_GET,
    CONFIG_REQUEST_SET,
    CONFIG_REQUEST_TEST,
    CONFIG_REQUEST_CLEAR_TEST,
    CONFIG_REQUEST_PREVIEW_START,
    CONFIG_REQUEST_PREVIEW_STOP,
} config_request_kind_t;

typedef enum {
    TEST_TARGET_CAKE,
    TEST_TARGET_CYCLOTRON,
} test_target_t;

typedef struct {
    config_request_kind_t kind;
    user_config_t config;
    test_target_t test_target;
    uint16_t test_led_index;
    uint16_t test_duration_ms;
    uint8_t test_red;
    uint8_t test_green;
    uint8_t test_blue;
    uint8_t test_color_index;
} config_request_t;

typedef struct {
    char text[CONFIG_MESSAGE_MAX];
} config_response_t;

// Mutable deadlines and debounce state owned by the core-0 event loop.
typedef struct {
    uint32_t frame_number;
    absolute_time_t last_pixel_time;
    absolute_time_t next_idle_off;
    absolute_time_t next_button_poll;
    absolute_time_t button_changed_at;
    absolute_time_t button_pressed_at;
    absolute_time_t next_lid_poll;
    absolute_time_t lid_changed_at;
    absolute_time_t next_cyclotron_refresh;
    absolute_time_t next_cake_refresh;
    absolute_time_t color_save_at;
    absolute_time_t config_flash_at;
    bool receiving_frame;
    bool button_raw_pressed;
    bool button_pressed;
    bool button_long_press_handled;
    bool lid_raw_closed;
    bool lid_closed;
    bool color_save_pending;
    uint8_t config_flash_phase;
} runtime_state_t;

// State shared by frame capture, settings persistence, and the second core.
static diagnostic_frame_t capture_frame;
static chase_state_t cake_chase;
static chase_state_t cyclotron_chase;
static queue_t config_request_queue;
static queue_t config_response_queue;
static absolute_time_t cake_test_until;
static bool cake_test_active;
static absolute_time_t cyclotron_test_until;
static bool cyclotron_test_active;
// WS2812 pixels retain their last frame. Track whether a clear has already
// been latched so an idle input does not continuously transmit zero frames.
// Repeated "off" traffic can itself be misread by marginal 3.3 V data links.
static bool cyclotron_output_is_off;
#if ENABLE_USB_DIAGNOSTICS
static queue_t diagnostic_queue;
static diagnostic_frame_t serial_frame;
static diagnostic_frame_t previous_serial_frame;
static bool previous_serial_frame_valid;
#endif
static user_config_t user_config;
static user_config_t preview_saved_config;
static bool preview_active;
static absolute_time_t preview_next_phase_at;
static uint8_t preview_phase;
static diagnostic_frame_t preview_frame;
static uint32_t settings_sequence;
static uint settings_next_slot;
static settings_flash_write_t settings_flash_write;
static PIO rx_wake_pio;
static uint rx_wake_sm;
static repeating_timer_t idle_wake_timer;

static void output_cyclotron_off(PIO pio, uint sm);

enum {
    LID_STATE_OPEN,
    LID_STATE_CLOSED,
    LID_STATE_BYPASSED,
};

// Wake-up support lets the main core sleep between incoming pixels and polls.
// Keep the repeating alarm active; its interrupt wakes the main core for
// button, lid, preview, and idle-output deadlines.
static bool idle_wake_timer_callback(repeating_timer_t *timer) {
    (void)timer;
    return true;
}

// Disable the one-shot PIO RX wake source after incoming data wakes core 0.
static void pio_rx_wake_irq_handler(void) {
    pio_set_irq0_source_enabled(
        rx_wake_pio,
        pio_get_rx_fifo_not_empty_interrupt_source(rx_wake_sm),
        false);
}

// Install the PIO RX interrupt used to wake the main loop from __wfi().
static void pio_rx_wake_init(PIO pio, uint sm) {
    rx_wake_pio = pio;
    rx_wake_sm = sm;
    pio_set_irq0_source_enabled(
        pio, pio_get_rx_fifo_not_empty_interrupt_source(sm), false);
    irq_set_exclusive_handler(PIO_IRQ_NUM(pio, 0), pio_rx_wake_irq_handler);
    irq_set_enabled(PIO_IRQ_NUM(pio, 0), true);
}

// Sleep atomically until either a complete input pixel or the poll timer
// arrives, avoiding the race between checking the FIFO and entering __wfi().
static void sleep_until_input_or_timer(PIO pio, uint sm) {
    const uint32_t flags = save_and_disable_interrupts();
    pio_set_irq0_source_enabled(
        pio, pio_get_rx_fifo_not_empty_interrupt_source(sm), true);
    if (pio_sm_is_rx_fifo_empty(pio, sm)) {
        __wfi();
    }
    restore_interrupts(flags);
}

// Lid control uses an open-drain-style output: drive low when active and
// switch to a high-impedance input when inactive.
static void set_lid_output(bool pull_low) {
    // The output latch always stays low. Direction controls whether the pin
    // actively sinks the line or presents a high-impedance input.
    gpio_put(LID_OUTPUT_PIN, false);
    gpio_set_dir(LID_OUTPUT_PIN, pull_low ? GPIO_OUT : GPIO_IN);
}

// Combine the compile-time override with the user's saved bypass preference.
static bool lid_bypass_active(void) {
    return LID_DETECTION_BYPASS || user_config.lid_bypass != 0;
}

// Initialize the lid sense input and its open-drain-style mirrored output.
static void lid_switch_init(void) {
    gpio_init(LID_OUTPUT_PIN);
    gpio_disable_pulls(LID_OUTPUT_PIN);
    gpio_put(LID_OUTPUT_PIN, false);
    gpio_set_dir(LID_OUTPUT_PIN, GPIO_IN);

    gpio_init(LID_SENSE_PIN);
    gpio_set_dir(LID_SENSE_PIN, GPIO_IN);
    gpio_pull_up(LID_SENSE_PIN);

    if (lid_bypass_active()) {
        set_lid_output(true);
    }
}

// BOOTSEL shares the QSPI chip-select pin, so reads must run from RAM while
// core 1 is locked out from flash access.
static bool __no_inline_not_in_flash_func(read_bootsel_pressed_raw)(void) {
    const uint cs_pin_index = 1;
    const uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int delay = 0; delay < 1000; ++delay) {
    }

    const bool pressed =
        (sio_hw->gpio_hi_in & (1u << cs_pin_index)) == 0;

    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

// Coordinate both cores around the RAM-only QSPI/BOOTSEL sampling routine.
static bool read_bootsel_pressed(void) {
    multicore_lockout_start_blocking();
    const bool pressed = read_bootsel_pressed_raw();
    multicore_lockout_end_blocking();
    return pressed;
}

// Settings validation accepts the previous color-only journal format so an
// upgrade preserves the user's existing outer color and lid-bypass choice.
// Reproduce the checksum used by the earliest color-only journal records.
static uint32_t legacy_color_settings_checksum(uint32_t sequence,
                                               uint8_t color_index) {
    return COLOR_SETTINGS_MAGIC ^ sequence ^ color_index ^ 0xa5c35a3cu;
}

// Extend the legacy checksum with the later lid-bypass field.
static uint32_t color_settings_checksum(uint32_t sequence,
                                        uint8_t color_index,
                                        bool bypass_enabled) {
    return legacy_color_settings_checksum(sequence, color_index) ^
           ((uint32_t)bypass_enabled << 24) ^ 0x19b40000u;
}

// Validate either legacy record variant and report whether it predates the
// lid-bypass field.
static bool legacy_settings_record_valid(
    const legacy_settings_record_t *record, bool *color_only) {
    const bool common_valid =
        record->magic == COLOR_SETTINGS_MAGIC &&
        record->color_index < OUTPUT_COLOR_COUNT &&
        record->color_inverse == (uint8_t)~record->color_index;
    if (!common_valid) {
        return false;
    }

    if (record->bypass_enabled == 0xffu &&
        record->bypass_inverse == 0xffu &&
        record->checksum == legacy_color_settings_checksum(
                                record->sequence, record->color_index)) {
        *color_only = true;
        return true;
    }

    *color_only = false;
    return record->magic == COLOR_SETTINGS_MAGIC &&
           record->bypass_enabled <= 1u &&
           record->bypass_inverse == (uint8_t)~record->bypass_enabled &&
           record->checksum ==
               color_settings_checksum(record->sequence, record->color_index,
                                       record->bypass_enabled != 0);
}

// Hash the current saved-schema bytes and journal sequence with FNV-1a.
static uint32_t user_settings_checksum(uint32_t sequence,
                                       const user_config_t *config) {
    uint32_t hash = 2166136261u;
    const uint8_t *sequence_bytes = (const uint8_t *)&sequence;
    const uint8_t *config_bytes = (const uint8_t *)config;

    for (size_t index = 0; index < sizeof(sequence); ++index) {
        hash = (hash ^ sequence_bytes[index]) * 16777619u;
    }
    for (size_t index = 0; index < sizeof(*config); ++index) {
        hash = (hash ^ config_bytes[index]) * 16777619u;
    }
    return hash ^ USER_SETTINGS_MAGIC;
}

// Reproduce schema-1 checksums over that version's exact packed layout.
static uint32_t user_settings_checksum_v1(
    uint32_t sequence, const user_config_v1_t *config) {
    uint32_t hash = 2166136261u;
    const uint8_t *sequence_bytes = (const uint8_t *)&sequence;
    const uint8_t *config_bytes = (const uint8_t *)config;

    for (size_t index = 0; index < sizeof(sequence); ++index) {
        hash = (hash ^ sequence_bytes[index]) * 16777619u;
    }
    for (size_t index = 0; index < sizeof(*config); ++index) {
        hash = (hash ^ config_bytes[index]) * 16777619u;
    }
    return hash ^ USER_SETTINGS_MAGIC;
}

// Reproduce schema-2 checksums over that version's exact packed layout.
static uint32_t user_settings_checksum_v2(
    uint32_t sequence, const user_config_v2_t *config) {
    uint32_t hash = 2166136261u;
    const uint8_t *sequence_bytes = (const uint8_t *)&sequence;
    const uint8_t *config_bytes = (const uint8_t *)config;

    for (size_t index = 0; index < sizeof(sequence); ++index) {
        hash = (hash ^ sequence_bytes[index]) * 16777619u;
    }
    for (size_t index = 0; index < sizeof(*config); ++index) {
        hash = (hash ^ config_bytes[index]) * 16777619u;
    }
    return hash ^ USER_SETTINGS_MAGIC;
}

// Reproduce schema-3 checksums over that version's exact packed layout.
static uint32_t user_settings_checksum_v3(
    uint32_t sequence, const user_config_v3_t *config) {
    uint32_t hash = 2166136261u;
    const uint8_t *sequence_bytes = (const uint8_t *)&sequence;
    const uint8_t *config_bytes = (const uint8_t *)config;

    for (size_t index = 0; index < sizeof(sequence); ++index) {
        hash = (hash ^ sequence_bytes[index]) * 16777619u;
    }
    for (size_t index = 0; index < sizeof(*config); ++index) {
        hash = (hash ^ config_bytes[index]) * 16777619u;
    }
    return hash ^ USER_SETTINGS_MAGIC;
}

// Validate a current-schema journal page before accepting it at boot.
static bool user_settings_record_valid(
    const user_settings_record_t *record) {
    char error[1];
    return record->magic == USER_SETTINGS_MAGIC &&
           user_config_validate(&record->config, OUTPUT_COLOR_COUNT,
                                error, sizeof(error)) &&
           record->checksum ==
               user_settings_checksum(record->sequence, &record->config);
}

// Validate a schema-1 page without reinterpreting it as the current struct.
static bool user_settings_record_v1_valid(
    const user_settings_record_v1_t *record) {
    const user_config_v1_t *config = &record->config;
    return record->magic == USER_SETTINGS_MAGIC &&
           config->version == 1u &&
           config->cake_led_count > 0 &&
           config->cake_led_count <= CAKE_LED_COUNT_MAX &&
           (config->cake_bit_rate_khz == 400 ||
            config->cake_bit_rate_khz == 800) &&
           config->cake_start_offset < config->cake_led_count &&
           config->outer_color_index < OUTPUT_COLOR_COUNT &&
           config->cake_led_type <= CAKE_LED_TYPE_WS2811 &&
           config->cake_color_order <= CAKE_COLOR_ORDER_RGB &&
           config->cake_reverse <= 1 && config->lid_bypass <= 1 &&
           record->checksum ==
               user_settings_checksum_v1(record->sequence, config);
}

// Validate a schema-2 page without reinterpreting it as the current struct.
static bool user_settings_record_v2_valid(
    const user_settings_record_v2_t *record) {
    const user_config_v2_t *config = &record->config;
    return record->magic == USER_SETTINGS_MAGIC &&
           config->version == 2u &&
           config->cake_led_count > 0 &&
           config->cake_led_count <= CAKE_LED_COUNT_MAX &&
           (config->cake_bit_rate_khz == 400 ||
            config->cake_bit_rate_khz == 800) &&
           config->cake_start_offset < config->cake_led_count &&
           config->cake_rotation_ms >= CAKE_ROTATION_MS_MIN &&
           config->cake_rotation_ms <= CAKE_ROTATION_MS_MAX &&
           config->outer_color_index < OUTPUT_COLOR_COUNT &&
           config->cake_led_type <= CAKE_LED_TYPE_WS2811 &&
           config->cake_color_order <= CAKE_COLOR_ORDER_RGB &&
           config->cake_reverse <= 1 && config->lid_bypass <= 1 &&
           config->cake_timing_mode <= CAKE_TIMING_FREE &&
           ((config->cake_speed_multiplier >= 1 &&
             config->cake_speed_multiplier <= 5) ||
            config->cake_speed_multiplier == 10 ||
            config->cake_speed_multiplier == 20) &&
           config->cake_effect <= CAKE_EFFECT_COLOR_SHIFT &&
           record->checksum ==
               user_settings_checksum_v2(record->sequence, config);
}

// Validate a schema-3 page without reinterpreting it as the current struct.
static bool user_settings_record_v3_valid(
    const user_settings_record_v3_t *record) {
    const user_config_v3_t *config = &record->config;
    return record->magic == USER_SETTINGS_MAGIC &&
           config->version == 3u &&
           config->cake_led_count > 0 &&
           config->cake_led_count <= CAKE_LED_COUNT_MAX &&
           (config->cake_bit_rate_khz == 400 ||
            config->cake_bit_rate_khz == 800) &&
           config->cake_start_offset < config->cake_led_count &&
           config->cake_rotation_ms >= CAKE_ROTATION_MS_MIN &&
           config->cake_rotation_ms <= CAKE_ROTATION_MS_MAX &&
           config->outer_color_index < OUTPUT_COLOR_COUNT &&
           config->cake_led_type <= CAKE_LED_TYPE_WS2811 &&
           config->cake_color_order <= CAKE_COLOR_ORDER_RGB &&
           config->cake_reverse <= 1 && config->lid_bypass <= 1 &&
           config->cake_timing_mode <= CAKE_TIMING_FREE &&
           ((config->cake_speed_multiplier >= 1 &&
             config->cake_speed_multiplier <= 5) ||
            config->cake_speed_multiplier == 10 ||
            config->cake_speed_multiplier == 20) &&
           config->cake_effect <= CAKE_EFFECT_COLOR_SHIFT &&
           config->cyclotron_led_count > 0 &&
           config->cyclotron_led_count <= CYCLOTRON_WINDOW_COUNT &&
           (config->cyclotron_bit_rate_khz == 400 ||
            config->cyclotron_bit_rate_khz == 800) &&
           config->cyclotron_rotation_ms >= CAKE_ROTATION_MS_MIN &&
           config->cyclotron_rotation_ms <= CAKE_ROTATION_MS_MAX &&
           config->cyclotron_led_type <= CAKE_LED_TYPE_WS2811 &&
           config->cyclotron_color_order <= CAKE_COLOR_ORDER_RGB &&
           config->cyclotron_reverse <= 1 &&
           config->cyclotron_timing_mode <= CAKE_TIMING_FREE &&
           ((config->cyclotron_speed_multiplier >= 1 &&
             config->cyclotron_speed_multiplier <= 5) ||
            config->cyclotron_speed_multiplier == 10 ||
            config->cyclotron_speed_multiplier == 20) &&
           config->cyclotron_effect <= CAKE_EFFECT_COLOR_SHIFT &&
           record->checksum ==
               user_settings_checksum_v3(record->sequence, config);
}

// Migrate schema 3 by preserving all representable fields and translating
// its one-to-four custom cyclotron count to the protocol-4 SINGLE style.
static void migrate_user_config_v3(const user_config_v3_t *old_config,
                                   user_config_t *new_config) {
    user_config_set_defaults(new_config);
    new_config->cake_led_count = old_config->cake_led_count;
    new_config->cake_bit_rate_khz = old_config->cake_bit_rate_khz;
    new_config->cake_start_offset = old_config->cake_start_offset;
    new_config->cake_rotation_ms = old_config->cake_rotation_ms;
    new_config->outer_color_index = old_config->outer_color_index;
    new_config->cake_led_type = old_config->cake_led_type;
    new_config->cake_color_order = old_config->cake_color_order;
    new_config->cake_red = old_config->cake_red;
    new_config->cake_green = old_config->cake_green;
    new_config->cake_blue = old_config->cake_blue;
    new_config->cake_reverse = old_config->cake_reverse;
    new_config->lid_bypass = old_config->lid_bypass;
    new_config->cake_timing_mode = old_config->cake_timing_mode;
    new_config->cake_speed_multiplier = old_config->cake_speed_multiplier;
    new_config->cake_effect = old_config->cake_effect;
    new_config->cyclotron_bit_rate_khz = old_config->cyclotron_bit_rate_khz;
    new_config->cyclotron_rotation_ms = old_config->cyclotron_rotation_ms;
    new_config->cyclotron_led_type = old_config->cyclotron_led_type;
    new_config->cyclotron_color_order = old_config->cyclotron_color_order;
    new_config->cyclotron_reverse = old_config->cyclotron_reverse;
    new_config->cyclotron_timing_mode = old_config->cyclotron_timing_mode;
    new_config->cyclotron_speed_multiplier =
        old_config->cyclotron_speed_multiplier;
    new_config->cyclotron_effect = old_config->cyclotron_effect;
    // Version 3 supported a custom count from one through four. The new
    // style model represents that legacy range with four single pixels.
    new_config->cyclotron_led_style = CYCLOTRON_STYLE_SINGLE;
    new_config->cyclotron_led_count =
        cyclotron_led_count_for_style(new_config->cyclotron_led_style);
}

// Migrate schema 2 Cake settings while retaining default cyclotron settings.
static void migrate_user_config_v2(const user_config_v2_t *old_config,
                                   user_config_t *new_config) {
    user_config_set_defaults(new_config);
    new_config->cake_led_count = old_config->cake_led_count;
    new_config->cake_bit_rate_khz = old_config->cake_bit_rate_khz;
    new_config->cake_start_offset = old_config->cake_start_offset;
    new_config->cake_rotation_ms = old_config->cake_rotation_ms;
    new_config->outer_color_index = old_config->outer_color_index;
    new_config->cake_led_type = old_config->cake_led_type;
    new_config->cake_color_order = old_config->cake_color_order;
    new_config->cake_red = old_config->cake_red;
    new_config->cake_green = old_config->cake_green;
    new_config->cake_blue = old_config->cake_blue;
    new_config->cake_reverse = old_config->cake_reverse;
    new_config->lid_bypass = old_config->lid_bypass;
    new_config->cake_timing_mode = old_config->cake_timing_mode;
    new_config->cake_speed_multiplier = old_config->cake_speed_multiplier;
    new_config->cake_effect = old_config->cake_effect;
}

// Migrate schema 1 hardware/color settings and default all newer controls.
static void migrate_user_config_v1(const user_config_v1_t *old_config,
                                   user_config_t *new_config) {
    user_config_set_defaults(new_config);
    new_config->cake_led_count = old_config->cake_led_count;
    new_config->cake_bit_rate_khz = old_config->cake_bit_rate_khz;
    new_config->cake_start_offset = old_config->cake_start_offset;
    new_config->outer_color_index = old_config->outer_color_index;
    new_config->cake_led_type = old_config->cake_led_type;
    new_config->cake_color_order = old_config->cake_color_order;
    new_config->cake_red = old_config->cake_red;
    new_config->cake_green = old_config->cake_green;
    new_config->cake_blue = old_config->cake_blue;
    new_config->cake_reverse = old_config->cake_reverse;
    new_config->lid_bypass = old_config->lid_bypass;
}

// Scan the flash journal for the newest valid record and the next free page.
static void load_user_setting(void) {
    const uint8_t *records =
        (const uint8_t *)(XIP_BASE + COLOR_SETTINGS_OFFSET);
    bool found = false;
    user_config_set_defaults(&user_config);
    settings_next_slot = COLOR_SETTINGS_SLOTS;

    for (uint slot = 0; slot < COLOR_SETTINGS_SLOTS; ++slot) {
        const uint8_t *page = records + slot * FLASH_PAGE_SIZE;
        const uint32_t magic = *(const uint32_t *)page;
        if (magic == 0xffffffffu &&
            settings_next_slot == COLOR_SETTINGS_SLOTS) {
            settings_next_slot = slot;
        }

        if (magic == USER_SETTINGS_MAGIC) {
            const user_settings_record_t *record =
                (const user_settings_record_t *)page;
            if (user_settings_record_valid(record) &&
                (!found || record->sequence >= settings_sequence)) {
                user_config = record->config;
                settings_sequence = record->sequence;
                found = true;
            } else {
                const user_settings_record_v3_t *v3_record =
                    (const user_settings_record_v3_t *)page;
                if (user_settings_record_v3_valid(v3_record) &&
                    (!found ||
                     v3_record->sequence >= settings_sequence)) {
                    migrate_user_config_v3(
                        &v3_record->config, &user_config);
                    settings_sequence = v3_record->sequence;
                    found = true;
                } else {
                    const user_settings_record_v2_t *v2_record =
                        (const user_settings_record_v2_t *)page;
                    if (user_settings_record_v2_valid(v2_record) &&
                        (!found ||
                         v2_record->sequence >= settings_sequence)) {
                        migrate_user_config_v2(
                            &v2_record->config, &user_config);
                        settings_sequence = v2_record->sequence;
                        found = true;
                    } else {
                        const user_settings_record_v1_t *old_record =
                            (const user_settings_record_v1_t *)page;
                        if (user_settings_record_v1_valid(old_record) &&
                            (!found ||
                             old_record->sequence >= settings_sequence)) {
                            migrate_user_config_v1(
                                &old_record->config, &user_config);
                            settings_sequence = old_record->sequence;
                            found = true;
                        }
                    }
                }
            }
        } else if (magic == COLOR_SETTINGS_MAGIC) {
            const legacy_settings_record_t *record =
                (const legacy_settings_record_t *)page;
            bool color_only = false;
            if (legacy_settings_record_valid(record, &color_only) &&
                (!found || record->sequence >= settings_sequence)) {
                user_config_set_defaults(&user_config);
                user_config.outer_color_index = record->color_index;
                user_config.lid_bypass =
                    color_only ? 0 : record->bypass_enabled;
                settings_sequence = record->sequence;
                found = true;
            }
        }
    }
}

// Flash erase/program code must execute from RAM because XIP is unavailable
// while the onboard flash is being modified.
static void __not_in_flash_func(write_user_setting_flash)(void *parameter) {
    const settings_flash_write_t *write =
        (const settings_flash_write_t *)parameter;
    if (write->erase_sector) {
        flash_range_erase(COLOR_SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    }
    flash_range_program(write->flash_offset,
                        (const uint8_t *)&write->record,
                        FLASH_PAGE_SIZE);
}

// Append a validated settings record, restarting the journal when it is full.
static bool save_user_setting(void) {
    const bool erase_sector = settings_next_slot >= COLOR_SETTINGS_SLOTS;
    const uint slot = erase_sector ? 0 : settings_next_slot;

    memset(&settings_flash_write.record, 0xff,
           sizeof(settings_flash_write.record));
    settings_flash_write.erase_sector = erase_sector;
    settings_flash_write.flash_offset =
        COLOR_SETTINGS_OFFSET + slot * FLASH_PAGE_SIZE;
    settings_flash_write.record.magic = USER_SETTINGS_MAGIC;
    settings_flash_write.record.sequence = ++settings_sequence;
    settings_flash_write.record.config = user_config;
    settings_flash_write.record.checksum = user_settings_checksum(
        settings_flash_write.record.sequence, &user_config);

    const int result = flash_safe_execute(write_user_setting_flash,
                                          &settings_flash_write, 1000u);
    if (result != PICO_OK) {
        --settings_sequence;
        return false;
    }

    settings_next_slot = slot + 1;
    return true;
}

// Configure a PIO state machine to decode incoming 24-bit GRB pixels.
static void ws2812_rx_init(PIO pio, uint sm, uint offset, uint pin) {
    pio_gpio_init(pio, pin);
    gpio_pull_down(pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false);

    pio_sm_config config = ws2812_rx_program_get_default_config(offset);
    sm_config_set_in_pins(&config, pin);
    sm_config_set_jmp_pin(&config, pin);
    sm_config_set_in_shift(&config, false, true, 24);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&config,
                         (float)clock_get_hz(clk_sys) / WS2812_RX_CLOCK);

    pio_sm_init(pio, sm, offset, &config);
}

// A continuous low interval marks the boundary between WS2812 frames.
static void wait_for_ws2812_reset(uint pin) {
    uint64_t low_started_us = 0;
    while (true) {
        const uint64_t now_us = time_us_64();
        if (!gpio_get(pin)) {
            if (low_started_us == 0) {
                low_started_us = now_us;
            } else if (now_us - low_started_us >= WS2812_RESET_US) {
                return;
            }
        } else {
            low_started_us = 0;
        }
        tight_loop_contents();
    }
}

// Reset the RX state machine at a detected frame boundary so the next word
// begins on the first bit of a new WS2812 frame.
static void restart_ws2812_rx(PIO pio, uint sm, uint offset) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm,
                pio_encode_jmp(offset + ws2812_rx_wrap_target));
    pio_sm_set_enabled(pio, sm, true);
}

// Configure a PIO state machine to transmit 24-bit pixels at the requested
// WS2811/WS2812-compatible bit rate.
static void ws2812_tx_init(PIO pio, uint sm, uint offset, uint pin,
                           uint32_t bit_rate_hz) {
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);

    pio_sm_config config = ws2812_tx_program_get_default_config(offset);
    sm_config_set_sideset_pins(&config, pin);
    sm_config_set_out_shift(&config, false, true, 24);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config,
                         (float)clock_get_hz(clk_sys) /
                             ((float)bit_rate_hz * 10.0f));

    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_enabled(pio, sm, true);
}

// Reconfigure an existing TX state machine after a runtime bit-rate change.
static void ws2812_tx_set_bit_rate(PIO pio, uint sm,
                                   uint32_t bit_rate_hz) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_set_clkdiv(
        pio, sm,
        (float)clock_get_hz(clk_sys) / ((float)bit_rate_hz * 10.0f));
    pio_sm_clkdiv_restart(pio, sm);
    pio_sm_set_enabled(pio, sm, true);
}

// Color conversion helpers reduce an input pixel to brightness, then apply
// that brightness to the selected output color.
// Return the dominant channel, which represents source intensity independent
// of the source pixel's hue.
static inline uint8_t max3(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t maximum = a > b ? a : b;
    return maximum > c ? maximum : c;
}

// Apply an 8-bit brightness factor with rounding and without overflow.
static inline uint8_t scale_channel(uint8_t channel, uint8_t brightness) {
    return (uint8_t)(((uint16_t)channel * brightness + 127u) / 255u);
}

// Extract brightness from one packed GRB source pixel.
static uint8_t input_brightness(uint32_t input_grb) {
    const uint8_t green = (uint8_t)(input_grb >> 16);
    const uint8_t red = (uint8_t)(input_grb >> 8);
    const uint8_t blue = (uint8_t)input_grb;
    return max3(red, green, blue);
}

// Sample the center emitter assigned to one of the four source phases.
static uint8_t source_phase_brightness(const diagnostic_frame_t *frame,
                                       uint phase) {
    // The four factory lenses consume these one-based address groups:
    // 2-4, 5-7, 8-10, and 11-12 plus 1 (wrapping around the frame).
    // Use each group's center emitter (inputs 3, 6, 9, and 12). Averaging
    // causes adjacent outputs 4 and 1 to overlap during the wrap transition.
    const uint center_input_index =
        2u + phase * INPUTS_PER_OUTPUT;
    return input_brightness(frame->pixels[center_input_index]);
}

// Map a logical cyclotron window directly to its source-phase brightness.
static uint8_t mapped_brightness(const diagnostic_frame_t *frame,
                                 uint window_index) {
    return source_phase_brightness(frame, window_index);
}

// Find the currently dominant source phase while retaining the prior phase
// on ties to avoid direction jitter during overlapping transitions.
static uint8_t brightest_cyclotron_phase(
    const diagnostic_frame_t *frame, uint8_t retained_phase,
    uint8_t *brightness) {
    uint8_t brightest_phase = retained_phase;
    uint8_t brightest = source_phase_brightness(frame, retained_phase);

    // Retain the current phase on equal brightness to prevent an overlap
    // frame from bouncing the chase backward and forward.
    for (uint8_t phase = 0; phase < OUTPUT_LED_COUNT; ++phase) {
        const uint8_t candidate = source_phase_brightness(frame, phase);
        if (candidate > brightest) {
            brightest = candidate;
            brightest_phase = phase;
        }
    }

    *brightness = brightest;
    return brightest_phase;
}

// Convert a synchronized or free-running chase clock into a logical LED,
// fractional step progress, and completed-rotation count.
static chase_animation_sample_t chase_animation_sample(
    const chase_state_t *chase, absolute_time_t now, uint8_t timing_mode,
    uint16_t rotation_ms, uint8_t speed_multiplier,
    uint16_t logical_led_count) {
    uint64_t rotation_progress_us = 0;
    uint64_t rotation_duration_us = 0;
    uint64_t completed_rotations = 0;

    if (timing_mode == CAKE_TIMING_FREE) {
        rotation_duration_us = (uint64_t)rotation_ms * 1000u;
        const int64_t measured_us =
            absolute_time_diff_us(chase->free_started_at, now);
        const uint64_t elapsed_us =
            measured_us > 0 ? (uint64_t)measured_us : 0u;
        completed_rotations = elapsed_us / rotation_duration_us;
        rotation_progress_us = elapsed_us % rotation_duration_us;
    } else {
        const uint8_t current_phase = chase->current_phase;
        uint32_t phase_duration_us = chase->fallback_duration_us;
        if ((chase->valid_duration_mask & (1u << current_phase)) != 0) {
            phase_duration_us = chase->phase_duration_us[current_phase];
        }

        uint64_t elapsed_us = 0;
        if (phase_duration_us > 0) {
            const int64_t measured_us =
                absolute_time_diff_us(chase->phase_started_at, now);
            if (measured_us > 0) {
                elapsed_us = (uint64_t)measured_us;
            }
            if (elapsed_us >= phase_duration_us) {
                elapsed_us = phase_duration_us - 1u;
            }
        }

        rotation_duration_us =
            (uint64_t)OUTPUT_LED_COUNT * phase_duration_us;
        if (rotation_duration_us == 0) {
            return (chase_animation_sample_t){
                .rotation = chase->sync_cycle_count * speed_multiplier,
                .logical_index = (uint16_t)(
                    ((uint32_t)current_phase * logical_led_count) /
                    OUTPUT_LED_COUNT),
                .step_progress = 0,
            };
        }

        const uint64_t outer_progress_us =
            (uint64_t)current_phase * phase_duration_us + elapsed_us;
        const uint64_t scaled_progress_us =
            outer_progress_us * speed_multiplier;
        completed_rotations =
            (uint64_t)chase->sync_cycle_count * speed_multiplier +
            scaled_progress_us / rotation_duration_us;
        rotation_progress_us = scaled_progress_us % rotation_duration_us;
    }

    const uint64_t pixel_progress =
        rotation_progress_us * logical_led_count;
    return (chase_animation_sample_t){
        .rotation = (uint32_t)completed_rotations,
        .logical_index =
            (uint16_t)(pixel_progress / rotation_duration_us),
        .step_progress = (uint8_t)(
            ((pixel_progress % rotation_duration_us) * 255u) /
            rotation_duration_us),
    };
}

// Sample the Cake animation using its configured clock and physical LED count.
static chase_animation_sample_t cake_chase_sample(absolute_time_t now) {
    return chase_animation_sample(
        &cake_chase, now, user_config.cake_timing_mode,
        user_config.cake_rotation_ms, user_config.cake_speed_multiplier,
        user_config.cake_led_count);
}

// Apply the configured Cake start offset and direction to a logical index.
static uint16_t cake_physical_led_index(uint16_t logical_index) {
    const uint16_t count = user_config.cake_led_count;
    if (user_config.cake_reverse != 0) {
        return (uint16_t)(
            (user_config.cake_start_offset + count - logical_index) % count);
    }
    return (uint16_t)(
        (user_config.cake_start_offset + logical_index) % count);
}

// Learn source-phase timing and brightness, then update the Cake chase state
// used by periodic rendering between incoming frames.
static void update_cake_chase(diagnostic_frame_t *frame,
                              absolute_time_t now) {
    uint8_t brightness = 0;
    const uint8_t retained_phase =
        cake_chase.initialized ? cake_chase.current_phase : 0;
    const uint8_t phase =
        brightest_cyclotron_phase(frame, retained_phase, &brightness);

    if (!cake_chase.initialized) {
        if (brightness == 0) {
            frame->cake_led_index = 0;
            frame->cake_brightness = 0;
            return;
        }

        cake_chase.initialized = true;
        cake_chase.current_phase = phase;
        cake_chase.phase_started_at = now;
        cake_chase.free_started_at = now;
    } else if (brightness > 0 && phase != cake_chase.current_phase) {
        if (phase < cake_chase.current_phase) {
            ++cake_chase.sync_cycle_count;
        }
        const int64_t measured_us =
            absolute_time_diff_us(cake_chase.phase_started_at, now);

        // The first observed phase may have begun before startup, so discard
        // that partial interval. Every later transition supplies full timing.
        if (cake_chase.phase_start_known && measured_us > 0) {
            const uint32_t duration_us =
                measured_us > UINT32_MAX ? UINT32_MAX
                                         : (uint32_t)measured_us;
            cake_chase.phase_duration_us[cake_chase.current_phase] =
                duration_us;
            cake_chase.fallback_duration_us = duration_us;
            cake_chase.valid_duration_mask |=
                (uint8_t)(1u << cake_chase.current_phase);
        }

        cake_chase.phase_start_known = true;
        cake_chase.current_phase = phase;
        cake_chase.phase_started_at = now;
    }

    cake_chase.brightness = brightness;
    frame->cake_led_index =
        cake_physical_led_index(cake_chase_sample(now).logical_index);
    frame->cake_brightness = brightness;
}

// Recolor a source brightness with one of the fixed cyclotron palette colors.
static uint32_t recolored_grb(const output_color_t *color,
                              uint8_t brightness) {
    return ((uint32_t)scale_channel(color->green, brightness) << 16) |
           ((uint32_t)scale_channel(color->red, brightness) << 8) |
           scale_channel(color->blue, brightness);
}

// Pack raw Cake channels in the configured RGB or GRB wire order.
static uint32_t packed_cake_pixel(uint8_t red, uint8_t green,
                                  uint8_t blue) {
    if (user_config.cake_color_order == CAKE_COLOR_ORDER_RGB) {
        return ((uint32_t)red << 16) |
               ((uint32_t)green << 8) |
               blue;
    }
    return ((uint32_t)green << 16) |
           ((uint32_t)red << 8) |
           blue;
}

// Scale and pack the configured Cake color for one physical LED.
static uint32_t recolored_cake_pixel(uint8_t red, uint8_t green,
                                     uint8_t blue, uint8_t brightness) {
    return packed_cake_pixel(
        scale_channel(red, brightness),
        scale_channel(green, brightness),
        scale_channel(blue, brightness));
}

// Scale and pack a cyclotron palette color in its configured wire order.
static uint32_t packed_cyclotron_pixel(uint8_t red, uint8_t green,
                                       uint8_t blue, uint8_t brightness) {
    red = scale_channel(red, brightness);
    green = scale_channel(green, brightness);
    blue = scale_channel(blue, brightness);
    if (user_config.cyclotron_color_order == CAKE_COLOR_ORDER_RGB) {
        return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
    }
    return ((uint32_t)green << 16) | ((uint32_t)red << 8) | blue;
}

// Map one of four logical windows to the center pixel of its physical puck
// segment, then mirror the chain when reverse direction is enabled.
static uint16_t cyclotron_physical_led_index(uint16_t logical_index) {
    // Each window occupies one puck segment; only its middle pixel is used.
    const uint16_t segment_count =
        user_config.cyclotron_led_count / OUTPUT_LED_COUNT;
    const uint16_t active_index =
        logical_index * segment_count + segment_count / 2u;
    const uint16_t count = user_config.cyclotron_led_count;
    if (user_config.cyclotron_reverse != 0) {
        return (uint16_t)(count - 1u - active_index);
    }
    return active_index;
}

// Return the source brightness assigned to a physical cyclotron pixel; pixels
// outside the four window centers remain dark.
static uint8_t cyclotron_physical_brightness(
    const diagnostic_frame_t *frame, uint16_t physical_index) {
    for (uint window = 0; window < OUTPUT_LED_COUNT; ++window) {
        if (cyclotron_physical_led_index(window) == physical_index) {
            return mapped_brightness(frame, window);
        }
    }
    return 0;
}

// Learn source-phase timing and brightness for the configurable cyclotron
// renderer, invalidating cached output whenever the phase advances.
static void update_cyclotron_chase(const diagnostic_frame_t *frame,
                                   absolute_time_t now) {
    uint8_t brightness = 0;
    const uint8_t retained_phase =
        cyclotron_chase.initialized ? cyclotron_chase.current_phase : 0;
    const uint8_t phase =
        brightest_cyclotron_phase(frame, retained_phase, &brightness);

    if (brightness == 0) {
        cyclotron_chase.initialized = false;
        cyclotron_chase.output_valid = false;
        cyclotron_chase.brightness = 0;
        return;
    }

    if (!cyclotron_chase.initialized) {
        cyclotron_chase.initialized = true;
        cyclotron_chase.current_phase = phase;
        cyclotron_chase.phase_started_at = now;
        cyclotron_chase.free_started_at = now;
    } else if (phase != cyclotron_chase.current_phase) {
        if (phase < cyclotron_chase.current_phase) {
            ++cyclotron_chase.sync_cycle_count;
        }
        const int64_t measured_us =
            absolute_time_diff_us(cyclotron_chase.phase_started_at, now);
        if (cyclotron_chase.phase_start_known && measured_us > 0) {
            const uint32_t duration_us =
                measured_us > UINT32_MAX ? UINT32_MAX
                                         : (uint32_t)measured_us;
            cyclotron_chase.phase_duration_us[
                cyclotron_chase.current_phase] = duration_us;
            cyclotron_chase.fallback_duration_us = duration_us;
            cyclotron_chase.valid_duration_mask |=
                (uint8_t)(1u << cyclotron_chase.current_phase);
        }
        cyclotron_chase.phase_start_known = true;
        cyclotron_chase.current_phase = phase;
        cyclotron_chase.phase_started_at = now;
        // The physical active index normally changes with the phase, but
        // explicitly invalidate the rendered frame as well. This keeps the
        // reverse-direction and wraparound transitions from depending on the
        // renderer's cached-index comparison.
        cyclotron_chase.output_valid = false;
    }

    cyclotron_chase.brightness =
        user_config.cyclotron_timing_mode == CAKE_TIMING_FREE
            ? UINT8_MAX
            : source_phase_brightness(frame, phase);
}

// Sample the cyclotron animation across its four logical window positions.
static chase_animation_sample_t cyclotron_chase_sample(absolute_time_t now) {
    return chase_animation_sample(
        &cyclotron_chase, now, user_config.cyclotron_timing_mode,
        user_config.cyclotron_rotation_ms,
        user_config.cyclotron_speed_multiplier, OUTPUT_LED_COUNT);
}

// Record the earliest legal start time for the next cyclotron frame, including
// its serialized pixels and required WS2812 reset-low interval.
static void mark_cyclotron_tx_busy(absolute_time_t started_at) {
    const uint32_t frame_us =
        user_config.cyclotron_led_count *
            (24000u / user_config.cyclotron_bit_rate_khz) +
        WS2812_RESET_US;
    cyclotron_chase.tx_ready_at = delayed_by_us(started_at, frame_us);
}

// Wait for both the calculated reset deadline and an empty PIO FIFO so a new
// frame cannot merge with words still queued from the previous frame.
static absolute_time_t wait_for_cyclotron_frame_boundary(PIO pio, uint sm) {
    while (!time_reached(cyclotron_chase.tx_ready_at) ||
           !pio_sm_is_tx_fifo_empty(pio, sm)) {
        tight_loop_contents();
    }
    return get_absolute_time();
}

// Render one cyclotron animation frame when its sampled position, brightness,
// fade level, or color-shift rotation differs from the latched frame.
static void refresh_cyclotron_output(PIO pio, uint sm,
                                     absolute_time_t now) {
    if (!cyclotron_chase.initialized ||
        !time_reached(cyclotron_chase.tx_ready_at)) {
        return;
    }

    // The timestamp is a conservative guard, but the PIO FIFO is the final
    // authority. Preview refreshes can be generated without a source-frame
    // boundary, so require the previous frame to be fully drained before
    // queuing another one. This prevents an occasional merged frame from
    // leaving the cyclotron WS2812 chain latched on its trail pair.
    now = wait_for_cyclotron_frame_boundary(pio, sm);

    const chase_animation_sample_t sample =
        cyclotron_chase_sample(now);
    const uint16_t active_index =
        cyclotron_physical_led_index(sample.logical_index);
    const uint8_t effect_level =
        user_config.cyclotron_effect == CAKE_EFFECT_FADE
            ? sample.step_progress
            : 0;
    const uint32_t rendered_rotation =
        user_config.cyclotron_effect == CAKE_EFFECT_COLOR_SHIFT
            ? sample.rotation
            : 0;
    if (cyclotron_chase.output_valid &&
        active_index == cyclotron_chase.output_led_index &&
        cyclotron_chase.brightness ==
            cyclotron_chase.output_brightness &&
        effect_level == cyclotron_chase.output_effect_level &&
        rendered_rotation == cyclotron_chase.output_rotation) {
        return;
    }

    const output_color_t *color =
        &output_colors[(user_config.outer_color_index + rendered_rotation) %
                       OUTPUT_COLOR_COUNT];
    static const uint8_t trail_levels[] = {255, 144, 72, 32};
    for (uint index = 0; index < user_config.cyclotron_led_count; ++index) {
        uint8_t effect_brightness = 0;
        if (user_config.cyclotron_effect == CAKE_EFFECT_TRAIL) {
            for (uint8_t distance = 0; distance < OUTPUT_LED_COUNT;
                 ++distance) {
                const uint16_t logical_index = (uint16_t)(
                    (sample.logical_index + OUTPUT_LED_COUNT - distance) %
                    OUTPUT_LED_COUNT);
                if (index == cyclotron_physical_led_index(logical_index)) {
                    effect_brightness = trail_levels[distance];
                    break;
                }
            }
        } else if (index == active_index) {
            effect_brightness =
                user_config.cyclotron_effect == CAKE_EFFECT_FADE
                    ? (uint8_t)(255u - sample.step_progress)
                    : 255u;
        }
        pio_sm_put_blocking(
            pio, sm,
            packed_cyclotron_pixel(color->red, color->green, color->blue,
                                   scale_channel(cyclotron_chase.brightness,
                                                 effect_brightness)) << 8);
    }
    cyclotron_chase.output_led_index = active_index;
    cyclotron_chase.output_brightness = cyclotron_chase.brightness;
    cyclotron_chase.output_effect_level = effect_level;
    cyclotron_chase.output_rotation = rendered_rotation;
    cyclotron_chase.output_valid = true;
    // Start the pacing window after all words have been accepted. A blocking
    // FIFO write can outlive the timestamp captured before the frame began.
    now = get_absolute_time();
    mark_cyclotron_tx_busy(now);
    cyclotron_output_is_off = false;
}

// Emit one completed source frame to the four outer cyclotron LEDs.
static void output_cyclotron_frame(PIO pio, uint sm,
                                   const diagnostic_frame_t *frame) {
    if (frame->pixel_count != HASBRO_INPUT_PIXELS) {
        return;
    }

    update_cyclotron_chase(frame, get_absolute_time());
    if (!cyclotron_chase.initialized) {
        output_cyclotron_off(pio, sm);
        return;
    }

    if (user_config.cyclotron_timing_mode == CAKE_TIMING_SYNCED &&
        user_config.cyclotron_speed_multiplier == 1 &&
        user_config.cyclotron_effect == CAKE_EFFECT_SOLID) {
        if (!time_reached(cyclotron_chase.tx_ready_at)) return;
        const output_color_t *color =
            &output_colors[user_config.outer_color_index];
        for (uint index = 0;
             index < user_config.cyclotron_led_count; ++index) {
            const uint8_t brightness =
                cyclotron_physical_brightness(frame, index);
            pio_sm_put_blocking(
                pio, sm,
                packed_cyclotron_pixel(color->red, color->green, color->blue,
                                       brightness) << 8);
        }
        mark_cyclotron_tx_busy(get_absolute_time());
        cyclotron_chase.output_valid = false;
        cyclotron_output_is_off = false;
        return;
    }

    refresh_cyclotron_output(pio, sm, get_absolute_time());
}

// Light one of the four cyclotron outputs at full selected-color brightness.
static void output_cyclotron_test(PIO pio, uint sm,
                                  const config_request_t *request,
                                  absolute_time_t now) {
    // A WS2812 frame must be followed by its reset-low interval before the
    // next frame starts. Test clicks can arrive back-to-back, so use the
    // same transmit pacing as the normal cyclotron output path.
    now = wait_for_cyclotron_frame_boundary(pio, sm);

    const output_color_t *color =
        &output_colors[request->test_color_index];
    const uint16_t active_index =
        cyclotron_physical_led_index(request->test_led_index);
    for (uint index = 0; index < user_config.cyclotron_led_count; ++index) {
        const uint32_t pixel = index == active_index
                                   ? packed_cyclotron_pixel(
                                         color->red, color->green,
                                         color->blue, UINT8_MAX)
                                   : 0u;
        pio_sm_put_blocking(pio, sm, pixel << 8);
    }
    now = get_absolute_time();
    mark_cyclotron_tx_busy(now);
    cyclotron_chase.output_valid = false;
    cyclotron_test_active = true;
    cyclotron_output_is_off = false;
    cyclotron_test_until =
        delayed_by_ms(now, request->test_duration_ms);
}

// Latch a zero frame once and reset the cyclotron render cache. Repeated calls
// are suppressed because WS2812 pixels retain the already-latched off state.
static void output_cyclotron_off(PIO pio, uint sm) {
    if (cyclotron_output_is_off) {
        return;
    }
    wait_for_cyclotron_frame_boundary(pio, sm);
    for (uint index = 0; index < user_config.cyclotron_led_count; ++index) {
        pio_sm_put_blocking(pio, sm, 0u);
    }
    mark_cyclotron_tx_busy(get_absolute_time());
    cyclotron_chase.output_valid = false;
    cyclotron_output_is_off = true;
}

// Record the earliest legal start time for the next Cake frame.
static void mark_cake_tx_busy(absolute_time_t started_at) {
    const uint32_t frame_us =
        user_config.cake_led_count *
            (24000u / user_config.cake_bit_rate_khz) +
        WS2812_RESET_US;
    cake_chase.tx_ready_at = delayed_by_us(started_at, frame_us);
}

// Render one Cake animation frame when its position, brightness, effect level,
// or color-shift rotation differs from the latched frame.
static void refresh_cake_output(PIO pio, uint sm, absolute_time_t now) {
    if (!cake_chase.initialized || !time_reached(cake_chase.tx_ready_at)) {
        return;
    }

    const chase_animation_sample_t sample = cake_chase_sample(now);
    const uint16_t active_index =
        cake_physical_led_index(sample.logical_index);
    const uint8_t effect_level =
        user_config.cake_effect == CAKE_EFFECT_FADE
            ? sample.step_progress
            : 0;
    const uint32_t rendered_rotation =
        user_config.cake_effect == CAKE_EFFECT_COLOR_SHIFT
            ? sample.rotation
            : 0;
    if (cake_chase.output_valid &&
        active_index == cake_chase.output_led_index &&
        cake_chase.brightness == cake_chase.output_brightness &&
        effect_level == cake_chase.output_effect_level &&
        rendered_rotation == cake_chase.output_rotation) {
        return;
    }

    uint8_t red = user_config.cake_red;
    uint8_t green = user_config.cake_green;
    uint8_t blue = user_config.cake_blue;
    if (user_config.cake_effect == CAKE_EFFECT_COLOR_SHIFT) {
        uint8_t closest_index = 0;
        uint32_t closest_distance = UINT32_MAX;
        for (uint8_t index = 0; index < OUTPUT_COLOR_COUNT; ++index) {
            const int32_t red_delta =
                (int32_t)output_colors[index].red - user_config.cake_red;
            const int32_t green_delta =
                (int32_t)output_colors[index].green - user_config.cake_green;
            const int32_t blue_delta =
                (int32_t)output_colors[index].blue - user_config.cake_blue;
            const uint32_t distance =
                (uint32_t)(red_delta * red_delta +
                           green_delta * green_delta +
                           blue_delta * blue_delta);
            if (distance < closest_distance) {
                closest_distance = distance;
                closest_index = index;
            }
        }
        const output_color_t *color = &output_colors[
            (closest_index + sample.rotation) % OUTPUT_COLOR_COUNT];
        red = color->red;
        green = color->green;
        blue = color->blue;
    }

    static const uint8_t trail_levels[] = {255, 144, 72, 32};
    for (uint cake_index = 0;
         cake_index < user_config.cake_led_count;
         ++cake_index) {
        uint8_t effect_brightness = 0;
        if (user_config.cake_effect == CAKE_EFFECT_TRAIL) {
            const uint8_t trail_length =
                user_config.cake_led_count < 4
                    ? (uint8_t)user_config.cake_led_count
                    : 4;
            for (uint8_t distance = 0; distance < trail_length; ++distance) {
                const uint16_t logical_index = (uint16_t)(
                    (sample.logical_index + user_config.cake_led_count -
                     distance) % user_config.cake_led_count);
                if (cake_index == cake_physical_led_index(logical_index)) {
                    effect_brightness = trail_levels[distance];
                    break;
                }
            }
        } else if (cake_index == active_index) {
            effect_brightness =
                user_config.cake_effect == CAKE_EFFECT_FADE
                    ? (uint8_t)(255u - sample.step_progress)
                    : 255u;
        }
        const uint8_t brightness =
            scale_channel(cake_chase.brightness, effect_brightness);
        pio_sm_put_blocking(
            pio, sm,
            recolored_cake_pixel(red, green, blue, brightness) << 8);
    }

    mark_cake_tx_busy(now);
    cake_chase.output_led_index = active_index;
    cake_chase.output_brightness = cake_chase.brightness;
    cake_chase.output_effect_level = effect_level;
    cake_chase.output_rotation = rendered_rotation;
    cake_chase.output_valid = true;
}

// Emit one moving cake LED whose phase is locked to the outer cyclotron.
static void output_cake_frame(PIO pio, uint sm,
                              diagnostic_frame_t *frame,
                              absolute_time_t now) {
    if (frame->pixel_count != HASBRO_INPUT_PIXELS) {
        return;
    }

    update_cake_chase(frame, now);
    refresh_cake_output(pio, sm, now);
}

// Preview has no live WS2812 source frame to map. Synthesize the same center
// emitter that the real four-phase input uses, one phase at a time.
static void make_preview_frame(uint8_t phase) {
    memset(&preview_frame, 0, sizeof(preview_frame));
    preview_frame.pixel_count = HASBRO_INPUT_PIXELS;
    preview_frame.pixels[2u + phase * INPUTS_PER_OUTPUT] = 0x00ffffffu;
}

// Reset both chase state machines and seed the synthetic synchronized timing
// used by input-independent preview mode.
static void initialize_preview_output_state(absolute_time_t now) {
    // Applying a preview configuration has already latched both chains off.
    // Preserve those transmit deadlines while restarting the animation state.
    const absolute_time_t cyclotron_tx_ready_at = cyclotron_chase.tx_ready_at;
    const absolute_time_t cake_tx_ready_at = cake_chase.tx_ready_at;
    const uint32_t preview_phase_us = PREVIEW_SYNC_PHASE_MS * 1000u;

    memset(&cyclotron_chase, 0, sizeof(cyclotron_chase));
    memset(&cake_chase, 0, sizeof(cake_chase));
    cyclotron_chase.tx_ready_at = cyclotron_tx_ready_at;
    cake_chase.tx_ready_at = cake_tx_ready_at;

    preview_phase = 0;
    preview_next_phase_at = delayed_by_ms(now, PREVIEW_SYNC_PHASE_MS);
    make_preview_frame(preview_phase);
    update_cyclotron_chase(&preview_frame, now);
    update_cake_chase(&preview_frame, now);

    // A synchronized preview uses the documented synthetic 250 ms pulse.
    // Seed every phase so the animation is responsive from its first frame,
    // instead of waiting for the live-input learning interval.
    cyclotron_chase.fallback_duration_us = preview_phase_us;
    cyclotron_chase.valid_duration_mask = (1u << OUTPUT_LED_COUNT) - 1u;
    for (uint phase = 0; phase < OUTPUT_LED_COUNT; ++phase) {
        cyclotron_chase.phase_duration_us[phase] = preview_phase_us;
    }
    cyclotron_chase.phase_start_known = true;
    cyclotron_chase.output_valid = false;

    cake_chase.fallback_duration_us = preview_phase_us;
    cake_chase.valid_duration_mask = (1u << OUTPUT_LED_COUNT) - 1u;
    for (uint phase = 0; phase < OUTPUT_LED_COUNT; ++phase) {
        cake_chase.phase_duration_us[phase] = preview_phase_us;
    }
    cake_chase.phase_start_known = true;
    cake_chase.output_valid = false;
}

// Advance the synthetic 250 ms source phase as needed, then refresh whichever
// output chains have reached their periodic service deadline.
static void run_preview_output(PIO pio, uint cyclotron_sm, uint cake_sm,
                               absolute_time_t now, bool refresh_cake,
                               bool refresh_cyclotron) {
    while (time_reached(preview_next_phase_at)) {
        preview_phase = (uint8_t)((preview_phase + 1u) % OUTPUT_LED_COUNT);
        make_preview_frame(preview_phase);
        // Use the scheduled boundary as the phase timestamp. This preserves
        // the synthetic 250 ms clock if the main loop is briefly busy.
        update_cyclotron_chase(&preview_frame, preview_next_phase_at);
        update_cake_chase(&preview_frame, preview_next_phase_at);
        preview_next_phase_at =
            delayed_by_ms(preview_next_phase_at, PREVIEW_SYNC_PHASE_MS);
    }

    if (refresh_cake) {
        refresh_cake_output(pio, cake_sm, now);
    }
    if (refresh_cyclotron) {
        refresh_cyclotron_output(pio, cyclotron_sm, now);
    }
}

// Wait for the prior Cake frame's reset interval, then latch an all-zero frame.
static void output_cake_off(PIO pio, uint sm) {
    while (!time_reached(cake_chase.tx_ready_at)) {
        tight_loop_contents();
    }
    const absolute_time_t now = get_absolute_time();
    for (uint index = 0; index < user_config.cake_led_count; ++index) {
        pio_sm_put_blocking(pio, sm, 0u);
    }
    mark_cake_tx_busy(now);
    cake_chase.output_valid = false;
}

// Latch a temporary single-pixel Cake test frame and its expiration deadline.
static void output_cake_test(PIO pio, uint sm,
                             const config_request_t *request,
                             absolute_time_t now) {
    while (!time_reached(cake_chase.tx_ready_at)) {
        tight_loop_contents();
    }
    now = get_absolute_time();

    for (uint index = 0; index < user_config.cake_led_count; ++index) {
        const uint32_t pixel =
            index == request->test_led_index
                ? packed_cake_pixel(request->test_red,
                                    request->test_green,
                                    request->test_blue)
                : 0u;
        pio_sm_put_blocking(pio, sm, pixel << 8);
    }
    mark_cake_tx_busy(now);
    cake_chase.output_valid = false;
    cake_test_active = true;
    cake_test_until =
        delayed_by_ms(now, request->test_duration_ms);
}

// Cancel both test timers, latch both chains off, and discard animation caches.
static void clear_test_outputs(PIO pio, uint cyclotron_sm, uint cake_sm) {
    cyclotron_test_active = false;
    cake_test_active = false;
    output_cyclotron_off(pio, cyclotron_sm);
    output_cake_off(pio, cake_sm);
    cyclotron_chase.initialized = false;
    cyclotron_chase.output_valid = false;
    cake_chase.initialized = false;
    cake_chase.output_valid = false;
}

// Format a core-0 response into the queue drained by the USB task on core 1.
static void queue_config_response(const char *format, ...) {
    config_response_t response;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(response.text, sizeof(response.text), format, arguments);
    va_end(arguments);
    queue_try_add(&config_response_queue, &response);
}

// Serialize every active setting so the configurator can reconstruct the
// device state after connect, save, preview, or reconnect.
static void queue_current_config(void) {
    queue_config_response(
        "PB84 CONFIG protocol=%u firmware=%s "
        "cyclotron_led_style=%s cyclotron_led_count=%u "
        "cyclotron_led_type=%s "
        "cyclotron_color_order=%s cyclotron_bit_rate_khz=%u "
        "cyclotron_timing_mode=%s cyclotron_speed_multiplier=%u "
        "cyclotron_effect=%s cyclotron_rotation_ms=%u "
        "cyclotron_reverse=%s "
        "cake_led_count=%u cake_led_type=%s cake_color_order=%s "
        "cake_bit_rate_khz=%u cake_red=%u cake_green=%u cake_blue=%u "
        "cake_reverse=%s cake_start_offset=%u outer_color_index=%u "
        "lid_bypass=%s cake_timing_mode=%s cake_speed_multiplier=%u "
        "cake_effect=%s cake_rotation_ms=%u preview=%s",
        CONFIG_PROTOCOL_VERSION, PROJECT_VERSION_STRING,
        cyclotron_led_style_name(user_config.cyclotron_led_style),
        user_config.cyclotron_led_count,
        cake_led_type_name(user_config.cyclotron_led_type),
        cake_color_order_name(user_config.cyclotron_color_order),
        user_config.cyclotron_bit_rate_khz,
        cake_timing_mode_name(user_config.cyclotron_timing_mode),
        user_config.cyclotron_speed_multiplier,
        cake_effect_name(user_config.cyclotron_effect),
        user_config.cyclotron_rotation_ms,
        user_config.cyclotron_reverse ? "true" : "false",
        user_config.cake_led_count,
        cake_led_type_name(user_config.cake_led_type),
        cake_color_order_name(user_config.cake_color_order),
        user_config.cake_bit_rate_khz,
        user_config.cake_red, user_config.cake_green,
        user_config.cake_blue,
        user_config.cake_reverse ? "true" : "false",
        user_config.cake_start_offset,
        user_config.outer_color_index,
        user_config.lid_bypass ? "true" : "false",
        cake_timing_mode_name(user_config.cake_timing_mode),
        user_config.cake_speed_multiplier,
        cake_effect_name(user_config.cake_effect),
        user_config.cake_rotation_ms,
        preview_active ? "true" : "false");
}

// Safely clear both chains under their old lengths, apply new runtime settings,
// update PIO bit rates, and reset animation/test state without writing flash.
static void apply_runtime_config(PIO pio, uint cyclotron_sm, uint cake_sm,
                                 const user_config_t *config) {
    output_cyclotron_off(pio, cyclotron_sm);
    while (!time_reached(cyclotron_chase.tx_ready_at)) {
        tight_loop_contents();
    }
    output_cake_off(pio, cake_sm);
    while (!time_reached(cake_chase.tx_ready_at)) {
        tight_loop_contents();
    }
    const absolute_time_t cyclotron_tx_ready_at = cyclotron_chase.tx_ready_at;
    const absolute_time_t cake_tx_ready_at = cake_chase.tx_ready_at;
    user_config = *config;
    memset(&cake_chase, 0, sizeof(cake_chase));
    cake_chase.tx_ready_at = cake_tx_ready_at;
    ws2812_tx_set_bit_rate(
        pio, cyclotron_sm,
        (uint32_t)user_config.cyclotron_bit_rate_khz * 1000u);
    ws2812_tx_set_bit_rate(
        pio, cake_sm, (uint32_t)user_config.cake_bit_rate_khz * 1000u);
    set_lid_output(lid_bypass_active());
    cake_test_active = false;
    cyclotron_test_active = false;
    memset(&cyclotron_chase, 0, sizeof(cyclotron_chase));
    cyclotron_chase.tx_ready_at = cyclotron_tx_ready_at;
}

// Execute a parsed USB request on core 0, where LED PIO and flash state are
// owned, then queue a completion or error response for core 1.
static void process_config_request(PIO pio, uint cyclotron_sm, uint cake_sm,
                                   const config_request_t *request) {
    if (request->kind == CONFIG_REQUEST_GET) {
        queue_current_config();
        return;
    }

    if (request->kind == CONFIG_REQUEST_TEST) {
        if (request->test_target == TEST_TARGET_CYCLOTRON) {
            if (request->test_led_index >= OUTPUT_LED_COUNT ||
                request->test_color_index >= OUTPUT_COLOR_COUNT) {
                queue_config_response(
                    "PB84 ERROR cyclotron test values are out of range");
                return;
            }
            output_cyclotron_test(
                pio, cyclotron_sm, request, get_absolute_time());
            queue_config_response(
                "PB84 OK tested_target=cyclotron tested_led=%u",
                request->test_led_index + 1);
            return;
        }

        if (request->test_led_index >= user_config.cake_led_count) {
            queue_config_response(
                "PB84 ERROR test LED must be between 1 and %u",
                user_config.cake_led_count);
            return;
        }
        output_cake_test(pio, cake_sm, request, get_absolute_time());
        queue_config_response("PB84 OK tested_target=cake tested_led=%u",
                              request->test_led_index + 1);
        return;
    }

    if (request->kind == CONFIG_REQUEST_CLEAR_TEST) {
        clear_test_outputs(pio, cyclotron_sm, cake_sm);
        queue_config_response("PB84 OK test_cleared=true");
        return;
    }

    if (request->kind == CONFIG_REQUEST_PREVIEW_START) {
        if (!preview_active) {
            preview_saved_config = user_config;
        }
        apply_runtime_config(pio, cyclotron_sm, cake_sm, &request->config);
        preview_active = true;
        initialize_preview_output_state(get_absolute_time());
        queue_config_response("PB84 OK preview=true");
        return;
    }

    if (request->kind == CONFIG_REQUEST_PREVIEW_STOP) {
        if (preview_active) {
            const user_config_t saved_config = preview_saved_config;
            apply_runtime_config(pio, cyclotron_sm, cake_sm, &saved_config);
            preview_active = false;
        }
        queue_config_response("PB84 OK preview=false");
        return;
    }

    const user_config_t active_before_save = user_config;
    user_config = request->config;
    if (!save_user_setting()) {
        user_config = active_before_save;
        queue_config_response("PB84 ERROR unable to save configuration");
        return;
    }

    // Latch an all-off frame using the active frame length before applying
    // the saved configuration and restarting chase calibration.
    user_config = active_before_save;
    apply_runtime_config(pio, cyclotron_sm, cake_sm, &request->config);
    preview_active = false;
    queue_config_response("PB84 OK saved=true");
}

// Drive the four physical cyclotron window pixels red for button feedback.
static void output_all_red(PIO pio, uint sm, bool on) {
    for (uint index = 0; index < user_config.cyclotron_led_count; ++index) {
        bool is_window_pixel = false;
        for (uint window = 0; window < OUTPUT_LED_COUNT; ++window) {
            if (cyclotron_physical_led_index(window) == index) {
                is_window_pixel = true;
                break;
            }
        }
        const uint32_t pixel =
            on && is_window_pixel
                ? packed_cyclotron_pixel(255, 0, 0, UINT8_MAX)
                : 0u;
        pio_sm_put_blocking(pio, sm, pixel << 8);
    }
    cyclotron_output_is_off = !on;
}

#if ENABLE_USB_DIAGNOSTICS
// Render frame mapping details as human-readable USB serial output.
static void print_frame(const diagnostic_frame_t *frame) {
    if (frame->pixel_count == HASBRO_INPUT_PIXELS) {
        const output_color_t *color = &output_colors[frame->color_index];
        const char *lid_status = frame->lid_state == LID_STATE_BYPASSED
                                     ? "Bypassed"
                                     : frame->lid_state == LID_STATE_CLOSED
                                           ? "Closed"
                                           : "Open";
        printf("Frame %lu: 12 inputs -> %u cyclotron LEDs + %u cake LEDs, "
               "color %s, cake RGB(%u,%u,%u), lid %s%s\n",
               (unsigned long)frame->number,
               user_config.cyclotron_led_count,
               user_config.cake_led_count,
               color->name,
               user_config.cake_red,
               user_config.cake_green,
               user_config.cake_blue,
               lid_status,
               frame->truncated ? " (diagnostic buffer full)" : "");
        for (uint output_index = 0;
             output_index < OUTPUT_LED_COUNT;
             ++output_index) {
            const uint8_t brightness = mapped_brightness(frame, output_index);
            if (brightness > 0) {
                printf("  Cyclotron LED %u: brightness %u -> %s\n",
                       output_index + 1, brightness, color->name);
            }
        }
        if (frame->cake_brightness > 0) {
            printf("  Cake LED %u: brightness %u -> RGB(%u,%u,%u)\n",
                   frame->cake_led_index + 1, frame->cake_brightness,
                   user_config.cake_red, user_config.cake_green,
                   user_config.cake_blue);
        }
        fflush(stdout);
        return;
    }

    printf("Frame %lu received: %u pixels%s\n",
           (unsigned long)frame->number, frame->pixel_count,
           frame->truncated ? " (diagnostic buffer full)" : "");

    bool any_led_on = false;
    for (uint index = 0; index < frame->pixel_count; ++index) {
        const uint32_t grb = frame->pixels[index];
        const uint8_t green = (uint8_t)(grb >> 16);
        const uint8_t red = (uint8_t)(grb >> 8);
        const uint8_t blue = (uint8_t)grb;
        const uint8_t brightness = max3(red, green, blue);

        if (brightness == 0) {
            continue;
        }

        any_led_on = true;
        printf("  LED %u ON: RGB(%u,%u,%u), brightness %u -> RED(%u)\n",
               index + 1, red, green, blue, brightness, brightness);
    }

    if (!any_led_on) {
        printf("  All LEDs off\n");
    }
    fflush(stdout);
}

// Suppress duplicate diagnostics when all rendered and source fields match.
static bool frames_match(const diagnostic_frame_t *a,
                         const diagnostic_frame_t *b) {
    return a->pixel_count == b->pixel_count &&
           a->color_index == b->color_index &&
           a->cake_led_index == b->cake_led_index &&
           a->cake_brightness == b->cake_brightness &&
           a->lid_state == b->lid_state &&
           a->truncated == b->truncated &&
           memcmp(a->pixels, b->pixels,
                  a->pixel_count * sizeof(a->pixels[0])) == 0;
}
#endif

// Emit one complete CRLF-terminated protocol response over USB CDC.
static void send_config_line(const char *line) {
    printf("%s\r\n", line);
    fflush(stdout);
}

// Hand a parsed request to core 0 without blocking USB service when busy.
static void submit_config_request(const config_request_t *request) {
    if (!queue_try_add(&config_request_queue, request)) {
        send_config_line("PB84 ERROR device is busy");
    }
}

// Parse one complete PB84 command line, validate its fields, and queue any
// operation that touches shared configuration, flash, or LED hardware.
static void handle_config_command(char *line) {
    while (*line == ' ' || *line == '\t') {
        ++line;
    }

    if (strcmp(line, "PB84 HELLO") == 0) {
        printf("PB84 HELLO protocol=%u product=%s firmware=%s\r\n",
               CONFIG_PROTOCOL_VERSION, PROJECT_NAME,
               PROJECT_VERSION_STRING);
        fflush(stdout);
        return;
    }

    if (strcmp(line, "PB84 GET") == 0) {
        const config_request_t request = {
            .kind = CONFIG_REQUEST_GET,
        };
        submit_config_request(&request);
        return;
    }

    if (strncmp(line, "PB84 SET ", 9) == 0) {
        config_request_t request = {
            .kind = CONFIG_REQUEST_SET,
        };
        char error[96];
        const user_config_t base = user_config;
        if (!user_config_parse_update(
                line + 9, &base, OUTPUT_COLOR_COUNT,
                &request.config, error, sizeof(error))) {
            printf("PB84 ERROR %s\r\n", error);
            fflush(stdout);
            return;
        }
        submit_config_request(&request);
        return;
    }

    if (strcmp(line, "PB84 PREVIEW STOP") == 0) {
        const config_request_t request = {
            .kind = CONFIG_REQUEST_PREVIEW_STOP,
        };
        submit_config_request(&request);
        return;
    }

    if (strncmp(line, "PB84 PREVIEW START ", 19) == 0) {
        config_request_t request = {
            .kind = CONFIG_REQUEST_PREVIEW_START,
        };
        char error[96];
        const user_config_t base = user_config;
        if (!user_config_parse_update(
                line + 19, &base, OUTPUT_COLOR_COUNT,
                &request.config, error, sizeof(error))) {
            printf("PB84 ERROR %s\r\n", error);
            fflush(stdout);
            return;
        }
        submit_config_request(&request);
        return;
    }

    if (strncmp(line, "PB84 TEST ", 10) == 0) {
        if (strcmp(line + 10, "CLEAR") == 0) {
            const config_request_t request = {
                .kind = CONFIG_REQUEST_CLEAR_TEST,
            };
            submit_config_request(&request);
            return;
        }

        if (strncmp(line + 10, "target=cyclotron ", 17) == 0) {
            unsigned int led = 0;
            unsigned int color_index = 0;
            unsigned int duration_ms = 0;
            const int matched = sscanf(
                line + 27,
                "led=%u color_index=%u duration_ms=%u",
                &led, &color_index, &duration_ms);
            if (matched != 3 || led == 0 || led > OUTPUT_LED_COUNT ||
                color_index >= OUTPUT_COLOR_COUNT ||
                duration_ms < 100 || duration_ms > 10000) {
                send_config_line(
                    "PB84 ERROR invalid cyclotron TEST values");
                return;
            }

            const config_request_t request = {
                .kind = CONFIG_REQUEST_TEST,
                .test_target = TEST_TARGET_CYCLOTRON,
                .test_led_index = (uint16_t)(led - 1),
                .test_duration_ms = (uint16_t)duration_ms,
                .test_color_index = (uint8_t)color_index,
            };
            submit_config_request(&request);
            return;
        }

        unsigned int led = 0;
        unsigned int red = 0;
        unsigned int green = 0;
        unsigned int blue = 0;
        unsigned int duration_ms = 0;
        const int matched = sscanf(
            line + 10,
            "led=%u red=%u green=%u blue=%u duration_ms=%u",
            &led, &red, &green, &blue, &duration_ms);
        if (matched != 5 || led == 0 || led > CAKE_LED_COUNT_MAX ||
            red > 255 || green > 255 || blue > 255 ||
            duration_ms < 100 || duration_ms > 10000) {
            send_config_line(
                "PB84 ERROR invalid TEST values");
            return;
        }

        const config_request_t request = {
            .kind = CONFIG_REQUEST_TEST,
            .test_target = TEST_TARGET_CAKE,
            .test_led_index = (uint16_t)(led - 1),
            .test_duration_ms = (uint16_t)duration_ms,
            .test_red = (uint8_t)red,
            .test_green = (uint8_t)green,
            .test_blue = (uint8_t)blue,
        };
        submit_config_request(&request);
        return;
    }

    send_config_line("PB84 ERROR unsupported command");
}

// Assemble non-blocking USB input into command lines and drain responses from
// core 0; this keeps serial traffic out of the timing-sensitive capture loop.
static void config_usb_task(void) {
    static char command[CONFIG_COMMAND_MAX];
    static size_t command_length;

    int character = getchar_timeout_us(0);
    while (character != PICO_ERROR_TIMEOUT) {
        if (character == '\n') {
            command[command_length] = '\0';
            if (command_length > 0 &&
                command[command_length - 1] == '\r') {
                command[--command_length] = '\0';
            }
            if (command_length > 0) {
                handle_config_command(command);
            }
            command_length = 0;
        } else if (command_length + 1 < sizeof(command)) {
            command[command_length++] = (char)character;
        } else {
            command_length = 0;
            send_config_line("PB84 ERROR command is too long");
        }
        character = getchar_timeout_us(0);
    }

    config_response_t response;
    while (queue_try_remove(&config_response_queue, &response)) {
        send_config_line(response.text);
    }
}

// Core 1 participates in safe flash operations and, when enabled, drains the
// diagnostic queue so serial output never delays time-sensitive frame capture.
static void diagnostics_core(void) {
    flash_safe_execute_core_init();
    multicore_fifo_push_blocking(1u);

    while (true) {
        config_usb_task();

#if ENABLE_USB_DIAGNOSTICS
        if (queue_try_remove(&diagnostic_queue, &serial_frame)) {
        if (previous_serial_frame_valid &&
            frames_match(&serial_frame, &previous_serial_frame)) {
                sleep_ms(1);
                continue;
        }

        print_frame(&serial_frame);
        previous_serial_frame = serial_frame;
        previous_serial_frame_valid = true;
        }
#endif
        sleep_ms(1);
    }
}

// Initialize all core-0 deadlines while leaving counters and debounce flags at
// their zero-valued startup state.
static void initialize_runtime_state(runtime_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->last_pixel_time = get_absolute_time();
    state->next_idle_off =
        delayed_by_ms(state->last_pixel_time, INPUT_IDLE_OFF_MS);
    state->next_button_poll = get_absolute_time();
    state->button_changed_at = get_absolute_time();
    state->button_pressed_at = get_absolute_time();
    state->next_lid_poll = get_absolute_time();
    state->lid_changed_at = get_absolute_time();
    state->next_cyclotron_refresh = get_absolute_time();
    state->next_cake_refresh = get_absolute_time();
    state->color_save_at = at_the_end_of_time;
    state->config_flash_at = at_the_end_of_time;
}

// Consume one complete GRB word from the PIO RX FIFO. Returning true tells the
// event loop to immediately check for another pixel before servicing timers.
static bool capture_input_pixel(PIO pio, uint rx_sm,
                                runtime_state_t *state) {
    if (pio_sm_is_rx_fifo_empty(pio, rx_sm)) {
        return false;
    }

    const uint32_t input_grb = pio_sm_get(pio, rx_sm) & 0x00ffffffu;
    if (capture_frame.pixel_count < MAX_FRAME_PIXELS) {
        capture_frame.pixels[capture_frame.pixel_count++] = input_grb;
    } else {
        capture_frame.truncated = true;
    }
    state->last_pixel_time = get_absolute_time();
    state->receiving_frame = true;
    return true;
}

// Detect the input reset gap, finalize diagnostics, and map a complete Hasbro
// frame to both output chains when preview and LED tests are not overriding it.
static void finish_input_frame(PIO pio, uint rx_sm, uint rx_offset,
                               uint cyclotron_sm, uint cake_sm,
                               runtime_state_t *state) {
    if (!state->receiving_frame ||
        absolute_time_diff_us(state->last_pixel_time, get_absolute_time()) <
            WS2812_RESET_US) {
        return;
    }

    capture_frame.number = ++state->frame_number;
    restart_ws2812_rx(pio, rx_sm, rx_offset);
    capture_frame.color_index = user_config.outer_color_index;
    capture_frame.lid_state = lid_bypass_active()
                                  ? LID_STATE_BYPASSED
                                  : state->lid_closed ? LID_STATE_CLOSED
                                                      : LID_STATE_OPEN;
    if (state->config_flash_phase == 0) {
        const absolute_time_t frame_time = get_absolute_time();
        if (capture_frame.pixel_count == HASBRO_INPUT_PIXELS) {
            state->next_idle_off =
                delayed_by_ms(frame_time, INPUT_IDLE_OFF_MS);
        }
        if (!preview_active) {
            if (!cyclotron_test_active) {
                output_cyclotron_frame(pio, cyclotron_sm, &capture_frame);
            }
            if (!cake_test_active) {
                output_cake_frame(pio, cake_sm, &capture_frame, frame_time);
            }
        }
    }
#if ENABLE_USB_DIAGNOSTICS
    queue_try_add(&diagnostic_queue, &capture_frame);
#endif
    capture_frame.pixel_count = 0;
    capture_frame.truncated = false;
    state->receiving_frame = false;
}

// Execute at most one queued USB request between captured input frames.
static void service_config_request(PIO pio, uint cyclotron_sm, uint cake_sm,
                                   const runtime_state_t *state) {
    config_request_t request;
    if (!state->receiving_frame &&
        queue_try_remove(&config_request_queue, &request)) {
        process_config_request(pio, cyclotron_sm, cake_sm, &request);
    }
}

// Expire temporary LED tests and make their normal render paths eligible to
// redraw on the next frame or refresh deadline.
static void service_test_deadlines(PIO pio, uint cyclotron_sm) {
    if (cyclotron_test_active && time_reached(cyclotron_test_until)) {
        cyclotron_test_active = false;
        output_cyclotron_off(pio, cyclotron_sm);
    }
    if (cake_test_active && time_reached(cake_test_until)) {
        cake_test_active = false;
        cake_chase.output_valid = false;
    }
}

// Latch one all-off frame after the source has been idle long enough, while
// preserving an active preview, LED test, or button-confirmation animation.
static void service_idle_outputs(PIO pio, uint cyclotron_sm, uint cake_sm,
                                 runtime_state_t *state) {
    if (state->config_flash_phase != 0 || preview_active ||
        !time_reached(state->next_idle_off)) {
        return;
    }

    const absolute_time_t now = get_absolute_time();
    state->next_idle_off = delayed_by_ms(now, INPUT_IDLE_OFF_MS);
    if (!cyclotron_test_active) {
        memset(&cyclotron_chase, 0, sizeof(cyclotron_chase));
        output_cyclotron_off(pio, cyclotron_sm);
    }
    if (!cake_test_active) {
        memset(&cake_chase, 0, sizeof(cake_chase));
        output_cake_off(pio, cake_sm);
    }
}

// Service preview and live animation refreshes from their independent 5 ms
// deadlines without changing the source-frame processing order.
static void service_animation_refresh(PIO pio, uint cyclotron_sm, uint cake_sm,
                                      runtime_state_t *state) {
    if (state->config_flash_phase != 0) {
        return;
    }

    if (preview_active &&
        (time_reached(state->next_cake_refresh) ||
         time_reached(state->next_cyclotron_refresh))) {
        const absolute_time_t now = get_absolute_time();
        const bool refresh_cake = time_reached(state->next_cake_refresh);
        const bool refresh_cyclotron =
            time_reached(state->next_cyclotron_refresh);
        if (refresh_cake) {
            state->next_cake_refresh = delayed_by_ms(now, CAKE_REFRESH_MS);
        }
        if (refresh_cyclotron) {
            state->next_cyclotron_refresh =
                delayed_by_ms(now, CAKE_REFRESH_MS);
        }
        run_preview_output(pio, cyclotron_sm, cake_sm, now, refresh_cake,
                           refresh_cyclotron);
    }

    if (!preview_active && !cake_test_active &&
        time_reached(state->next_cake_refresh)) {
        const absolute_time_t now = get_absolute_time();
        state->next_cake_refresh = delayed_by_ms(now, CAKE_REFRESH_MS);
        refresh_cake_output(pio, cake_sm, now);
    }

    const bool cyclotron_needs_periodic_refresh =
        user_config.cyclotron_timing_mode == CAKE_TIMING_FREE ||
        user_config.cyclotron_speed_multiplier != 1 ||
        user_config.cyclotron_effect != CAKE_EFFECT_SOLID;
    if (!preview_active && !cyclotron_test_active &&
        cyclotron_needs_periodic_refresh &&
        time_reached(state->next_cyclotron_refresh)) {
        const absolute_time_t now = get_absolute_time();
        state->next_cyclotron_refresh =
            delayed_by_ms(now, CAKE_REFRESH_MS);
        refresh_cyclotron_output(pio, cyclotron_sm, now);
    }
}

// Debounce the lid sense input and mirror its stable state to the open-drain
// output unless the saved or compile-time bypass is active.
static void service_lid_switch(runtime_state_t *state) {
    if (lid_bypass_active() || !time_reached(state->next_lid_poll)) {
        return;
    }

    state->next_lid_poll =
        delayed_by_ms(state->next_lid_poll, LID_POLL_MS);
    const bool closed = !gpio_get(LID_SENSE_PIN);
    if (closed != state->lid_raw_closed) {
        state->lid_raw_closed = closed;
        state->lid_changed_at = get_absolute_time();
    } else if (closed != state->lid_closed &&
               absolute_time_diff_us(state->lid_changed_at,
                                     get_absolute_time()) >=
                   LID_DEBOUNCE_MS * 1000u) {
        state->lid_closed = closed;
        set_lid_output(state->lid_closed);
    }
}

// Debounce BOOTSEL, cycle the cyclotron palette on a short press, and toggle
// lid bypass plus its confirmation animation on a long press.
static void service_bootsel_button(PIO pio, uint cyclotron_sm, uint cake_sm,
                                   runtime_state_t *state) {
    if (!time_reached(state->next_button_poll)) {
        return;
    }

    state->next_button_poll =
        delayed_by_ms(state->next_button_poll, BUTTON_POLL_MS);
    const bool pressed = read_bootsel_pressed();
    if (pressed != state->button_raw_pressed) {
        state->button_raw_pressed = pressed;
        state->button_changed_at = get_absolute_time();
    } else if (pressed != state->button_pressed &&
               absolute_time_diff_us(state->button_changed_at,
                                     get_absolute_time()) >=
                   BUTTON_DEBOUNCE_MS * 1000u) {
        state->button_pressed = pressed;
        if (state->button_pressed) {
            state->button_pressed_at = get_absolute_time();
            state->button_long_press_handled = false;
        } else if (!state->button_long_press_handled) {
            user_config.outer_color_index =
                (user_config.outer_color_index + 1) % OUTPUT_COLOR_COUNT;
            state->color_save_at =
                delayed_by_ms(get_absolute_time(), COLOR_SAVE_DELAY_MS);
            state->color_save_pending = true;
        }
    }

    if (!state->button_pressed || state->button_long_press_handled ||
        absolute_time_diff_us(state->button_pressed_at, get_absolute_time()) <
            BUTTON_LONG_PRESS_MS * 1000u) {
        return;
    }

    state->button_long_press_handled = true;
    user_config.lid_bypass = user_config.lid_bypass == 0 ? 1 : 0;
    if (lid_bypass_active()) {
        set_lid_output(true);
    } else {
        state->lid_raw_closed = false;
        state->lid_closed = false;
        state->lid_changed_at = get_absolute_time();
        state->next_lid_poll = get_absolute_time();
        set_lid_output(false);
    }

    output_all_red(pio, cyclotron_sm, true);
    output_cake_off(pio, cake_sm);
    state->config_flash_phase = 1;
    state->config_flash_at =
        delayed_by_ms(get_absolute_time(), CONFIG_FLASH_ON_MS);
    state->color_save_at =
        delayed_by_ms(get_absolute_time(), COLOR_SAVE_DELAY_MS);
    state->color_save_pending = true;
}

// Advance the non-blocking two-flash confirmation shown after a long press.
static void service_config_flash(PIO pio, uint cyclotron_sm,
                                 runtime_state_t *state) {
    if (state->config_flash_phase == 0 ||
        !time_reached(state->config_flash_at)) {
        return;
    }

    if (state->config_flash_phase == 1) {
        output_all_red(pio, cyclotron_sm, false);
        state->config_flash_phase = 2;
        state->config_flash_at =
            delayed_by_ms(get_absolute_time(), CONFIG_FLASH_OFF_MS);
    } else if (state->config_flash_phase == 2) {
        output_all_red(pio, cyclotron_sm, true);
        state->config_flash_phase = 3;
        state->config_flash_at =
            delayed_by_ms(get_absolute_time(), CONFIG_FLASH_ON_MS);
    } else {
        output_all_red(pio, cyclotron_sm, false);
        state->config_flash_phase = 0;
    }
}

// Coalesce rapid button changes and retry a failed journal write later.
static void service_pending_save(runtime_state_t *state) {
    if (!state->color_save_pending || state->button_pressed ||
        !time_reached(state->color_save_at)) {
        return;
    }

    if (save_user_setting()) {
        state->color_save_pending = false;
    } else {
        state->color_save_at =
            delayed_by_ms(get_absolute_time(), COLOR_SAVE_DELAY_MS);
    }
}

// Initialize hardware and run the core-0 event loop that captures WS2812 input,
// services LED output deadlines, handles controls, and persists settings.
int main(void) {
    // Restore saved preferences and bring up GPIO, multicore, and PIO hardware.
    set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true);
    gpio_init(WS2812_OUTPUT_PIN);
    gpio_pull_down(WS2812_OUTPUT_PIN);
    gpio_put(WS2812_OUTPUT_PIN, false);
    gpio_set_dir(WS2812_OUTPUT_PIN, GPIO_OUT);
    gpio_init(CAKE_OUTPUT_PIN);
    gpio_pull_down(CAKE_OUTPUT_PIN);
    gpio_put(CAKE_OUTPUT_PIN, false);
    gpio_set_dir(CAKE_OUTPUT_PIN, GPIO_OUT);
    stdio_init_all();
    load_user_setting();
    lid_switch_init();
    queue_init(&config_request_queue, sizeof(config_request_t),
               CONFIG_QUEUE_DEPTH);
    queue_init(&config_response_queue, sizeof(config_response_t),
               CONFIG_QUEUE_DEPTH);
#if ENABLE_USB_DIAGNOSTICS
    queue_init(&diagnostic_queue, sizeof(diagnostic_frame_t),
               DIAGNOSTIC_QUEUE_DEPTH);
#endif
    multicore_launch_core1(diagnostics_core);
    multicore_fifo_pop_blocking();

    PIO pio = pio0;
    const uint rx_offset = pio_add_program(pio, &ws2812_rx_program);
    const uint tx_offset = pio_add_program(pio, &ws2812_tx_program);
    const uint rx_sm = pio_claim_unused_sm(pio, true);
    const uint tx_sm = pio_claim_unused_sm(pio, true);
    const uint cake_tx_sm = pio_claim_unused_sm(pio, true);

    ws2812_rx_init(pio, rx_sm, rx_offset, WS2812_INPUT_PIN);
    ws2812_tx_init(pio, tx_sm, tx_offset, WS2812_OUTPUT_PIN,
                   (uint32_t)user_config.cyclotron_bit_rate_khz * 1000u);
    ws2812_tx_init(pio, cake_tx_sm, tx_offset, CAKE_OUTPUT_PIN,
                   (uint32_t)user_config.cake_bit_rate_khz * 1000u);
    // Clear every configured output immediately. Later shutdown clears are
    // suppressed once the chain is known to be off, leaving the data pin low.
    output_cyclotron_off(pio, tx_sm);
    output_cake_off(pio, cake_tx_sm);
    wait_for_ws2812_reset(WS2812_INPUT_PIN);
    restart_ws2812_rx(pio, rx_sm, rx_offset);
    pio_rx_wake_init(pio, rx_sm);
    add_repeating_timer_ms(-(int32_t)LID_POLL_MS,
                           idle_wake_timer_callback, NULL,
                           &idle_wake_timer);

#if ENABLE_USB_DIAGNOSTICS
    printf("%s v%s: GPIO %u -> GPIO %u (%u cyclotron LEDs, %s), "
           "GPIO %u (%u cake LEDs, RGB(%u,%u,%u))\n",
           PROJECT_NAME, PROJECT_VERSION_STRING,
           WS2812_INPUT_PIN, WS2812_OUTPUT_PIN,
           user_config.cyclotron_led_count,
           output_colors[user_config.outer_color_index].name,
           CAKE_OUTPUT_PIN, user_config.cake_led_count,
           user_config.cake_red, user_config.cake_green,
           user_config.cake_blue);
#endif

    runtime_state_t runtime;
    initialize_runtime_state(&runtime);

    while (true) {
        if (capture_input_pixel(pio, rx_sm, &runtime)) {
            continue;
        }

        finish_input_frame(pio, rx_sm, rx_offset, tx_sm, cake_tx_sm,
                           &runtime);
        service_config_request(pio, tx_sm, cake_tx_sm, &runtime);
        service_test_deadlines(pio, tx_sm);
        service_idle_outputs(pio, tx_sm, cake_tx_sm, &runtime);
        service_animation_refresh(pio, tx_sm, cake_tx_sm, &runtime);
        service_lid_switch(&runtime);
        service_bootsel_button(pio, tx_sm, cake_tx_sm, &runtime);
        service_config_flash(pio, tx_sm, &runtime);
        service_pending_save(&runtime);

        // Stay responsive during a frame; otherwise sleep until RX or a poll
        // timer interrupt requires attention.
        if (runtime.receiving_frame) {
            tight_loop_contents();
        } else {
            sleep_until_input_or_timer(pio, rx_sm);
        }
    }
}
