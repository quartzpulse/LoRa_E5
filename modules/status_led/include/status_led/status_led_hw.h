/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Hardware abstraction layer for the status LED pattern engine.
 *
 * Exactly one backend (WS2812 / GPIO / PWM) is compiled in, selected by the
 * STATUS_LED_BACKEND Kconfig choice. The pattern engine and everything above
 * it only ever calls these two functions and never touches GPIO/SPI/PWM APIs
 * directly, keeping the library portable across boards and backends.
 */

#ifndef STATUS_LED_HW_H_
#define STATUS_LED_HW_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the selected hardware backend.
 *
 * @return 0 on success, negative errno on failure (for example, the backing
 *         device is not ready).
 */
int status_led_hw_init(void);

/**
 * @brief Render one RGB frame to the physical LED(s).
 *
 * Implementations must be safe to call from workqueue context and must not
 * block for more than a few hundred microseconds (no network/file I/O).
 *
 * @param r Red channel, 0-255, prior to brightness scaling.
 * @param g Green channel, 0-255, prior to brightness scaling.
 * @param b Blue channel, 0-255, prior to brightness scaling.
 * @param brightness Global brightness scale, 0-255.
 *
 * @return 0 on success, negative errno on failure.
 */
int status_led_hw_write(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_HW_H_ */
