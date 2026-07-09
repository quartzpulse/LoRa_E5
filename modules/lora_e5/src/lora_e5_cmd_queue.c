/**
 * @file lora_e5_cmd_queue.c
 * @brief Core transaction engine. See lora_e5_cmd_queue.h for the
 * internal entry points and lora_e5_at.h for the public contract this
 * implements (via lora_e5_at.c's thin wrapper).
 *
 * Threading: all mutation of `active`/pend_msgq happens under `lock`.
 * Callback invocation (result_cb, urc_cb) ALWAYS happens outside the
 * lock -- holding the lock across a callback risks deadlock if the
 * callback re-enters via lora_e5_at_submit() (a realistic case: the
 * Modem Manager's CONFIG sequence submits the next sub-command from
 * inside the previous one's result callback).
 */

#include "lora_e5_cmd_queue.h"

#include <string.h>
#include <errno.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lora_e5_cmdq, CONFIG_LORA_E5_LOG_LEVEL);

/* Worst case: every currently-queued command plus the one active
 * command all resolve in a single cascade (e.g. transport vanishes
 * mid-burst). Bounds the stack-local array every entry point uses to
 * collect callback invocations for out-of-lock delivery.
 */
#define MAX_CASCADE (CONFIG_LORA_E5_CMD_QUEUE_DEPTH + 1)

struct queued_cmd {
	struct lora_e5_at_cmd_desc desc;
	lora_e5_at_result_cb_t result_cb;
};

struct active_cmd {
	struct lora_e5_at_cmd_desc desc;
	lora_e5_at_result_cb_t result_cb;
	uint8_t retry_count;
	uint8_t match_counts[LORA_E5_AT_MAX_TERMINAL_EVENTS];
	bool in_use;
};

struct cb_invocation {
	lora_e5_at_result_cb_t cb;
	struct lora_e5_at_result result;
};

K_MSGQ_DEFINE(pend_msgq, sizeof(struct queued_cmd),
	      CONFIG_LORA_E5_CMD_QUEUE_DEPTH, 4);

static struct active_cmd active;
static struct k_mutex lock;
static struct k_work_delayable timeout_work;
static struct k_work_q *rx_wq;
static lora_e5_at_send_fn_t transport_send;
static lora_e5_at_urc_cb_t urc_cb;
static bool initialized;

static void timeout_handler(struct k_work *work);

/* ------------------------------------------------------------------- */
/* Matching                                                              */
/* ------------------------------------------------------------------- */

static bool line_matches_entry(const struct lora_e5_at_line *line,
				const struct lora_e5_at_terminal_event *te)
{
	switch (te->match_mode) {
	case LORA_E5_AT_MATCH_BARE_OK:
		return line->kind == LORA_E5_AT_LINE_OK && line->prefix[0] == '\0';

	case LORA_E5_AT_MATCH_ANY_ERROR:
		return line->kind == LORA_E5_AT_LINE_ERROR;

	case LORA_E5_AT_MATCH_ANY_URC:
		/* Explicitly excludes ERROR-kind lines -- see the corrected
		 * doc comment in lora_e5_at.h. A line matches here if its
		 * prefix equals te->prefix AND its kind is OK or URC.
		 */
		if (line->kind == LORA_E5_AT_LINE_ERROR) {
			return false;
		}
		if (te->prefix == NULL) {
			return true;
		}
		return strcmp(line->prefix, te->prefix) == 0;

	case LORA_E5_AT_MATCH_EXACT:
		if (line->kind == LORA_E5_AT_LINE_ERROR) {
			return false;
		}
		if (te->prefix != NULL && strcmp(line->prefix, te->prefix) != 0) {
			return false;
		}
		if (te->remainder == NULL || line->remainder == NULL) {
			return false;
		}
		{
			size_t want_len = strlen(te->remainder);

			return line->remainder_len == want_len &&
			       memcmp(line->remainder, te->remainder, want_len) == 0;
		}

	case LORA_E5_AT_MATCH_STARTSWITH:
		if (line->kind == LORA_E5_AT_LINE_ERROR) {
			return false;
		}
		if (te->prefix != NULL && strcmp(line->prefix, te->prefix) != 0) {
			return false;
		}
		if (te->remainder == NULL || line->remainder == NULL) {
			return false;
		}
		{
			size_t want_len = strlen(te->remainder);

			return line->remainder_len >= want_len &&
			       memcmp(line->remainder, te->remainder, want_len) == 0;
		}
	}
	return false;
}

/* ------------------------------------------------------------------- */
/* Validation                                                             */
/* ------------------------------------------------------------------- */

static int validate_desc(const struct lora_e5_at_cmd_desc *desc)
{
	if (desc == NULL || desc->cmd == NULL || desc->cmd_len == 0) {
		return -EINVAL;
	}
	/* spec §2.1: command length never exceeds 528 characters. */
	if (desc->cmd_len > 528) {
		return -EINVAL;
	}
	if (desc->terminal_event_count > LORA_E5_AT_MAX_TERMINAL_EVENTS) {
		return -EINVAL;
	}
	if (desc->terminal_event_count > 0 && desc->terminal_events == NULL) {
		return -EINVAL;
	}
	if (desc->timeout_ms == 0) {
		/* Zero timeout is never a legitimate request -- every
		 * lora_e5_hf_build_*() function leaves this at 0
		 * deliberately as a "caller (Modem Manager) must set this"
		 * signal (see lora_e5_hf_commands.c comments). Catching it
		 * here turns a silent hang into an immediate, loud -EINVAL
		 * at submit time.
		 */
		return -EINVAL;
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* Dispatch / resolve (all require `lock` held on entry)                 */
/* ------------------------------------------------------------------- */

/**
 * Pop from pend_msgq and try to start it. On send failure, resolve
 * that one immediately (append to out[]) and try the next queued
 * command, repeating until either a send succeeds (timer armed) or
 * the queue is empty. Returns number of cascade entries appended.
 */
static size_t try_dispatch_and_cascade_locked(struct cb_invocation *out, size_t out_cap)
{
	size_t n = 0;
	struct queued_cmd next;

	while (k_msgq_get(&pend_msgq, &next, K_NO_WAIT) == 0) {
		active.desc = next.desc;
		active.result_cb = next.result_cb;
		active.retry_count = 0;
		memset(active.match_counts, 0, sizeof(active.match_counts));
		active.in_use = true;

		int rc = transport_send != NULL
			? transport_send(active.desc.cmd, active.desc.cmd_len)
			: -EIO;

		if (rc == 0) {
			/* k_work_reschedule_for_queue(), not
			 * k_work_schedule_for_queue() -- this path can run
			 * from *inside* timeout_handler() itself (a retry
			 * re-dispatching after its own timeout fired,
			 * cascading through resolve_current_locked() above).
			 * At that point timeout_work is still marked running
			 * by the kernel (its own handler hasn't returned
			 * yet), and k_work_schedule_for_queue() is
			 * documented as a no-op "if the work item is already
			 * scheduled or submitted" -- confirmed against real
			 * hardware: this silently dropped the retry's new
			 * timeout (schedule call returned 0, meaning nothing
			 * was armed), so a retry whose resend also went
			 * unanswered would then wait forever with no backup
			 * timeout. k_work_reschedule_for_queue() cancels and
			 * unconditionally re-arms even mid-run, which is
			 * exactly "restart my own timer" -- safe for both
			 * this self-rescheduling case and the normal
			 * fresh-dispatch-after-RX-match case.
			 */
			k_work_reschedule_for_queue(rx_wq, &timeout_work,
						     K_MSEC(active.desc.timeout_ms));
			return n;
		}

		LOG_WRN("transport_send failed (%d), resolving as UART_ERROR", rc);

		if (n < out_cap) {
			out[n].cb = active.result_cb;
			out[n].result = (struct lora_e5_at_result){
				.outcome = LORA_E5_AT_OUTCOME_UART_ERROR,
				.result_tag = 0,
				.error_code = 0,
				.user_data = active.desc.user_data,
			};
			n++;
		} else {
			LOG_ERR("cascade buffer exhausted, dropping a UART_ERROR "
				"result callback -- MAX_CASCADE sizing bug");
		}
		active.in_use = false;
	}
	return n;
}

/**
 * Resolve whatever is currently active, append its callback to out[],
 * mark it free, then advance the queue (which may itself cascade).
 * *n_inout is both the starting write offset and the running count --
 * caller initializes it to 0 before the first call in a given
 * entry-point invocation.
 */
static void resolve_current_locked(enum lora_e5_at_outcome outcome, int tag,
				    enum lora_e5_at_error err,
				    struct cb_invocation *out, size_t out_cap,
				    size_t *n_inout)
{
	k_work_cancel_delayable(&timeout_work);

	if (*n_inout < out_cap) {
		out[*n_inout].cb = active.result_cb;
		out[*n_inout].result = (struct lora_e5_at_result){
			.outcome = outcome,
			.result_tag = tag,
			.error_code = err,
			.user_data = active.desc.user_data,
		};
		(*n_inout)++;
	} else {
		LOG_ERR("cascade buffer exhausted, dropping a result callback");
	}
	active.in_use = false;

	*n_inout += try_dispatch_and_cascade_locked(out + *n_inout,
						     out_cap - *n_inout);
}

/* ------------------------------------------------------------------- */
/* Timeout handler                                                        */
/* ------------------------------------------------------------------- */

static void timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct cb_invocation cascade[MAX_CASCADE];
	size_t n = 0;

	k_mutex_lock(&lock, K_FOREVER);

	if (active.in_use) {
		if (active.retry_count < active.desc.max_retries) {
			active.retry_count++;
			memset(active.match_counts, 0, sizeof(active.match_counts));

			int rc = transport_send != NULL
				? transport_send(active.desc.cmd, active.desc.cmd_len)
				: -EIO;

			if (rc == 0) {
				/* k_work_reschedule_for_queue(), not
				 * k_work_schedule_for_queue() -- see the
				 * matching comment in
				 * try_dispatch_and_cascade_locked(). This
				 * call is *inside* timeout_work's own
				 * handler, so the kernel still has it marked
				 * running; k_work_schedule_for_queue() would
				 * silently no-op here (confirmed against real
				 * hardware), leaving this retry's resend
				 * with no timeout backing it at all.
				 */
				k_work_reschedule_for_queue(
					rx_wq, &timeout_work,
					K_MSEC(active.desc.timeout_ms));
			} else {
				resolve_current_locked(LORA_E5_AT_OUTCOME_UART_ERROR,
							0, 0, cascade, MAX_CASCADE, &n);
			}
		} else {
			resolve_current_locked(LORA_E5_AT_OUTCOME_TIMEOUT,
						0, 0, cascade, MAX_CASCADE, &n);
		}
	}
	/* else: timer fired for a transaction that already resolved via
	 * a race with process_line() -- k_work_cancel_delayable() inside
	 * resolve_current_locked() should prevent this in practice, but
	 * if it doesn't (cancellation racing an already-queued work item
	 * per Zephyr's own k_work_cancel_delayable() caveats), this is a
	 * safe no-op rather than resolving a stale/reused `active`.
	 */

	k_mutex_unlock(&lock);

	for (size_t i = 0; i < n; i++) {
		if (cascade[i].cb != NULL) {
			cascade[i].cb(&cascade[i].result);
		}
	}
}

/* ------------------------------------------------------------------- */
/* Public entry points                                                    */
/* ------------------------------------------------------------------- */

int lora_e5_cmd_queue_init(struct k_work_q *rx_work_q)
{
	if (rx_work_q == NULL) {
		return -EINVAL;
	}

	rx_wq = rx_work_q;
	k_mutex_init(&lock);
	k_work_init_delayable(&timeout_work, timeout_handler);
	memset(&active, 0, sizeof(active));
	transport_send = NULL;
	urc_cb = NULL;
	initialized = true;
	return 0;
}

void lora_e5_cmd_queue_set_transport(lora_e5_at_send_fn_t send_fn)
{
	k_mutex_lock(&lock, K_FOREVER);
	transport_send = send_fn;
	k_mutex_unlock(&lock);
}

int lora_e5_cmd_queue_set_urc_callback(lora_e5_at_urc_cb_t cb)
{
	if (cb == NULL) {
		return -EINVAL;
	}
	urc_cb = cb;
	return 0;
}

int lora_e5_cmd_queue_submit(const struct lora_e5_at_cmd_desc *desc,
			      lora_e5_at_result_cb_t result_cb)
{
	int rc;
	struct queued_cmd q;
	struct cb_invocation cascade[MAX_CASCADE];
	size_t n = 0;

	if (!initialized) {
		return -EINVAL;
	}

	rc = validate_desc(desc);
	if (rc != 0) {
		return rc;
	}

	q.desc = *desc;
	q.result_cb = result_cb;

	k_mutex_lock(&lock, K_FOREVER);

	if (k_msgq_put(&pend_msgq, &q, K_NO_WAIT) != 0) {
		k_mutex_unlock(&lock);
		return -ENOMEM;
	}

	if (!active.in_use) {
		n = try_dispatch_and_cascade_locked(cascade, MAX_CASCADE);
	}
	/* else: something is already in flight; the command we just
	 * enqueued will be picked up by resolve_current_locked() when
	 * the active one finishes -- nothing more to do here.
	 */

	k_mutex_unlock(&lock);

	for (size_t i = 0; i < n; i++) {
		if (cascade[i].cb != NULL) {
			cascade[i].cb(&cascade[i].result);
		}
	}
	return 0;
}

void lora_e5_cmd_queue_process_line(const struct lora_e5_at_line *line)
{
	struct cb_invocation cascade[MAX_CASCADE];
	size_t n = 0;
	bool matched = false;

	if (!initialized || line == NULL) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if (active.in_use) {
		size_t count = active.desc.terminal_event_count;

		if (count > LORA_E5_AT_MAX_TERMINAL_EVENTS) {
			count = LORA_E5_AT_MAX_TERMINAL_EVENTS; /* defensive;
					validate_desc() should have already
					rejected this at submit time. */
		}

		for (size_t i = 0; i < count; i++) {
			const struct lora_e5_at_terminal_event *te =
				&active.desc.terminal_events[i];

			if (!line_matches_entry(line, te)) {
				continue;
			}

			matched = true;
			active.match_counts[i]++;

			uint8_t required = te->required_matches != 0
				? te->required_matches : 1;

			if (active.match_counts[i] >= required) {
				resolve_current_locked(
					LORA_E5_AT_OUTCOME_MATCHED,
					te->result_tag,
					line->kind == LORA_E5_AT_LINE_ERROR
						? line->error_code : 0,
					cascade, MAX_CASCADE, &n);
			}
			/* else: partial match toward a multi-occurrence
			 * terminal condition -- consumed, transaction stays
			 * active, NOT forwarded to URC callback below.
			 */
			break; /* first matching entry wins */
		}
	}

	k_mutex_unlock(&lock);

	if (!matched && urc_cb != NULL) {
		urc_cb(line);
	}

	for (size_t i = 0; i < n; i++) {
		if (cascade[i].cb != NULL) {
			cascade[i].cb(&cascade[i].result);
		}
	}
}

void lora_e5_cmd_queue_test_reset(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	k_work_cancel_delayable(&timeout_work);
	k_msgq_purge(&pend_msgq);
	memset(&active, 0, sizeof(active));
	transport_send = NULL;
	urc_cb = NULL;
	k_mutex_unlock(&lock);
}

bool lora_e5_cmd_queue_test_is_active(void)
{
	bool result;

	k_mutex_lock(&lock, K_FOREVER);
	result = active.in_use;
	k_mutex_unlock(&lock);
	return result;
}

size_t lora_e5_cmd_queue_test_pending_count(void)
{
	return k_msgq_num_used_get(&pend_msgq);
}