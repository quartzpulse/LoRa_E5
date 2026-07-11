/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <status_led/status_led.h>
#include <status_led/status_led_hw.h>
#include "status_led_priority.h"
#include "status_led_pattern_engine.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(status_led, CONFIG_STATUS_LED_LOG_LEVEL);

/** Bit N of active_mask set <=> state N (see enum status_led_state) is active. */
static atomic_t active_mask = ATOMIC_INIT(BIT(STATUS_LED_SLEEP));

/** Currently displayed state, protected by state_lock. */
static enum status_led_state displayed_state = STATUS_LED_SLEEP;
static bool stopped;

K_MUTEX_DEFINE(state_lock);

static bool is_valid_state(enum status_led_state state)
{
	return state < STATUS_LED_STATE_COUNT;
}

/**
 * @brief Recompute the resolved state and, if it changed, restart the
 * pattern engine on the new pattern. Must be called with state_lock held.
 */
static void refresh_display_locked(void)
{
	enum status_led_state resolved = status_led_priority_resolve((uint32_t)atomic_get(&active_mask));

	if (resolved != displayed_state) {
		LOG_DBG("state change: %d -> %d", displayed_state, resolved);
		displayed_state = resolved;
		status_led_pattern_restart(resolved);
	}
}

int status_led_init(void)
{
	int ret = status_led_hw_init();

	if (ret < 0) {
		LOG_ERR("hardware backend init failed: %d", ret);
		return ret;
	}

	status_led_pattern_init();

	k_mutex_lock(&state_lock, K_FOREVER);
	refresh_display_locked();
	k_mutex_unlock(&state_lock);

	LOG_INF("status LED library initialized");
	return 0;
}

int status_led_set_state(enum status_led_state state)
{
	if (!is_valid_state(state)) {
		return -EINVAL;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	if (stopped) {
		k_mutex_unlock(&state_lock);
		return 0;
	}
	atomic_set_bit(&active_mask, state);
	refresh_display_locked();
	k_mutex_unlock(&state_lock);

	return 0;
}

int status_led_clear_state(enum status_led_state state)
{
	if (!is_valid_state(state)) {
		return -EINVAL;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	if (stopped) {
		k_mutex_unlock(&state_lock);
		return 0;
	}
	atomic_clear_bit(&active_mask, state);
	refresh_display_locked();
	k_mutex_unlock(&state_lock);

	return 0;
}

enum status_led_state status_led_get_state(void)
{
	enum status_led_state state;

	k_mutex_lock(&state_lock, K_FOREVER);
	state = displayed_state;
	k_mutex_unlock(&state_lock);

	return state;
}

void status_led_stop(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	stopped = true;
	atomic_set(&active_mask, 0);
	displayed_state = STATUS_LED_SLEEP;
	k_mutex_unlock(&state_lock);

	/* Stop pattern engine, cancel delayable work, and synchronously force LED off */
	status_led_pattern_stop();
}
