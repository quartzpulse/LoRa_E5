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