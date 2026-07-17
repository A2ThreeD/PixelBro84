# RP2040 WS2812 Recolor

Firmware for a Waveshare RP2040-Zero that receives an 800 kHz WS2812 data
stream and retransmits the same pixel pattern in a chosen color. The default
target is red.

The input pixel's strongest RGB channel is used as its brightness. An off pixel
therefore remains off, a dim pixel remains dim, and any lit color becomes red.
The output follows the input with approximately one LED of latency.

## Wiring

| Signal | RP2040-Zero pin |
| --- | --- |
| Incoming WS2812 data | GPIO2 |
| Recolored WS2812 output | GPIO3 |
| Source/controller ground | GND |
| LED power-supply ground | GND |

The controller, RP2040-Zero, and LED power supply must share ground. Do not feed
a 5 V data signal directly into the RP2040. Use a 5 V-tolerant buffer or a
resistor divider on GPIO2 when the source logic level exceeds 3.3 V. For a
reliable 5 V WS2812 output, use a 74AHCT125 or 74HCT245 level shifter between
GPIO3 and the first LED. A 220-470 ohm series resistor near the output driver is
also recommended.

## Build

Install the Raspberry Pi Pico SDK and export `PICO_SDK_PATH`, then run:

```sh
cmake -S . -B build -DPICO_BOARD=waveshare_rp2040_zero
cmake --build build
```

Copy `build/ws2812_recolor.uf2` to the RP2040 boot drive while holding BOOT.

If an older Pico SDK does not include the Waveshare board definition, configure
with `-DPICO_BOARD=pico`; this firmware only depends on the standard RP2040 GPIO
and PIO hardware.

## Configuration

Pin assignments and target RGB values are near the top of `src/main.c`:

```c
#define WS2812_INPUT_PIN  2u
#define WS2812_OUTPUT_PIN 3u

#define TARGET_RED   255u
#define TARGET_GREEN 0u
#define TARGET_BLUE  0u
```

The receiver expects standard 24-bit, MSB-first, GRB WS2812/SK6812 data at
800 kHz. RGBW strips and 400 kHz variants are not supported by this version.
