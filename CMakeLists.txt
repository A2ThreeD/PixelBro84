# PixelBro84
# Copyright (c) 2026 Aaron Morris / A2ThreeD
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# See LICENSE and THIRD_PARTY_NOTICES.md.

cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(PixelBro84 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
option(ENABLE_USB_DIAGNOSTICS "Enable USB CDC frame diagnostics" OFF)

pico_sdk_init()

add_executable(PixelBro84
    src/main.c
)

pico_set_program_name(PixelBro84 "PixelBro84")
pico_set_program_version(PixelBro84 "0.8.0")

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

if(ENABLE_USB_DIAGNOSTICS)
    target_compile_definitions(PixelBro84 PRIVATE ENABLE_USB_DIAGNOSTICS=1)
    pico_enable_stdio_usb(PixelBro84 1)
else()
    target_compile_definitions(PixelBro84 PRIVATE ENABLE_USB_DIAGNOSTICS=0)
    pico_enable_stdio_usb(PixelBro84 0)
endif()
pico_enable_stdio_uart(PixelBro84 0)
pico_add_extra_outputs(PixelBro84)
