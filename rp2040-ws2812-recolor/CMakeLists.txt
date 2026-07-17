cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(ws2812_recolor C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(ws2812_recolor
    src/main.c
)

pico_generate_pio_header(ws2812_recolor
    ${CMAKE_CURRENT_LIST_DIR}/src/ws2812_rx.pio
)
pico_generate_pio_header(ws2812_recolor
    ${CMAKE_CURRENT_LIST_DIR}/src/ws2812_tx.pio
)

target_link_libraries(ws2812_recolor
    pico_stdlib
    hardware_pio
)

pico_enable_stdio_usb(ws2812_recolor 1)
pico_enable_stdio_uart(ws2812_recolor 0)
pico_add_extra_outputs(ws2812_recolor)
