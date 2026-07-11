/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_led_priority.h"

#include <zephyr/sys/util.h>

enum status_led_state status_led_priority_resolve(uint32_t mask)
{
	for (int state = STATUS_LED_STATE_COUNT - 1; state >= 0; state--) {
		if (mask & BIT(state)) {
			return (enum status_led_state)state;
		}
	}

	return STATUS_LED_SLEEP;
}
