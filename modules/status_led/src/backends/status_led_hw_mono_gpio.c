/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Monochrome single-GPIO hardware backend.
 *
 * Resolves the LED from the "mono-led" devicetree alias -- no hardcoded pin
 * numbers here. Plain GPIO has no dimming capability, so the pattern's color
 * is first collapsed to a single intensity via max(r, g, b), then
 * thresholded to fully on or fully off directly from that raw
 * (pre-brightness-scaling) value -- the brightness parameter is
 * intentionally ignored, since scaling it in would push every "on" value
 * below the threshold at low brightness settings and the LED would never
 * light. States that differ only by color (not by blink pattern) render
 * identically on monochrome hardware.
 */

#include <status_led/status_led_hw.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(status_led, CONFIG_STATUS_LED_LOG_LEVEL);

/** A scaled intensity at or above this threshold is treated as "on". */
#define STATUS_LED_GPIO_ON_THRESHOLD 128U

static const struct gpio_dt_spec mono_led = GPIO_DT_SPEC_GET(DT_ALIAS(mono_led), gpios);

static inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t max_rg = (r > g) ? r : g;

	return (max_rg > b) ? max_rg : b;
}

int status_led_hw_init(void)
{
	if (!gpio_is_ready_dt(&mono_led)) {
		LOG_ERR("Mono LED GPIO not ready (pin %d)", mono_led.pin);
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&mono_led, GPIO_OUTPUT_INACTIVE);
}

int status_led_hw_write(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
	ARG_UNUSED(brightness);

	return gpio_pin_set_dt(&mono_led, luma(r, g, b) >= STATUS_LED_GPIO_ON_THRESHOLD);
}
