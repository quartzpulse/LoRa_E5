/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Public API for the portable RGB status LED library.
 *
 * Applications must only call the four functions declared here. GPIO, PWM,
 * and SPI details are fully encapsulated below this API and are never the
 * caller's concern.
 *
 * All three mutating calls (@ref status_led_init, @ref status_led_set_state,
 * @ref status_led_clear_state) are thread-safe and may be called from
 * application threads, network/MQTT callbacks, the OTA subsystem, or any
 * other non-ISR context. They never block on LED I/O: the actual hardware
 * write is deferred to an internal work item.
 *
 * Multiple states may be active simultaneously (for example,
 * @ref STATUS_LED_NETWORK_CONNECTED and @ref STATUS_LED_CONNECTING_MQTT at
 * the same time). The library always renders the single highest-priority
 * active state; priority is fixed and matches the declaration order of
 * @ref status_led_state below, lowest to highest.
 */

#ifndef STATUS_LED_H_
#define STATUS_LED_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status LED states, in ascending priority order.
 *
 * The numeric value of each state is its priority rank: higher value always
 * overrides lower value while both are active. Do not reorder existing
 * entries; append new states directly below @ref STATUS_LED_SLEEP or above
 * @ref STATUS_LED_FATAL_ERROR only if their priority is genuinely lowest or
 * highest, respectively. Inserting elsewhere requires renumbering everything
 * above the insertion point.
 */
enum status_led_state {
	STATUS_LED_SLEEP = 0,           /**< LED off; implicit floor state, lowest priority. */
	STATUS_LED_NETWORK_CONNECTED,   /**< Network up, MQTT not yet connected. */
	STATUS_LED_MQTT_CONNECTED,      /**< MQTT connected; normal operation. */
	STATUS_LED_CONNECTING_MQTT,     /**< Connecting to MQTT broker. */
	STATUS_LED_CONNECTING_NETWORK,  /**< Connecting to the network interface (Wi-Fi or cellular). */
	STATUS_LED_BOOTING,             /**< Booting. */
	STATUS_LED_PROVISIONING,        /**< Configuration / AP provisioning mode. */
	STATUS_LED_OTA_PREPARING,       /**< Firmware upgrade preparing. */
	STATUS_LED_OTA_UPDATING,        /**< OTA update in progress. */
	STATUS_LED_HW_FAILURE,          /**< Hardware initialization failed. */
	STATUS_LED_FATAL_ERROR,         /**< Fatal error / watchdog reset; highest priority. */

	STATUS_LED_STATE_COUNT,         /**< Not a real state; array sizing only. */
};

/**
 * @brief Initialize the status LED library and its hardware backend.
 *
 * Must be called once before any other status_led_*() function. Safe to
 * call from the application's main thread during boot.
 *
 * @return 0 on success, negative errno on failure (backend not ready).
 */
int status_led_init(void);

/**
 * @brief Mark a state as active.
 *
 * If @p state is already active, this is a no-op. If @p state becomes the
 * new highest-priority active state, the displayed pattern switches
 * immediately (asynchronously, on the pattern engine's work item).
 *
 * @param state State to activate.
 *
 * @return 0 on success, -EINVAL if @p state is not a valid state.
 */
int status_led_set_state(enum status_led_state state);

/**
 * @brief Mark a state as no longer active.
 *
 * If @p state is not active, this is a no-op. If @p state was the displayed
 * state, the library automatically falls back to the next-highest active
 * state (or @ref STATUS_LED_SLEEP if none remain).
 *
 * @param state State to deactivate.
 *
 * @return 0 on success, -EINVAL if @p state is not a valid state.
 */
int status_led_clear_state(enum status_led_state state);

/**
 * @brief Get the state currently being displayed.
 *
 * @return The highest-priority active state, or @ref STATUS_LED_SLEEP if no
 *         state is active.
 */
enum status_led_state status_led_get_state(void);

/**
 * @brief Stop the status LED library and shut down the LED synchronously.
 *
 * Disables all future state mutations, cancels the pattern engine work queue,
 * and writes the OFF color synchronously to the hardware.
 */
void status_led_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_H_ */
