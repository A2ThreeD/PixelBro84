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
#include <string.h>

#include "hardware/clocks.h"
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
#include "ws2812_rx.pio.h"
#include "ws2812_tx.pio.h"

#ifndef ENABLE_USB_DIAGNOSTICS
#define ENABLE_USB_DIAGNOSTICS 0
#endif

bi_decl(bi_program_name(PROJECT_NAME));
bi_decl(bi_program_version_string(PROJECT_VERSION_STRING));
bi_decl(bi_program_description(PROJECT_CHANGE_SUMMARY));

// Waveshare RP2040-Zero pins. Change these if your wiring uses other GPIOs.
#define WS2812_INPUT_PIN  2u
#define WS2812_OUTPUT_PIN 3u
#define LID_SENSE_PIN     4u
#define LID_OUTPUT_PIN    29u

// Set true to permanently force bypass regardless of the saved setting.
#define LID_DETECTION_BYPASS false

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
#define COLOR_SAVE_DELAY_MS 1000u
#define CONFIG_FLASH_ON_MS 150u
#define CONFIG_FLASH_OFF_MS 120u
#define COLOR_SETTINGS_MAGIC 0x434f4c52u
#define COLOR_SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define COLOR_SETTINGS_SLOTS (FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE)

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    const char *name;
} output_color_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint8_t color_index;
    uint8_t color_inverse;
    uint8_t bypass_enabled;
    uint8_t bypass_inverse;
    uint32_t checksum;
    uint8_t padding[FLASH_PAGE_SIZE - 16];
} color_settings_record_t;

typedef struct {
    uint32_t flash_offset;
    bool erase_sector;
    color_settings_record_t record;
} color_flash_write_t;

_Static_assert(sizeof(color_settings_record_t) == FLASH_PAGE_SIZE,
               "Color setting must occupy exactly one flash page");

static const output_color_t output_colors[] = {
    {255, 0, 0, "Red"},
    {0, 255, 0, "Green"},
    {0, 0, 255, "Blue"},
    {255, 255, 0, "Yellow"},
    {128, 0, 255, "Purple"},
};

#define OUTPUT_COLOR_COUNT (sizeof(output_colors) / sizeof(output_colors[0]))

typedef struct {
    uint32_t number;
    uint32_t pixels[MAX_FRAME_PIXELS];
    uint16_t pixel_count;
    uint8_t color_index;
    uint8_t lid_state;
    bool truncated;
} diagnostic_frame_t;

static diagnostic_frame_t capture_frame;
#if ENABLE_USB_DIAGNOSTICS
static queue_t diagnostic_queue;
static diagnostic_frame_t serial_frame;
static diagnostic_frame_t previous_serial_frame;
static bool previous_serial_frame_valid;
#endif
static uint8_t selected_color_index;
static bool lid_bypass_enabled;
static uint32_t color_settings_sequence;
static uint color_settings_next_slot;
static color_flash_write_t color_flash_write;
static PIO rx_wake_pio;
static uint rx_wake_sm;
static repeating_timer_t idle_wake_timer;

enum {
    LID_STATE_OPEN,
    LID_STATE_CLOSED,
    LID_STATE_BYPASSED,
};

static bool idle_wake_timer_callback(repeating_timer_t *timer) {
    (void)timer;
    return true;
}

static void pio_rx_wake_irq_handler(void) {
    pio_set_irq0_source_enabled(
        rx_wake_pio,
        pio_get_rx_fifo_not_empty_interrupt_source(rx_wake_sm),
        false);
}

static void pio_rx_wake_init(PIO pio, uint sm) {
    rx_wake_pio = pio;
    rx_wake_sm = sm;
    pio_set_irq0_source_enabled(
        pio, pio_get_rx_fifo_not_empty_interrupt_source(sm), false);
    irq_set_exclusive_handler(PIO_IRQ_NUM(pio, 0), pio_rx_wake_irq_handler);
    irq_set_enabled(PIO_IRQ_NUM(pio, 0), true);
}

static void sleep_until_input_or_timer(PIO pio, uint sm) {
    const uint32_t flags = save_and_disable_interrupts();
    pio_set_irq0_source_enabled(
        pio, pio_get_rx_fifo_not_empty_interrupt_source(sm), true);
    if (pio_sm_is_rx_fifo_empty(pio, sm)) {
        __wfi();
    }
    restore_interrupts(flags);
}

static void set_lid_output(bool pull_low) {
    // The output latch always stays low. Direction controls whether GPIO29
    // actively sinks the line or presents a high-impedance input.
    gpio_put(LID_OUTPUT_PIN, false);
    gpio_set_dir(LID_OUTPUT_PIN, pull_low ? GPIO_OUT : GPIO_IN);
}

static bool lid_bypass_active(void) {
    return LID_DETECTION_BYPASS || lid_bypass_enabled;
}

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

static bool read_bootsel_pressed(void) {
    multicore_lockout_start_blocking();
    const bool pressed = read_bootsel_pressed_raw();
    multicore_lockout_end_blocking();
    return pressed;
}

static uint32_t legacy_color_settings_checksum(uint32_t sequence,
                                               uint8_t color_index) {
    return COLOR_SETTINGS_MAGIC ^ sequence ^ color_index ^ 0xa5c35a3cu;
}

static uint32_t color_settings_checksum(uint32_t sequence,
                                        uint8_t color_index,
                                        bool bypass_enabled) {
    return legacy_color_settings_checksum(sequence, color_index) ^
           ((uint32_t)bypass_enabled << 24) ^ 0x19b40000u;
}

static bool color_settings_record_valid(const color_settings_record_t *record,
                                        bool *legacy) {
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
        *legacy = true;
        return true;
    }

    *legacy = false;
    return record->magic == COLOR_SETTINGS_MAGIC &&
           record->bypass_enabled <= 1u &&
           record->bypass_inverse == (uint8_t)~record->bypass_enabled &&
           record->checksum ==
               color_settings_checksum(record->sequence, record->color_index,
                                       record->bypass_enabled != 0);
}

static void load_color_setting(void) {
    const color_settings_record_t *records =
        (const color_settings_record_t *)(XIP_BASE + COLOR_SETTINGS_OFFSET);
    bool found = false;
    color_settings_next_slot = COLOR_SETTINGS_SLOTS;

    for (uint slot = 0; slot < COLOR_SETTINGS_SLOTS; ++slot) {
        const color_settings_record_t *record = &records[slot];
        bool legacy = false;
        if (record->magic == 0xffffffffu &&
            color_settings_next_slot == COLOR_SETTINGS_SLOTS) {
            color_settings_next_slot = slot;
        }
        if (color_settings_record_valid(record, &legacy) &&
            (!found || record->sequence >= color_settings_sequence)) {
            selected_color_index = record->color_index;
            lid_bypass_enabled = legacy ? false : record->bypass_enabled != 0;
            color_settings_sequence = record->sequence;
            found = true;
        }
    }
}

static void __not_in_flash_func(write_color_setting_flash)(void *parameter) {
    const color_flash_write_t *write = (const color_flash_write_t *)parameter;
    if (write->erase_sector) {
        flash_range_erase(COLOR_SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    }
    flash_range_program(write->flash_offset,
                        (const uint8_t *)&write->record,
                        FLASH_PAGE_SIZE);
}

static bool save_color_setting(void) {
    const bool erase_sector =
        color_settings_next_slot >= COLOR_SETTINGS_SLOTS;
    const uint slot = erase_sector ? 0 : color_settings_next_slot;

    memset(&color_flash_write.record, 0xff,
           sizeof(color_flash_write.record));
    color_flash_write.erase_sector = erase_sector;
    color_flash_write.flash_offset =
        COLOR_SETTINGS_OFFSET + slot * FLASH_PAGE_SIZE;
    color_flash_write.record.magic = COLOR_SETTINGS_MAGIC;
    color_flash_write.record.sequence = ++color_settings_sequence;
    color_flash_write.record.color_index = selected_color_index;
    color_flash_write.record.color_inverse =
        (uint8_t)~selected_color_index;
    color_flash_write.record.bypass_enabled = lid_bypass_enabled ? 1u : 0u;
    color_flash_write.record.bypass_inverse =
        (uint8_t)~color_flash_write.record.bypass_enabled;
    color_flash_write.record.checksum = color_settings_checksum(
        color_flash_write.record.sequence, selected_color_index,
        lid_bypass_enabled);

    const int result = flash_safe_execute(write_color_setting_flash,
                                          &color_flash_write, 1000u);
    if (result != PICO_OK) {
        --color_settings_sequence;
        return false;
    }

    color_settings_next_slot = slot + 1;
    return true;
}

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

static void restart_ws2812_rx(PIO pio, uint sm, uint offset) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm,
                pio_encode_jmp(offset + ws2812_rx_wrap_target));
    pio_sm_set_enabled(pio, sm, true);
}

static void ws2812_tx_init(PIO pio, uint sm, uint offset, uint pin) {
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);

    pio_sm_config config = ws2812_tx_program_get_default_config(offset);
    sm_config_set_sideset_pins(&config, pin);
    sm_config_set_out_shift(&config, false, true, 24);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config,
                         (float)clock_get_hz(clk_sys) /
                             (WS2812_BIT_RATE * 10.0f));

    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_enabled(pio, sm, true);
}

static inline uint8_t max3(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t maximum = a > b ? a : b;
    return maximum > c ? maximum : c;
}

static inline uint8_t scale_channel(uint8_t channel, uint8_t brightness) {
    return (uint8_t)(((uint16_t)channel * brightness + 127u) / 255u);
}

static uint8_t input_brightness(uint32_t input_grb) {
    const uint8_t green = (uint8_t)(input_grb >> 16);
    const uint8_t red = (uint8_t)(input_grb >> 8);
    const uint8_t blue = (uint8_t)input_grb;
    return max3(red, green, blue);
}

static uint8_t mapped_brightness(const diagnostic_frame_t *frame,
                                 uint output_index) {
    // The four factory lenses consume these one-based address groups:
    // 2-4, 5-7, 8-10, and 11-12 plus 1 (wrapping around the frame).
    // Use each group's center emitter (inputs 3, 6, 9, and 12). Averaging
    // causes adjacent outputs 4 and 1 to overlap during the wrap transition.
    const uint center_input_index =
        2u + output_index * INPUTS_PER_OUTPUT;
    return input_brightness(frame->pixels[center_input_index]);
}

static void output_frame(PIO pio, uint sm, const diagnostic_frame_t *frame) {
    if (frame->pixel_count != HASBRO_INPUT_PIXELS) {
        return;
    }

    for (uint output_index = 0; output_index < OUTPUT_LED_COUNT; ++output_index) {
        const uint8_t brightness = mapped_brightness(frame, output_index);
        const output_color_t *color = &output_colors[selected_color_index];
        const uint32_t output_grb =
            ((uint32_t)scale_channel(color->green, brightness) << 16) |
            ((uint32_t)scale_channel(color->red, brightness) << 8) |
            scale_channel(color->blue, brightness);
        pio_sm_put_blocking(pio, sm, output_grb << 8);
    }
}

static void output_all_red(PIO pio, uint sm, bool on) {
    const uint32_t red_grb = on ? 0x00ff00u : 0u;
    for (uint index = 0; index < OUTPUT_LED_COUNT; ++index) {
        pio_sm_put_blocking(pio, sm, red_grb << 8);
    }
}

#if ENABLE_USB_DIAGNOSTICS
static void print_frame(const diagnostic_frame_t *frame) {
    if (frame->pixel_count == HASBRO_INPUT_PIXELS) {
        const output_color_t *color = &output_colors[frame->color_index];
        const char *lid_status = frame->lid_state == LID_STATE_BYPASSED
                                     ? "Bypassed"
                                     : frame->lid_state == LID_STATE_CLOSED
                                           ? "Closed"
                                           : "Open";
        printf("Frame %lu: 12 inputs -> 4 cyclotron LEDs, color %s, lid %s%s\n",
               (unsigned long)frame->number,
               color->name,
               lid_status,
               frame->truncated ? " (diagnostic buffer full)" : "");
        for (uint output_index = 0; output_index < OUTPUT_LED_COUNT;
             ++output_index) {
            const uint8_t brightness = mapped_brightness(frame, output_index);
            if (brightness > 0) {
                printf("  Cyclotron LED %u: brightness %u -> %s\n",
                       output_index + 1, brightness, color->name);
            }
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

static bool frames_match(const diagnostic_frame_t *a,
                         const diagnostic_frame_t *b) {
    return a->pixel_count == b->pixel_count &&
           a->color_index == b->color_index &&
           a->lid_state == b->lid_state &&
           a->truncated == b->truncated &&
           memcmp(a->pixels, b->pixels,
                  a->pixel_count * sizeof(a->pixels[0])) == 0;
}
#endif

static void diagnostics_core(void) {
    flash_safe_execute_core_init();
    multicore_fifo_push_blocking(1u);

#if ENABLE_USB_DIAGNOSTICS
    while (true) {
        queue_remove_blocking(&diagnostic_queue, &serial_frame);

        if (previous_serial_frame_valid &&
            frames_match(&serial_frame, &previous_serial_frame)) {
            continue;
        }

        print_frame(&serial_frame);
        previous_serial_frame = serial_frame;
        previous_serial_frame_valid = true;
    }
#else
    while (true) {
        __wfi();
    }
#endif
}

int main(void) {
    set_sys_clock_khz(SYSTEM_CLOCK_KHZ, true);
#if ENABLE_USB_DIAGNOSTICS
    stdio_init_all();
#endif
    load_color_setting();
    lid_switch_init();
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

    ws2812_rx_init(pio, rx_sm, rx_offset, WS2812_INPUT_PIN);
    ws2812_tx_init(pio, tx_sm, tx_offset, WS2812_OUTPUT_PIN);
    wait_for_ws2812_reset(WS2812_INPUT_PIN);
    restart_ws2812_rx(pio, rx_sm, rx_offset);
    pio_rx_wake_init(pio, rx_sm);
    add_repeating_timer_ms(-(int32_t)LID_POLL_MS,
                           idle_wake_timer_callback, NULL,
                           &idle_wake_timer);

#if ENABLE_USB_DIAGNOSTICS
    printf("%s v%s: GPIO %u -> GPIO %u, color %s\n",
           PROJECT_NAME, PROJECT_VERSION_STRING,
           WS2812_INPUT_PIN, WS2812_OUTPUT_PIN,
           output_colors[selected_color_index].name);
#endif

    uint32_t frame_number = 0;
    absolute_time_t last_pixel_time = get_absolute_time();
    absolute_time_t next_button_poll = get_absolute_time();
    absolute_time_t button_changed_at = get_absolute_time();
    absolute_time_t button_pressed_at = get_absolute_time();
    absolute_time_t next_lid_poll = get_absolute_time();
    absolute_time_t lid_changed_at = get_absolute_time();
    absolute_time_t color_save_at = at_the_end_of_time;
    absolute_time_t config_flash_at = at_the_end_of_time;
    bool receiving_frame = false;
    bool button_raw_pressed = false;
    bool button_pressed = false;
    bool button_long_press_handled = false;
    bool lid_raw_closed = false;
    bool lid_closed = false;
    bool color_save_pending = false;
    uint8_t config_flash_phase = 0;

    while (true) {
        if (!pio_sm_is_rx_fifo_empty(pio, rx_sm)) {
            // RX autopushes once per complete 24-bit GRB pixel. The complete
            // frame is retained so the Hasbro address groups can be mapped.
            const uint32_t input_grb =
                pio_sm_get(pio, rx_sm) & 0x00ffffffu;

            if (capture_frame.pixel_count < MAX_FRAME_PIXELS) {
                capture_frame.pixels[capture_frame.pixel_count++] = input_grb;
            } else {
                capture_frame.truncated = true;
            }
            last_pixel_time = get_absolute_time();
            receiving_frame = true;
            continue;
        }

        if (receiving_frame &&
            absolute_time_diff_us(last_pixel_time, get_absolute_time()) >=
                WS2812_RESET_US) {
            capture_frame.number = ++frame_number;
            restart_ws2812_rx(pio, rx_sm, rx_offset);
            capture_frame.color_index = selected_color_index;
            capture_frame.lid_state = lid_bypass_active()
                                          ? LID_STATE_BYPASSED
                                          : lid_closed ? LID_STATE_CLOSED
                                                       : LID_STATE_OPEN;
            if (config_flash_phase == 0) {
                output_frame(pio, tx_sm, &capture_frame);
            }
#if ENABLE_USB_DIAGNOSTICS
            queue_try_add(&diagnostic_queue, &capture_frame);
#endif
            capture_frame.pixel_count = 0;
            capture_frame.truncated = false;
            receiving_frame = false;
        }

        if (!lid_bypass_active() && time_reached(next_lid_poll)) {
            next_lid_poll = delayed_by_ms(next_lid_poll, LID_POLL_MS);
            const bool closed = !gpio_get(LID_SENSE_PIN);
            if (closed != lid_raw_closed) {
                lid_raw_closed = closed;
                lid_changed_at = get_absolute_time();
            } else if (closed != lid_closed &&
                       absolute_time_diff_us(lid_changed_at,
                                             get_absolute_time()) >=
                           LID_DEBOUNCE_MS * 1000u) {
                lid_closed = closed;
                set_lid_output(lid_closed);
            }
        }

        if (time_reached(next_button_poll)) {
            next_button_poll = delayed_by_ms(next_button_poll, BUTTON_POLL_MS);
            const bool pressed = read_bootsel_pressed();
            if (pressed != button_raw_pressed) {
                button_raw_pressed = pressed;
                button_changed_at = get_absolute_time();
            } else if (pressed != button_pressed &&
                       absolute_time_diff_us(button_changed_at,
                                             get_absolute_time()) >=
                           BUTTON_DEBOUNCE_MS * 1000u) {
                button_pressed = pressed;
                if (button_pressed) {
                    button_pressed_at = get_absolute_time();
                    button_long_press_handled = false;
                } else if (!button_long_press_handled) {
                    selected_color_index =
                        (selected_color_index + 1) % OUTPUT_COLOR_COUNT;
                    color_save_at = delayed_by_ms(get_absolute_time(),
                                                  COLOR_SAVE_DELAY_MS);
                    color_save_pending = true;
                }
            }

            if (button_pressed && !button_long_press_handled &&
                absolute_time_diff_us(button_pressed_at,
                                      get_absolute_time()) >=
                    BUTTON_LONG_PRESS_MS * 1000u) {
                button_long_press_handled = true;
                lid_bypass_enabled = !lid_bypass_enabled;

                if (lid_bypass_active()) {
                    set_lid_output(true);
                } else {
                    // Return to the default floating state, then debounce
                    // GPIO4 before allowing it to pull the output low.
                    lid_raw_closed = false;
                    lid_closed = false;
                    lid_changed_at = get_absolute_time();
                    next_lid_poll = get_absolute_time();
                    set_lid_output(false);
                }

                output_all_red(pio, tx_sm, true);
                config_flash_phase = 1;
                config_flash_at = delayed_by_ms(get_absolute_time(),
                                                CONFIG_FLASH_ON_MS);
                color_save_at = delayed_by_ms(get_absolute_time(),
                                              COLOR_SAVE_DELAY_MS);
                color_save_pending = true;
            }
        }

        if (config_flash_phase != 0 && time_reached(config_flash_at)) {
            if (config_flash_phase == 1) {
                output_all_red(pio, tx_sm, false);
                config_flash_phase = 2;
                config_flash_at = delayed_by_ms(get_absolute_time(),
                                                CONFIG_FLASH_OFF_MS);
            } else if (config_flash_phase == 2) {
                output_all_red(pio, tx_sm, true);
                config_flash_phase = 3;
                config_flash_at = delayed_by_ms(get_absolute_time(),
                                                CONFIG_FLASH_ON_MS);
            } else {
                output_all_red(pio, tx_sm, false);
                config_flash_phase = 0;
            }
        }

        if (color_save_pending && !button_pressed &&
            time_reached(color_save_at)) {
            if (save_color_setting()) {
                color_save_pending = false;
            } else {
                color_save_at = delayed_by_ms(get_absolute_time(),
                                              COLOR_SAVE_DELAY_MS);
            }
        }

        if (receiving_frame) {
            tight_loop_contents();
        } else {
            sleep_until_input_or_timer(pio, rx_sm);
        }
    }
}
