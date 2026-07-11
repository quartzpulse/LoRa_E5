/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Discrete 3x GPIO (R/G/B) hardware backend.
 *
 * Resolves each channel from the "red-led" / "green-led" / "blue-led"
 * devicetree aliases -- no hardcoded pin numbers here. Plain GPIO has no
 * dimming capability, so each channel is thresholded to fully on or fully
 * off directly from its raw (pre-brightness-scaling) color value --
 * the brightness parameter is intentionally ignored, since scaling it in
 * would push every "on" value below the threshold at low brightness
 * settings and the LED would never light.
 */

#include <status_led/status_led_hw.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(status_led, CONFIG_STATUS_LED_LOG_LEVEL);

/** A scaled channel value at or above this threshold is treated as "on". */
#define STATUS_LED_GPIO_ON_THRESHOLD 128U

static const struct gpio_dt_spec red_led = GPIO_DT_SPEC_GET(DT_ALIAS(red_led), gpios);
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(green_led), gpios);
static const struct gpio_dt_spec blue_led = GPIO_DT_SPEC_GET(DT_ALIAS(blue_led), gpios);

static int configure_channel(const struct gpio_dt_spec *led)
{
	if (!gpio_is_ready_dt(led)) {
		LOG_ERR("GPIO channel not ready (pin %d)", led->pin);
		return -ENODEV;
	}

	return gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
}

int status_led_hw_init(void)
{
	int ret;

	ret = configure_channel(&red_led);
	if (ret < 0) {
		return ret;
	}

	ret = configure_channel(&green_led);
	if (ret < 0) {
		return ret;
	}

	return configure_channel(&blue_led);
}

int status_led_hw_write(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
	int ret;

	ARG_UNUSED(brightness);

	ret = gpio_pin_set_dt(&red_led, r >= STATUS_LED_GPIO_ON_THRESHOLD);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&green_led, g >= STATUS_LED_GPIO_ON_THRESHOLD);
	if (ret < 0) {
		return ret;
	}

	return gpio_pin_set_dt(&blue_led, b >= STATUS_LED_GPIO_ON_THRESHOLD);
}
