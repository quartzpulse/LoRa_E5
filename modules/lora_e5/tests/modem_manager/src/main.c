/**
 * @file tests/modem_manager/src/main.c
 * @brief Unit tests for the Modem Manager (lora_e5_modem_manager.c),
 * using the same mock-transport pattern as tests/cmd_queue -- a
 * function pointer standing in for lora_e5_uart.c (which does not
 * exist yet), driven through the real AT Command Manager and real
 * lora_e5_hf_commands.c builders/tables (no mocking below the Modem
 * Manager itself, so these tests exercise the full stack down to
 * wire-format command strings).
 *
 * Primary focus: the VERIFICATION_NEEDED.md item 5 port-cache fix in
 * lora_e5_mm_send(), plus CONFIG sequencing, JOIN (OTAA/ABP), CMSG ack
 * disambiguation, downlink URC parsing, and the explicitly-flagged
 * -ENOTSUP contracts (get_version/get_ids/get_max_payload/at_raw).
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include "lora_e5/lora_e5_at.h"
#include "lora_e5_cmd_queue.h"
#include "lora_e5_modem_manager.h"

/* Mirrors CFG_STEP_COUNT from lora_e5_modem_manager.c's private
 * `enum config_step` (not exposed to this test file -- internal to
 * that translation unit) -- OTAA and ABP both drive exactly 10
 * sub-commands through lora_e5_mm_configure().
 */
#define CFG_STEP_COUNT_FOR_TEST 10

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

#define MOCK_LAST_CMD_BUF_SIZE 600

struct mock_transport_state {
	int send_count;
	char last_cmd[MOCK_LAST_CMD_BUF_SIZE];
	size_t last_cmd_len;
	int forced_rc;
};

static struct mock_transport_state mock;

static void feed_line(const char *text); /* defined below, in Helpers */

/* Scripted auto-response for the synchronous-query tests
 * (get_version/get_ids): lora_e5_at_submit_sync() blocks the calling
 * (ztest) thread on a semaphore that only test_wq's line processing
 * can give, so a script armed here is "replayed" as the modem's
 * response once mock_send() is invoked. Fed via a work item on test_wq
 * rather than called directly from mock_send() -- lora_e5_at_process_line()
 * must run on the same work queue passed to lora_e5_at_init() (its own
 * doc comment), and calling it synchronously from here would also
 * deadlock: mock_send() runs while lora_e5_cmd_queue_submit() still
 * holds its internal lock.
 */
#define MOCK_SCRIPT_MAX 4
static const char *mock_script_lines[MOCK_SCRIPT_MAX];
static size_t mock_script_count;
static struct k_work mock_script_work;

static void mock_script_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	size_t n = mock_script_count;

	mock_script_count = 0;
	for (size_t i = 0; i < n; i++) {
		feed_line(mock_script_lines[i]);
	}
}

static int mock_send(const char *cmd, size_t len)
{
	mock.send_count++;
	mock.last_cmd_len = len < sizeof(mock.last_cmd) - 1 ? len : sizeof(mock.last_cmd) - 1;
	memcpy(mock.last_cmd, cmd, mock.last_cmd_len);
	mock.last_cmd[mock.last_cmd_len] = '\0';

	if (mock_script_count > 0) {
		k_work_submit_to_queue(&test_wq, &mock_script_work);
	}

	return mock.forced_rc;
}

/* ------------------------------------------------------------------- */
/* Event capture (stands in for the FSM's event queue)                   */
/* ------------------------------------------------------------------- */

#define EVENT_LOG_MAX 16

static struct lora_e5_fsm_event event_log[EVENT_LOG_MAX];
static int event_log_count;

static void capture_event_cb(const struct lora_e5_fsm_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	if (event_log_count < EVENT_LOG_MAX) {
		event_log[event_log_count] = *event;
		event_log_count++;
	}
}

/* ------------------------------------------------------------------- */
/* Helpers                                                               */
/* ------------------------------------------------------------------- */

static void feed_line(const char *text)
{
	struct lora_e5_at_line line;

	zassert_equal(lora_e5_at_parse_line(text, strlen(text), &line), 0, NULL);
	lora_e5_at_process_line(&line);
}

static void hex_upper(const uint8_t *b, size_t n, char *out)
{
	static const char digits[] = "0123456789ABCDEF";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[(b[i] >> 4) & 0xF];
		out[2 * i + 1] = digits[b[i] & 0xF];
	}
	out[2 * n] = '\0';
}

static struct lora_e5_config make_otaa_config(uint8_t port)
{
	struct lora_e5_config cfg = { 0 };

	cfg.region = LORA_E5_REGION_EU868;
	cfg.dev_class = LORA_E5_CLASS_A;
	cfg.join_type = LORA_E5_JOIN_OTAA;
	memcpy(cfg.otaa.dev_eui.bytes, ((uint8_t[]){ 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 }), 8);
	memcpy(cfg.otaa.app_eui.bytes, ((uint8_t[]){ 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18 }), 8);
	memcpy(cfg.otaa.app_key.bytes,
	       ((uint8_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }), 16);
	cfg.adr_enable = true;
	cfg.port = port;
	cfg.unconfirmed_repeats = 1;
	cfg.confirmed_retries = 3;
	return cfg;
}

static struct lora_e5_config make_abp_config(uint8_t port)
{
	struct lora_e5_config cfg = { 0 };

	cfg.region = LORA_E5_REGION_EU868;
	cfg.dev_class = LORA_E5_CLASS_A;
	cfg.join_type = LORA_E5_JOIN_ABP;
	memcpy(cfg.abp.dev_addr.bytes, ((uint8_t[]){ 0x01, 0x02, 0x03, 0x04 }), 4);
	memcpy(cfg.abp.nwk_skey.bytes,
	       ((uint8_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }), 16);
	memcpy(cfg.abp.app_skey.bytes,
	       ((uint8_t[]){ 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
			     0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F }), 16);
	cfg.adr_enable = false;
	cfg.port = port;
	cfg.unconfirmed_repeats = 1;
	cfg.confirmed_retries = 3;
	return cfg;
}

/** Feeds a success line shaped correctly for whatever command was last
 *  sent -- every CONFIG sub-command's terminal_events[] table matches
 *  on ITS OWN prefix (LORA_E5_AT_MATCH_ANY_URC requires an exact
 *  prefix match, see lora_e5_cmd_queue.c's line_matches_entry()), so a
 *  single generic response text doesn't work here.
 */
static void feed_ok_for_last_cmd(bool adr_enable)
{
	if (strncmp(mock.last_cmd, "AT+ADR=", 7) == 0) {
		feed_line(adr_enable ? "+ADR: ON" : "+ADR: OFF");
	} else if (strncmp(mock.last_cmd, "AT+MODE", 7) == 0) {
		feed_line("+MODE: OK");
	} else if (strncmp(mock.last_cmd, "AT+ID", 5) == 0) {
		feed_line("+ID: OK");
	} else if (strncmp(mock.last_cmd, "AT+KEY", 6) == 0) {
		feed_line("+KEY: OK");
	} else if (strncmp(mock.last_cmd, "AT+DR", 5) == 0) {
		feed_line("+DR: OK");
	} else if (strncmp(mock.last_cmd, "AT+CLASS", 8) == 0) {
		feed_line("+CLASS: OK");
	} else if (strncmp(mock.last_cmd, "AT+PORT", 7) == 0) {
		feed_line("+PORT: OK");
	} else if (strncmp(mock.last_cmd, "AT+REPT", 7) == 0) {
		feed_line("+REPT: OK");
	} else if (strncmp(mock.last_cmd, "AT+RETRY", 8) == 0) {
		feed_line("+RETRY: OK");
	} else {
		zassert_unreachable("unrecognized CONFIG command: %s", mock.last_cmd);
	}
}

/** Drives a full 10-step CONFIG sequence to completion. Asserts
 *  nothing about the command text itself -- callers that care about
 *  exact wire format drive the sequence by hand instead (see
 *  test_configure_otaa_happy_path).
 */
static void drive_configure_to_completion(const struct lora_e5_config *cfg)
{
	int rc = lora_e5_mm_configure(cfg);

	zassert_equal(rc, 0, NULL);

	for (int i = 0; i < CFG_STEP_COUNT_FOR_TEST; i++) {
		feed_ok_for_last_cmd(cfg->adr_enable);
	}
}

static void *suite_setup(void)
{
	static const struct lora_e5_mm_init_params init_params = {
		.reset_backend = LORA_E5_RESET_BACKEND_AT_COMMAND,
		.reset_gpio = NULL,
	};

	k_work_queue_init(&test_wq);
	k_work_queue_start(&test_wq, test_wq_stack,
			    K_THREAD_STACK_SIZEOF(test_wq_stack),
			    TEST_WQ_PRIORITY, NULL);

	k_work_init(&mock_script_work, mock_script_work_handler);

	zassert_equal(lora_e5_at_init(&test_wq), 0, NULL);

	ARG_UNUSED(init_params);
	return NULL;
}

static void case_before(void *f)
{
	ARG_UNUSED(f);

	static const struct lora_e5_mm_init_params init_params = {
		.reset_backend = LORA_E5_RESET_BACKEND_AT_COMMAND,
		.reset_gpio = NULL,
	};

	lora_e5_cmd_queue_test_reset();
	memset(&mock, 0, sizeof(mock));
	memset(event_log, 0, sizeof(event_log));
	event_log_count = 0;
	memset(mock_script_lines, 0, sizeof(mock_script_lines));
	mock_script_count = 0;

	lora_e5_at_set_transport(mock_send);

	/* lora_e5_mm_init() re-registers the Modem Manager's URC callback
	 * (wiped by lora_e5_cmd_queue_test_reset() above) and resets this
	 * module's own transient state via its internal
	 * lora_e5_mm_test_reset() call -- see that function's doc comment.
	 */
	zassert_equal(lora_e5_mm_init(&init_params), 0, NULL);
	zassert_equal(lora_e5_mm_set_event_callback(capture_event_cb, NULL), 0, NULL);
}

/* ------------------------------------------------------------------- */
/* Probe                                                                 */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_probe_success)
{
	zassert_equal(lora_e5_mm_probe(), 0, NULL);
	zassert_equal(mock.send_count, 1, NULL);
	zassert_mem_equal(mock.last_cmd, "AT", 2, NULL);

	feed_line("OK");

	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_PROBE_RESULT, NULL);
	zassert_equal(event_log[0].reset_result, 0, NULL);
}

ZTEST(lora_e5_modem_manager, test_probe_retries_then_fails)
{
	zassert_equal(lora_e5_mm_probe(), 0, NULL);

	/* CONFIG_LORA_E5_MAX_RETRIES defaults to 3 in this file (no Kconfig
	 * yet) -- initial attempt + 3 retries = 4 sends total before
	 * giving up.
	 */
	for (int i = 0; i < 4; i++) {
		zassert_equal(mock.send_count, i + 1, "attempt %d", i + 1);
		feed_line("ERROR(-22)");
	}

	zassert_equal(event_log_count, 1,
		      "must resolve exactly once after exhausting retries");
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_PROBE_RESULT, NULL);
	zassert_true(event_log[0].reset_result < 0, NULL);
}

/* ------------------------------------------------------------------- */
/* CONFIG sequencing                                                     */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_configure_otaa_happy_path)
{
	struct lora_e5_config cfg = make_otaa_config(9);
	char hex[64];
	char expect[80];

	zassert_equal(lora_e5_mm_configure(&cfg), 0, NULL);

	zassert_equal(mock.send_count, 1, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+MODE=LWOTAA", 14, NULL);
	feed_line("+MODE: LWOTAA");

	zassert_equal(mock.send_count, 2, NULL);
	hex_upper(cfg.otaa.dev_eui.bytes, 8, hex);
	snprintf(expect, sizeof(expect), "AT+ID=DevEui,%s", hex);
	zassert_equal(mock.last_cmd_len, strlen(expect), NULL);
	zassert_mem_equal(mock.last_cmd, expect, strlen(expect), NULL);
	feed_line("+ID: OK");

	zassert_equal(mock.send_count, 3, NULL);
	hex_upper(cfg.otaa.app_eui.bytes, 8, hex);
	snprintf(expect, sizeof(expect), "AT+ID=AppEui,%s", hex);
	zassert_mem_equal(mock.last_cmd, expect, strlen(expect), NULL);
	feed_line("+ID: OK");

	zassert_equal(mock.send_count, 4, NULL);
	hex_upper(cfg.otaa.app_key.bytes, 16, hex);
	snprintf(expect, sizeof(expect), "AT+KEY=APPKEY,%s", hex);
	zassert_mem_equal(mock.last_cmd, expect, strlen(expect), NULL);
	feed_line("+KEY: OK");

	zassert_equal(mock.send_count, 5, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+DR=EU868", 11, NULL);
	feed_line("+DR: OK");

	zassert_equal(mock.send_count, 6, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+CLASS=A", 10, NULL);
	feed_line("+CLASS: OK");

	zassert_equal(mock.send_count, 7, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+PORT=9", 9, NULL);
	feed_line("+PORT: OK");

	zassert_equal(mock.send_count, 8, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+ADR=ON", 9, NULL);
	feed_line("+ADR: ON");

	zassert_equal(mock.send_count, 9, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+REPT=1", 9, NULL);
	feed_line("+REPT: OK");

	zassert_equal(mock.send_count, 10, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+RETRY=3", 10, NULL);
	feed_line("+RETRY: OK");

	zassert_equal(event_log_count, CFG_STEP_COUNT_FOR_TEST,
		      "expected one CONFIG_STEP_RESULT per sub-command");
	for (int i = 0; i < CFG_STEP_COUNT_FOR_TEST; i++) {
		zassert_equal(event_log[i].type, LORA_E5_FSM_EVT_CONFIG_STEP_RESULT, NULL);
		zassert_equal(event_log[i].config_step_result.error, 0, "step %d", i);
		zassert_equal(event_log[i].config_step_result.is_last_step,
			      i == CFG_STEP_COUNT_FOR_TEST - 1, "step %d", i);
	}

	/* CONFIG's own AT+PORT= step must seed the send-time port cache --
	 * verified indirectly here: a subsequent send on the same port
	 * must NOT reissue AT+PORT=.
	 */
	static const uint8_t payload[] = { 0x01 };

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 9, false), 0, NULL);
	zassert_equal(mock.send_count, 11,
		      "port already matches CONFIG's own AT+PORT=9 -- must not reissue it");
	zassert_mem_equal(mock.last_cmd, "AT+MSGHEX=", 10, NULL);
}

ZTEST(lora_e5_modem_manager, test_configure_abp_command_shape)
{
	struct lora_e5_config cfg = make_abp_config(3);
	char hex[64];
	char expect[80];

	zassert_equal(lora_e5_mm_configure(&cfg), 0, NULL);

	zassert_mem_equal(mock.last_cmd, "AT+MODE=LWABP", 13, NULL);
	feed_line("+MODE: LWABP");

	hex_upper(cfg.abp.dev_addr.bytes, 4, hex);
	snprintf(expect, sizeof(expect), "AT+ID=DevAddr,%s", hex);
	zassert_mem_equal(mock.last_cmd, expect, strlen(expect), NULL);
	feed_line("+ID: OK");

	hex_upper(cfg.abp.nwk_skey.bytes, 16, hex);
	snprintf(expect, sizeof(expect), "AT+KEY=NWKSKEY,%s", hex);
	zassert_mem_equal(mock.last_cmd, expect, strlen(expect), NULL);
	feed_line("+KEY: OK");

	hex_upper(cfg.abp.app_skey.bytes, 16, hex);
	snprintf(expect, sizeof(expect), "AT+KEY=APPSKEY,%s", hex);
	zassert_mem_equal(mock.last_cmd, expect, strlen(expect), NULL);
	feed_line("+KEY: OK");

	/* Drain the remaining common steps. */
	feed_line("+DR: OK");
	feed_line("+CLASS: OK");
	feed_line("+PORT: OK");
	feed_line("+ADR: OFF");
	feed_line("+REPT: OK");
	feed_line("+RETRY: OK");

	zassert_equal(event_log_count, CFG_STEP_COUNT_FOR_TEST, NULL);
	for (int i = 0; i < CFG_STEP_COUNT_FOR_TEST; i++) {
		zassert_equal(event_log[i].config_step_result.error, 0, "step %d", i);
		zassert_equal(event_log[i].config_step_result.is_last_step,
			      i == CFG_STEP_COUNT_FOR_TEST - 1, "step %d", i);
	}
}

ZTEST(lora_e5_modem_manager, test_configure_halts_on_step_failure)
{
	struct lora_e5_config cfg = make_otaa_config(1);

	zassert_equal(lora_e5_mm_configure(&cfg), 0, NULL);

	feed_line("+MODE: LWOTAA");        /* step 0 OK */
	feed_line("ERROR(-1)");            /* step 1 (ID DevEui) fails */

	zassert_equal(mock.send_count, 2,
		      "must NOT proceed past the failed step");
	zassert_equal(event_log_count, 2, NULL);
	zassert_equal(event_log[0].config_step_result.error, 0, NULL);
	zassert_equal(event_log[1].config_step_result.error, LORA_E5_AT_ERR_INVALID_PARAM, NULL);
	zassert_false(event_log[1].config_step_result.is_last_step,
		      "a failed step must not be reported as the last step");
}

ZTEST(lora_e5_modem_manager, test_configure_rejects_non_class_a)
{
	struct lora_e5_config cfg = make_otaa_config(1);

	cfg.dev_class = LORA_E5_CLASS_B;

	zassert_equal(lora_e5_mm_configure(&cfg), -ENOTSUP, NULL);
	zassert_equal(mock.send_count, 0, "must not touch the transport at all");
}

/* ------------------------------------------------------------------- */
/* JOIN                                                                   */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_join_otaa_success_caches_devaddr_netid)
{
	zassert_equal(lora_e5_mm_join(LORA_E5_JOIN_OTAA), 0, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+JOIN", 7, NULL);

	feed_line("+JOIN: Start");
	feed_line("+JOIN: NetID 000024 DevAddr 48:00:00:01");
	zassert_equal(event_log_count, 0, "must still be open before Done");

	feed_line("+JOIN: Done");

	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_JOIN_RESULT, NULL);
	zassert_equal(event_log[0].join_result.outcome, LORA_E5_JOIN_OUTCOME_SUCCESS, NULL);
	zassert_equal(event_log[0].join_result.dev_addr.bytes[0], 0x48, NULL);
	zassert_equal(event_log[0].join_result.dev_addr.bytes[1], 0x00, NULL);
	zassert_equal(event_log[0].join_result.dev_addr.bytes[2], 0x00, NULL);
	zassert_equal(event_log[0].join_result.dev_addr.bytes[3], 0x01, NULL);
	zassert_equal(event_log[0].join_result.net_id, 0x24, NULL);
}

ZTEST(lora_e5_modem_manager, test_join_otaa_failed)
{
	zassert_equal(lora_e5_mm_join(LORA_E5_JOIN_OTAA), 0, NULL);
	feed_line("+JOIN: Join failed");

	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].join_result.outcome, LORA_E5_JOIN_OUTCOME_FAILED, NULL);
}

ZTEST(lora_e5_modem_manager, test_join_abp_synthesizes_immediate_result)
{
	struct lora_e5_config cfg = make_abp_config(4);

	drive_configure_to_completion(&cfg);
	event_log_count = 0; /* isolate the join event from CONFIG's own */

	zassert_equal(lora_e5_mm_join(LORA_E5_JOIN_ABP), 0, NULL);

	zassert_equal(mock.send_count, CFG_STEP_COUNT_FOR_TEST,
		      "ABP join must issue NO AT command at all");
	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_JOIN_RESULT, NULL);
	zassert_equal(event_log[0].join_result.outcome, LORA_E5_JOIN_OUTCOME_ABP_SKIP, NULL);
	zassert_mem_equal(event_log[0].join_result.dev_addr.bytes,
			   cfg.abp.dev_addr.bytes, 4, NULL);
	zassert_equal(event_log[0].join_result.net_id, 0, NULL);
}

/* ------------------------------------------------------------------- */
/* SEND -- port-cache fix (VERIFICATION_NEEDED.md item 5)                */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_send_port_cache_behavior)
{
	static const uint8_t payload[] = { 0xAA, 0xBB, 0xCC };

	/* (1) First send ever -- no cached port yet, must reissue AT+PORT=
	 * before the actual MSGHEX.
	 */
	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 5, false), 0, NULL);
	zassert_equal(mock.send_count, 1, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+PORT=5", 9, NULL);

	feed_line("+PORT: OK");
	zassert_equal(mock.send_count, 2, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+MSGHEX=AABBCC", 16, NULL);

	feed_line("+MSGHEX: Done");
	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_TX_RESULT, NULL);
	zassert_equal(event_log[0].tx_result.fail_reason, LORA_E5_TX_FAIL_NONE, NULL);
	zassert_false(event_log[0].tx_result.confirmed, NULL);

	/* (2) Same port again -- must go straight to MSGHEX, no PORT
	 * reissue.
	 */
	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 5, false), 0, NULL);
	zassert_equal(mock.send_count, 3,
		      "same port as last time -- AT+PORT= must NOT be reissued");
	zassert_mem_equal(mock.last_cmd, "AT+MSGHEX=AABBCC", 16, NULL);
	feed_line("+MSGHEX: Done");

	/* (3) Different port -- must reissue AT+PORT= again before the
	 * next MSGHEX. This is the core of the VERIFICATION_NEEDED.md item
	 * 5 fix: without it, this send would have silently gone out on the
	 * stale port 5 instead of the requested port 7.
	 */
	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 7, false), 0, NULL);
	zassert_equal(mock.send_count, 4, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+PORT=7", 9, NULL);
	feed_line("+PORT: OK");
	zassert_equal(mock.send_count, 5, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+MSGHEX=AABBCC", 16, NULL);
	feed_line("+MSGHEX: Done");

	zassert_equal(event_log_count, 3, NULL);
}

ZTEST(lora_e5_modem_manager, test_send_port_reissue_failure_reports_tx_result)
{
	static const uint8_t payload[] = { 0x01 };

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 5, false), 0, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+PORT=5", 9, NULL);

	feed_line("ERROR(-1)");

	zassert_equal(event_log_count, 1,
		      "a failed AT+PORT= reissue must still surface a TX_RESULT, "
		      "not silently drop the send");
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_TX_RESULT, NULL);
	zassert_equal(event_log[0].tx_result.fail_reason, LORA_E5_TX_FAIL_MODEM_ERROR, NULL);
	zassert_equal(mock.send_count, 1, "must not have gone on to send MSGHEX at all");
}

/* ------------------------------------------------------------------- */
/* CMSG ack disambiguation                                               */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_send_cmsg_ack_disambiguation)
{
	static const uint8_t payload[] = { 0x01, 0x02 };

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 3, true), 0, NULL);
	feed_line("+PORT: OK");
	zassert_mem_equal(mock.last_cmd, "AT+CMSGHEX=0102", 15, NULL);

	feed_line("+CMSGHEX: ACK Received");
	feed_line("+CMSGHEX: Done");

	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].tx_result.fail_reason, LORA_E5_TX_FAIL_NONE, NULL);
	zassert_true(event_log[0].tx_result.confirmed, NULL);

	/* Second confirmed send, same port (cached, no reissue) -- no ACK
	 * Received URC this time.
	 */
	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 3, true), 0, NULL);
	feed_line("+CMSGHEX: Done");

	zassert_equal(event_log_count, 2, NULL);
	zassert_equal(event_log[1].tx_result.fail_reason, LORA_E5_TX_FAIL_NO_ACK, NULL);
}

/* ------------------------------------------------------------------- */
/* MSG error-reason mapping                                              */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_send_error_reason_mapping)
{
	static const uint8_t payload[] = { 0x01 };

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 2, false), 0, NULL);
	feed_line("+PORT: OK");
	feed_line("+MSGHEX: Please join network first");
	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].tx_result.fail_reason, LORA_E5_TX_FAIL_NOT_JOINED, NULL);

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 2, false), 0, NULL);
	feed_line("+MSGHEX: No free channel -1");
	zassert_equal(event_log_count, 2, NULL);
	zassert_equal(event_log[1].tx_result.fail_reason,
		      LORA_E5_TX_FAIL_NO_FREE_CHANNEL, NULL);

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 2, false), 0, NULL);
	feed_line("+MSGHEX: Length error 5");
	zassert_equal(event_log_count, 3, NULL);
	zassert_equal(event_log[2].tx_result.fail_reason,
		      LORA_E5_TX_FAIL_LENGTH_ERROR, NULL);
}

/* ------------------------------------------------------------------- */
/* Downlink URC parsing                                                  */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_downlink_urc_parsed_before_tx_result)
{
	static const uint8_t payload[] = { 0x01 };

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 2, false), 0, NULL);
	feed_line("+PORT: OK");

	feed_line("+MSGHEX: RXWIN1, RSSI -42, SNR 7");
	feed_line("+MSGHEX: PORT: 5; RX: \"48656C6C6F\"");

	zassert_equal(event_log_count, 1,
		      "downlink must be forwarded immediately as its own event");
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_DOWNLINK, NULL);
	zassert_equal(event_log[0].downlink.port, 5, NULL);
	zassert_equal(event_log[0].downlink.len, 5, NULL);
	zassert_mem_equal(event_log[0].downlink.data, "Hello", 5, NULL);
	zassert_equal(event_log[0].downlink.rssi_dbm, -42, NULL);
	zassert_equal(event_log[0].downlink.snr_db, 7, NULL);
	zassert_equal(event_log[0].downlink.window, LORA_E5_RX_WINDOW_RX1, NULL);

	feed_line("+MSGHEX: Done");

	zassert_equal(event_log_count, 2,
		      "TX_RESULT must arrive after the downlink, in wire order");
	zassert_equal(event_log[1].type, LORA_E5_FSM_EVT_TX_RESULT, NULL);
	zassert_equal(event_log[1].tx_result.fail_reason, LORA_E5_TX_FAIL_NONE, NULL);
}

/* ------------------------------------------------------------------- */
/* RESET / FACTORY_RESET -- interaction with the port cache              */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_reset_at_command_backend)
{
	zassert_equal(lora_e5_mm_reset(), 0, NULL);
	zassert_equal(mock.send_count, 1, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+RESET", 8, NULL);

	feed_line("+RESET: OK");

	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_RESET_RESULT, NULL);
	zassert_equal(event_log[0].reset_result, 0, NULL);
}

ZTEST(lora_e5_modem_manager, test_plain_reset_does_not_invalidate_port_cache)
{
	struct lora_e5_config cfg = make_otaa_config(9);
	static const uint8_t payload[] = { 0x01 };

	drive_configure_to_completion(&cfg);
	event_log_count = 0;

	zassert_equal(lora_e5_mm_reset(), 0, NULL);
	feed_line("+RESET: OK");

	int sends_before = mock.send_count;

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 9, false), 0, NULL);
	zassert_equal(mock.send_count, sends_before + 1,
		      "plain AT+RESET must not wipe the port cache -- config persists "
		      "in NVM across a reboot, only AT+FDEFAULT wipes it");
	zassert_mem_equal(mock.last_cmd, "AT+MSGHEX=", 10, NULL);
}

ZTEST(lora_e5_modem_manager, test_factory_reset_invalidates_port_cache)
{
	struct lora_e5_config cfg = make_otaa_config(9);
	static const uint8_t payload[] = { 0x01 };

	drive_configure_to_completion(&cfg);
	event_log_count = 0;

	zassert_equal(lora_e5_mm_factory_reset(), 0, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+FDEFAULT", 11, NULL);
	feed_line("+FDEFAULT: OK");

	zassert_equal(event_log_count, 1, NULL);
	zassert_equal(event_log[0].type, LORA_E5_FSM_EVT_RESET_RESULT, NULL);
	zassert_equal(event_log[0].reset_result, 0, NULL);

	int sends_before = mock.send_count;

	zassert_equal(lora_e5_mm_send(payload, sizeof(payload), 9, false), 0, NULL);
	zassert_equal(mock.send_count, sends_before + 1, NULL);
	zassert_mem_equal(mock.last_cmd, "AT+PORT=9", 9,
			   "AT+FDEFAULT wipes the modem's own port setting -- the cache "
			   "must have been invalidated so this reissues AT+PORT=");
}

/* ------------------------------------------------------------------- */
/* Explicitly-flagged -ENOTSUP contracts                                 */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_modem_manager, test_get_max_payload_returns_enotsup)
{
	size_t max_payload = 123;

	zassert_equal(lora_e5_mm_get_max_payload(&max_payload, K_MSEC(100)), -ENOTSUP, NULL);
	zassert_equal(max_payload, 0, NULL);
	zassert_equal(mock.send_count, 0,
		      "must not touch the transport -- AT+LW=LEN syntax is unconfirmed, "
		      "see VERIFICATION_NEEDED.md item 3");
}

ZTEST(lora_e5_modem_manager, test_get_version_returns_enotsup_after_successful_transaction)
{
	struct lora_e5_version ver = { .major = 9, .minor = 9, .patch = 9 };

	mock_script_lines[0] = "+VER: 1.2.3";
	mock_script_count = 1;

	int rc = lora_e5_mm_get_version(&ver, K_MSEC(500));

	zassert_equal(rc, -ENOTSUP,
		      "the AT transaction itself succeeds, but the value cannot be "
		      "extracted -- see the architecture-gap comment in "
		      "lora_e5_mm_get_version() and VERIFICATION_NEEDED.md");
	zassert_equal(ver.major, 0, NULL);
	zassert_equal(ver.minor, 0, NULL);
	zassert_equal(ver.patch, 0, NULL);
}

ZTEST(lora_e5_modem_manager, test_get_ids_returns_enotsup_after_successful_transaction)
{
	struct lora_e5_ids ids;

	memset(&ids, 0xAA, sizeof(ids));

	mock_script_lines[0] = "+ID: DevAddr, 32:30:84:63";
	mock_script_lines[1] = "+ID: DevEui, 2C:F7:F1:20:32:30:84:63";
	mock_script_lines[2] = "+ID: AppEui, 80:00:00:00:00:00:00:06";
	mock_script_count = 3;

	int rc = lora_e5_mm_get_ids(&ids, K_MSEC(500));

	zassert_equal(rc, -ENOTSUP, NULL);
	zassert_equal(ids.dev_addr.bytes[0], 0, NULL);
}

ZTEST(lora_e5_modem_manager, test_at_raw_returns_enotsup)
{
	char resp[16];

	zassert_equal(lora_e5_mm_at_raw("AT", 2, resp, sizeof(resp), K_MSEC(100)),
		      -ENOTSUP, NULL);
	zassert_equal(mock.send_count, 0,
		      "must not touch the transport -- see the match-mode-vocabulary "
		      "gap comment in lora_e5_mm_at_raw() and VERIFICATION_NEEDED.md");
}

ZTEST_SUITE(lora_e5_modem_manager, NULL, suite_setup, case_before, NULL, NULL);
