# PixelBro84
# Copyright (c) 2026 Aaron Morris / A2ThreeD
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# See LICENSE and THIRD_PARTY_NOTICES.md.

cmake_minimum_required(VERSION 3.13)

# Import and initialize the Raspberry Pi Pico SDK toolchain.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(PixelBro84 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(ENABLE_USB_DIAGNOSTICS "Enable USB CDC frame diagnostics" OFF)

pico_sdk_init()

# Build the firmware and attach the identity shown by Pico tooling.
add_executable(PixelBro84
    src/main.c
    src/user_config.c
)

pico_set_program_name(PixelBro84 "PixelBro84")
pico_set_program_version(PixelBro84 "1.2.4")

# Compile both PIO assembly programs into headers consumed by main.c.
pico_generate_pio_header(PixelBro84
    ${CMAKE_CURRENT_LIST_DIR}/src/ws2812_rx.pio
)
pico_generate_pio_header(PixelBro84
    ${CMAKE_CURRENT_LIST_DIR}/src/ws2812_tx.pio
)

target_link_libraries(PixelBro84
    pico_stdlib
    pico_flash
    pico_multicore
    hardware_flash
    hardware_pio
)

# USB diagnostics are optional; UART output remains disabled in all builds.
if(ENABLE_USB_DIAGNOSTICS)
    target_compile_definitions(PixelBro84 PRIVATE ENABLE_USB_DIAGNOSTICS=1)
else()
    target_compile_definitions(PixelBro84 PRIVATE ENABLE_USB_DIAGNOSTICS=0)
endif()
pico_enable_stdio_usb(PixelBro84 1)
pico_enable_stdio_uart(PixelBro84 0)
pico_add_extra_outputs(PixelBro84)
