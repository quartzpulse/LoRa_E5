/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Data-driven LED pattern descriptors for the status LED pattern engine.
 */

#ifndef STATUS_LED_PATTERN_H_
#define STATUS_LED_PATTERN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 8-bit RGB color, prior to global brightness scaling. */
struct rgb_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/** @brief Animation types supported by the generic pattern interpreter. */
enum led_pattern_type {
	LED_PATTERN_OFF = 0,     /**< LED held off. */
	LED_PATTERN_SOLID,       /**< LED held on at a fixed color. */
	LED_PATTERN_BLINK,       /**< Single on/off toggle, 50% duty cycle. */
	LED_PATTERN_DOUBLE_BLINK,/**< Two short pulses, then a long gap. */
	LED_PATTERN_TRIPLE_BLINK,/**< Three short pulses, then a long gap. */
	LED_PATTERN_BREATHING,   /**< Smooth brightness fade up and down. */
	LED_PATTERN_PULSE,       /**< One brief bright pulse, then dim/off. */
	LED_PATTERN_FLASH,       /**< One very short, sharp flash. */
};

/**
 * @brief Declarative descriptor for a single LED animation.
 *
 * The pattern engine interprets @ref led_pattern_type descriptors uniformly;
 * adding a new state never requires new interpreter code, only a new table
 * entry (see status_led_patterns.c).
 */
struct led_pattern {
	struct rgb_color color;      /**< Base color, before brightness scaling. */
	enum led_pattern_type type;  /**< Animation shape. */
	uint16_t pulse_ms;           /**< On-time per pulse; DOUBLE/TRIPLE_BLINK, PULSE, FLASH only. */
	uint16_t period_ms;          /**< Full animation cycle time, in milliseconds. */
};

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_PATTERN_H_ */
