/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internal priority engine: resolves the active-state bitmap to a
 * single displayable state.
 */

#ifndef STATUS_LED_PRIORITY_H_
#define STATUS_LED_PRIORITY_H_

#include <status_led/status_led.h>
#include <stdint.h>

/**
 * @brief Resolve the highest-priority active state from a bitmap.
 *
 * Priority is positional: bit N corresponds to the state whose enum value
 * is N, and higher N always wins over lower N while both bits are set. This
 * keeps the priority table and the enum permanently in sync -- there is no
 * separate rank table to fall out of date.
 *
 * @param mask Bitmap of active states, one bit per status_led_state value.
 *
 * @return The highest-priority state whose bit is set in @p mask, or
 *         STATUS_LED_SLEEP if @p mask is zero.
 */
enum status_led_state status_led_priority_resolve(uint32_t mask);

#endif /* STATUS_LED_PRIORITY_H_ */
