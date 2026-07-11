/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Discrete 3x PWM channel (R/G/B) hardware backend.
 *
 * Resolves each channel from the "pwm-red-led" / "pwm-green-led" /
 * "pwm-blue-led" devicetree aliases -- no hardcoded pin/timer numbers here.
 * Brightness is applied as true duty-cycle dimming.
 */

#include <status_led/status_led_hw.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(status_led, CONFIG_STATUS_LED_LOG_LEVEL);

static const struct pwm_dt_spec red_led = PWM_DT_SPEC_GET(DT_ALIAS(pwm_red_led));
static const struct pwm_dt_spec green_led = PWM_DT_SPEC_GET(DT_ALIAS(pwm_green_led));
static const struct pwm_dt_spec blue_led = PWM_DT_SPEC_GET(DT_ALIAS(pwm_blue_led));

static inline uint8_t scale_channel(uint8_t value, uint8_t brightness)
{
	return (uint8_t)(((uint16_t)value * brightness) / UINT8_MAX);
}

static int write_channel(const struct pwm_dt_spec *led, uint8_t duty)
{
	uint32_t pulse_ns = ((uint64_t)led->period * duty) / UINT8_MAX;

	return pwm_set_pulse_dt(led, pulse_ns);
}

int status_led_hw_init(void)
{
	if (!pwm_is_ready_dt(&red_led) || !pwm_is_ready_dt(&green_led) ||
	    !pwm_is_ready_dt(&blue_led)) {
		LOG_ERR("PWM RGB channel(s) not ready");
		return -ENODEV;
	}

	return 0;
}

int status_led_hw_write(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
	int ret;

	ret = write_channel(&red_led, scale_channel(r, brightness));
	if (ret < 0) {
		return ret;
	}

	ret = write_channel(&green_led, scale_channel(g, brightness));
	if (ret < 0) {
		return ret;
	}

	return write_channel(&blue_led, scale_channel(b, brightness));
}
