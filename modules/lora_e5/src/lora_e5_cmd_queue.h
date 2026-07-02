/**
 * @file lora_e5_cmd_queue.h
 * @brief Internal engine backing lora_e5_at.c's public surface. Split
 * out per the original directory layout (lora_e5_at.c stays a thin
 * public-API wrapper; this file owns the actual queue/dispatch/
 * timeout/matching state machine).
 *
 * INTERNAL ONLY -- not installed, not included by anything outside
 * this module's own .c files.
 */
#ifndef LORA_E5_CMD_QUEUE_H_
#define LORA_E5_CMD_QUEUE_H_

#include <zephyr/kernel.h>

#include "lora_e5/lora_e5_at.h"

#ifdef __cplusplus
extern "C" {
#endif

int lora_e5_cmd_queue_init(struct k_work_q *rx_work_q);

void lora_e5_cmd_queue_set_transport(lora_e5_at_send_fn_t send_fn);

int lora_e5_cmd_queue_set_urc_callback(lora_e5_at_urc_cb_t cb);

int lora_e5_cmd_queue_submit(const struct lora_e5_at_cmd_desc *desc,
			      lora_e5_at_result_cb_t result_cb);

void lora_e5_cmd_queue_process_line(const struct lora_e5_at_line *line);

/**
 * @brief Test-only: reset all state (active command, pending queue,
 * transport, URC callback) back to post-init. Does NOT re-run
 * lora_e5_cmd_queue_init()'s rx_work_q assignment -- call this
 * between test cases on an already-initialized instance.
 */
void lora_e5_cmd_queue_test_reset(void);

/**
 * @brief Test-only accessor: is a command currently active? Used by
 * the mock-transport test harness to assert dispatch/queueing
 * behavior without depending on timing.
 */
bool lora_e5_cmd_queue_test_is_active(void);

/** @brief Test-only accessor: how many commands are currently queued
 *  behind the active one. */
size_t lora_e5_cmd_queue_test_pending_count(void);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_CMD_QUEUE_H_ */
