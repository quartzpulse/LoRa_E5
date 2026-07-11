/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Monochrome single-PWM-channel hardware backend.
 *
 * Resolves the LED from the "pwm-mono-led" devicetree alias -- no hardcoded
 * pin/timer numbers here. The pattern's color is collapsed to a single
 * intensity via max(r, g, b) and rendered as true duty-cycle brightness.
 * States that differ only by color (not by blink pattern) render
 * identically on monochrome hardware.
 */

#include <status_led/status_led_hw.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(status_led, CONFIG_STATUS_LED_LOG_LEVEL);

static const struct pwm_dt_spec mono_led = PWM_DT_SPEC_GET(DT_ALIAS(pwm_mono_led));

static inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t max_rg = (r > g) ? r : g;

	return (max_rg > b) ? max_rg : b;
}

static inline uint8_t scale_channel(uint8_t value, uint8_t brightness)
{
	return (uint8_t)(((uint16_t)value * brightness) / UINT8_MAX);
}

int status_led_hw_init(void)
{
	if (!pwm_is_ready_dt(&mono_led)) {
		LOG_ERR("Mono LED PWM channel not ready");
		return -ENODEV;
	}

	return 0;
}

int status_led_hw_write(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
	uint8_t duty = scale_channel(luma(r, g, b), brightness);
	uint32_t pulse_ns = ((uint64_t)mono_led.period * duty) / UINT8_MAX;

	return pwm_set_pulse_dt(&mono_led, pulse_ns);
}
