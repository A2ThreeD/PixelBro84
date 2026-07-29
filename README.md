# PixelBro84

| Project field | Value |
| --- | --- |
| Version | 1.0.0 |
| Release date | July 29, 2026 |
| Programmer | Aaron Morris |
| Company | A2ThreeD |

Firmware for a Waveshare (or clone) RP2040-Zero that receives the Hasbro
cyclotron's 800 kHz WS2812 data stream and drives four replacement RGB WS2812B
LEDs in a chosen color. It also drives a synchronized cyclotron cake string on
GPIO27. Both outputs default to red.

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
| Incoming WS2812 data | GPIO29 |
| Recolored WS2812 output | GPIO3 |
| Cyclotron cake WS2812 output | GPIO27 |
| Cyclotron lid sense, grounded when closed | GPIO4 |
| Cyclotron lid open-drain output | GPIO28 |
| Source/controller ground | GND |
| LED power-supply ground | GND |

The controller, RP2040-Zero, and LED power supply must share ground. The voltages that I've measure on the Hasbro 1984 pack are around 4.2V for the cyclotron LED output on the factory electronics. It's a good idea to use a 5V tolerant buffer or a
resistor divider on GPIO29 when the source logic level exceeds 3.3V. My builds use a 1K/2K voltage divider circuit as an inexpensive method. A 220-470 ohm series resistor near the output driver is also recommended.

GPIO27 drives the data input of the first cake WS2812 LED. The cake string must
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

Cake length, controller type, color order, bitrate, RGB color, direction, and
start offset are runtime settings managed by the browser configurator.

## Cyclotron lid switch

GPIO4 uses its internal pull-up and treats a connection to ground as lid
installed/closed. GPIO28 mirrors that state as an open-drain-style output:

- Lid closed: GPIO28 actively drives low.
- Lid open: GPIO28 is an input with pulls disabled, so it is high-impedance.
- At startup: GPIO28 defaults to high-impedance until a closed lid is debounced.

GPIO28 never drives high. To bypass lid detection and keep GPIO28 low, change
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
#define WS2812_INPUT_PIN  29u
#define WS2812_OUTPUT_PIN 3u
#define CAKE_OUTPUT_PIN   27u
#define LID_SENSE_PIN     4u
#define LID_OUTPUT_PIN    28u
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

All configuration values are stored in a wear-leveled log in the final 4 KB
flash sector and restored when the pack powers on. Firmware 1.0.0 migrates
existing color and lid-bypass records automatically. Holding BOOT while
powering or resetting the board still enters the RP2040 UF2 bootloader.

## Browser configurator

Production firmware exposes a USB CDC configuration port. Open the standalone
PixelBro84 Configurator in a Web Serial-capable desktop browser, connect the
controller, adjust settings, test individual cake LEDs, and select **Save to
PixelBro84**. The device validates the complete configuration before appending
it to the flash journal.

The configurator source is maintained in the sibling
`PixelBro84-Configurator` project. USB CDC uses 115200 baud as a conventional
host setting, although the emulated USB port does not depend on that baud rate.

The versioned text protocol is also usable from a terminal:

```text
PB84 HELLO
PB84 GET
PB84 SET version=1 cake_led_count=12 cake_led_type=WS2812B cake_color_order=GRB cake_bit_rate_khz=800
PB84 TEST led=1 red=255 green=0 blue=0 duration_ms=700
```

`SET` accepts partial updates, validates the resulting complete configuration,
and replies with either `PB84 OK` or a descriptive `PB84 ERROR`.

## Power Considerations

The default mode of this project when idling and serial diagnostics enabled seemed to show about a 22mA draw. Disabling diagnostics and adding sleep should reduce that considerably, but I don't have a meter with resolution good enough to measure it now. With the pack fully powered up and this mod, it seems to consume around 100mA.

## Serial diagnostics

Verbose frame diagnostics are disabled by default. The lightweight USB
configuration port remains available in every build. Enable frame reporting in
a separate debug build with:

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

With diagnostics disabled, USB serial carries configuration responses only and
the `picotool -f` reset interface remains available. Holding BOOT while
connecting USB still mounts the `RPI-RP2` drive for recovery and firmware
updates.
