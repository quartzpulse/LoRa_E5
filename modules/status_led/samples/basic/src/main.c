/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Minimal demonstration of the portable RGB status LED library.
 *
 * Walks through a representative boot -> provisioning -> network -> MQTT
 * lifecycle, then simulates an OTA update and, finally, a fatal error that
 * overrides everything else. No networking/MQTT/OTA subsystem is actually
 * used here -- k_sleep() stands in for the real asynchronous events an
 * application would otherwise wait on.
 */

#include <status_led/status_led.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_led_sample, LOG_LEVEL_INF);

#define DEMO_STEP_DELAY K_SECONDS(3)

int main(void)
{
	int ret = status_led_init();

	if (ret < 0) {
		LOG_ERR("status_led_init failed: %d", ret);
		return ret;
	}

	LOG_INF("booting");
	status_led_set_state(STATUS_LED_BOOTING);
	k_sleep(DEMO_STEP_DELAY);
	status_led_clear_state(STATUS_LED_BOOTING);

	LOG_INF("provisioning");
	status_led_set_state(STATUS_LED_PROVISIONING);
	k_sleep(DEMO_STEP_DELAY);
	status_led_clear_state(STATUS_LED_PROVISIONING);

	LOG_INF("connecting to network");
	status_led_set_state(STATUS_LED_CONNECTING_NETWORK);
	k_sleep(DEMO_STEP_DELAY);
	status_led_clear_state(STATUS_LED_CONNECTING_NETWORK);
	status_led_set_state(STATUS_LED_NETWORK_CONNECTED);

	LOG_INF("connecting to MQTT");
	status_led_set_state(STATUS_LED_CONNECTING_MQTT);
	k_sleep(DEMO_STEP_DELAY);
	status_led_clear_state(STATUS_LED_CONNECTING_MQTT);
	status_led_set_state(STATUS_LED_MQTT_CONNECTED);
	k_sleep(DEMO_STEP_DELAY);

	LOG_INF("OTA preparing then updating");
	status_led_set_state(STATUS_LED_OTA_PREPARING);
	k_sleep(DEMO_STEP_DELAY);
	status_led_clear_state(STATUS_LED_OTA_PREPARING);
	status_led_set_state(STATUS_LED_OTA_UPDATING);
	k_sleep(DEMO_STEP_DELAY);
	status_led_clear_state(STATUS_LED_OTA_UPDATING);

	LOG_INF("still MQTT-connected: %d", status_led_get_state());
	k_sleep(DEMO_STEP_DELAY);

	LOG_INF("simulating a fatal error -- overrides everything else");
	status_led_set_state(STATUS_LED_FATAL_ERROR);

	return 0;
}
