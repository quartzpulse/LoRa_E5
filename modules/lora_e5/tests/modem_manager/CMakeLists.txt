cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_e5_modem_manager_test)

target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../include
	${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_sources(app PRIVATE
	src/main.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_parser.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_at.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_cmd_queue.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_hf_commands.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_modem_manager.c
)

# See tests/cmd_queue/CMakeLists.txt's comment: this suite builds only
# this file subset directly rather than pulling in the whole module via
# ZEPHYR_EXTRA_MODULES + CONFIG_LORA_E5=y, to avoid a duplicate-symbol
# link error against the module's own zephyr_library() build of the
# same files.
target_compile_definitions(app PRIVATE
	CONFIG_LORA_E5_CMD_QUEUE_DEPTH=8
	CONFIG_LORA_E5_LOG_LEVEL=3
	CONFIG_LORA_E5_CMD_TIMEOUT_MS=2000
	CONFIG_LORA_E5_TX_TIMEOUT_MS=10000
	CONFIG_LORA_E5_JOIN_TIMEOUT_MS=15000
	CONFIG_LORA_E5_MAX_RETRIES=3
)
