cmake_minimum_required(VERSION 3.20.0)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Generate src/autogen-code/settings_lorawan.{c,h,_list.h} from
# config/settings_lorawan.yaml at every configure -- see
# scripts/README.md. Never edit src/autogen-code/* by hand; edit the
# YAML instead and rebuild (builds are pristine by convention, see
# build.sh's -p always, so this reliably reruns).
#
# Deliberately NOT using the README's --aggregate pass or compiling
# rpc_settings_lorawan.c: that machinery (RPC dispatch by module name,
# a rpc_helpers.h JSON param parser) is for a multi-module,
# RPC-connected app -- this sample has exactly one settings module and
# no RPC layer, so pulling that in would be unused complexity. The
# generator still writes rpc_settings_lorawan.{c,h} to disk; they're
# simply never added to target_sources() below.
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_settings_module.py
            --yaml ${CMAKE_CURRENT_SOURCE_DIR}/config/settings_lorawan.yaml
            --kconfig-dir ${CMAKE_CURRENT_SOURCE_DIR}/kconfig
            --force
    RESULT_VARIABLE gen_res
)
if(NOT gen_res EQUAL 0)
    message(FATAL_ERROR "Failed to generate settings_lorawan")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_e5_device_node)

target_include_directories(app PRIVATE src/autogen-code)

target_sources(app PRIVATE
    src/main.c
    src/autogen-code/settings_lorawan.c
)
