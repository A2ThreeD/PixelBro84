# PixelBro84

| Project field | Value |
| --- | --- |
| Version | 0.9.0 |
| Release date | July 26, 2026 |
| Programmer | Aaron Morris |
| Company | A2ThreeD |

Firmware for a Waveshare (or clone) RP2040-Zero that receives the Hasbro
cyclotron's 800 kHz WS2812 data stream and drives four replacement RGB WS2812B
LEDs in a chosen color. It also drives a synchronized cyclotron cake string on
GPIO28. Both outputs default to red.

## License

The original A2ThreeD source code in this repository is available under the
[PolyForm Noncommercial License 1.0.0](LICENSE). It may be used for permitted
noncommercial purposes under that license. Using it in a commercial product or
service requires separate written permission from Aaron Morris / A2ThreeD.

Third-party software remains under its own license:

- Raspberry Pi Pico SDK: BSD 3-Clause License
- Pico SDK WS2812 example material: BSD 3-Clause License
- TinyUSB, included by the Pico SDK when USB diagnostics are enabled: MIT License

See [NOTICE](NOTICE) for the required project notices,
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party license text,
and [CHANGELOG.md](CHANGELOG.md) for version history.

Live capture showed that each factory cyclotron lens uses three of the 12 input
addresses. The groups are 2-4, 5-7, 8-10, and 11-12 plus 1. Each group's
center address (3, 6, 9, or 12) controls one output LED. Using the center
address avoids the brief address-1/address-4 overlap at the wraparound while
preserving the factory sequence and brightness timing.

At startup the receiver waits for a complete WS2812 reset-low interval before
capturing data. It realigns at every frame boundary and ignores malformed frames
instead of forwarding partial startup data.

Between LED frames, core 0 enters `WFI` sleep. A PIO RX interrupt wakes it as
soon as the first complete input pixel arrives, while a 5 ms timer services the
lid and BOOT controls. The system clock runs at 48 MHz; the PIO clocks remain at
their exact 8 MHz receive and 800 kHz transmit rates.

## Wiring

| Signal | RP2040-Zero pin |
| --- | --- |
| Incoming WS2812 data | GPIO2 |
| Recolored WS2812 output | GPIO3 |
| Cyclotron cake WS2812 output | GPIO28 |
| Cyclotron lid sense, grounded when closed | GPIO4 |
| Cyclotron lid open-drain output | GPIO29 |
| Source/controller ground | GND |
| LED power-supply ground | GND |

The controller, RP2040-Zero, and LED power supply must share ground. The voltages that I've measure on the Hasbro 1984 pack are around 4.2V for the cyclotron LED output on the factory electronics. It's a good idea to use a 5V tolerant buffer or a
resistor divider on GPIO2 when the source logic level exceeds 3.3V. My builds use a 1K/2K voltage divider circuit as an inexpensive method. A 220-470 ohm series resistor near the output driver is also recommended.

GPIO28 drives the data input of the first cake WS2812 LED. The cake string must
share ground with the RP2040 and its LED power supply. Use the same appropriate
logic-level shifting and series-resistor practices described for GPIO3.

## Cyclotron cake

The cake defaults to 12 red WS2812 LEDs. A single lit LED chases continuously
from cake LED 1 through the end of the string, then wraps back to LED 1. The
firmware measures the time between outer cyclotron transitions and divides each
interval across one quarter of the cake. Every outer transition is a hard sync
point, so the cake follows speed changes without accumulating timer drift.

For a 12-LED cake, LEDs 1, 4, 7, and 10 align with outer cyclotron LEDs 1, 2,
3, and 4. The first complete outer interval after startup calibrates the chase
speed. Other cake lengths are divided evenly across the same four phases.

Set the cake length and fixed RGB color near the top of `src/main.c`:

```c
#define CAKE_LED_COUNT   12u
#define CAKE_COLOR_RED   255u
#define CAKE_COLOR_GREEN 0u
#define CAKE_COLOR_BLUE  0u
```

## Cyclotron lid switch

GPIO4 uses its internal pull-up and treats a connection to ground as lid
installed/closed. GPIO29 mirrors that state as an open-drain-style output:

- Lid closed: GPIO29 actively drives low.
- Lid open: GPIO29 is an input with pulls disabled, so it is high-impedance.
- At startup: GPIO29 defaults to high-impedance until a closed lid is debounced.

GPIO29 never drives high. To bypass lid detection and keep GPIO29 low, change
this setting near the top of `src/main.c`:

```c
#define LID_DETECTION_BYPASS true
```

## Build

Install the Raspberry Pi Pico SDK and export `PICO_SDK_PATH`, then run:

```sh
cmake -S . -B build -DPICO_BOARD=waveshare_rp2040_zero
cmake --build build
```

Copy `build/PixelBro84.uf2` to the RP2040 boot drive while holding BOOT.

If an older Pico SDK does not include the Waveshare board definition, configure
with `-DPICO_BOARD=pico`; this firmware only depends on the standard RP2040 GPIO
and PIO hardware.

## Configuration

Pin assignments and target RGB values are near the top of `src/main.c`:

```c
#define WS2812_INPUT_PIN  2u
#define WS2812_OUTPUT_PIN 3u
#define CAKE_OUTPUT_PIN   28u
#define LID_SENSE_PIN     4u
#define LID_OUTPUT_PIN    29u
#define LID_DETECTION_BYPASS false
```

The receiver expects standard 24-bit, MSB-first, GRB WS2812/SK6812 data at
800 kHz. RGBW strips and 400 kHz variants are not supported by this version.

## Function button

Momentarily press and release BOOT while the firmware is running to cycle the
output color:

```text
Red -> Green -> Blue -> Yellow -> Purple -> Red
```

Hold BOOT for two seconds while running to toggle the cyclotron lid bypass. All
four output LEDs flash red together twice to confirm the configuration change.

The selected color and bypass state are saved one second after the last button
action and restored the next time the pack powers on. Settings use a small
wear-leveled log in the final 4 KB flash sector. Holding BOOT while powering or
resetting the board still enters the RP2040 UF2 bootloader.

## Power Considerations

The default mode of this project when idling and serial diagnostics enabled seemed to show about a 22mA draw. Disabling diagnostics and adding sleep should reduce that considerably, but I don't have a meter with resolution good enough to measure it now. With the pack fully powered up and this mod, it seems to consume around 100mA.

## Serial diagnostics

USB diagnostics are disabled by default to reduce idle power. Enable them in a
separate debug build with:

```sh
cmake -S . -B build-debug \
  -DPICO_BOARD=waveshare_rp2040_zero \
  -DENABLE_USB_DIAGNOSTICS=ON
cmake --build build-debug
```

When enabled, USB serial reports changed frames and each LED instructed to turn
on. Repeated identical frames are suppressed. Diagnostics run on the RP2040's
second core so USB output cannot block the time-sensitive LED forwarding loop.
LED numbering starts at 1. Connect at 115200 baud; USB CDC does not depend on
the selected baud rate, but 115200 is a convenient terminal default.

Example:

```text
Frame 12: 12 inputs -> 4 cyclotron LEDs
  Cyclotron LED 2: brightness 255 -> RED(255)
```

The diagnostic frame buffer holds 256 pixels. Recoloring and output continue if
a larger frame arrives, but only the first 256 pixels are listed in the report.

With diagnostics disabled, the running firmware does not expose USB serial or
the `picotool -f` reset interface. Hold BOOT while connecting USB to mount the
`RPI-RP2` drive for future production firmware updates.
