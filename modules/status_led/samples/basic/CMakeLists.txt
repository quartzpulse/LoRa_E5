cmake_minimum_required(VERSION 3.20.0)

# Standalone sample: register the status_led module without relying on a
# west manifest entry, so this sample builds on its own against any board.
get_filename_component(STATUS_LED_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
list(APPEND ZEPHYR_EXTRA_MODULES ${STATUS_LED_MODULE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(status_led_basic_sample)

target_sources(app PRIVATE src/main.c)
