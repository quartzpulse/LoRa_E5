/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Generic, data-driven LED pattern interpreter.
 *
 * One k_work_delayable renders every pattern type. Static patterns (OFF,
 * SOLID) write a single frame and never reschedule -- the LED is only
 * touched again on the next status_led_pattern_restart() call, keeping CPU
 * and GPIO/SPI traffic at zero while idle. Cyclic patterns (BLINK and its
 * variants, PULSE, FLASH) precompute a short step sequence once and replay
 * it, rescheduling for exactly the next step's duration. BREATHING is the
 * only pattern that ticks on a fixed cadence (CONFIG_STATUS_LED_UPDATE_PERIOD_MS)
 * while it is the displayed pattern.
 */

#include "status_led_pattern_engine.h"
#include "status_led_patterns.h"
#include <status_led/status_led_hw.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/** Longest step sequence needed: TRIPLE_BLINK (3 on/off pairs). */
#define STATUS_LED_MAX_STEPS 6U

struct pattern_ctx {
	struct led_pattern pattern;
	struct rgb_color step_color[STATUS_LED_MAX_STEPS];
	uint32_t step_delay_ms[STATUS_LED_MAX_STEPS];
	uint8_t step_count;
	uint8_t step_index;
	uint32_t breathing_elapsed_ms;
};

static struct pattern_ctx ctx;
static struct k_work_delayable pattern_work;

/**
 * @brief Expand a pattern descriptor into a concrete step sequence.
 *
 * Not called for LED_PATTERN_BREATHING, which is rendered by
 * compute_breathing_frame() instead of a discrete step table.
 */
static void generate_discrete_steps(const struct led_pattern *pattern)
{
	const struct rgb_color on = pattern->color;
	const struct rgb_color off = { 0U, 0U, 0U };
	uint32_t period = pattern->period_ms;

	switch (pattern->type) {
	case LED_PATTERN_SOLID:
		ctx.step_color[0] = on;
		ctx.step_delay_ms[0] = period;
		ctx.step_count = 1U;
		break;

	case LED_PATTERN_BLINK: {
		uint32_t half = period / 2U;

		ctx.step_color[0] = on;
		ctx.step_delay_ms[0] = half;
		ctx.step_color[1] = off;
		ctx.step_delay_ms[1] = period - half;
		ctx.step_count = 2U;
		break;
	}

	case LED_PATTERN_DOUBLE_BLINK: {
		uint32_t pulse = pattern->pulse_ms;

		ctx.step_color[0] = on;
		ctx.step_delay_ms[0] = pulse;
		ctx.step_color[1] = off;
		ctx.step_delay_ms[1] = pulse;
		ctx.step_color[2] = on;
		ctx.step_delay_ms[2] = pulse;
		ctx.step_color[3] = off;
		ctx.step_delay_ms[3] = period - (3U * pulse);
		ctx.step_count = 4U;
		break;
	}

	case LED_PATTERN_TRIPLE_BLINK: {
		uint32_t pulse = pattern->pulse_ms;

		ctx.step_color[0] = on;
		ctx.step_delay_ms[0] = pulse;
		ctx.step_color[1] = off;
		ctx.step_delay_ms[1] = pulse;
		ctx.step_color[2] = on;
		ctx.step_delay_ms[2] = pulse;
		ctx.step_color[3] = off;
		ctx.step_delay_ms[3] = pulse;
		ctx.step_color[4] = on;
		ctx.step_delay_ms[4] = pulse;
		ctx.step_color[5] = off;
		ctx.step_delay_ms[5] = period - (5U * pulse);
		ctx.step_count = 6U;
		break;
	}

	case LED_PATTERN_PULSE: {
		uint32_t pulse = pattern->pulse_ms;

		ctx.step_color[0] = on;
		ctx.step_delay_ms[0] = pulse;
		ctx.step_color[1] = off;
		ctx.step_delay_ms[1] = period - pulse;
		ctx.step_count = 2U;
		break;
	}

	case LED_PATTERN_FLASH: {
		uint32_t pulse = pattern->pulse_ms;

		ctx.step_color[0] = on;
		ctx.step_delay_ms[0] = pulse;
		ctx.step_color[1] = off;
		ctx.step_delay_ms[1] = period - pulse;
		ctx.step_count = 2U;
		break;
	}

	case LED_PATTERN_OFF:
	case LED_PATTERN_BREATHING:
	default:
		ctx.step_color[0] = off;
		ctx.step_delay_ms[0] = period;
		ctx.step_count = 1U;
		break;
	}
}

/** Integer-only triangular fade: ramps 0 -> 255 -> 0 over one period_ms cycle. */
static struct rgb_color compute_breathing_frame(void)
{
	uint32_t period = ctx.pattern.period_ms;
	uint32_t half = period / 2U;
	uint32_t phase = ctx.breathing_elapsed_ms % period;
	uint32_t ramp = (phase <= half) ? phase : (period - phase);
	uint32_t frac = (ramp * 255U) / half;
	struct rgb_color frame = {
		.r = (uint8_t)(((uint32_t)ctx.pattern.color.r * frac) / 255U),
		.g = (uint8_t)(((uint32_t)ctx.pattern.color.g * frac) / 255U),
		.b = (uint8_t)(((uint32_t)ctx.pattern.color.b * frac) / 255U),
	};

	return frame;
}

static void pattern_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (ctx.pattern.type == LED_PATTERN_BREATHING) {
		struct rgb_color frame = compute_breathing_frame();

		status_led_hw_write(frame.r, frame.g, frame.b, CONFIG_STATUS_LED_DEFAULT_BRIGHTNESS);
		ctx.breathing_elapsed_ms += CONFIG_STATUS_LED_UPDATE_PERIOD_MS;
		k_work_reschedule(&pattern_work, K_MSEC(CONFIG_STATUS_LED_UPDATE_PERIOD_MS));
		return;
	}

	const struct rgb_color *color = &ctx.step_color[ctx.step_index];

	status_led_hw_write(color->r, color->g, color->b, CONFIG_STATUS_LED_DEFAULT_BRIGHTNESS);

	if (ctx.step_count <= 1U) {
		/* Static pattern (OFF/SOLID): idle until the next restart. */
		return;
	}

	uint32_t delay = ctx.step_delay_ms[ctx.step_index];

	ctx.step_index = (uint8_t)((ctx.step_index + 1U) % ctx.step_count);
	k_work_reschedule(&pattern_work, K_MSEC(delay));
}

void status_led_pattern_init(void)
{
	k_work_init_delayable(&pattern_work, pattern_work_handler);
}

void status_led_pattern_restart(enum status_led_state state)
{
	k_work_cancel_delayable(&pattern_work);

	ctx.pattern = status_led_pattern_table[state];
	ctx.step_index = 0U;
	ctx.breathing_elapsed_ms = 0U;

	if (ctx.pattern.type != LED_PATTERN_BREATHING) {
		generate_discrete_steps(&ctx.pattern);
	}

	k_work_reschedule(&pattern_work, K_NO_WAIT);
}

void status_led_pattern_stop(void)
{
	k_work_cancel_delayable(&pattern_work);
	status_led_hw_write(0, 0, 0, 0);
}
