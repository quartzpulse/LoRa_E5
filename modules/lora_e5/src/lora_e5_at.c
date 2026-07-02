/**
 * @file lora_e5_at.c
 * @brief Thin public-API wrapper over lora_e5_cmd_queue.c's engine.
 * lora_e5_at_parse_line() lives in lora_e5_parser.c, not here.
 */

#include "lora_e5/lora_e5_at.h"
#include "lora_e5_cmd_queue.h"

#include <errno.h>
#include <zephyr/logging/log.h>

#ifndef CONFIG_LORA_E5_LOG_LEVEL
/* Same placeholder as lora_e5_cmd_queue.c -- remove once Kconfig
 * exists and defines this for real. Both files need their own
 * fallback since each is compiled as an independent translation
 * unit; a #define in one .c file is not visible in another.
 */
#define CONFIG_LORA_E5_LOG_LEVEL LOG_LEVEL_INF
#endif

LOG_MODULE_DECLARE(lora_e5_cmdq, CONFIG_LORA_E5_LOG_LEVEL);

int lora_e5_at_init(struct k_work_q *rx_work_q)
{
	return lora_e5_cmd_queue_init(rx_work_q);
}

void lora_e5_at_set_transport(lora_e5_at_send_fn_t send_fn)
{
	lora_e5_cmd_queue_set_transport(send_fn);
}

int lora_e5_at_set_urc_callback(lora_e5_at_urc_cb_t cb)
{
	return lora_e5_cmd_queue_set_urc_callback(cb);
}

void lora_e5_at_process_line(const struct lora_e5_at_line *line)
{
	lora_e5_cmd_queue_process_line(line);
}

int lora_e5_at_submit(const struct lora_e5_at_cmd_desc *desc,
		       lora_e5_at_result_cb_t result_cb)
{
	return lora_e5_cmd_queue_submit(desc, result_cb);
}

/* ------------------------------------------------------------------- */
/* Synchronous wrapper                                                    */
/* ------------------------------------------------------------------- */

/*
 * DESIGN NOTE -- read before changing this function.
 *
 * A stack-allocated context handed to the async result callback is a
 * use-after-return hazard: if the caller-supplied `timeout` fires
 * before the underlying transaction actually resolves (e.g. the
 * caller passed a timeout shorter than desc->timeout_ms *
 * (max_retries + 1), violating this function's own documented
 * contract), lora_e5_at_submit()'s eventual callback would write into
 * a stack frame that has already been torn down.
 *
 * Fix chosen for v1: ONE static context, guarded by g_sync_lock,
 * which is held for the ENTIRE call -- meaning only one
 * lora_e5_at_submit_sync() call is in flight across the whole
 * application at any time. This is a real, deliberate constraint, not
 * an oversight: it trades concurrency (multiple threads calling
 * *_sync() APIs simultaneously will serialize, not run in parallel)
 * for memory safety without dynamic allocation. Given the public API
 * documents _sync variants as existing for occasional
 * queries/blocking calls rather than a hot path (Phase 2 §7), this
 * trade is reasonable for v1 -- revisit with a small static context
 * pool (bounded by CONFIG_LORA_E5_CMD_QUEUE_DEPTH) if concurrent sync
 * callers become a real requirement.
 *
 * On outer-timeout misuse (caller's `timeout` elapses before the
 * transaction resolves): this function does NOT return early with a
 * still-pending transaction outstanding against the static context --
 * doing so would either (a) free the context while a callback is
 * still going to fire into it, or (b) require releasing g_sync_lock
 * while a foreign submit_sync() call's data is still in flight,
 * corrupting the NEXT caller's result. Instead, on outer-timeout it
 * logs an error (this is a caller contract violation, not expected
 * behavior) and blocks with K_FOREVER for the actual resolution
 * before returning -ETIMEDOUT. The AT Command Manager's own internal
 * timeout/retry bound (desc->timeout_ms * (max_retries + 1)) is what
 * actually bounds this wait in practice -- if a caller's `timeout`
 * argument is correct per this function's documented contract, this
 * fallback path is never exercised.
 */

static struct k_mutex g_sync_lock;
static bool g_sync_lock_initialized;

struct sync_ctx {
	struct k_sem sem;
	struct lora_e5_at_result result;
};

static void sync_result_cb(const struct lora_e5_at_result *result)
{
	struct sync_ctx *ctx = (struct sync_ctx *)result->user_data;

	ctx->result = *result;
	k_sem_give(&ctx->sem);
}

int lora_e5_at_submit_sync(const struct lora_e5_at_cmd_desc *desc,
			    k_timeout_t timeout,
			    struct lora_e5_at_result *result)
{
	static struct sync_ctx ctx; /* protected by g_sync_lock */
	struct lora_e5_at_cmd_desc desc_copy;
	int rc;

	if (desc == NULL || result == NULL) {
		return -EINVAL;
	}

	if (!g_sync_lock_initialized) {
		/* Lazy init is safe here ONLY because Zephyr's k_mutex_init
		 * is idempotent-safe against a benign race on first use in
		 * practice is NOT guaranteed -- this is a known gap, not a
		 * confident claim. TODO: move this init into
		 * lora_e5_at_init() instead of lazy-initializing here, so
		 * there is a single well-defined init point with no
		 * first-call race at all. Left as a lazy init for now only
		 * because lora_e5_at_init()'s signature would need to grow
		 * to reach this file-local state, which is a small API
		 * surface question worth a deliberate decision rather than
		 * a quick patch.
		 */
		k_mutex_init(&g_sync_lock);
		g_sync_lock_initialized = true;
	}

	k_mutex_lock(&g_sync_lock, K_FOREVER);

	k_sem_init(&ctx.sem, 0, 1);

	desc_copy = *desc;
	desc_copy.user_data = &ctx; /* overrides caller's user_data --
				      * documented in lora_e5_at.h's
				      * lora_e5_at_submit_sync() doc comment;
				      * a caller relying on user_data being
				      * preserved through the sync path will
				      * not get their value back. */

	rc = lora_e5_cmd_queue_submit(&desc_copy, sync_result_cb);
	if (rc != 0) {
		k_mutex_unlock(&g_sync_lock);
		return rc;
	}

	if (k_sem_take(&ctx.sem, timeout) != 0) {
		LOG_ERR("submit_sync: caller timeout elapsed before AT "
			"transaction resolved -- this indicates `timeout` "
			"was set shorter than the transaction's own "
			"timeout_ms*(max_retries+1) bound, which violates "
			"this function's documented contract. Blocking for "
			"actual resolution to avoid a use-after-return.");
		k_sem_take(&ctx.sem, K_FOREVER);
		*result = ctx.result;
		k_mutex_unlock(&g_sync_lock);
		return -ETIMEDOUT;
	}

	*result = ctx.result;
	k_mutex_unlock(&g_sync_lock);

	switch (ctx.result.outcome) {
	case LORA_E5_AT_OUTCOME_MATCHED:
		return 0;
	case LORA_E5_AT_OUTCOME_TIMEOUT:
		return -ETIMEDOUT;
	case LORA_E5_AT_OUTCOME_UART_ERROR:
	default:
		return -EIO;
	}
}