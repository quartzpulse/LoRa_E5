/**
 * @file lora_e5_fsm.c
 * @brief The LoRaWAN modem lifecycle FSM -- the only module that knows
 * what "JOINING" or "confirmed uplink" means (Phase 1 §2.4). Registers
 * itself as the Modem Manager's sole event sink; every state
 * transition happens on this file's single dedicated work queue
 * (fsm_wq), so no mutex guards the state variable itself -- see
 * Phase 1 §4.
 *
 * Contains NO AT command strings or "+PREFIX:" literals anywhere
 * (CLAUDE.md non-negotiable decision #6) -- only calls into
 * lora_e5_modem_manager.h's abstract operations and reacts to the
 * lora_e5_fsm_event values it reports back.
 *
 * State ownership matches the finalized decisions this codebase has
 * already settled on:
 *   - Class A only, no Class B/C states (CLAUDE.md #1).
 *   - JOINED is the sole steady state; TX/sleep/reset/leave always
 *     return to JOINED, never READY (CLAUDE.md #2).
 *   - RX1/RX2/ACK are event metadata (lora_e5_downlink.window /
 *     lora_e5_fsm_tx_result), not FSM states -- single
 *     WAIT_TX_RESULT state (CLAUDE.md #3).
 *   - ABP vs OTAA is NOT branched on here -- lora_e5_mm_join()
 *     already resolves to a uniform JOIN_RESULT event regardless of
 *     activation method (CLAUDE.md #4).
 *
 * No dedicated FSM-level join/ack timeout is implemented (Phase 3
 * bringup plan's Phase 2 rationale): both AT+JOIN and AT+MSG/CMSG are
 * single AT-manager transactions end-to-end, already bounded by
 * CONFIG_LORA_E5_JOIN_TIMEOUT_MS / CONFIG_LORA_E5_TX_TIMEOUT_MS at
 * that layer. LORA_E5_FSM_EVT_JOIN_TIMEOUT is still handled below,
 * defensively, in case a future caller posts it.
 *
 * Recovery ladder (Phase 1 §8.2) simplification flagged here, not
 * silently omitted: this pass uses a fixed inter-pass retry delay
 * (RECOVERY_PASS_RETRY_DELAY_MS) rather than the full exponential
 * join-duty-cycle-aware backoff Phase 1 §8.3 discusses -- see
 * docs/VERIFICATION_NEEDED.md.
 *
 * LORA_E5_STATE_RESUMING (added post-Phase-3, 2026-07-11) is the one
 * deliberate exception to "JOINED is the only way in without an
 * explicit join() call" -- lora_e5_resume()/resume_sync() is an
 * explicitly opt-in alternative to lora_e5_start(), not new autonomous
 * FSM behavior: CLAUDE.md decision #2 ("v1 does NOT auto-join after
 * CONFIG") still holds for the ordinary start()/CONFIG path, which is
 * completely unchanged. See handle_resume_request()/
 * handle_probe_result()'s RESUMING branch and
 * docs/VERIFICATION_NEEDED.md's "Resolved 2026-07-11" section for why
 * this exists (a confirmed real-hardware ~60ms "+JOIN: Joined already"
 * fast path when the modem never lost power, vs. the ordinary ~8s
 * RESET+CONFIG+JOIN sequence).
 */

#include "lora_e5_internal.h"
#include "lora_e5_modem_manager.h"

#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lora_e5_fsm, CONFIG_LORA_E5_LOG_LEVEL);

/* Fixed inter-pass delay for the recovery ladder -- NOT a documented
 * Kconfig symbol (see file doc comment above and
 * docs/VERIFICATION_NEEDED.md): a deliberate v1 simplification, not a
 * value derived from any duty-cycle measurement. Plain local #define,
 * not CONFIG_-prefixed, so it can't be mistaken for a pending real
 * Kconfig symbol (same convention as lora_e5_modem_manager.c's
 * WAKEUP_SETTLE_MS).
 */
#define RECOVERY_PASS_RETRY_DELAY_MS 2000

/* ------------------------------------------------------------------- */
/* State                                                                 */
/* ------------------------------------------------------------------- */

enum recovery_step {
	RECOVERY_STEP_RESET = 0,
	RECOVERY_STEP_RECONFIGURE,
	RECOVERY_STEP_REJOIN,
};

struct fsm_ctx {
	enum lora_e5_state state;

	/* Provisioning config copy (Phase 1 §2.4 ownership) -- written once
	 * via lora_e5_fsm_stage_config() before START_REQUEST, reused by
	 * the recovery ladder's reconfigure step without depending on
	 * lora_e5.c or the Modem Manager retaining their own copy.
	 */
	struct lora_e5_config provision_cfg;
	bool provision_cfg_valid;

	/* Recovery ladder (Phase 1 §8.2). */
	enum recovery_step recovery_step;
	uint8_t recovery_pass_count;
	bool recovery_was_joined;   /* Gates the rejoin step. */
	bool recovery_tx_retried;   /* "resume pending TX once" guard. */
	bool reset_factory;         /* Consumed by do_recovery_reset(). */

	/* TX inbox -- guarded by g_stage_lock, written by
	 * lora_e5_fsm_stage_tx() from an arbitrary application thread,
	 * consumed exactly once by handle_tx_request() on fsm_wq.
	 */
	uint8_t stage_tx_buf[CONFIG_LORA_E5_TX_BUFFER_SIZE];
	size_t stage_tx_len;
	uint8_t stage_tx_port;
	bool stage_tx_confirmed;
	bool stage_tx_pending;

	/* Active TX -- fsm_wq-only from here on, valid while a TX is in
	 * flight (TX_PENDING/WAIT_TX_RESULT) or being resumed once after a
	 * recovery ladder pass succeeds.
	 */
	uint8_t active_tx_buf[CONFIG_LORA_E5_TX_BUFFER_SIZE];
	size_t active_tx_len;
	uint8_t active_tx_port;
	bool active_tx_confirmed;
	bool active_tx_valid;

	/* Sleep/reset side-channel staging -- guarded by g_stage_lock. */
	uint32_t stage_sleep_duration_ms;
	bool stage_reset_factory;
};

static struct fsm_ctx g_fsm;

K_MUTEX_DEFINE(g_stage_lock);
K_MUTEX_DEFINE(g_state_query_lock);

static struct k_work_q *g_fsm_wq;

K_MSGQ_DEFINE(g_fsm_msgq, sizeof(struct lora_e5_fsm_event),
	      CONFIG_LORA_E5_EVENT_QUEUE_DEPTH, 4);

static struct k_work g_fsm_drain_work;
static struct k_work_delayable g_boot_settle_work;
static struct k_work_delayable g_recovery_retry_work;

/* ------------------------------------------------------------------- */
/* Forward declarations (mutually-referencing recovery-ladder helpers)  */
/* ------------------------------------------------------------------- */

static void enter_recovery(void);
static void recovery_step_failed(void);
static void fail_recovery_pass(void);
static void advance_recovery_ladder(void);
static void finish_recovery(enum lora_e5_state target);

/* ------------------------------------------------------------------- */
/* Small helpers                                                        */
/* ------------------------------------------------------------------- */

static void set_state(enum lora_e5_state new_state)
{
	if (g_fsm.state == new_state) {
		return;
	}
	g_fsm.state = new_state;

	struct lora_e5_app_event evt = {
		.type = LORA_E5_APP_EVT_STATE_CHANGED,
		.state_changed = new_state,
	};

	lora_e5_notify_post(&evt);
}

static void emit_error(int err)
{
	struct lora_e5_app_event evt = {
		.type = LORA_E5_APP_EVT_ERROR,
		.error_errno = err,
	};

	lora_e5_notify_post(&evt);
}

/* ------------------------------------------------------------------- */
/* Recovery ladder (Phase 1 §8.2)                                       */
/* ------------------------------------------------------------------- */

static int do_recovery_reset(void)
{
	if (g_fsm.reset_factory) {
		g_fsm.reset_factory = false;
		return lora_e5_mm_factory_reset();
	}
	return lora_e5_mm_reset();
}

static void recovery_retry_expired(struct k_work *work)
{
	ARG_UNUSED(work);
	enter_recovery();
}

static void fail_recovery_pass(void)
{
	g_fsm.recovery_pass_count++;
	if (g_fsm.recovery_pass_count >= CONFIG_LORA_E5_MAX_RETRIES) {
		struct lora_e5_fsm_event evt = {
			.type = LORA_E5_FSM_EVT_RECOVERY_LADDER_EXHAUSTED,
		};

		lora_e5_fsm_post_event(&evt);
		return;
	}

	LOG_WRN("recovery pass %u failed, retrying in %d ms",
		g_fsm.recovery_pass_count, RECOVERY_PASS_RETRY_DELAY_MS);
	/* k_work_reschedule_for_queue(), not k_work_schedule_for_queue() --
	 * this can run from *inside* g_recovery_retry_work's own handler
	 * (recovery_retry_expired() -> enter_recovery() -> here, when
	 * do_recovery_reset() fails synchronously), where the kernel still
	 * has it marked running and a plain schedule call would silently
	 * no-op (see the matching, hardware-confirmed fix in
	 * lora_e5_cmd_queue.c's timeout_handler()/
	 * try_dispatch_and_cascade_locked()).
	 */
	k_work_reschedule_for_queue(g_fsm_wq, &g_recovery_retry_work,
				     K_MSEC(RECOVERY_PASS_RETRY_DELAY_MS));
}

/** @brief Enter (or restart the current pass of) the recovery ladder.
 *  Does NOT itself count a failed pass -- callers that already know a
 *  pass failed should call recovery_step_failed()/fail_recovery_pass()
 *  instead. */
static void enter_recovery(void)
{
	if (g_fsm.state == LORA_E5_STATE_ERROR) {
		return;
	}

	bool already_recovering = (g_fsm.state == LORA_E5_STATE_RECOVERING);

	if (!already_recovering) {
		g_fsm.recovery_was_joined =
			(g_fsm.state == LORA_E5_STATE_JOINED ||
			 g_fsm.state == LORA_E5_STATE_TX_PENDING ||
			 g_fsm.state == LORA_E5_STATE_WAIT_TX_RESULT ||
			 g_fsm.state == LORA_E5_STATE_SLEEP);
		g_fsm.recovery_tx_retried = false;
		g_fsm.recovery_pass_count = 0;
		set_state(LORA_E5_STATE_RECOVERING);
	}

	g_fsm.recovery_step = RECOVERY_STEP_RESET;

	if (do_recovery_reset() != 0) {
		fail_recovery_pass();
	}
}

/** @brief Route a failure to the recovery ladder, correctly counting
 *  it as a failed pass if a ladder is already in progress. Use this
 *  (not enter_recovery() directly) from every fault-triggered call
 *  site so CONFIG_LORA_E5_MAX_RETRIES bounds retries even when the
 *  failure recurs mid-ladder (e.g. reconfigure itself fails
 *  transiently). */
static void recovery_step_failed(void)
{
	if (g_fsm.state != LORA_E5_STATE_RECOVERING) {
		enter_recovery();
	} else {
		fail_recovery_pass();
	}
}

static void resume_pending_tx(void)
{
	g_fsm.recovery_tx_retried = true;
	set_state(LORA_E5_STATE_TX_PENDING);

	int rc = lora_e5_mm_send(g_fsm.active_tx_buf, g_fsm.active_tx_len,
				   g_fsm.active_tx_port, g_fsm.active_tx_confirmed);

	if (rc == 0) {
		set_state(LORA_E5_STATE_WAIT_TX_RESULT);
		return;
	}

	struct lora_e5_app_event evt = {
		.type = LORA_E5_APP_EVT_TX_FAILED,
		.tx = {
			.confirmed = g_fsm.active_tx_confirmed,
			.fail_reason = LORA_E5_TX_FAIL_MODEM_ERROR,
		},
	};

	lora_e5_notify_post(&evt);
	g_fsm.active_tx_valid = false;
	set_state(LORA_E5_STATE_JOINED);
}

static void finish_recovery(enum lora_e5_state target)
{
	set_state(target);

	/* "Resume operation: any pending TX_REQUEST re-attempted once, not
	 * silently retried forever" (Phase 1 §8.2). active_tx_valid stays
	 * true across a fault iff the fault interrupted an in-flight TX
	 * before its TX_RESULT ever arrived -- see handle_tx_result().
	 */
	if (target == LORA_E5_STATE_JOINED && g_fsm.active_tx_valid &&
	    !g_fsm.recovery_tx_retried) {
		resume_pending_tx();
	}
}

static void advance_recovery_ladder(void)
{
	switch (g_fsm.recovery_step) {
	case RECOVERY_STEP_RESET:
		g_fsm.recovery_step = RECOVERY_STEP_RECONFIGURE;
		if (!g_fsm.provision_cfg_valid) {
			/* Fault occurred before lora_e5_start() ever supplied
			 * provisioning -- the ladder cannot reconfigure with
			 * nothing to reconfigure from.
			 */
			set_state(LORA_E5_STATE_ERROR);
			emit_error(-EINVAL);
			return;
		}
		if (lora_e5_mm_configure(&g_fsm.provision_cfg) != 0) {
			fail_recovery_pass();
		}
		break;

	case RECOVERY_STEP_RECONFIGURE:
		if (g_fsm.recovery_was_joined && CONFIG_LORA_E5_AUTO_REJOIN) {
			g_fsm.recovery_step = RECOVERY_STEP_REJOIN;
			set_state(LORA_E5_STATE_JOINING);
			if (lora_e5_mm_join(g_fsm.provision_cfg.join_type) != 0) {
				fail_recovery_pass();
			}
		} else {
			finish_recovery(LORA_E5_STATE_READY);
		}
		break;

	case RECOVERY_STEP_REJOIN:
		finish_recovery(LORA_E5_STATE_JOINED);
		break;
	}
}

/* ------------------------------------------------------------------- */
/* Per-event handlers                                                    */
/* ------------------------------------------------------------------- */

static void handle_start_request(void)
{
	if (g_fsm.state != LORA_E5_STATE_OFF) {
		return; /* Already started/starting -- idempotent no-op. */
	}
	if (!g_fsm.provision_cfg_valid) {
		emit_error(-EINVAL);
		return;
	}

	set_state(LORA_E5_STATE_RESET);
	if (lora_e5_mm_reset() != 0) {
		recovery_step_failed();
	}
}

/** @brief lora_e5_resume()/resume_sync() entry point -- probes WITHOUT
 *  a prior AT+RESET, on the assumption the modem never lost power and
 *  may still hold a joined session (confirmed real-hardware behavior,
 *  see docs/VERIFICATION_NEEDED.md's "Resolved 2026-07-11" section: a
 *  bare AT+JOIN issued with no reset first returns "+JOIN: Joined
 *  already" in ~60ms when the assumption holds). Falls back to the
 *  ordinary full RESET+CONFIG path (via recovery_step_failed(), same
 *  as any other fault -- deliberately NOT a special-cased fallback)
 *  if the probe or the immediate join attempt fails for any reason,
 *  including a genuine power loss this was betting against -- see
 *  handle_probe_result()'s RESUMING branch below. */
static void handle_resume_request(void)
{
	if (g_fsm.state != LORA_E5_STATE_OFF) {
		return; /* Already started/starting -- idempotent no-op. */
	}
	if (!g_fsm.provision_cfg_valid) {
		emit_error(-EINVAL);
		return;
	}

	set_state(LORA_E5_STATE_RESUMING);
	if (lora_e5_mm_probe() != 0) {
		recovery_step_failed();
	}
}

static void boot_settle_expired(struct k_work *work)
{
	ARG_UNUSED(work);

	struct lora_e5_fsm_event evt = {
		.type = LORA_E5_FSM_EVT_BOOT_SETTLE_EXPIRED,
	};

	lora_e5_fsm_post_event(&evt);
}

static void handle_boot_settle_expired(void)
{
	if (g_fsm.state != LORA_E5_STATE_BOOT) {
		return;
	}

	set_state(LORA_E5_STATE_CHECK_AT);
	if (lora_e5_mm_probe() != 0) {
		set_state(LORA_E5_STATE_ERROR);
		emit_error(-EIO);
	}
}

static void handle_probe_result(const struct lora_e5_fsm_event *evt)
{
	if (g_fsm.state == LORA_E5_STATE_RESUMING) {
		if (evt->reset_result != 0) {
			/* Unlike CHECK_AT's probe failure below, this is NOT
			 * treated as a hard hardware fault straight to ERROR
			 * -- RESUMING never issued a real AT+RESET, so a
			 * failed probe here is much more likely to mean "the
			 * modem actually lost power after all" or "its AT
			 * parser desynced from MCU boot-time UART noise"
			 * (see docs/VERIFICATION_NEEDED.md) than a genuine
			 * wiring fault -- both recoverable by the ordinary
			 * full RESET path, which is exactly what
			 * recovery_step_failed() does.
			 */
			recovery_step_failed();
			return;
		}

		set_state(LORA_E5_STATE_JOINING);
		if (lora_e5_mm_join(g_fsm.provision_cfg.join_type) != 0) {
			recovery_step_failed();
		}
		return;
	}

	if (g_fsm.state != LORA_E5_STATE_CHECK_AT) {
		return;
	}

	if (evt->reset_result != 0) {
		/* lora_e5_mm_probe() already exhausted its own bounded
		 * retries (Phase 1 §5.2: "CHECK_AT | TIMEOUT after
		 * MAX_RETRIES | ERROR -- modem not responding at all,
		 * likely wiring/power fault, not worth auto-recovering
		 * indefinitely").
		 */
		set_state(LORA_E5_STATE_ERROR);
		emit_error(evt->reset_result);
		return;
	}

	set_state(LORA_E5_STATE_CONFIG);
	if (!g_fsm.provision_cfg_valid || lora_e5_mm_configure(&g_fsm.provision_cfg) != 0) {
		set_state(LORA_E5_STATE_ERROR);
		emit_error(-EINVAL);
	}
}

static void handle_config_step_result(const struct lora_e5_fsm_event *evt)
{
	const struct lora_e5_fsm_config_step_result *r = &evt->config_step_result;

	if (r->error != 0) {
		if (lora_e5_at_error_is_structural(r->error)) {
			set_state(LORA_E5_STATE_ERROR);
			emit_error(-EINVAL);
		} else {
			recovery_step_failed();
		}
		return;
	}

	if (!r->is_last_step) {
		return; /* Modem Manager already submitted the next step. */
	}

	if (g_fsm.state == LORA_E5_STATE_RECOVERING) {
		advance_recovery_ladder();
	} else {
		set_state(LORA_E5_STATE_READY);
	}
}

static void handle_join_request(void)
{
	if (g_fsm.state != LORA_E5_STATE_READY) {
		return;
	}

	set_state(LORA_E5_STATE_JOINING);
	if (lora_e5_mm_join(g_fsm.provision_cfg.join_type) != 0) {
		set_state(LORA_E5_STATE_ERROR);
		emit_error(-EIO);
	}
}

static void handle_join_result(const struct lora_e5_fsm_event *evt)
{
	const struct lora_e5_fsm_join_result *r = &evt->join_result;
	bool success = (r->outcome == LORA_E5_JOIN_OUTCOME_SUCCESS ||
			r->outcome == LORA_E5_JOIN_OUTCOME_ABP_SKIP);

	if (g_fsm.state == LORA_E5_STATE_RECOVERING) {
		if (success) {
			struct lora_e5_app_event app_evt = {
				.type = LORA_E5_APP_EVT_JOIN_SUCCESS,
				.join = *r,
			};

			lora_e5_notify_post(&app_evt);
			finish_recovery(LORA_E5_STATE_JOINED);
		} else {
			fail_recovery_pass();
		}
		return;
	}

	if (g_fsm.state != LORA_E5_STATE_JOINING) {
		return; /* Stray/unexpected -- ignore defensively. */
	}

	if (success) {
		struct lora_e5_app_event app_evt = {
			.type = LORA_E5_APP_EVT_JOIN_SUCCESS,
			.join = *r,
		};

		lora_e5_notify_post(&app_evt);
		set_state(LORA_E5_STATE_JOINED);
	} else {
		struct lora_e5_app_event app_evt = {
			.type = LORA_E5_APP_EVT_JOIN_FAILED,
			.join = *r,
		};

		lora_e5_notify_post(&app_evt);
		recovery_step_failed();
	}
}

static void handle_join_timeout(void)
{
	/* Defensive only -- see file doc comment: no FSM-level join timer
	 * is armed in this pass, so this event is not expected to fire in
	 * practice.
	 */
	if (g_fsm.state != LORA_E5_STATE_JOINING) {
		return;
	}

	struct lora_e5_app_event app_evt = {
		.type = LORA_E5_APP_EVT_JOIN_FAILED,
		.join = { .outcome = LORA_E5_JOIN_OUTCOME_TIMEOUT },
	};

	lora_e5_notify_post(&app_evt);
	recovery_step_failed();
}

static void handle_tx_request(void)
{
	if (g_fsm.state != LORA_E5_STATE_JOINED) {
		return;
	}

	k_mutex_lock(&g_stage_lock, K_FOREVER);
	if (!g_fsm.stage_tx_pending) {
		k_mutex_unlock(&g_stage_lock);
		return; /* Nothing staged -- lora_e5.c should not post this. */
	}
	memcpy(g_fsm.active_tx_buf, g_fsm.stage_tx_buf, g_fsm.stage_tx_len);
	g_fsm.active_tx_len = g_fsm.stage_tx_len;
	g_fsm.active_tx_port = g_fsm.stage_tx_port;
	g_fsm.active_tx_confirmed = g_fsm.stage_tx_confirmed;
	g_fsm.stage_tx_pending = false;
	k_mutex_unlock(&g_stage_lock);

	g_fsm.active_tx_valid = true;
	set_state(LORA_E5_STATE_TX_PENDING);

	int rc = lora_e5_mm_send(g_fsm.active_tx_buf, g_fsm.active_tx_len,
				   g_fsm.active_tx_port, g_fsm.active_tx_confirmed);

	if (rc == 0) {
		set_state(LORA_E5_STATE_WAIT_TX_RESULT);
		return;
	}

	struct lora_e5_app_event evt = {
		.type = LORA_E5_APP_EVT_TX_FAILED,
		.tx = {
			.confirmed = g_fsm.active_tx_confirmed,
			.fail_reason = LORA_E5_TX_FAIL_MODEM_ERROR,
		},
	};

	lora_e5_notify_post(&evt);
	g_fsm.active_tx_valid = false;
	set_state(LORA_E5_STATE_JOINED);
}

static void handle_tx_result(const struct lora_e5_fsm_event *evt)
{
	const struct lora_e5_fsm_tx_result *r = &evt->tx_result;

	g_fsm.active_tx_valid = false;

	if (r->fail_reason == LORA_E5_TX_FAIL_NONE) {
		struct lora_e5_app_event app_evt = {
			.type = LORA_E5_APP_EVT_TX_SUCCESS,
			.tx = *r,
		};

		lora_e5_notify_post(&app_evt);
		set_state(LORA_E5_STATE_JOINED);
		return;
	}

	struct lora_e5_app_event app_evt = {
		.type = LORA_E5_APP_EVT_TX_FAILED,
		.tx = *r,
	};

	lora_e5_notify_post(&app_evt);

	if (r->fail_reason == LORA_E5_TX_FAIL_NOT_JOINED) {
		/* Modem-reported "please join first" while the FSM believes
		 * it's joined -- FSM/modem state desync, corrective rejoin
		 * recommended (Phase 1 §5.2).
		 */
		recovery_step_failed();
	} else {
		set_state(LORA_E5_STATE_JOINED);
	}
}

static void handle_downlink(const struct lora_e5_fsm_event *evt)
{
	/* Forwarded regardless of current state -- a downlink can arrive
	 * piggybacked on an unconfirmed uplink's RX window (Phase 1 §6).
	 */
	struct lora_e5_app_event app_evt = {
		.type = LORA_E5_APP_EVT_DOWNLINK_RECEIVED,
		.downlink = evt->downlink,
	};

	lora_e5_notify_post(&app_evt);
}

static void handle_sleep_request(void)
{
	if (g_fsm.state != LORA_E5_STATE_JOINED) {
		return;
	}

	uint32_t duration_ms;

	k_mutex_lock(&g_stage_lock, K_FOREVER);
	duration_ms = g_fsm.stage_sleep_duration_ms;
	k_mutex_unlock(&g_stage_lock);

	set_state(LORA_E5_STATE_SLEEP);
	if (lora_e5_mm_sleep(duration_ms) != 0) {
		/* No dedicated app event for a rejected sleep request exists
		 * in the current event vocabulary (same gap already flagged
		 * in lora_e5_modem_manager.c's mm_sleep_result_cb()) -- fall
		 * back to JOINED rather than getting stuck in SLEEP.
		 */
		set_state(LORA_E5_STATE_JOINED);
		return;
	}

	struct lora_e5_app_event evt = { .type = LORA_E5_APP_EVT_SLEEP_ENTERED };

	lora_e5_notify_post(&evt);
}

static void handle_wake_request(void)
{
	if (g_fsm.state != LORA_E5_STATE_SLEEP) {
		return;
	}

	if (lora_e5_mm_wakeup() != 0) {
		emit_error(-EIO);
	}
	/* State transition happens on WAKE_RESULT -- wakeup is a two-step
	 * async operation (probe + settle delay) at the Modem Manager
	 * layer, not resolved synchronously here.
	 */
}

static void handle_wake_result(void)
{
	if (g_fsm.state != LORA_E5_STATE_SLEEP) {
		return;
	}

	set_state(LORA_E5_STATE_JOINED);

	struct lora_e5_app_event evt = { .type = LORA_E5_APP_EVT_WAKE_COMPLETE };

	lora_e5_notify_post(&evt);
}

static void handle_reset_request(void)
{
	if (g_fsm.state == LORA_E5_STATE_OFF) {
		return;
	}

	k_mutex_lock(&g_stage_lock, K_FOREVER);
	g_fsm.reset_factory = g_fsm.stage_reset_factory;
	k_mutex_unlock(&g_stage_lock);

	/* Explicit application-triggered reset always routes through
	 * RECOVERING so configuration gets reapplied afterward (lora_e5.h
	 * lora_e5_reset()/lora_e5_factory_reset() doc comments) -- never a
	 * raw reset-and-hope.
	 */
	enter_recovery();
}

static void handle_reset_result(const struct lora_e5_fsm_event *evt)
{
	bool ok = (evt->reset_result == 0);

	if (g_fsm.state == LORA_E5_STATE_RESET) {
		if (ok) {
			set_state(LORA_E5_STATE_BOOT);
			k_work_schedule_for_queue(g_fsm_wq, &g_boot_settle_work,
						   K_MSEC(CONFIG_LORA_E5_BOOT_SETTLE_MS));
		} else {
			recovery_step_failed();
		}
		return;
	}

	if (g_fsm.state == LORA_E5_STATE_RECOVERING &&
	    g_fsm.recovery_step == RECOVERY_STEP_RESET) {
		if (ok) {
			advance_recovery_ladder();
		} else {
			fail_recovery_pass();
		}
	}
	/* else: stray RESET_RESULT with no matching in-flight reset --
	 * ignore defensively.
	 */
}

static void handle_uart_fault(void)
{
	if (g_fsm.state == LORA_E5_STATE_OFF || g_fsm.state == LORA_E5_STATE_ERROR) {
		return;
	}
	recovery_step_failed();
}

static void handle_recovery_ladder_exhausted(void)
{
	if (g_fsm.state != LORA_E5_STATE_RECOVERING) {
		return;
	}
	LOG_ERR("recovery ladder exhausted after %u passes -- staying in ERROR "
		"until an explicit lora_e5_reset()", g_fsm.recovery_pass_count);
	set_state(LORA_E5_STATE_ERROR);
	emit_error(-EIO);
}

/* ------------------------------------------------------------------- */
/* Dispatch                                                              */
/* ------------------------------------------------------------------- */

static void process_event(const struct lora_e5_fsm_event *evt)
{
	switch (evt->type) {
	case LORA_E5_FSM_EVT_START_REQUEST:
		handle_start_request();
		break;
	case LORA_E5_FSM_EVT_RESUME_REQUEST:
		handle_resume_request();
		break;
	case LORA_E5_FSM_EVT_BOOT_SETTLE_EXPIRED:
		handle_boot_settle_expired();
		break;
	case LORA_E5_FSM_EVT_PROBE_RESULT:
		handle_probe_result(evt);
		break;
	case LORA_E5_FSM_EVT_CONFIG_STEP_RESULT:
		handle_config_step_result(evt);
		break;
	case LORA_E5_FSM_EVT_JOIN_REQUEST:
		handle_join_request();
		break;
	case LORA_E5_FSM_EVT_JOIN_RESULT:
		handle_join_result(evt);
		break;
	case LORA_E5_FSM_EVT_JOIN_TIMEOUT:
		handle_join_timeout();
		break;
	case LORA_E5_FSM_EVT_TX_REQUEST:
		handle_tx_request();
		break;
	case LORA_E5_FSM_EVT_TX_RESULT:
		handle_tx_result(evt);
		break;
	case LORA_E5_FSM_EVT_DOWNLINK:
		handle_downlink(evt);
		break;
	case LORA_E5_FSM_EVT_SLEEP_REQUEST:
		handle_sleep_request();
		break;
	case LORA_E5_FSM_EVT_WAKE_REQUEST:
		handle_wake_request();
		break;
	case LORA_E5_FSM_EVT_WAKE_RESULT:
		handle_wake_result();
		break;
	case LORA_E5_FSM_EVT_RESET_REQUEST:
		handle_reset_request();
		break;
	case LORA_E5_FSM_EVT_RESET_RESULT:
		handle_reset_result(evt);
		break;
	case LORA_E5_FSM_EVT_UART_FAULT:
		handle_uart_fault();
		break;
	case LORA_E5_FSM_EVT_RECOVERY_LADDER_EXHAUSTED:
		handle_recovery_ladder_exhausted();
		break;
	}
}

static void fsm_drain_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct lora_e5_fsm_event evt;

	while (k_msgq_get(&g_fsm_msgq, &evt, K_NO_WAIT) == 0) {
		process_event(&evt);
	}
}

static void mm_event_trampoline(const struct lora_e5_fsm_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	lora_e5_fsm_post_event(event);
}

/* ------------------------------------------------------------------- */
/* Diagnostic state read (Phase 1 §4: round-trip, not a raw peek)       */
/* ------------------------------------------------------------------- */

struct state_query {
	struct k_work work;
	struct k_sem sem;
	enum lora_e5_state result;
};

static void state_query_work_handler(struct k_work *work)
{
	struct state_query *q = CONTAINER_OF(work, struct state_query, work);

	q->result = g_fsm.state;
	k_sem_give(&q->sem);
}

enum lora_e5_state lora_e5_fsm_get_state_sync(void)
{
	static struct state_query q;
	enum lora_e5_state result;

	k_mutex_lock(&g_state_query_lock, K_FOREVER);

	k_sem_init(&q.sem, 0, 1);
	k_work_init(&q.work, state_query_work_handler);
	k_work_submit_to_queue(g_fsm_wq, &q.work);
	k_sem_take(&q.sem, K_FOREVER);
	result = q.result;

	k_mutex_unlock(&g_state_query_lock);
	return result;
}

/* ------------------------------------------------------------------- */
/* Local-only leave (see lora_e5_internal.h doc comment for the        */
/* CLAUDE.md-vs-lora_e5.h conflict this resolves)                       */
/* ------------------------------------------------------------------- */

struct leave_query {
	struct k_work work;
	struct k_sem sem;
	int result;
};

K_MUTEX_DEFINE(g_leave_lock);

static void leave_work_handler(struct k_work *work)
{
	struct leave_query *q = CONTAINER_OF(work, struct leave_query, work);

	if (g_fsm.state == LORA_E5_STATE_JOINING || g_fsm.state == LORA_E5_STATE_JOINED) {
		g_fsm.active_tx_valid = false;
		set_state(LORA_E5_STATE_READY);
		q->result = 0;
	} else {
		q->result = -EINVAL;
	}
	k_sem_give(&q->sem);
}

int lora_e5_fsm_leave(void)
{
	static struct leave_query q;
	int result;

	k_mutex_lock(&g_leave_lock, K_FOREVER);
	k_sem_init(&q.sem, 0, 1);
	k_work_init(&q.work, leave_work_handler);
	k_work_submit_to_queue(g_fsm_wq, &q.work);
	k_sem_take(&q.sem, K_FOREVER);
	result = q.result;
	k_mutex_unlock(&g_leave_lock);
	return result;
}

/* ------------------------------------------------------------------- */
/* Public (internal-library-scope) entry points                         */
/* ------------------------------------------------------------------- */

int lora_e5_fsm_post_event(const struct lora_e5_fsm_event *event)
{
	if (event == NULL || g_fsm_wq == NULL) {
		return -EINVAL;
	}

	if (k_msgq_put(&g_fsm_msgq, event, K_NO_WAIT) != 0) {
		LOG_ERR("FSM event queue full (depth %d), dropping event type %d",
			CONFIG_LORA_E5_EVENT_QUEUE_DEPTH, (int)event->type);
		return -ENOMEM;
	}

	k_work_submit_to_queue(g_fsm_wq, &g_fsm_drain_work);
	return 0;
}

int lora_e5_fsm_stage_tx(const uint8_t *data, size_t len, uint8_t port, bool confirmed)
{
	int rc = 0;

	if (data == NULL || len == 0 || len > sizeof(g_fsm.stage_tx_buf)) {
		return -EINVAL;
	}

	k_mutex_lock(&g_stage_lock, K_FOREVER);
	if (g_fsm.stage_tx_pending) {
		rc = -EBUSY;
	} else {
		memcpy(g_fsm.stage_tx_buf, data, len);
		g_fsm.stage_tx_len = len;
		g_fsm.stage_tx_port = port;
		g_fsm.stage_tx_confirmed = confirmed;
		g_fsm.stage_tx_pending = true;
	}
	k_mutex_unlock(&g_stage_lock);
	return rc;
}

int lora_e5_fsm_stage_sleep(uint32_t duration_ms)
{
	k_mutex_lock(&g_stage_lock, K_FOREVER);
	g_fsm.stage_sleep_duration_ms = duration_ms;
	k_mutex_unlock(&g_stage_lock);
	return 0;
}

int lora_e5_fsm_stage_reset(bool factory)
{
	k_mutex_lock(&g_stage_lock, K_FOREVER);
	g_fsm.stage_reset_factory = factory;
	k_mutex_unlock(&g_stage_lock);
	return 0;
}

int lora_e5_fsm_stage_config(const struct lora_e5_config *cfg)
{
	if (cfg == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&g_stage_lock, K_FOREVER);
	g_fsm.provision_cfg = *cfg;
	g_fsm.provision_cfg_valid = true;
	k_mutex_unlock(&g_stage_lock);
	return 0;
}

int lora_e5_fsm_init(struct k_work_q *fsm_wq)
{
	if (fsm_wq == NULL) {
		return -EINVAL;
	}

	g_fsm_wq = fsm_wq;
	k_work_init(&g_fsm_drain_work, fsm_drain_work_handler);

	/* Cancel before re-initializing -- a repeated lora_e5_fsm_init()
	 * call (as tests do, between cases) must not run
	 * k_work_init_delayable() on a work item a previous call left
	 * scheduled (e.g. the recovery ladder's inter-pass retry timer);
	 * that corrupts the kernel's internal timeout list rather than
	 * cleanly replacing it.
	 */
	k_work_cancel_delayable(&g_boot_settle_work);
	k_work_cancel_delayable(&g_recovery_retry_work);
	k_work_init_delayable(&g_boot_settle_work, boot_settle_expired);
	k_work_init_delayable(&g_recovery_retry_work, recovery_retry_expired);
	k_msgq_purge(&g_fsm_msgq);
	memset(&g_fsm, 0, sizeof(g_fsm));
	g_fsm.state = LORA_E5_STATE_OFF;

	/* Modem Manager must already be initialized (lora_e5_mm_init()
	 * returned) before this call -- see lora_e5_internal.h doc comment.
	 */
	return lora_e5_mm_set_event_callback(mm_event_trampoline, NULL);
}

void lora_e5_fsm_test_reset(void)
{
	k_msgq_purge(&g_fsm_msgq);
	k_work_cancel_delayable(&g_boot_settle_work);
	k_work_cancel_delayable(&g_recovery_retry_work);
	memset(&g_fsm, 0, sizeof(g_fsm));
	g_fsm.state = LORA_E5_STATE_OFF;
}
