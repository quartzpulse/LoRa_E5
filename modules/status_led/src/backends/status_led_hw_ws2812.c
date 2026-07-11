/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief WS2812 (single addressable RGB LED) hardware backend.
 *
 * Resolves the device purely from the "led-strip" devicetree alias -- no
 * hardcoded GPIO/SPI pin numbers here. A new board only needs its own
 * overlay defining that alias (see boards/esp32s3_devkitc_procpu.overlay for
 * the reference ws2812-over-SPI wiring).
 *
 * WS2812 has no separate dimming input: brightness is applied by scaling the
 * RGB values in software before they are sent to the strip.
 */

#include <status_led/status_led_hw.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(status_led, CONFIG_STATUS_LED_LOG_LEVEL);

static const struct device *const led_strip_dev = DEVICE_DT_GET(DT_ALIAS(led_strip));

static inline uint8_t scale_channel(uint8_t value, uint8_t brightness)
{
	return (uint8_t)(((uint16_t)value * brightness) / UINT8_MAX);
}

int status_led_hw_init(void)
{
	if (!device_is_ready(led_strip_dev)) {
		LOG_ERR("WS2812 led-strip device not ready");
		return -ENODEV;
	}

	return 0;
}

int status_led_hw_write(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
	struct led_rgb pixel = {
		.r = scale_channel(r, brightness),
		.g = scale_channel(g, brightness),
		.b = scale_channel(b, brightness),
	};

	return led_strip_update_rgb(led_strip_dev, &pixel, 1U);
}
