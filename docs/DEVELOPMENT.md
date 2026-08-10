# PixelBro84 Development Guide

This document is the canonical guide for developing the PixelBro84 firmware
and browser configurator together. The firmware repository owns the device
protocol and release artifacts; the configurator repository owns the Web
Serial user interface and Cloudflare Worker deployment.

## Repository map

| Repository | Responsibility |
| --- | --- |
| `A2ThreeD/PixelBro84` | RP2040 firmware, USB protocol, saved configuration migrations, UF2 releases |
| `A2ThreeD/PixelBro84-Configurator` | Browser UI, Web Serial client, firmware-update detection, Cloudflare Worker |
| `A2ThreeD/a2threed-website` | Static company site and published product guides on Cloudflare Pages |

When checked out side by side, the usual local layout is:

```text
A2ThreeD/
├── PixelBro84/
├── PixelBro84-Configurator/
└── a2threed-website/
```

## Coordinated version workflow

Firmware and configurator changes that alter settings or serial commands must
be developed together.

1. Start from the stable `main` branch in both PixelBro84 repositories.
2. Create matching release branches, such as `v1.3`, in both repositories.
3. Define the firmware protocol and saved-setting migration before depending
   on new fields in the configurator.
4. Implement configurator feature gates using the reported protocol number,
   not only the firmware version string.
5. Test the new configurator with both the previous stable firmware and the
   new firmware.
6. Commit and push each repository separately. Never assume that committing
   one repository includes the sibling repository.
7. Publish the firmware UF2 using the process in [RELEASING.md](RELEASING.md).
8. Deploy the configurator using its `docs/DEPLOYMENT.md` instructions.
9. After the release is accepted, merge the release branches into `main` so
   `main` again represents the current stable version.

Use patch releases for release corrections (`1.2.1`) rather than moving an
existing release tag. Git tags and published UF2 artifacts should be treated
as immutable. The consolidated PixelBro84 1.2.2 release combines the
previously unpublished 1.2.2–1.2.6 development fixes into one release; future
release notes should continue from this published version.

## Compatibility rules

- `PB84 HELLO` is the capability handshake. The configurator uses its
  `protocol` field to decide which controls and commands are available.
- Add fields and commands in a backward-compatible way whenever possible.
- Do not send fields to an older protocol that its parser does not support.
- Keep migration readers for every saved configuration version that has been
  shipped or used by a development build.
- Validate a migrated configuration before using or writing it.
- A failed connection must preserve its original error; serial cleanup must
  be safe when the browser or device has already closed the port.
- Firmware-update detection is independent of the USB protocol. It compares
  the connected firmware version with GitHub's latest published release.

See [PROTOCOL.md](PROTOCOL.md) for the current commands, fields, and version
matrix.

## Where version numbers live

Before a firmware release, keep these values synchronized:

- `src/project_info.h`: runtime version, release date, and change summary.
- `CMakeLists.txt`: Pico binary metadata version.
- `src/ws2812_rx.pio` and `src/ws2812_tx.pio`: source header comments.
- `README.md`: displayed version and release date.
- `CHANGELOG.md`: release entry.

For a coordinated configurator release, also update:

- `PixelBro84-Configurator/package.json` and its lockfile.
- User-facing minimum-version or compatibility text.

Protocol and saved-configuration versions are capability/schema identifiers,
not semantic firmware versions. Increase them only when their formats change.

## Local development

Firmware production build:

```sh
cmake -S . -B build-production \
  -DPICO_BOARD=waveshare_rp2040_zero \
  -DENABLE_USB_DIAGNOSTICS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-production --parallel
```

Firmware diagnostic build:

```sh
cmake -S . -B build-debug \
  -DPICO_BOARD=waveshare_rp2040_zero \
  -DENABLE_USB_DIAGNOSTICS=ON
cmake --build build-debug --parallel
```

Configurator development server:

```sh
cd ../PixelBro84-Configurator
npm install
npm run dev
```

Use Chrome or Edge on `localhost` because Web Serial requires a supported
browser and a secure context. Only one application may hold the serial port at
a time; close serial terminals before connecting from the configurator.

## Firmware runtime architecture

Core 0 owns WS2812 input capture, both PIO transmitters, runtime configuration,
button/lid state, and flash-setting requests. Its event loop preserves this
service order after input capture: finish a source frame, process one USB
request, expire tests, clear idle outputs, refresh animations, poll controls,
advance confirmation feedback, and save deferred settings.

Core 1 owns USB command assembly and optional diagnostic output so serial work
does not delay frame capture. Requests and responses cross cores through fixed
queues; hardware and flash mutations stay on core 0.

Cake and cyclotron outputs share the phase-clock state and animation sampling
math. Their physical index mapping, color selection, effect rendering, chain
length, bit rate, and PIO pacing remain separate. Keep that boundary when
adding effects so common timing fixes apply to both outputs without coupling
their hardware-specific frame generation.

## Required compatibility checks

For a change that touches the protocol or settings, test at least:

| Configurator | Firmware | Expected result |
| --- | --- | --- |
| New | Previous stable | Connects; legacy controls work; unsupported controls are disabled |
| New | New | All new controls, preview, tests, save, and reconnect work |
| Previous stable | New | Existing commands and fields remain accepted where promised |

Also verify:

- A saved configuration survives power cycling.
- Upgrading from the previous stable firmware retains existing settings.
- Startup with no input data leaves both LED strings off.
- Loss of the input stream clears outputs without late flashes.
- Free-running effects start and stop using the incoming activity signal.
- `npm run lint`, `npm run build`, and `npm run build:worker` pass for the
  configurator.

## Documentation ownership

- `README.md` explains the product and common setup.
- `CHANGELOG.md` records what changed in each released version.
- `docs/` explains development, protocol, and release procedures.
- `AGENTS.md` contains concise operational rules for coding agents and links
  back to these human-readable documents.

Do not store secrets, API tokens, or private keys in documentation or agent
files.
