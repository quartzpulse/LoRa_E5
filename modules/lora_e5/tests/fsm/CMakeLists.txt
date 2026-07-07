cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_e5_fsm_test)

target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../include
	${CMAKE_CURRENT_SOURCE_DIR}/../../src
	${CMAKE_CURRENT_SOURCE_DIR}/../mock_uart
)

target_sources(app PRIVATE
	src/main.c
	${CMAKE_CURRENT_SOURCE_DIR}/../mock_uart/mock_uart.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_parser.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_at.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_cmd_queue.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_hf_commands.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_modem_manager.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_fsm.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5_events.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/lora_e5.c
)

# Same rationale as tests/cmd_queue/tests/modem_manager: build this
# file subset (everything except lora_e5_uart.c, replaced by the
# tests/mock_uart fixture) directly into `app` rather than pulling in
# the whole module via ZEPHYR_EXTRA_MODULES + CONFIG_LORA_E5=y, to
# avoid a duplicate-symbol link error against the module's own
# zephyr_library() build of the same files.
target_compile_definitions(app PRIVATE
	CONFIG_LORA_E5_CMD_QUEUE_DEPTH=8
	CONFIG_LORA_E5_LOG_LEVEL=3
	CONFIG_LORA_E5_CMD_TIMEOUT_MS=500
	CONFIG_LORA_E5_TX_TIMEOUT_MS=1000
	CONFIG_LORA_E5_JOIN_TIMEOUT_MS=1000
	CONFIG_LORA_E5_MAX_RETRIES=3
	CONFIG_LORA_E5_BOOT_SETTLE_MS=50
	CONFIG_LORA_E5_TX_BUFFER_SIZE=242
	CONFIG_LORA_E5_AUTO_REJOIN=1
	CONFIG_LORA_E5_EVENT_QUEUE_DEPTH=16
	CONFIG_LORA_E5_RX_STACK_SIZE=1536
	CONFIG_LORA_E5_FSM_STACK_SIZE=2048
	CONFIG_LORA_E5_NOTIFY_STACK_SIZE=1024
)
