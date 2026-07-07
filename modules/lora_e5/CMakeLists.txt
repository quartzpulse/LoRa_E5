# SPDX-License-Identifier: Apache-2.0

if(CONFIG_LORA_E5)

zephyr_include_directories(include)

zephyr_library()

zephyr_library_sources(
  src/lora_e5.c
  src/lora_e5_parser.c
  src/lora_e5_at.c
  src/lora_e5_cmd_queue.c
  src/lora_e5_hf_commands.c
  src/lora_e5_modem_manager.c
  src/lora_e5_fsm.c
  src/lora_e5_events.c
  src/lora_e5_uart.c
)

endif()
