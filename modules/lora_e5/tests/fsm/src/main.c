/**
 * @file tests/fsm/src/main.c
 * @brief Integration tests for the LoRaWAN FSM (lora_e5_fsm.c), driven
 * through real lora_e5_fsm_post_event() calls plus the real Modem
 * Manager/AT stack underneath -- mock only at the transport boundary
 * (tests/mock_uart), everything above it is real code, same
 * integration-style approach tests/modem_manager already uses.
 *
 * Also folds in public-API-level smoke assertions (Phase 1 Testing
 * Strategy / Phase 3 bring-up plan): calling lora_e5_init()-adjacent
 * public functions (lora_e5_join_sync()/lora_e5_send_sync()/etc, via
 * the same internal wiring lora_e5_init() would perform, minus the
 * real UART step) against the mock transport, rather than standing up
 * a fourth, separate test directory.
 *
 * Covers the properties Phase 1 §10 calls out explicitly:
 *   - happy-path boot->join->send->sleep->wake
 *   - join failure -> RECOVERING -> rejoin
 *   - structural CONFIG error -> ERROR, no retry loop
 *   - confirmed-uplink no-ack -> TX_FAILED
 *   - recovery-ladder exhaustion -> ERROR-and-stay
 *   - regression property: a transport-timeout-classified fault never
 *     retries a structural-classified fault
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

#include "lora_e5/lora_e5.h"
#include "lora_e5/lora_e5_at.h"
#include "lora_e5_cmd_queue.h"
#include "lora_e5_modem_manager.h"
#include "lora_e5_internal.h"
#include "mock_uart.h"

/* ------------------------------------------------------------------- */
/* Test work queues (stand in for lora_e5_init()'s three real ones)     */
/* ------------------------------------------------------------------- */

#define TEST_WQ_STACK_SIZE 2048
#define TEST_WQ_PRIORITY   5

static K_THREAD_STACK_DEFINE(rx_wq_stack, TEST_WQ_STACK_SIZE);
static K_THREAD_STACK_DEFINE(fsm_wq_stack, TEST_WQ_STACK_SIZE);
static K_THREAD_STACK_DEFINE(notify_wq_stack, TEST_WQ_STACK_SIZE);

static struct k_work_q rx_wq;
static struct k_work_q fsm_wq;
static struct k_work_q notify_wq;

/* ------------------------------------------------------------------- */
/* App event capture (stands in for the application's registered       */
/* callback)                                                            */
/* ------------------------------------------------------------------- */

#define APP_EVENT_LOG_MAX 32

static struct lora_e5_app_event app_event_log[APP_EVENT_LOG_MAX];
static int app_event_log_count;

static void capture_app_event_cb(const struct lora_e5_app_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	if (app_event_log_count < APP_EVENT_LOG_MAX) {
		app_event_log[app_event_log_count] = *event;
		app_event_log_count++;
	}
}

static int count_app_events(enum lora_e5_app_event_type type)
{
	int n = 0;

	for (int i = 0; i < app_event_log_count; i++) {
		if (app_event_log[i].type == type) {
			n++;
		}
	}
	return n;
}

/** Waits for both the FSM and notify work queues to drain everything
 *  queued up to this point -- see lora_e5_fsm_get_state_sync()'s and
 *  lora_e5_notify_test_barrier()'s doc comments.
 */
static void sync_barrier(void)
{
	(void)lora_e5_fsm_get_state_sync();
	lora_e5_notify_test_barrier();
}

/*
 * NOTE on synchronization: sync_barrier() only proves the FSM/notify
 * work queues have drained whatever was queued on THEM as of the call
 * -- it does NOT cover the mock transport's own hop through rx_wq
 * (mock_uart_send()'s scripted/auto-responder feed is itself a k_work
 * item on rx_wq, and CONFIG's multi-step chain re-enters rx_wq once
 * per step). Every test below that triggers a NEW AT command (as
 * opposed to a direct mock_uart_feed_line() call, which runs
 * synchronously on the caller's own thread) therefore pairs a real
 * k_sleep() margin with sync_barrier(), or uses one of the blocking
 * `_sync()` public API calls (which have no such race -- they block on
 * a semaphore only given once the real terminal event is observed).
 * Margins are generous on purpose, since native_sim's simulated clock
 * fast-forwards through idle time anyway (confirmed empirically: the
 * existing suites' real 2000-15000ms Kconfig timeouts do not slow the
 * overall test run down).
 */

/* ------------------------------------------------------------------- */
/* Auto-responder for the generic boot/CONFIG/JOIN happy path           */
/* ------------------------------------------------------------------- */

static const char *g_join_reply = "+JOIN: Done";

static const char *happy_path_responder(const char *cmd, size_t len)
{
	ARG_UNUSED(len);

	if (strncmp(cmd, "AT+RESET", 8) == 0) {
		return "+RESET: OK";
	}
	if (strcmp(cmd, "AT") == 0) {
		/* "+AT: OK", not bare "OK" -- confirmed against real
		 * hardware (firmware V4.0.11); see
		 * docs/VERIFICATION_NEEDED.md's te_probe entry.
		 */
		return "+AT: OK";
	}
	if (strncmp(cmd, "AT+MODE", 7) == 0) {
		return "+MODE: OK";
	}
	if (strncmp(cmd, "AT+ID", 5) == 0) {
		return "+ID: OK";
	}
	if (strncmp(cmd, "AT+KEY", 6) == 0) {
		return "+KEY: OK";
	}
	if (strncmp(cmd, "AT+DR", 5) == 0) {
		return "+DR: OK";
	}
	if (strncmp(cmd, "AT+CLASS", 8) == 0) {
		return "+CLASS: OK";
	}
	if (strncmp(cmd, "AT+PORT", 7) == 0) {
		return "+PORT: OK";
	}
	if (strncmp(cmd, "AT+ADR=", 7) == 0) {
		return (strstr(cmd, "=ON") != NULL) ? "+ADR: ON" : "+ADR: OFF";
	}
	if (strncmp(cmd, "AT+REPT", 7) == 0) {
		return "+REPT: OK";
	}
	if (strncmp(cmd, "AT+RETRY", 8) == 0) {
		return "+RETRY: OK";
	}
	if (strncmp(cmd, "AT+JOIN", 7) == 0) {
		return g_join_reply;
	}
	if (strncmp(cmd, "AT+MSGHEX", 9) == 0) {
		return "+MSGHEX: Done";
	}
	if (strncmp(cmd, "AT+CMSGHEX", 10) == 0) {
		return "+CMSGHEX: Done";
	}
	if (strncmp(cmd, "AT+LOWPOWER", 11) == 0) {
		return "+LOWPOWER: SLEEP";
	}
	return NULL;
}

/**
 * Same as happy_path_responder() through AT+MODE, but returns NULL
 * (no auto-response) for AT+ID onward -- since every hop the
 * always-on auto-responder drives is near-instantaneous, a full boot
 * would otherwise race straight through to READY before a fixed
 * k_sleep() margin could ever "catch" it mid-CONFIG for manual fault
 * injection. This reliably pauses the chain right after CONFIG's
 * first sub-command (MODE) resolves and its second (ID) is submitted,
 * awaiting a manual mock_uart_feed_line().
 */
static const char *boot_then_pause_before_id_responder(const char *cmd, size_t len)
{
	ARG_UNUSED(len);

	if (strncmp(cmd, "AT+RESET", 8) == 0) {
		return "+RESET: OK";
	}
	if (strcmp(cmd, "AT") == 0) {
		return "+AT: OK";
	}
	if (strncmp(cmd, "AT+MODE", 7) == 0) {
		return "+MODE: OK";
	}
	return NULL;
}

/**
 * Models "the modem's AT parser doesn't respond to a bare AT probe
 * issued without a prior AT+RESET" -- fails bare "AT" until AT+RESET
 * has actually been sent, then behaves exactly like
 * happy_path_responder() for everything else (including a subsequent
 * bare "AT", i.e. CHECK_AT's own probe after the reset succeeds).
 * Used to test lora_e5_resume()'s fallback path: the fast-path probe
 * genuinely fails, and the FSM must fall back to the ordinary full
 * RESET+CONFIG sequence and land at READY (not silently stall).
 */
static bool g_reset_seen;

static const char *resume_probe_fails_responder(const char *cmd, size_t len)
{
	if (strncmp(cmd, "AT+RESET", 8) == 0) {
		g_reset_seen = true;
		return "+RESET: OK";
	}
	if (strcmp(cmd, "AT") == 0 && !g_reset_seen) {
		return NULL; /* no response -- probe times out */
	}
	return happy_path_responder(cmd, len);
}

/* ------------------------------------------------------------------- */
/* Setup / teardown                                                     */
/* ------------------------------------------------------------------- */

static struct lora_e5_otaa_config make_otaa(void)
{
	struct lora_e5_otaa_config otaa = { 0 };

	memcpy(otaa.dev_eui.bytes,
	       ((uint8_t[]){ 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 }), 8);
	memcpy(otaa.app_eui.bytes,
	       ((uint8_t[]){ 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18 }), 8);
	memcpy(otaa.app_key.bytes,
	       ((uint8_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }), 16);
	return otaa;
}

static void *suite_setup(void)
{
	k_work_queue_init(&rx_wq);
	k_work_queue_start(&rx_wq, rx_wq_stack, K_THREAD_STACK_SIZEOF(rx_wq_stack),
			    TEST_WQ_PRIORITY, NULL);
	k_work_queue_init(&fsm_wq);
	k_work_queue_start(&fsm_wq, fsm_wq_stack, K_THREAD_STACK_SIZEOF(fsm_wq_stack),
			    TEST_WQ_PRIORITY, NULL);
	k_work_queue_init(&notify_wq);
	k_work_queue_start(&notify_wq, notify_wq_stack, K_THREAD_STACK_SIZEOF(notify_wq_stack),
			    TEST_WQ_PRIORITY, NULL);

	mock_uart_init(&rx_wq);
	return NULL;
}

static void case_before(void *f)
{
	ARG_UNUSED(f);

	static const struct lora_e5_mm_init_params mm_params = {
		.reset_backend = LORA_E5_RESET_BACKEND_AT_COMMAND,
		.reset_gpio = NULL,
	};
	struct lora_e5_otaa_config otaa = make_otaa();

	mock_uart_reset();
	g_join_reply = "+JOIN: Done";
	g_reset_seen = false;
	memset(app_event_log, 0, sizeof(app_event_log));
	app_event_log_count = 0;

	zassert_equal(lora_e5_at_init(&rx_wq), 0, NULL);
	lora_e5_at_set_transport(mock_uart_send);

	zassert_equal(lora_e5_mm_init(&mm_params), 0, NULL);

	/* Cancel any delayable work (e.g. the recovery ladder's inter-pass
	 * retry timer) a previous test left pending before re-initializing
	 * the FSM -- re-running k_work_init_delayable() on a still-scheduled
	 * work item corrupts the kernel's internal timeout list instead of
	 * cleanly replacing it.
	 */
	lora_e5_fsm_test_reset();
	zassert_equal(lora_e5_fsm_init(&fsm_wq), 0, NULL);
	zassert_equal(lora_e5_notify_init(&notify_wq), 0, NULL);
	zassert_equal(lora_e5_notify_set_callback(capture_app_event_cb, NULL), 0, NULL);
	lora_e5_test_bind();

	zassert_equal(lora_e5_set_otaa(&otaa), 0, NULL);
	zassert_equal(lora_e5_set_region(LORA_E5_REGION_EU868), 0, NULL);
}

/* ------------------------------------------------------------------- */
/* Happy path: boot -> join -> send -> sleep -> wake                    */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_happy_path_boot_join_send_sleep_wake)
{
	mock_uart_set_auto_responder(happy_path_responder);

	zassert_equal(lora_e5_start_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_READY, NULL);

	zassert_equal(lora_e5_join_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED, NULL);

	static const uint8_t payload[] = { 0xAA, 0xBB };

	zassert_equal(lora_e5_send_sync(payload, sizeof(payload), K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED, NULL);

	zassert_equal(lora_e5_sleep(0), 0, NULL);
	sync_barrier();
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_SLEEP, NULL);

	zassert_equal(lora_e5_wakeup_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED, NULL);

	sync_barrier();
	zassert_true(count_app_events(LORA_E5_APP_EVT_JOIN_SUCCESS) >= 1, NULL);
	zassert_true(count_app_events(LORA_E5_APP_EVT_TX_SUCCESS) >= 1, NULL);
	zassert_true(count_app_events(LORA_E5_APP_EVT_SLEEP_ENTERED) >= 1, NULL);
	zassert_true(count_app_events(LORA_E5_APP_EVT_WAKE_COMPLETE) >= 1, NULL);
}

/* ------------------------------------------------------------------- */
/* Join failure -> RECOVERING -> rejoin                                  */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_join_failure_enters_recovering_then_rejoins)
{
	mock_uart_set_auto_responder(happy_path_responder);

	zassert_equal(lora_e5_start_sync(K_MSEC(5000)), 0, NULL);

	g_join_reply = "+JOIN: Join failed";
	/* join_sync() blocks on a real semaphore given only when the
	 * observer sees a terminal join event (JOIN_SUCCESS or
	 * JOIN_FAILED) -- unlike an async lora_e5_join() + a bare
	 * sync_barrier(), this has no race against the mock's own rx_wq
	 * hop. It's still not safe to inspect app_event_log immediately on
	 * return, though: the observer's k_sem_give() can preempt notify_wq
	 * before it reaches the app callback for that SAME event (both are
	 * invoked from drain_work_handler(), but nothing prevents a
	 * preemption between the two calls) -- sync_barrier() below (a
	 * further round-trip through notify_wq) forces that in-progress
	 * drain_work_handler() invocation to finish first, since a work
	 * queue always completes its current work item before starting the
	 * next one.
	 */
	zassert_equal(lora_e5_join_sync(K_MSEC(2000)), -EIO, NULL);
	sync_barrier();

	zassert_true(count_app_events(LORA_E5_APP_EVT_JOIN_FAILED) >= 1,
		     "join failure must be surfaced to the application");

	/* Recovery ladder: reset -> reconfigure -> rejoin (AUTO_REJOIN
	 * default on, and the FSM was mid-JOINING, i.e. not yet JOINED, so
	 * recovery_was_joined is false and the ladder stops at READY --
	 * this exercises the "stop at READY, don't rejoin" branch, which is
	 * correct here since the fault happened before any successful join
	 * ever occurred. Margin generous for the same reason as
	 * test_fault_while_joined_rejoins_automatically: reset + all 10
	 * CONFIG steps, each its own rx_wq/fsm_wq hop.
	 */
	k_sleep(K_MSEC(800));
	sync_barrier();
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_READY, NULL);

	/* Application can now retry the join explicitly. */
	g_join_reply = "+JOIN: Done";
	zassert_equal(lora_e5_join_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED, NULL);
}

ZTEST(lora_e5_fsm, test_fault_while_joined_rejoins_automatically)
{
	mock_uart_set_auto_responder(happy_path_responder);

	zassert_equal(lora_e5_start_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_join_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED, NULL);

	/* Simulate a UART-level fault while JOINED -- recovery_was_joined
	 * is true here, so (CONFIG_LORA_E5_AUTO_REJOIN default on) the
	 * ladder should run all the way through reset -> reconfigure ->
	 * rejoin and land back in JOINED, unattended.
	 */
	int rc = lora_e5_fsm_post_event(
		&(struct lora_e5_fsm_event){ .type = LORA_E5_FSM_EVT_UART_FAULT });
	zassert_equal(rc, 0, NULL);

	/* Give the ladder's chain of AT transactions (reset, all 10 CONFIG
	 * steps, join) time to resolve via the auto-responder -- each step
	 * is its own rx_wq/fsm_wq hop, so this needs more margin than a
	 * single-transaction settle().
	 */
	k_sleep(K_MSEC(800));
	sync_barrier();

	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED,
		      "ladder must automatically rejoin when the fault occurred while JOINED");
	zassert_true(count_app_events(LORA_E5_APP_EVT_JOIN_SUCCESS) >= 2,
		     "expect one JOIN_SUCCESS for the initial join and one for the rejoin");
}

/* ------------------------------------------------------------------- */
/* Structural CONFIG error -> ERROR, no retry loop                       */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_structural_config_error_goes_straight_to_error)
{
	mock_uart_set_auto_responder(boot_then_pause_before_id_responder);

	zassert_equal(lora_e5_start(), 0, NULL);

	/* Let RESET/BOOT-settle/CHECK_AT/first CONFIG step (MODE) resolve
	 * via the auto-responder, which pauses right after submitting the
	 * second CONFIG step (AT+ID=...) -- margin short of
	 * CONFIG_LORA_E5_CMD_TIMEOUT_MS (500ms in this suite's Kconfig
	 * overrides) so the transaction is still open when we intercept it
	 * with a structural error below.
	 */
	k_sleep(K_MSEC(150));
	sync_barrier();
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_CONFIG, NULL);
	zassert_true(strncmp(mock_uart_last_cmd(), "AT+ID", 5) == 0,
		     "expected AT+ID as CONFIG's second sub-command, got: %s",
		     mock_uart_last_cmd());

	mock_uart_set_auto_responder(NULL);
	mock_uart_feed_line("ERROR(-1)"); /* LORA_E5_AT_ERR_INVALID_PARAM */
	sync_barrier();

	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_ERROR,
		      "a structural AT error during CONFIG must go straight to ERROR");
	zassert_equal(count_app_events(LORA_E5_APP_EVT_ERROR), 1, NULL);

	int sends_before = mock_uart_send_count();

	k_sleep(K_MSEC(3000));
	sync_barrier();
	zassert_equal(mock_uart_send_count(), sends_before,
		      "ERROR state must not retry on its own -- no further AT "
		      "transactions should be issued without an explicit lora_e5_reset()");
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_ERROR, NULL);
}

/* ------------------------------------------------------------------- */
/* Confirmed uplink, no ACK -> TX_FAILED                                  */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_confirmed_uplink_no_ack_reports_tx_failed)
{
	mock_uart_set_auto_responder(happy_path_responder);

	zassert_equal(lora_e5_start_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_join_sync(K_MSEC(5000)), 0, NULL);

	/* "+CMSGHEX: Done" with no prior "ACK Received" URC -> NO_ACK,
	 * matching mm_send_result_cb()'s g_cmsg_ack_seen disambiguation.
	 */
	static const uint8_t payload[] = { 0x01 };

	mock_uart_set_auto_responder(NULL);
	zassert_equal(lora_e5_send_confirmed(payload, sizeof(payload)), 0, NULL);
	sync_barrier();
	zassert_true(strncmp(mock_uart_last_cmd(), "AT+CMSGHEX", 10) == 0, NULL);

	mock_uart_feed_line("+CMSGHEX: Done");
	sync_barrier();

	zassert_equal(count_app_events(LORA_E5_APP_EVT_TX_FAILED), 1, NULL);
	for (int i = 0; i < app_event_log_count; i++) {
		if (app_event_log[i].type == LORA_E5_APP_EVT_TX_FAILED) {
			zassert_equal(app_event_log[i].tx.fail_reason,
				      LORA_E5_TX_FAIL_NO_ACK, NULL);
		}
	}
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED,
		      "a semantic TX rejection must not affect connectivity state");
}

/* ------------------------------------------------------------------- */
/* Recovery-ladder exhaustion -> ERROR-and-stay                          */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_recovery_ladder_exhaustion_goes_to_error_and_stays)
{
	mock_uart_set_auto_responder(happy_path_responder);

	zassert_equal(lora_e5_start_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_join_sync(K_MSEC(5000)), 0, NULL);

	/* Force every reset attempt to fail at the transport level (never
	 * resolves as MATCHED) by disabling the auto-responder and never
	 * feeding a matching response -- each attempt times out instead.
	 * Directly posting RECOVERY_LADDER_EXHAUSTED below exercises the
	 * ladder's stop condition without waiting out
	 * CONFIG_LORA_E5_MAX_RETRIES real timeout windows -- the
	 * timeout-driven path itself is already covered by tests/cmd_queue's
	 * timeout handling.
	 */
	mock_uart_set_auto_responder(NULL);

	int rc = lora_e5_fsm_post_event(
		&(struct lora_e5_fsm_event){ .type = LORA_E5_FSM_EVT_UART_FAULT });
	zassert_equal(rc, 0, NULL);
	sync_barrier();
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_RECOVERING, NULL);

	rc = lora_e5_fsm_post_event(&(struct lora_e5_fsm_event){
		.type = LORA_E5_FSM_EVT_RECOVERY_LADDER_EXHAUSTED });
	zassert_equal(rc, 0, NULL);
	sync_barrier();

	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_ERROR, NULL);
	zassert_equal(count_app_events(LORA_E5_APP_EVT_ERROR), 1, NULL);

	/* Must stay in ERROR -- no silent infinite retry loop. */
	int sends_before = mock_uart_send_count();

	k_sleep(K_MSEC(3000));
	sync_barrier();
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_ERROR, NULL);
	zassert_equal(mock_uart_send_count(), sends_before, NULL);
}

/* ------------------------------------------------------------------- */
/* Regression: a transport-timeout fault must never retry a structural  */
/* fault (Phase 1 §10's explicit regression guard)                      */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_structural_error_not_retried_after_unrelated_uart_fault)
{
	mock_uart_set_auto_responder(boot_then_pause_before_id_responder);

	zassert_equal(lora_e5_start(), 0, NULL);
	k_sleep(K_MSEC(150));
	sync_barrier();
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_CONFIG, NULL);
	zassert_true(strncmp(mock_uart_last_cmd(), "AT+ID", 5) == 0, NULL);

	mock_uart_set_auto_responder(NULL);
	mock_uart_feed_line("ERROR(-1)"); /* structural: LORA_E5_AT_ERR_INVALID_PARAM */
	sync_barrier();
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_ERROR, NULL);

	int sends_before = mock_uart_send_count();

	/* An unrelated transport-level fault arriving afterward must not
	 * resurrect a retry loop around the already-classified structural
	 * fault -- ERROR is terminal until an explicit lora_e5_reset().
	 */
	int rc = lora_e5_fsm_post_event(
		&(struct lora_e5_fsm_event){ .type = LORA_E5_FSM_EVT_UART_FAULT });
	zassert_equal(rc, 0, NULL);
	sync_barrier();

	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_ERROR,
		      "ERROR must stay ERROR -- a later transport fault must not reopen "
		      "a structural failure for retry");
	zassert_equal(mock_uart_send_count(), sends_before, NULL);
}

/* ------------------------------------------------------------------- */
/* lora_e5_resume(): fast path reaches JOINED without ever sending      */
/* AT+RESET -- confirmed real-hardware behavior, see                    */
/* docs/VERIFICATION_NEEDED.md's "Resolved 2026-07-11" section          */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_resume_fast_path_skips_reset_and_config)
{
	mock_uart_set_auto_responder(happy_path_responder);
	g_join_reply = "+JOIN: Joined already";

	zassert_equal(lora_e5_resume_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED, NULL);

	/* The whole point: only "AT" then "AT+JOIN" were ever sent -- no
	 * AT+RESET, no CONFIG sub-commands (AT+MODE/AT+ID/AT+KEY/...).
	 */
	zassert_equal(mock_uart_send_count(), 2, NULL);

	sync_barrier();
	zassert_true(count_app_events(LORA_E5_APP_EVT_JOIN_SUCCESS) >= 1, NULL);
}

/* ------------------------------------------------------------------- */
/* lora_e5_resume(): fast-path probe failure falls back to the ordinary */
/* full RESET+CONFIG sequence and lands at READY (not silently stuck)   */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_fsm, test_resume_fallback_to_full_start_when_probe_fails)
{
	mock_uart_set_auto_responder(resume_probe_fails_responder);

	/* resume_probe_fails_responder() never answers the fast path's
	 * bare "AT" -- lora_e5_mm_probe() exhausts its own bounded retries
	 * and reports failure, which handle_probe_result()'s RESUMING
	 * branch routes to recovery_step_failed() (same fallback machinery
	 * every other fault uses) rather than straight to ERROR. That
	 * falls all the way through AT+RESET -> CHECK_AT -> full CONFIG,
	 * landing at READY -- not JOINED, since recovery_was_joined is
	 * false for a fault that occurred before any prior JOINED state
	 * (CLAUDE.md decision #2: no auto-join after CONFIG on the
	 * fallback path either).
	 */
	zassert_equal(lora_e5_resume_sync(K_SECONDS(30)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_READY, NULL);
	zassert_true(g_reset_seen, "fallback must have actually sent AT+RESET");

	/* Finish the job exactly as an application would after resume_sync()
	 * lands at READY instead of JOINED.
	 */
	zassert_equal(lora_e5_join_sync(K_MSEC(5000)), 0, NULL);
	zassert_equal(lora_e5_get_state(), LORA_E5_STATE_JOINED, NULL);
}

ZTEST_SUITE(lora_e5_fsm, NULL, suite_setup, case_before, NULL, NULL);
