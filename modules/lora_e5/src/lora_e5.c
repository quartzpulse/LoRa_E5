/**
 * @file lora_e5.c
 * @brief Public API implementation. Thin translation layer (Phase 1
 * §2.7): every function here either builds/posts a matching
 * lora_e5_fsm_event, or (get_version/get_ids/get_max_payload) calls
 * straight into a synchronous Modem Manager query. Owns the three
 * dedicated work queues (rx/fsm/notify -- never k_sys_work_q, Phase 1
 * §4) and wires the layers together in lora_e5_init().
 *
 * `_sync` variants share one static waiter + one mutex serializing all
 * synchronous callers app-wide (same trade-off already accepted at the
 * AT layer, see lora_e5_at_submit_sync()'s design-note comment) --
 * signaled by an internal observer registered with lora_e5_events.c
 * that runs alongside (not instead of) the application's own
 * registered callback.
 *
 * Two documented gaps carried over unchanged from the header review,
 * not fixed here:
 *   - get_version()/get_ids()/get_max_payload() thin-wrap Modem
 *     Manager's -ENOTSUP (VERIFICATION_NEEDED.md item 10 -- no
 *     line-text-capture path exists yet).
 *   - lora_e5_config's port/adr/repeat/retry fields have no dedicated
 *     public setter in lora_e5.h; fixed v1 defaults are used (see
 *     g_pending_cfg's initializer below). Flagged, not silently
 *     invented as a new public API surface.
 */

#include "lora_e5/lora_e5.h"
#include "lora_e5/lora_e5_at.h"
#include "lora_e5_internal.h"
#include "lora_e5_modem_manager.h"

#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lora_e5, CONFIG_LORA_E5_LOG_LEVEL);

/* Not a documented Kconfig symbol -- work queue priority is an
 * implementation detail, not something the header's Kconfig list
 * names. Plain local #define, matching the non-CONFIG_-prefixed
 * convention used for other v1 simplifications in this codebase.
 */
#define LORA_E5_WQ_PRIORITY 5

/* ------------------------------------------------------------------- */
/* Work queues                                                          */
/* ------------------------------------------------------------------- */

static K_THREAD_STACK_DEFINE(g_rx_wq_stack, CONFIG_LORA_E5_RX_STACK_SIZE);
static K_THREAD_STACK_DEFINE(g_fsm_wq_stack, CONFIG_LORA_E5_FSM_STACK_SIZE);
static K_THREAD_STACK_DEFINE(g_notify_wq_stack, CONFIG_LORA_E5_NOTIFY_STACK_SIZE);

static struct k_work_q g_rx_wq;
static struct k_work_q g_fsm_wq;
static struct k_work_q g_notify_wq;

static bool g_initialized;

/* ------------------------------------------------------------------- */
/* Pending LoRaWAN provisioning config (Phase 1 §7: no I/O until        */
/* lora_e5_start())                                                     */
/* ------------------------------------------------------------------- */

/* Fixed v1 defaults for the fields lora_e5.h exposes no setter for --
 * see file doc comment. port=1/adr on/single-attempt repeat+retry are
 * reasonable, unremarkable defaults, not derived from any requirement.
 */
static struct lora_e5_config g_pending_cfg = {
	.dev_class = LORA_E5_CLASS_A,
	.adr_enable = true,
	.port = 1,
	.unconfirmed_repeats = 1,
	.confirmed_retries = 1,
};
static bool g_activation_set;
static bool g_region_set;

/* ------------------------------------------------------------------- */
/* Sync-wait mechanism                                                   */
/* ------------------------------------------------------------------- */

typedef bool (*sync_match_fn)(const struct lora_e5_app_event *event);
typedef int (*sync_trigger_fn)(void *arg);

struct sync_wait_ctx {
	struct k_sem sem;
	bool armed;
	sync_match_fn match;
	struct lora_e5_app_event result;
};

K_MUTEX_DEFINE(g_sync_lock);
static struct sync_wait_ctx g_sync_ctx; /* protected by g_sync_lock */

static void sync_observer_cb(const struct lora_e5_app_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!g_sync_ctx.armed || g_sync_ctx.match == NULL) {
		return;
	}
	if (g_sync_ctx.match(event)) {
		g_sync_ctx.result = *event;
		g_sync_ctx.armed = false;
		k_sem_give(&g_sync_ctx.sem);
	}
}

/**
 * One static waiter + one mutex serializing all synchronous callers
 * app-wide (same trade-off as lora_e5_at_submit_sync()) -- `trigger`
 * runs synchronously while still holding g_sync_lock and already
 * armed, so there is no window between arming and posting the
 * triggering FSM event in which a fast terminal event could be missed.
 */
static int sync_wait(sync_match_fn match, sync_trigger_fn trigger, void *trigger_arg,
		      k_timeout_t timeout, struct lora_e5_app_event *out)
{
	int rc;

	k_mutex_lock(&g_sync_lock, K_FOREVER);

	k_sem_init(&g_sync_ctx.sem, 0, 1);
	g_sync_ctx.match = match;
	g_sync_ctx.armed = true;

	rc = trigger(trigger_arg);
	if (rc != 0) {
		g_sync_ctx.armed = false;
		k_mutex_unlock(&g_sync_lock);
		return rc;
	}

	rc = k_sem_take(&g_sync_ctx.sem, timeout);
	g_sync_ctx.armed = false;

	if (rc == 0 && out != NULL) {
		*out = g_sync_ctx.result;
	}

	k_mutex_unlock(&g_sync_lock);
	return (rc == 0) ? 0 : -ETIMEDOUT;
}

static bool match_join(const struct lora_e5_app_event *e)
{
	return e->type == LORA_E5_APP_EVT_JOIN_SUCCESS || e->type == LORA_E5_APP_EVT_JOIN_FAILED;
}

static bool match_tx(const struct lora_e5_app_event *e)
{
	return e->type == LORA_E5_APP_EVT_TX_SUCCESS || e->type == LORA_E5_APP_EVT_TX_FAILED;
}

/** @brief Matches lora_e5_start()'s and lora_e5_reset()'s shared
 *  terminal shape: reaching READY/JOINED (STATE_CHANGED) or ERROR.
 *  Deliberately NOT "any STATE_CHANGED" -- boot passes through several
 *  intermediate states (RESET/BOOT/CHECK_AT/CONFIG), each of which
 *  fires its own STATE_CHANGED that must NOT be mistaken for the
 *  terminal one. */
static bool match_ready_or_error(const struct lora_e5_app_event *e)
{
	if (e->type == LORA_E5_APP_EVT_ERROR) {
		return true;
	}
	return e->type == LORA_E5_APP_EVT_STATE_CHANGED &&
	       e->state_changed == LORA_E5_STATE_READY;
}

static bool match_reset_done(const struct lora_e5_app_event *e)
{
	if (e->type == LORA_E5_APP_EVT_ERROR) {
		return true;
	}
	return e->type == LORA_E5_APP_EVT_STATE_CHANGED &&
	       (e->state_changed == LORA_E5_STATE_READY ||
		e->state_changed == LORA_E5_STATE_JOINED);
}

static bool match_wakeup(const struct lora_e5_app_event *e)
{
	return e->type == LORA_E5_APP_EVT_WAKE_COMPLETE || e->type == LORA_E5_APP_EVT_ERROR;
}

struct tx_trigger_args {
	const uint8_t *data;
	size_t len;
};

static int trigger_start(void *arg)
{
	ARG_UNUSED(arg);
	return lora_e5_start();
}

static int trigger_join(void *arg)
{
	ARG_UNUSED(arg);
	return lora_e5_join();
}

static int trigger_send(void *arg)
{
	const struct tx_trigger_args *a = arg;

	return lora_e5_send(a->data, a->len);
}

static int trigger_send_confirmed(void *arg)
{
	const struct tx_trigger_args *a = arg;

	return lora_e5_send_confirmed(a->data, a->len);
}

static int trigger_reset(void *arg)
{
	ARG_UNUSED(arg);
	return lora_e5_reset();
}

static int trigger_factory_reset(void *arg)
{
	ARG_UNUSED(arg);
	return lora_e5_factory_reset();
}

static int trigger_wakeup(void *arg)
{
	ARG_UNUSED(arg);
	return lora_e5_wakeup();
}

static void register_sync_observer(void)
{
	lora_e5_notify_set_observer(sync_observer_cb, NULL);
}

void lora_e5_test_bind(void)
{
	register_sync_observer();
}

/* ------------------------------------------------------------------- */
/* Init / lifecycle                                                     */
/* ------------------------------------------------------------------- */

int lora_e5_init(const struct lora_e5_hw_config *hw)
{
	if (hw == NULL || hw->uart_dev == NULL) {
		return -EINVAL;
	}
	if (g_initialized) {
		return -EALREADY;
	}

	k_work_queue_start(&g_rx_wq, g_rx_wq_stack, K_THREAD_STACK_SIZEOF(g_rx_wq_stack),
			    LORA_E5_WQ_PRIORITY, NULL);
	k_work_queue_start(&g_fsm_wq, g_fsm_wq_stack, K_THREAD_STACK_SIZEOF(g_fsm_wq_stack),
			    LORA_E5_WQ_PRIORITY, NULL);
	k_work_queue_start(&g_notify_wq, g_notify_wq_stack,
			    K_THREAD_STACK_SIZEOF(g_notify_wq_stack), LORA_E5_WQ_PRIORITY, NULL);

	int rc = lora_e5_at_init(&g_rx_wq);

	if (rc != 0) {
		return rc;
	}

	rc = lora_e5_uart_init(hw->uart_dev, &g_rx_wq);
	if (rc != 0) {
		return rc;
	}

	struct lora_e5_mm_init_params mm_params = {
		.reset_backend = (hw->reset_gpio != NULL) ? LORA_E5_RESET_BACKEND_GPIO
							    : LORA_E5_RESET_BACKEND_AT_COMMAND,
		.reset_gpio = hw->reset_gpio,
	};

	rc = lora_e5_mm_init(&mm_params);
	if (rc != 0) {
		return rc;
	}

	rc = lora_e5_fsm_init(&g_fsm_wq);
	if (rc != 0) {
		return rc;
	}

	rc = lora_e5_notify_init(&g_notify_wq);
	if (rc != 0) {
		return rc;
	}

	register_sync_observer();

	g_initialized = true;
	return 0;
}

int lora_e5_start(void)
{
	if (!g_activation_set || !g_region_set) {
		return -EINVAL;
	}

	int rc = lora_e5_fsm_stage_config(&g_pending_cfg);

	if (rc != 0) {
		return rc;
	}

	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_START_REQUEST };

	return lora_e5_fsm_post_event(&evt);
}

int lora_e5_start_sync(k_timeout_t timeout)
{
	struct lora_e5_app_event result;
	int rc = sync_wait(match_ready_or_error, trigger_start, NULL, timeout, &result);

	if (rc != 0) {
		return rc;
	}
	if (result.type == LORA_E5_APP_EVT_ERROR) {
		return result.error_errno;
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* Activation configuration                                             */
/* ------------------------------------------------------------------- */

int lora_e5_set_otaa(const struct lora_e5_otaa_config *otaa)
{
	if (otaa == NULL) {
		return -EINVAL;
	}
	g_pending_cfg.join_type = LORA_E5_JOIN_OTAA;
	g_pending_cfg.otaa = *otaa;
	g_activation_set = true;
	return 0;
}

int lora_e5_set_abp(const struct lora_e5_abp_config *abp)
{
	if (abp == NULL) {
		return -EINVAL;
	}
	g_pending_cfg.join_type = LORA_E5_JOIN_ABP;
	g_pending_cfg.abp = *abp;
	g_activation_set = true;
	return 0;
}

int lora_e5_set_region(enum lora_e5_region region)
{
	g_pending_cfg.region = region;
	g_region_set = true;
	return 0;
}

int lora_e5_set_class(enum lora_e5_class dev_class)
{
	if (dev_class != LORA_E5_CLASS_A) {
		return -ENOTSUP;
	}
	g_pending_cfg.dev_class = dev_class;
	return 0;
}

/* ------------------------------------------------------------------- */
/* Join / leave                                                         */
/* ------------------------------------------------------------------- */

int lora_e5_join(void)
{
	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_JOIN_REQUEST };

	return lora_e5_fsm_post_event(&evt);
}

int lora_e5_join_sync(k_timeout_t timeout)
{
	struct lora_e5_app_event result;
	int rc = sync_wait(match_join, trigger_join, NULL, timeout, &result);

	if (rc != 0) {
		return rc;
	}
	return (result.type == LORA_E5_APP_EVT_JOIN_SUCCESS) ? 0 : -EIO;
}

int lora_e5_leave(void)
{
	return lora_e5_fsm_leave();
}

/* ------------------------------------------------------------------- */
/* Send / receive                                                       */
/* ------------------------------------------------------------------- */

static int send_common(const uint8_t *data, size_t len, bool confirmed)
{
	if (data == NULL) {
		return -EINVAL;
	}
	if (lora_e5_fsm_get_state_sync() != LORA_E5_STATE_JOINED) {
		return -ENOTCONN;
	}

	int rc = lora_e5_fsm_stage_tx(data, len, g_pending_cfg.port, confirmed);

	if (rc != 0) {
		return rc;
	}

	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_TX_REQUEST };

	return lora_e5_fsm_post_event(&evt);
}

int lora_e5_send(const uint8_t *data, size_t len)
{
	return send_common(data, len, false);
}

int lora_e5_send_sync(const uint8_t *data, size_t len, k_timeout_t timeout)
{
	struct tx_trigger_args args = { .data = data, .len = len };
	struct lora_e5_app_event result;
	int rc = sync_wait(match_tx, trigger_send, &args, timeout, &result);

	if (rc != 0) {
		return rc;
	}
	return (result.type == LORA_E5_APP_EVT_TX_SUCCESS) ? 0 : -EIO;
}

int lora_e5_send_confirmed(const uint8_t *data, size_t len)
{
	return send_common(data, len, true);
}

int lora_e5_send_confirmed_sync(const uint8_t *data, size_t len, k_timeout_t timeout)
{
	struct tx_trigger_args args = { .data = data, .len = len };
	struct lora_e5_app_event result;
	int rc = sync_wait(match_tx, trigger_send_confirmed, &args, timeout, &result);

	if (rc != 0) {
		return rc;
	}
	return (result.type == LORA_E5_APP_EVT_TX_SUCCESS) ? 0 : -EIO;
}

/* ------------------------------------------------------------------- */
/* Power management                                                     */
/* ------------------------------------------------------------------- */

int lora_e5_sleep(uint32_t duration_ms)
{
	int rc = lora_e5_fsm_stage_sleep(duration_ms);

	if (rc != 0) {
		return rc;
	}

	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_SLEEP_REQUEST };

	return lora_e5_fsm_post_event(&evt);
}

int lora_e5_wakeup(void)
{
	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_WAKE_REQUEST };

	return lora_e5_fsm_post_event(&evt);
}

int lora_e5_wakeup_sync(k_timeout_t timeout)
{
	struct lora_e5_app_event result;
	int rc = sync_wait(match_wakeup, trigger_wakeup, NULL, timeout, &result);

	if (rc != 0) {
		return rc;
	}
	if (result.type == LORA_E5_APP_EVT_ERROR) {
		return result.error_errno;
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* Reset / recovery                                                     */
/* ------------------------------------------------------------------- */

int lora_e5_reset(void)
{
	int rc = lora_e5_fsm_stage_reset(false);

	if (rc != 0) {
		return rc;
	}

	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_RESET_REQUEST };

	return lora_e5_fsm_post_event(&evt);
}

int lora_e5_reset_sync(k_timeout_t timeout)
{
	struct lora_e5_app_event result;
	int rc = sync_wait(match_reset_done, trigger_reset, NULL, timeout, &result);

	if (rc != 0) {
		return rc;
	}
	if (result.type == LORA_E5_APP_EVT_ERROR) {
		return result.error_errno;
	}
	return 0;
}

int lora_e5_factory_reset(void)
{
	int rc = lora_e5_fsm_stage_reset(true);

	if (rc != 0) {
		return rc;
	}

	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_RESET_REQUEST };

	return lora_e5_fsm_post_event(&evt);
}

int lora_e5_factory_reset_sync(k_timeout_t timeout)
{
	struct lora_e5_app_event result;
	int rc = sync_wait(match_reset_done, trigger_factory_reset, NULL, timeout, &result);

	if (rc != 0) {
		return rc;
	}
	if (result.type == LORA_E5_APP_EVT_ERROR) {
		return result.error_errno;
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* Identity / capability                                                 */
/* ------------------------------------------------------------------- */

int lora_e5_get_version(struct lora_e5_version *out, k_timeout_t timeout)
{
	return lora_e5_mm_get_version(out, timeout);
}

int lora_e5_get_ids(struct lora_e5_ids *out, k_timeout_t timeout)
{
	return lora_e5_mm_get_ids(out, timeout);
}

int lora_e5_get_max_payload(size_t *out, k_timeout_t timeout)
{
	return lora_e5_mm_get_max_payload(out, timeout);
}

int lora_e5_get_public_network_mode(bool *out, k_timeout_t timeout)
{
	return lora_e5_mm_get_public_network_mode(out, timeout);
}

enum lora_e5_state lora_e5_get_state(void)
{
	return lora_e5_fsm_get_state_sync();
}

/* ------------------------------------------------------------------- */
/* Callback registration                                                */
/* ------------------------------------------------------------------- */

int lora_e5_register_callback(lora_e5_event_cb_t cb, void *user_data)
{
	return lora_e5_notify_set_callback(cb, user_data);
}

/* ------------------------------------------------------------------- */
/* Debug escape hatch                                                   */
/* ------------------------------------------------------------------- */

int lora_e5_at_raw(const char *cmd, char *resp_buf, size_t resp_buf_len, k_timeout_t timeout)
{
	enum lora_e5_state state = lora_e5_fsm_get_state_sync();

	if (state != LORA_E5_STATE_READY && state != LORA_E5_STATE_JOINED) {
		return -EBUSY;
	}

	/* lora_e5_mm_at_raw() itself documents that it cannot enforce this
	 * READY/JOINED gate (Modem Manager holds no FSM state) -- this is
	 * exactly the caller-side check its own doc comment says belongs
	 * here.
	 */
	return lora_e5_mm_at_raw(cmd, cmd != NULL ? strlen(cmd) : 0, resp_buf, resp_buf_len,
				   timeout);
}
