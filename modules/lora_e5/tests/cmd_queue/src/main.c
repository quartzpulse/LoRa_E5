/**
 * @file tests/cmd_queue/src/main.c
 * @brief Unit tests for the AT Command Manager engine
 * (lora_e5_cmd_queue.c / lora_e5_at.c), using a mock transport
 * function in place of lora_e5_uart.c (which does not exist yet).
 *
 * This is the "mock UART" fixture referenced in the Phase 1 testing
 * strategy, in its minimal current form: a function pointer standing
 * in for the real UART backend's send path. It does not simulate ring
 * buffers, byte-level framing, or async TX completion -- those only
 * become testable once lora_e5_uart.c exists. What it DOES validate,
 * fully: queueing, single-in-flight enforcement, retry/timeout,
 * terminal-event matching (including the required_matches and
 * ANY_URC-vs-ERROR fixes made during Phase 3 review), and URC
 * forwarding.
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "lora_e5/lora_e5_at.h"
#include "lora_e5_cmd_queue.h"

/* ------------------------------------------------------------------- */
/* Test work queue (stands in for the real RX work queue)               */
/* ------------------------------------------------------------------- */

#define TEST_WQ_STACK_SIZE 2048
#define TEST_WQ_PRIORITY   5

static K_THREAD_STACK_DEFINE(test_wq_stack, TEST_WQ_STACK_SIZE);
static struct k_work_q test_wq;

/* ------------------------------------------------------------------- */
/* Mock transport                                                        */
/* ------------------------------------------------------------------- */

#define MOCK_CMD_LOG_MAX 8
#define MOCK_LAST_CMD_BUF_SIZE 64 /* comfortably covers every literal
				    * command string used in this file */

struct mock_transport_state {
	int send_count;
	char last_cmd[MOCK_LAST_CMD_BUF_SIZE];
	size_t last_cmd_len;
	int forced_rc; /* 0 = succeed, nonzero = simulate send failure */
};

static struct mock_transport_state mock;

static int mock_send(const char *cmd, size_t len)
{
	mock.send_count++;
	mock.last_cmd_len = len < sizeof(mock.last_cmd) - 1
		? len : sizeof(mock.last_cmd) - 1;
	memcpy(mock.last_cmd, cmd, mock.last_cmd_len);
	mock.last_cmd[mock.last_cmd_len] = '\0';
	return mock.forced_rc;
}

/* ------------------------------------------------------------------- */
/* Result capture                                                        */
/* ------------------------------------------------------------------- */

#define RESULT_LOG_MAX 8

struct result_log_entry {
	struct lora_e5_at_result result;
};

static struct result_log_entry result_log[RESULT_LOG_MAX];
static int result_log_count;

static void capture_result_cb(const struct lora_e5_at_result *result)
{
	if (result_log_count < RESULT_LOG_MAX) {
		result_log[result_log_count].result = *result;
		result_log_count++;
	}
}

/* ------------------------------------------------------------------- */
/* URC capture                                                            */
/* ------------------------------------------------------------------- */

#define URC_LOG_MAX 8

static struct lora_e5_at_line urc_log[URC_LOG_MAX];
static int urc_log_count;

static void capture_urc_cb(const struct lora_e5_at_line *line)
{
	if (urc_log_count < URC_LOG_MAX) {
		urc_log[urc_log_count] = *line;
		urc_log_count++;
	}
}

/* ------------------------------------------------------------------- */
/* Helpers                                                                */
/* ------------------------------------------------------------------- */

static void feed_line(const char *text)
{
	struct lora_e5_at_line line;

	zassert_equal(lora_e5_at_parse_line(text, strlen(text), &line), 0, NULL);
	lora_e5_at_process_line(&line);
}

static void *suite_setup(void)
{
	k_work_queue_init(&test_wq);
	k_work_queue_start(&test_wq, test_wq_stack,
			    K_THREAD_STACK_SIZEOF(test_wq_stack),
			    TEST_WQ_PRIORITY, NULL);

	zassert_equal(lora_e5_at_init(&test_wq), 0, NULL);
	return NULL;
}

static void case_before(void *f)
{
	ARG_UNUSED(f);
	lora_e5_cmd_queue_test_reset();
	memset(&mock, 0, sizeof(mock));
	memset(result_log, 0, sizeof(result_log));
	result_log_count = 0;
	memset(urc_log, 0, sizeof(urc_log));
	urc_log_count = 0;
	lora_e5_at_set_transport(mock_send);
	lora_e5_at_set_urc_callback(capture_urc_cb);
}

/* ------------------------------------------------------------------- */
/* Basic dispatch + match                                                */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_basic_dispatch_and_match)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "AT", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 42 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT",
		.cmd_len = 2,
		.timeout_ms = 1000,
		.terminal_events = te,
		.terminal_event_count = ARRAY_SIZE(te),
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);
	zassert_equal(mock.send_count, 1, "transport should have been called once");
	zassert_true(lora_e5_cmd_queue_test_is_active(), NULL);

	feed_line("+AT: OK");

	zassert_equal(result_log_count, 1, NULL);
	zassert_equal(result_log[0].result.outcome, LORA_E5_AT_OUTCOME_MATCHED, NULL);
	zassert_equal(result_log[0].result.result_tag, 42, NULL);
	zassert_false(lora_e5_cmd_queue_test_is_active(), NULL);
}

/* ------------------------------------------------------------------- */
/* Queueing / single in-flight                                           */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_second_command_queues_behind_first)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "FOO", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 1 },
	};
	struct lora_e5_at_cmd_desc desc1 = {
		.cmd = "AT+FOO", .cmd_len = 6, .timeout_ms = 1000,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};
	struct lora_e5_at_cmd_desc desc2 = desc1;

	desc2.cmd = "AT+BAR";

	zassert_equal(lora_e5_at_submit(&desc1, capture_result_cb), 0, NULL);
	zassert_equal(lora_e5_at_submit(&desc2, capture_result_cb), 0, NULL);

	/* Only the first should have been sent; second sits in the queue. */
	zassert_equal(mock.send_count, 1, NULL);
	zassert_equal(mock.last_cmd_len, 6, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+FOO", 6, NULL);
	zassert_equal(lora_e5_cmd_queue_test_pending_count(), 1, NULL);

	feed_line("+FOO: something");

	/* First resolved, second should now have been auto-dispatched. */
	zassert_equal(result_log_count, 1, NULL);
	zassert_equal(mock.send_count, 2, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+BAR", 6, NULL);
	zassert_equal(lora_e5_cmd_queue_test_pending_count(), 0, NULL);
}

/* ------------------------------------------------------------------- */
/* Timeout + retry                                                        */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_timeout_with_no_retries)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "X", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 1 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+X", .cmd_len = 4, .timeout_ms = 50,
		.max_retries = 0,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);
	zassert_equal(mock.send_count, 1, NULL);

	/* Never feed a matching line -- wait past the 50ms timeout. */
	k_sleep(K_MSEC(200));

	zassert_equal(result_log_count, 1,
		      "timeout should have resolved the command exactly once");
	zassert_equal(result_log[0].result.outcome, LORA_E5_AT_OUTCOME_TIMEOUT, NULL);
	zassert_equal(mock.send_count, 1, "max_retries=0 must not resend");
}

ZTEST(lora_e5_cmd_queue, test_retry_resends_before_final_timeout)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "X", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 1 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+X", .cmd_len = 4, .timeout_ms = 50,
		.max_retries = 2,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);

	k_sleep(K_MSEC(400)); /* comfortably past 3 * 50ms */

	zassert_equal(mock.send_count, 3,
		      "expected 1 initial send + 2 retries, got %d",
		      mock.send_count);
	zassert_equal(result_log_count, 1, NULL);
	zassert_equal(result_log[0].result.outcome, LORA_E5_AT_OUTCOME_TIMEOUT, NULL);
}

/* ------------------------------------------------------------------- */
/* required_matches -- regression test for the AT+ID query fix           */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_required_matches_three_occurrences)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "ID", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 7,
		  .required_matches = 3 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+ID", .cmd_len = 5, .timeout_ms = 1000,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);

	feed_line("+ID: DevAddr, 32:30:84:63");
	zassert_equal(result_log_count, 0,
		      "must NOT resolve after only 1 of 3 required matches");
	zassert_equal(urc_log_count, 0,
		      "partial match must NOT be forwarded as a URC either");

	feed_line("+ID: DevEui, 2C:F7:F1:20:32:30:84:63");
	zassert_equal(result_log_count, 0,
		      "must NOT resolve after only 2 of 3 required matches");

	feed_line("+ID: AppEui, 80:00:00:00:00:00:00:06");
	zassert_equal(result_log_count, 1,
		      "must resolve exactly on the 3rd matching line");
	zassert_equal(result_log[0].result.outcome, LORA_E5_AT_OUTCOME_MATCHED, NULL);
	zassert_equal(result_log[0].result.result_tag, 7, NULL);
}

/* ------------------------------------------------------------------- */
/* ANY_URC-vs-ERROR regression (the bug caught during Phase 3 review)    */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_any_urc_does_not_swallow_error)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "MODE", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 100 },
		{ .prefix = NULL, .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_ERROR, .result_tag = -1 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+MODE=LWOTAA", .cmd_len = 14, .timeout_ms = 1000,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);

	feed_line("+MODE: ERROR(-1)");

	zassert_equal(result_log_count, 1, NULL);
	zassert_equal(result_log[0].result.result_tag, -1,
		      "an ERROR-kind line must resolve via the ANY_ERROR "
		      "entry, NOT be misreported as tag 100 (success) by "
		      "the same-prefixed ANY_URC entry checked first");
	zassert_equal(result_log[0].result.error_code,
		      LORA_E5_AT_ERR_INVALID_PARAM, NULL);
}

ZTEST(lora_e5_cmd_queue, test_any_urc_matches_non_error_same_prefix)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "MODE", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 100 },
		{ .prefix = NULL, .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_ERROR, .result_tag = -1 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+MODE=LWOTAA", .cmd_len = 14, .timeout_ms = 1000,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);

	feed_line("+MODE: LWOTAA"); /* plausible success echo */

	zassert_equal(result_log_count, 1, NULL);
	zassert_equal(result_log[0].result.result_tag, 100,
		      "a non-error same-prefix line must still resolve via "
		      "ANY_URC as before the fix");
}

/* ------------------------------------------------------------------- */
/* URC forwarding                                                        */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_unrelated_line_forwarded_as_urc)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "JOIN", .remainder = "Done",
		  .match_mode = LORA_E5_AT_MATCH_EXACT, .result_tag = 1 },
		{ .prefix = NULL, .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_ERROR, .result_tag = -1 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+JOIN", .cmd_len = 7, .timeout_ms = 5000,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);

	feed_line("+JOIN: Start");   /* not terminal -- must be forwarded */
	feed_line("+JOIN: NORMAL");  /* not terminal -- must be forwarded */

	zassert_equal(result_log_count, 0, "transaction must still be open");
	zassert_equal(urc_log_count, 2, "both intermediate lines forwarded");

	feed_line("+JOIN: Done");

	zassert_equal(result_log_count, 1, NULL);
	zassert_equal(urc_log_count, 2, "terminal line must NOT also be forwarded as a URC");
}

/* ------------------------------------------------------------------- */
/* No transport registered                                               */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_no_transport_resolves_uart_error_immediately)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "X", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 1 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+X", .cmd_len = 4, .timeout_ms = 1000,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	lora_e5_at_set_transport(NULL); /* undo case_before's registration */

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);

	zassert_equal(result_log_count, 1, NULL);
	zassert_equal(result_log[0].result.outcome,
		      LORA_E5_AT_OUTCOME_UART_ERROR, NULL);
}

/* ------------------------------------------------------------------- */
/* Cascade: transport fails for every queued command                     */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_cascade_resolves_all_on_persistent_send_failure)
{
	static const struct lora_e5_at_terminal_event te[] = {
		{ .prefix = "X", .remainder = NULL,
		  .match_mode = LORA_E5_AT_MATCH_ANY_URC, .result_tag = 1 },
	};
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT+X", .cmd_len = 4, .timeout_ms = 1000,
		.terminal_events = te, .terminal_event_count = ARRAY_SIZE(te),
	};

	mock.forced_rc = -EIO; /* every send fails */

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), 0, NULL);

	zassert_equal(result_log_count, 1,
		      "the only submitted command must resolve as "
		      "UART_ERROR even though nothing was ever 'sent' "
		      "successfully");
	zassert_equal(result_log[0].result.outcome,
		      LORA_E5_AT_OUTCOME_UART_ERROR, NULL);
	zassert_false(lora_e5_cmd_queue_test_is_active(), NULL);
}

/* ------------------------------------------------------------------- */
/* Validation                                                             */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_cmd_queue, test_zero_timeout_rejected)
{
	struct lora_e5_at_cmd_desc desc = {
		.cmd = "AT", .cmd_len = 2, .timeout_ms = 0,
	};

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), -EINVAL,
		      "a descriptor with timeout_ms==0 must be rejected at "
		      "submit time, not silently hang forever");
}

ZTEST(lora_e5_cmd_queue, test_oversized_command_rejected)
{
	static char big_cmd[600];
	struct lora_e5_at_cmd_desc desc = {
		.cmd = big_cmd, .cmd_len = sizeof(big_cmd), .timeout_ms = 1000,
	};

	memset(big_cmd, 'A', sizeof(big_cmd));

	zassert_equal(lora_e5_at_submit(&desc, capture_result_cb), -EINVAL,
		      "spec limit is 528 bytes (Table -21 / section 2.1) -- "
		      "must reject before ever touching the transport");
}

ZTEST_SUITE(lora_e5_cmd_queue, NULL, suite_setup, case_before, NULL, NULL);
