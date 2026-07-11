/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Priority-table-to-pattern mapping (task.md's LED state table).
 *
 * Data only -- no logic. Extending or re-coloring a state means editing a
 * single line here; the pattern engine (status_led_pattern_engine.c) never
 * needs to change.
 *
 * Shapes and timings are deliberately simple and borrowed directly from this
 * project's original monochrome indicator (single/double/triple pulse
 * rhythms), rather than more elaborate per-state periods or breathing fades.
 * Color is what carries most of the meaning here; shape only distinguishes
 * severity tiers (steady vs. connecting vs. urgent).
 */

#include "status_led_patterns.h"

/* Named 8-bit RGB colors, full intensity (scaled by CONFIG_STATUS_LED_DEFAULT_BRIGHTNESS
 * at render time -- see status_led_pattern_engine.c).
 */
#define COLOR_OFF     ((struct rgb_color){ 0x00, 0x00, 0x00 })
#define COLOR_RED     ((struct rgb_color){ 0xFF, 0x00, 0x00 })
#define COLOR_GREEN   ((struct rgb_color){ 0x00, 0xFF, 0x00 })
#define COLOR_BLUE    ((struct rgb_color){ 0x00, 0x00, 0xFF })
#define COLOR_CYAN    ((struct rgb_color){ 0x00, 0xFF, 0xFF })
#define COLOR_MAGENTA ((struct rgb_color){ 0xFF, 0x00, 0xFF })
#define COLOR_YELLOW  ((struct rgb_color){ 0xFF, 0xFF, 0x00 })
#define COLOR_WHITE   ((struct rgb_color){ 0xFF, 0xFF, 0xFF })

/* Period is unused by static OFF/SOLID patterns; kept nonzero for clarity. */
#define PERIOD_STATIC_MS 1000U

/* Steady-state single pulse ("sleepy blip"/"heartbeat") rhythms, reused
 * as-is from the original monochrome indicator's pat_all_ok / pat_gps_no_fix.
 */
#define MQTT_CONNECTED_PULSE_MS     50U
#define MQTT_CONNECTED_PERIOD_MS  5050U /* 50 on, 5000 off */
#define NETWORK_CONNECTED_PULSE_MS  100U
#define NETWORK_CONNECTED_PERIOD_MS 3000U /* 100 on, 2900 off */

/* Double-pulse rhythm, reused as-is from pat_mqtt_down. */
#define DOUBLE_PULSE_MS   150U
#define DOUBLE_PERIOD_MS 1950U /* 150,150,150 on/off/on, 1500 off */

/* Fast blink, reused as-is from pat_lte_down. */
#define FAST_BLINK_PERIOD_MS 500U /* 250 on, 250 off */

/* Booting: slow blink, no equivalent in the original indicator. */
#define BOOTING_PERIOD_MS 2000U /* 1000 on, 1000 off */

/* Triple-pulse rhythm, reused as-is from pat_ota_active (OTA in progress). */
#define OTA_UPDATING_PULSE_MS   150U
#define OTA_UPDATING_PERIOD_MS 1950U /* 150 x3 on/off, 1200 off */

/* Provisioning: same triple-pulse shape, slower rhythm and longer gap,
 * approximating the original quad-pulse pat_provisioning closely enough.
 */
#define PROVISIONING_PULSE_MS   100U
#define PROVISIONING_PERIOD_MS 2500U /* 100 x3 on/off, 2000 off */

/* Fatal error: fastest blink of all, faster than a down network interface. */
#define FATAL_ERROR_PERIOD_MS 200U /* 100 on, 100 off */

const struct led_pattern status_led_pattern_table[STATUS_LED_STATE_COUNT] = {
	[STATUS_LED_SLEEP] = {
		.color = COLOR_OFF,
		.type = LED_PATTERN_OFF,
		.period_ms = PERIOD_STATIC_MS,
	},
	[STATUS_LED_NETWORK_CONNECTED] = {
		.color = COLOR_GREEN,
		.type = LED_PATTERN_PULSE,
		.pulse_ms = NETWORK_CONNECTED_PULSE_MS,
		.period_ms = NETWORK_CONNECTED_PERIOD_MS,
	},
	[STATUS_LED_MQTT_CONNECTED] = {
		.color = COLOR_CYAN,
		.type = LED_PATTERN_PULSE,
		.pulse_ms = MQTT_CONNECTED_PULSE_MS,
		.period_ms = MQTT_CONNECTED_PERIOD_MS,
	},
	[STATUS_LED_CONNECTING_MQTT] = {
		.color = COLOR_CYAN,
		.type = LED_PATTERN_DOUBLE_BLINK,
		.pulse_ms = DOUBLE_PULSE_MS,
		.period_ms = DOUBLE_PERIOD_MS,
	},
	[STATUS_LED_CONNECTING_NETWORK] = {
		.color = COLOR_GREEN,
		.type = LED_PATTERN_BLINK,
		.period_ms = FAST_BLINK_PERIOD_MS,
	},
	[STATUS_LED_BOOTING] = {
		.color = COLOR_WHITE,
		.type = LED_PATTERN_BLINK,
		.period_ms = BOOTING_PERIOD_MS,
	},
	[STATUS_LED_PROVISIONING] = {
		.color = COLOR_BLUE,
		.type = LED_PATTERN_TRIPLE_BLINK,
		.pulse_ms = PROVISIONING_PULSE_MS,
		.period_ms = PROVISIONING_PERIOD_MS,
	},
	[STATUS_LED_OTA_PREPARING] = {
		.color = COLOR_YELLOW,
		.type = LED_PATTERN_DOUBLE_BLINK,
		.pulse_ms = DOUBLE_PULSE_MS,
		.period_ms = DOUBLE_PERIOD_MS,
	},
	[STATUS_LED_OTA_UPDATING] = {
		.color = COLOR_MAGENTA,
		.type = LED_PATTERN_TRIPLE_BLINK,
		.pulse_ms = OTA_UPDATING_PULSE_MS,
		.period_ms = OTA_UPDATING_PERIOD_MS,
	},
	[STATUS_LED_HW_FAILURE] = {
		.color = COLOR_RED,
		.type = LED_PATTERN_SOLID,
		.period_ms = PERIOD_STATIC_MS,
	},
	[STATUS_LED_FATAL_ERROR] = {
		.color = COLOR_RED,
		.type = LED_PATTERN_BLINK,
		.period_ms = FATAL_ERROR_PERIOD_MS,
	},
};
