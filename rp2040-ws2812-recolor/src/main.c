#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "ws2812_rx.pio.h"
#include "ws2812_tx.pio.h"

// Waveshare RP2040-Zero pins. Change these if your wiring uses other GPIOs.
#define WS2812_INPUT_PIN  2u
#define WS2812_OUTPUT_PIN 3u

#define WS2812_BIT_RATE 800000.0f
#define WS2812_RX_CLOCK 8000000.0f

// Desired output tint. Red is selected by default.
#define TARGET_RED   255u
#define TARGET_GREEN 0u
#define TARGET_BLUE  0u

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

int main(void) {
    stdio_init_all();

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

    while (true) {
        // RX autopushes once per complete 24-bit GRB pixel. The TX state
        // machine runs concurrently, giving the output about one-pixel delay.
        const uint32_t input_grb = pio_sm_get_blocking(pio, rx_sm) & 0x00ffffffu;
        const uint32_t output_grb = recolor_grb(input_grb);
        pio_sm_put_blocking(pio, tx_sm, output_grb << 8);
    }
}
