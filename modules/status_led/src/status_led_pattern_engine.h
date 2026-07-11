/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internal pattern engine: interprets led_pattern descriptors and
 * drives the hardware backend via a k_work_delayable.
 */

#ifndef STATUS_LED_PATTERN_ENGINE_H_
#define STATUS_LED_PATTERN_ENGINE_H_

#include <status_led/status_led.h>

/** One-time setup of the pattern engine's work item. Call before first restart. */
void status_led_pattern_init(void);

/**
 * @brief Switch the pattern engine to the descriptor for @p state and begin
 * rendering it immediately (on the system workqueue, not the caller's stack).
 *
 * @param state The newly-resolved display state.
 */
void status_led_pattern_restart(enum status_led_state state);
void status_led_pattern_stop(void);

#endif /* STATUS_LED_PATTERN_ENGINE_H_ */
