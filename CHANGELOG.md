# Changelog

All notable project changes are documented here.

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
