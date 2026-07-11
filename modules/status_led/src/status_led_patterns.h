/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The static pattern table: one led_pattern descriptor per status_led_state.
 */

#ifndef STATUS_LED_PATTERNS_H_
#define STATUS_LED_PATTERNS_H_

#include <status_led/status_led.h>
#include <status_led/status_led_pattern.h>

/** Indexed by enum status_led_state; one descriptor per state, data only. */
extern const struct led_pattern status_led_pattern_table[STATUS_LED_STATE_COUNT];

#endif /* STATUS_LED_PATTERNS_H_ */
