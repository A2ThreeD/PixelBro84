/*
 * Project:    PixelBro84
 * Programmer: Aaron Morris
 * Company:    A2ThreeD
 * Version:    1.2.4
 * Date:       2026-08-09
 *
 * Copyright (c) 2026 Aaron Morris / A2ThreeD.
 *
 * Licensed under the PolyForm Noncommercial License 1.0.0.
 * Noncommercial use, modification, and distribution are permitted under the
 * terms in LICENSE. Commercial use, including use in a product or service
 * offered for sale, requires separate written permission from Aaron Morris /
 * A2ThreeD.
 *
 * Third-party components retain their own licenses. See
 * THIRD_PARTY_NOTICES.md for the Raspberry Pi Pico SDK (BSD-3-Clause),
 * TinyUSB (MIT), and related attribution.
 *
 * Release history: see CHANGELOG.md.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#ifndef A2THREED_PIXELBRO84_PROJECT_INFO_H
#define A2THREED_PIXELBRO84_PROJECT_INFO_H

// Human-readable identity embedded in the firmware image.
#define PROJECT_NAME "PixelBro84"
#define PROJECT_PROGRAMMER "Aaron Morris"
#define PROJECT_COMPANY "A2ThreeD"

// Keep the numeric components and display string synchronized for releases.
#define PROJECT_VERSION_MAJOR 1
#define PROJECT_VERSION_MINOR 2
#define PROJECT_VERSION_PATCH 4
#define PROJECT_VERSION_STRING "1.2.4"
#define PROJECT_RELEASE_DATE "2026-08-09"

// License and release summary reported by the Pico binary metadata.
#define PROJECT_LICENSE "PolyForm-Noncommercial-1.0.0"
#define PROJECT_CHANGE_SUMMARY \
    "Added a clear-all LED test command"

#endif
