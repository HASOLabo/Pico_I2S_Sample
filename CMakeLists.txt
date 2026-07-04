cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(Pico_I2S_Sample C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(Pico_I2S_Sample)

target_sources(Pico_I2S_Sample PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        )

# pull in common dependencies
target_link_libraries(Pico_I2S_Sample pico_stdlib pico_audio_i2s)

# enable usb output, disable uart output
pico_enable_stdio_usb(Pico_I2S_Sample 1)
pico_enable_stdio_uart(Pico_I2S_Sample 0)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(Pico_I2S_Sample)

# add url via pico_set_program_url
#example_auto_set_url(Pico_I2S_Sample)

