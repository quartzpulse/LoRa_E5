/**
 * @file lora_e5_config.h
 * @brief Hardware/init-time configuration, supplied to lora_e5_init().
 *
 * Deliberately separate from struct lora_e5_config's LoRaWAN
 * activation fields... wait, naming collision avoided below: this
 * file defines struct lora_e5_hw_config for UART/reset-GPIO wiring.
 * The LoRaWAN activation/region/class configuration
 * (struct lora_e5_config) lives in lora_e5_types.h and is supplied
 * separately via lora_e5_set_otaa()/lora_e5_set_abp()/
 * lora_e5_set_region(), consumed during lora_e5_start()'s CONFIG
 * state -- these are two different configuration concerns (board
 * wiring vs LoRaWAN network provisioning) and are kept as two
 * separate structs/headers rather than one large blob, to keep
 * board-level integration code from needing to touch keys/EUIs and
 * vice versa.
 */
#ifndef LORA_E5_CONFIG_H_
#define LORA_E5_CONFIG_H_

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Board/hardware wiring, supplied once at lora_e5_init().
 *
 * Typically populated from devicetree via a DT_INST-based macro in
 * the application/board overlay, not hand-constructed -- see
 * README.md sample usage once Phase 3 implementation exists.
 */
struct lora_e5_hw_config {
	const struct device *uart_dev;

	/**
	 * Optional hardware reset line. NULL if
	 * CONFIG_LORA_E5_HAS_RESET_GPIO is disabled or no `reset-gpios`
	 * devicetree property is present on this board -- in that case
	 * the Modem Manager falls back to LORA_E5_RESET_BACKEND_AT_COMMAND
	 * automatically (Phase 1 decision #4, never hardcoded).
	 */
	const struct gpio_dt_spec *reset_gpio;
};

/*
 * Kconfig symbols referenced by the implementation (declared here in
 * comment form for design-review visibility; actual values come from
 * the build's autoconf.h, not from this header):
 *
 *   CONFIG_LORA_E5                       - master enable
 *   CONFIG_LORA_E5_HAS_RESET_GPIO        - gates GPIO reset backend
 *   CONFIG_LORA_E5_RX_STACK_SIZE         - RX work queue stack
 *   CONFIG_LORA_E5_FSM_STACK_SIZE        - FSM work queue stack
 *   CONFIG_LORA_E5_NOTIFY_STACK_SIZE     - application callback queue stack
 *   CONFIG_LORA_E5_RX_BUFFER_SIZE        - UART ring buffer size
 *   CONFIG_LORA_E5_CMD_TIMEOUT_MS        - generic AT transaction timeout
 *   CONFIG_LORA_E5_TX_TIMEOUT_MS         - MSG/CMSG transaction timeout
 *                                          (separate from CMD_TIMEOUT_MS --
 *                                          see Phase 1 §9 rationale)
 *   CONFIG_LORA_E5_JOIN_TIMEOUT_MS       - single join attempt bound
 *   CONFIG_LORA_E5_MAX_RETRIES           - generic transport retry bound
 *   CONFIG_LORA_E5_JOIN_MAX_RETRIES      - join-specific retry bound
 *   CONFIG_LORA_E5_AUTO_REJOIN           - auto rejoin after recovery
 *   CONFIG_LORA_E5_AUTO_RESET            - watchdog-style self-reset from ERROR
 *   CONFIG_LORA_E5_AUTO_RESET_TIMEOUT_MS
 *   CONFIG_LORA_E5_EVENT_QUEUE_DEPTH
 *   CONFIG_LORA_E5_TX_BUFFER_SIZE        - static TX staging buffer,
 *                                          sized against worst-case
 *                                          242-byte payload (Table 3-3)
 *                                          unless the application
 *                                          commits to a smaller region set
 *   CONFIG_LORA_E5_DEFAULT_REGION
 *   CONFIG_LORA_E5_DEBUG
 *   CONFIG_LORA_E5_SHELL
 *
 * Class B/C Kconfig gates are intentionally NOT listed -- Phase 1
 * decision #1: no dormant/gated states for unimplemented classes in
 * v1. Add CONFIG_LORA_E5_CLASS_B/_C only alongside actual FSM support.
 */

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_CONFIG_H_ */
