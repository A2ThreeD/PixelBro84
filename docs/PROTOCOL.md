# PixelBro84 USB Protocol

PixelBro84 exposes a line-oriented ASCII protocol through USB CDC. The browser
opens the emulated serial port at 115200 baud; USB CDC does not depend on the
selected baud rate.

Each command and response occupies one line. Commands begin with `PB84` and
fields use whitespace-separated `key=value` tokens.

## Connection handshake

The configurator begins with:

```text
PB84 HELLO
```

The device responds with:

```text
PB84 HELLO protocol=4 product=PixelBro84 firmware=1.2.0
```

The configurator must verify `product=PixelBro84` and use `protocol` for
feature detection. The `firmware` value is used for release comparison and
display.

## Commands

```text
PB84 GET
PB84 SET <fields>
PB84 PREVIEW START <fields>
PB84 PREVIEW STOP
PB84 TEST led=<index> red=<0-255> green=<0-255> blue=<0-255> duration_ms=<ms>
PB84 TEST target=cyclotron led=<1-4> color_index=<index> duration_ms=<ms>
PB84 TEST CLEAR
```

- `GET` returns the complete active configuration.
- `SET` validates and persists a configuration update, then replies with
  `PB84 OK` or `PB84 ERROR <message>`.
- `PREVIEW START` applies settings without writing flash and starts both the
  cyclotron and Cake animations even when no WS2812 input is arriving.
- In synchronized preview mode, the firmware uses a synthetic 250 ms phase
  pulse as the assumed input clock. This is a preview-only assumption; live
  synchronized operation continues to learn timing from the incoming frame.
- In free-running preview mode, each output uses its configured rotation
  duration and effect settings. The preview continues until `PREVIEW STOP`.
- `PREVIEW STOP` restores the saved settings and clears the preview output.
- `TEST` temporarily illuminates a selected Cake or cyclotron LED.
- `TEST CLEAR` clears both LED output chains and cancels active LED tests.

`SET` and `PREVIEW START` accept partial updates based on the active
configuration. The configurator normally sends every field supported by the
connected protocol.

## Protocol versions

| Protocol | Firmware/use | Capability summary |
| --- | --- | --- |
| 1 | Firmware 1.0/1.1 | Original Cake hardware settings, colors, direction, lid bypass, and LED tests |
| 2 | Firmware 1.2 development schema | Added Cake timing, speed, effect, rotation, and preview fields |
| 3 | Firmware 1.2 development schema | Added configurable cyclotron animation and hardware fields with a legacy count |
| 4 | Firmware 1.2.0 | Replaced the cyclotron count UI with explicit single-pixel and puck styles |

Versions 2 and 3 were used during 1.2 development. Their saved records remain
readable so devices flashed with intermediate builds retain their settings.

## Configuration fields

Fields available from protocol 1:

```text
version
cake_led_count
cake_led_type
cake_color_order
cake_bit_rate_khz
cake_red
cake_green
cake_blue
cake_reverse
cake_start_offset
outer_color_index
lid_bypass
```

Protocol 2 adds:

```text
cake_timing_mode
cake_speed_multiplier
cake_effect
cake_rotation_ms
preview
```

Protocol 3 adds:

```text
cyclotron_led_count
cyclotron_led_type
cyclotron_color_order
cyclotron_bit_rate_khz
cyclotron_timing_mode
cyclotron_speed_multiplier
cyclotron_effect
cyclotron_rotation_ms
cyclotron_reverse
```

Protocol 4 adds:

```text
cyclotron_led_style
```

Current enumerated values:

| Field | Values |
| --- | --- |
| `cake_led_type`, `cyclotron_led_type` | `WS2812B`, `WS2811` |
| `cake_color_order`, `cyclotron_color_order` | `GRB`, `RGB` |
| `cake_timing_mode`, `cyclotron_timing_mode` | `SYNCED`, `FREE` |
| `cake_effect`, `cyclotron_effect` | `SOLID`, `FADE`, `TRAIL`, `COLOR_SHIFT` |
| `cyclotron_led_style` | `SINGLE`, `PUCK3`, `PUCK5`, `PUCK9` |
| speed multiplier | `1`, `2`, `3`, `4`, `5`, `10`, `20` |

Cyclotron styles map to physical output lengths as follows:

| Style | Physical pixels | Illuminated one-based positions |
| --- | ---: | --- |
| `SINGLE` | 4 | 1, 2, 3, 4 |
| `PUCK3` | 12 | 2, 5, 8, 11 |
| `PUCK5` | 20 | 3, 8, 13, 18 |
| `PUCK9` | 36 | 5, 14, 23, 32 |

## Saved configuration

Settings are stored in a wear-leveled journal in the final 4 KB flash sector.
Each schema has a versioned fixed layout and checksum. Firmware 1.2.0 uses
schema version 4 and migrates legacy color/lid records plus schema versions 1,
2, and 3.

When changing the schema:

1. Preserve the exact old structure in a version-specific type.
2. Validate the old record and checksum before migration.
3. Start from current defaults, then copy supported old values.
4. Normalize values that have a new representation.
5. Validate the completed current configuration before use.

Never reinterpret an existing schema version with a different memory layout.

## Compatibility policy

- Unknown fields must not be sent to older firmware.
- New configurator controls must be gated by the handshake protocol number.
- The current configurator supports protocol 1 devices and fills missing
  fields with safe display defaults.
- The firmware should continue accepting older request versions when the
  requested fields can be represented safely.
- Increase command/response buffer sizes when new serialized fields approach
  existing limits; test a complete save command, not only partial commands.
