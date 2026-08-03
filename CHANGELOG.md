# Changelog

All notable project changes are documented here.

## [1.2.0] - 2026-08-03

- Added synchronized Cake chase speeds of 1×–5×, 10×, and 20×.
- Added a free-running timing mode with a configurable 100–10,000 ms rotation.
- Added solid, fade-out, four-pixel trail, and per-rotation color-shift effects.
- Added non-persistent animation preview start/stop commands to protocol 2.
- Migrated version 1 saved configurations without changing their existing
  Cake behavior.

## [1.1.0] - 2026-07-29

- Added browser-configurator testing for each of the four cyclotron LEDs.
- Added a cyclotron test target to the USB configuration protocol.

## [1.0.0] - 2026-07-29

- Added an always-available USB CDC configuration protocol for the browser
  configurator.
- Made cake LED count, controller type, color order, bitrate, RGB color,
  direction, and start offset configurable at runtime.
- Added remote configuration of the outer color and lid bypass.
- Added individual cake LED testing through the configuration protocol.
- Generalized the wear-leveled flash journal while retaining compatibility
  with existing color and lid-bypass records.
- Added the standalone PixelBro84 Configurator web project.

## [0.9.0] - 2026-07-26

- Added a dedicated WS2812 cyclotron cake output, now assigned to GPIO27.
- Added compile-time cake LED count and RGB configuration, defaulting to 12
  red LEDs.
- Changed the cake animation to a continuous, single-LED circular chase.
- Phase-locked the cake chase to measured outer cyclotron transitions, with
  quarter points aligned to the four outer LEDs and no accumulated timer drift.
- Added cake activity to optional USB diagnostics.

## [0.8.0] - 2026-07-25

- Renamed the project and firmware artifacts to PixelBro84.
- Added project ownership, version, release-date, and change-summary metadata.
- Licensed project code under PolyForm Noncommercial 1.0.0.
- Added third-party notices for Pico SDK, Pico examples, and TinyUSB.
- Added 48 MHz operation, event-driven idle sleep, and production builds with
  USB diagnostics disabled by default.
- Added persistent lid-bypass configuration and two-flash red confirmation.

## [0.7.0] - 2026-07-25

- Added active-low lid detection on GPIO4.
- Added open-drain-style lid output on GPIO29.
- Added BOOT long-press bypass control and flash persistence.

## [0.6.0] - 2026-07-20

- Added short-press color selection using the BOOT button.
- Added persistent color storage with a wear-leveled flash-page log.

## [0.5.0] - 2026-07-18

- Corrected the four-lens mapping to representative inputs 3, 6, 9, and 12.
- Added startup and frame-boundary synchronization.
- Rejected malformed or partial input frames.

## [0.3.0] - 2026-07-17

- Added USB serial frame diagnostics.
- Moved diagnostics to core 1 and suppressed duplicate frame reports.

## [0.1.0] - 2026-07-12

- Added initial RP2040 PIO receiver and WS2812 transmitter.
- Added brightness-preserving recoloring with red as the default.
