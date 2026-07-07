cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_e5_cmd_queue_test)

target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../include
	${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_sources(app PRIVATE
	src/main.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_parser.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_at.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_cmd_queue.c
)

# This suite deliberately builds only this file subset directly into
# `app` rather than pulling in the whole module via ZEPHYR_EXTRA_MODULES
# + CONFIG_LORA_E5=y -- doing the latter would make the module's own
# CMakeLists.txt ALSO build these same files into a second library,
# causing duplicate-symbol link errors (confirmed empirically: real
# Kconfig values for symbols nested under `if LORA_E5` require
# CONFIG_LORA_E5=y, but that unconditionally triggers the module's
# zephyr_library() build of every src/*.c file). Supply the two symbols
# this subset still needs directly instead, now that the real Kconfig
# exists and the in-source #ifndef fallbacks are gone (CLAUDE.md: don't
# leave both defined).
target_compile_definitions(app PRIVATE
	CONFIG_LORA_E5_CMD_QUEUE_DEPTH=8
	CONFIG_LORA_E5_LOG_LEVEL=3
)