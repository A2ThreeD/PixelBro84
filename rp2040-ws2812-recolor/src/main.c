#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "ws2812_rx.pio.h"
#include "ws2812_tx.pio.h"

// Waveshare RP2040-Zero pins. Change these if your wiring uses other GPIOs.
#define WS2812_INPUT_PIN  2u
#define WS2812_OUTPUT_PIN 3u

#define WS2812_BIT_RATE 800000.0f
#define WS2812_RX_CLOCK 8000000.0f
#define WS2812_RESET_US 80u
#define MAX_FRAME_PIXELS 256u
#define DIAGNOSTIC_QUEUE_DEPTH 2u
#define HASBRO_INPUT_PIXELS 12u
#define OUTPUT_LED_COUNT 4u
#define INPUTS_PER_OUTPUT 3u

// Desired output tint. Red is selected by default.
#define TARGET_RED   255u
#define TARGET_GREEN 0u
#define TARGET_BLUE  0u

typedef struct {
    uint32_t number;
    uint32_t pixels[MAX_FRAME_PIXELS];
    uint16_t pixel_count;
    bool truncated;
} diagnostic_frame_t;

static queue_t diagnostic_queue;
static diagnostic_frame_t capture_frame;
static diagnostic_frame_t serial_frame;
static diagnostic_frame_t previous_serial_frame;
static bool previous_serial_frame_valid;

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

static uint32_t recolor_grb(uint32_t input_grb) {
    const uint8_t green = (uint8_t)(input_grb >> 16);
    const uint8_t red = (uint8_t)(input_grb >> 8);
    const uint8_t blue = (uint8_t)input_grb;
    const uint8_t brightness = max3(red, green, blue);
    const uint8_t output_red = scale_channel(TARGET_RED, brightness);
    const uint8_t output_green = scale_channel(TARGET_GREEN, brightness);
    const uint8_t output_blue = scale_channel(TARGET_BLUE, brightness);

    return ((uint32_t)output_green << 16) |
           ((uint32_t)output_red << 8) |
           output_blue;
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
    const uint group_start = 1u + output_index * INPUTS_PER_OUTPUT;
    uint16_t sum = 0;
    for (uint offset = 0; offset < INPUTS_PER_OUTPUT; ++offset) {
        const uint input_index = (group_start + offset) % HASBRO_INPUT_PIXELS;
        sum += input_brightness(frame->pixels[input_index]);
    }
    return (uint8_t)((sum + INPUTS_PER_OUTPUT / 2) / INPUTS_PER_OUTPUT);
}

static void output_frame(PIO pio, uint sm, const diagnostic_frame_t *frame) {
    if (frame->pixel_count != HASBRO_INPUT_PIXELS) {
        for (uint index = 0; index < frame->pixel_count; ++index) {
            pio_sm_put_blocking(pio, sm, recolor_grb(frame->pixels[index]) << 8);
        }
        return;
    }

    for (uint output_index = 0; output_index < OUTPUT_LED_COUNT; ++output_index) {
        const uint8_t brightness = mapped_brightness(frame, output_index);
        const uint32_t red_grb = (uint32_t)brightness << 8;
        pio_sm_put_blocking(pio, sm, red_grb << 8);
    }
}

static void print_frame(const diagnostic_frame_t *frame) {
    if (frame->pixel_count == HASBRO_INPUT_PIXELS) {
        printf("Frame %lu: 12 inputs -> 4 cyclotron LEDs%s\n",
               (unsigned long)frame->number,
               frame->truncated ? " (diagnostic buffer full)" : "");
        for (uint output_index = 0; output_index < OUTPUT_LED_COUNT;
             ++output_index) {
            const uint8_t brightness = mapped_brightness(frame, output_index);
            if (brightness > 0) {
                printf("  Cyclotron LED %u: brightness %u -> RED(%u)\n",
                       output_index + 1, brightness, brightness);
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
           a->truncated == b->truncated &&
           memcmp(a->pixels, b->pixels,
                  a->pixel_count * sizeof(a->pixels[0])) == 0;
}

static void diagnostics_core(void) {
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
}

int main(void) {
    stdio_init_all();
    queue_init(&diagnostic_queue, sizeof(diagnostic_frame_t),
               DIAGNOSTIC_QUEUE_DEPTH);
    multicore_launch_core1(diagnostics_core);

    PIO pio = pio0;
    const uint rx_offset = pio_add_program(pio, &ws2812_rx_program);
    const uint tx_offset = pio_add_program(pio, &ws2812_tx_program);
    const uint rx_sm = pio_claim_unused_sm(pio, true);
    const uint tx_sm = pio_claim_unused_sm(pio, true);

    ws2812_rx_init(pio, rx_sm, rx_offset, WS2812_INPUT_PIN);
    ws2812_tx_init(pio, tx_sm, tx_offset, WS2812_OUTPUT_PIN);

    printf("WS2812 recolor: GPIO %u -> GPIO %u, target RGB(%u,%u,%u)\n",
           WS2812_INPUT_PIN, WS2812_OUTPUT_PIN,
           TARGET_RED, TARGET_GREEN, TARGET_BLUE);

    uint32_t frame_number = 0;
    absolute_time_t last_pixel_time = get_absolute_time();
    bool receiving_frame = false;

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
            output_frame(pio, tx_sm, &capture_frame);
            queue_try_add(&diagnostic_queue, &capture_frame);
            capture_frame.pixel_count = 0;
            capture_frame.truncated = false;
            receiving_frame = false;
        }

        tight_loop_contents();
    }
}
