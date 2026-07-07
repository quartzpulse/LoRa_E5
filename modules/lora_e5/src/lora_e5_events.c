/**
 * @file lora_e5_events.c
 * @brief Owns the "notify" work queue's k_msgq and the k_work item
 * that drains it, dispatching application-facing events to whatever
 * is registered -- the application's own callback AND (Phase 3's
 * internal sync-wait mechanism) lora_e5.c's observer, side by side.
 *
 * No FSM logic here -- per Phase 1 §2.5, this module is transport for
 * already-fully-shaped struct lora_e5_app_event values the FSM itself
 * constructs; it does not interpret or translate them.
 */

#include "lora_e5_internal.h"

#include <errno.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lora_e5_events, CONFIG_LORA_E5_LOG_LEVEL);

K_MSGQ_DEFINE(g_notify_msgq, sizeof(struct lora_e5_app_event),
	      CONFIG_LORA_E5_EVENT_QUEUE_DEPTH, 4);

static struct k_work_q *g_notify_wq;
static struct k_work g_drain_work;

static lora_e5_event_cb_t g_app_cb;
static void *g_app_cb_user_data;

static lora_e5_notify_observer_t g_observer_cb;
static void *g_observer_user_data;

static uint32_t g_overflow_count;

static void drain_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct lora_e5_app_event evt;

	while (k_msgq_get(&g_notify_msgq, &evt, K_NO_WAIT) == 0) {
		/* Observer first: it exists to unblock a `_sync` caller
		 * waiting on a semaphore, which should not be made to wait
		 * on the application's own callback returning first.
		 */
		if (g_observer_cb != NULL) {
			g_observer_cb(&evt, g_observer_user_data);
		}
		if (g_app_cb != NULL) {
			g_app_cb(&evt, g_app_cb_user_data);
		}
	}
}

int lora_e5_notify_init(struct k_work_q *notify_wq)
{
	if (notify_wq == NULL) {
		return -EINVAL;
	}

	g_notify_wq = notify_wq;
	k_work_init(&g_drain_work, drain_work_handler);
	k_msgq_purge(&g_notify_msgq);
	g_app_cb = NULL;
	g_app_cb_user_data = NULL;
	g_observer_cb = NULL;
	g_observer_user_data = NULL;
	g_overflow_count = 0;
	return 0;
}

int lora_e5_notify_set_callback(lora_e5_event_cb_t cb, void *user_data)
{
	g_app_cb = cb;
	g_app_cb_user_data = user_data;
	return 0;
}

int lora_e5_notify_set_observer(lora_e5_notify_observer_t cb, void *user_data)
{
	g_observer_cb = cb;
	g_observer_user_data = user_data;
	return 0;
}

int lora_e5_notify_post(const struct lora_e5_app_event *event)
{
	if (event == NULL) {
		return -EINVAL;
	}
	if (g_notify_wq == NULL) {
		return -EINVAL;
	}

	if (k_msgq_put(&g_notify_msgq, event, K_NO_WAIT) != 0) {
		g_overflow_count++;
		LOG_ERR("notify queue full (depth %d), dropping app event "
			"type %d (overflow count now %u)",
			CONFIG_LORA_E5_EVENT_QUEUE_DEPTH, (int)event->type,
			g_overflow_count);
		return -ENOMEM;
	}

	k_work_submit_to_queue(g_notify_wq, &g_drain_work);
	return 0;
}

struct notify_barrier_query {
	struct k_work work;
	struct k_sem sem;
};

static void barrier_work_handler(struct k_work *work)
{
	struct notify_barrier_query *q = CONTAINER_OF(work, struct notify_barrier_query, work);

	k_sem_give(&q->sem);
}

K_MUTEX_DEFINE(g_barrier_lock);

void lora_e5_notify_test_barrier(void)
{
	static struct notify_barrier_query q;

	k_mutex_lock(&g_barrier_lock, K_FOREVER);
	k_sem_init(&q.sem, 0, 1);
	k_work_init(&q.work, barrier_work_handler);
	if (g_notify_wq != NULL) {
		k_work_submit_to_queue(g_notify_wq, &q.work);
		k_sem_take(&q.sem, K_FOREVER);
	}
	k_mutex_unlock(&g_barrier_lock);
}

void lora_e5_notify_test_reset(void)
{
	k_msgq_purge(&g_notify_msgq);
	g_app_cb = NULL;
	g_app_cb_user_data = NULL;
	g_observer_cb = NULL;
	g_observer_user_data = NULL;
	g_overflow_count = 0;
}
