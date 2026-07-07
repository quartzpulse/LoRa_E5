/**
 * @file lora_e5_internal.h
 * @brief Shared internal-only prototypes used across lora_e5.c,
 * lora_e5_fsm.c, and lora_e5_events.c.
 *
 * NOT installed under include/lora_e5/ -- the application must never
 * see these. Kept deliberately thin per Phase 1 §2.8: this file is
 * types + entry points, not a place to grow shared logic.
 */
#ifndef LORA_E5_INTERNAL_H_
#define LORA_E5_INTERNAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include "lora_e5/lora_e5_types.h"
#include "lora_e5/lora_e5_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */
/* FSM (lora_e5_fsm.c)                                                  */
/* ------------------------------------------------------------------- */

/**
 * @brief Initialize the FSM. Registers itself as the Modem Manager's
 * sole event sink (lora_e5_mm_set_event_callback()) -- Modem Manager
 * must already be initialized (lora_e5_mm_init() returned) before
 * this is called.
 *
 * @param fsm_wq  Dedicated work queue all FSM state transitions run
 *                 on (never k_sys_work_q -- Phase 1 §4). Passed
 *                 explicitly, same rationale as lora_e5_at_init()'s
 *                 rx_work_q parameter: callers/tests supply their own
 *                 queue instead of depending on a hidden global.
 */
int lora_e5_fsm_init(struct k_work_q *fsm_wq);

/**
 * @brief Post an event to the FSM's queue for processing on fsm_wq.
 * Non-blocking. Called by lora_e5.c (translating public API calls)
 * and by the FSM's own Modem-Manager-event trampoline.
 *
 * @return 0 if queued, -ENOMEM if the FSM event queue
 *         (CONFIG_LORA_E5_EVENT_QUEUE_DEPTH) is full.
 */
int lora_e5_fsm_post_event(const struct lora_e5_fsm_event *event);

/**
 * @brief Diagnostic read of the current FSM state. Round-trips
 * through the FSM work queue rather than a raw mutex-guarded peek
 * (Phase 1 §4), so it never returns a value the FSM is mid-transition
 * on. Must NOT be called from the FSM/RX/notify work queues -- this
 * blocks the calling thread.
 */
enum lora_e5_state lora_e5_fsm_get_state_sync(void);

/**
 * @brief Copy application TX data into the FSM-owned static TX
 * staging buffer (Phase 1 §6 -- the application's own buffer does
 * not need to outlive the call). Guarded internally so concurrent
 * callers from different application threads cannot corrupt the
 * single staging buffer; the FSM's single-TX-in-flight-by-construction
 * property means only one lora_e5_send()/lora_e5_send_confirmed()
 * should legitimately be outstanding at a time -- callers (lora_e5.c)
 * are expected to have already checked lora_e5_fsm_get_state_sync()
 * == JOINED before staging.
 *
 * @return 0 on success, -EINVAL if len exceeds
 *         CONFIG_LORA_E5_TX_BUFFER_SIZE, -EBUSY if a previously
 *         staged TX has not yet been consumed by the FSM.
 */
int lora_e5_fsm_stage_tx(const uint8_t *data, size_t len, uint8_t port,
			  bool confirmed);

/**
 * @brief Stage a sleep duration for the following SLEEP_REQUEST event,
 * same rationale as lora_e5_fsm_stage_tx() -- LORA_E5_FSM_EVT_SLEEP_REQUEST
 * carries no payload in the public lora_e5_fsm_event contract, so the
 * duration crosses from lora_e5.c to the FSM via this side channel
 * instead.
 */
int lora_e5_fsm_stage_sleep(uint32_t duration_ms);

/**
 * @brief Stage whether the next RESET_REQUEST should perform a factory
 * reset (AT+FDEFAULT via lora_e5_mm_factory_reset()) instead of a plain
 * reset (AT+RESET via lora_e5_mm_reset()) -- same side-channel rationale
 * as lora_e5_fsm_stage_sleep(): LORA_E5_FSM_EVT_RESET_REQUEST carries no
 * payload to distinguish the two in the public event contract.
 */
int lora_e5_fsm_stage_reset(bool factory);

/**
 * @brief Stage the application's LoRaWAN provisioning config ahead of
 * the following START_REQUEST event -- the FSM keeps its own copy
 * (Phase 1 §2.4 ownership: "provisioning config copy") for reuse
 * during the recovery ladder's reconfigure step, independent of
 * whatever lora_e5.c itself retains.
 */
int lora_e5_fsm_stage_config(const struct lora_e5_config *cfg);

/**
 * @brief Local-only leave: clears join state and returns the FSM to
 * READY without any AT interaction (LoRaWAN has no network-initiated
 * leave for Class A -- lora_e5.h's lora_e5_leave() doc comment).
 *
 * NOTE: this reading of "leave" (-> READY) is taken directly from
 * lora_e5.h's own doc comment. CLAUDE.md's decision #2 prose lists
 * "leave" alongside TX/sleep/reset as transitioning "out of and back
 * into JOINED, never through READY" -- which reads as a direct
 * conflict with lora_e5.h's explicit contract. Per CLAUDE.md's own
 * instruction ("if something here conflicts with what you find in the
 * code, the code is more current"), this implementation follows the
 * concrete, specific header contract; the conflict itself is flagged
 * in docs/VERIFICATION_NEEDED.md rather than silently resolved.
 *
 * Only valid from JOINING/JOINED -- returns -EINVAL otherwise.
 */
int lora_e5_fsm_leave(void);

/* ------------------------------------------------------------------- */
/* UART backend (lora_e5_uart.c)                                       */
/* ------------------------------------------------------------------- */

/**
 * @brief Initialize the UART Async API backend and register it as the
 * AT Command Manager's transport (lora_e5_at_set_transport()) --
 * "registers transport" per lora_e5.h's lora_e5_init() doc comment.
 *
 * @param uart_dev  Board UART device (struct lora_e5_hw_config::uart_dev).
 * @param rx_wq     Work queue line-copy/parse/routing runs on (the same
 *                   queue passed to lora_e5_at_init()).
 */
int lora_e5_uart_init(const struct device *uart_dev, struct k_work_q *rx_wq);

/* ------------------------------------------------------------------- */
/* Notify (lora_e5_events.c)                                           */
/* ------------------------------------------------------------------- */

/**
 * @brief Initialize the notify subsystem: its k_msgq and the k_work
 * item that drains it on notify_wq.
 *
 * @param notify_wq  Dedicated, lower-priority work queue application
 *                    callback dispatch runs on (Phase 1 §4) -- never
 *                    the FSM work queue, so a slow/blocking
 *                    application callback cannot stall modem
 *                    processing.
 */
int lora_e5_notify_init(struct k_work_q *notify_wq);

/**
 * @brief Register the application's single event callback. Mirrors
 * lora_e5_register_callback()'s replace-on-reregister semantics.
 */
int lora_e5_notify_set_callback(lora_e5_event_cb_t cb, void *user_data);

/**
 * @brief Internal observer invoked on every dispatched app event,
 * alongside (not instead of) the application's registered callback --
 * this is the hook lora_e5.c uses to implement its `_sync` API
 * variants (Phase 3: "one static waiter record + one mutex" pattern,
 * signaled here when a matching terminal app-event fires).
 */
typedef void (*lora_e5_notify_observer_t)(const struct lora_e5_app_event *event,
					    void *user_data);

/** @brief Register the sync-wait observer. Called once, by lora_e5.c's
 *  own init, analogous to lora_e5_notify_set_callback() but for the
 *  library's internal use rather than the application's. */
int lora_e5_notify_set_observer(lora_e5_notify_observer_t cb, void *user_data);

/**
 * @brief Post an application event for dispatch on notify_wq.
 * Non-blocking. Called by the FSM whenever a public-facing outcome
 * occurs (join success/fail, tx success/fail, downlink, state change,
 * error).
 *
 * @return 0 if queued, -ENOMEM if CONFIG_LORA_E5_EVENT_QUEUE_DEPTH is
 *         exhausted (an overflow counter is logged, not silently
 *         dropped -- see lora_e5_events.c).
 */
int lora_e5_notify_post(const struct lora_e5_app_event *event);

/* ------------------------------------------------------------------- */
/* Test-only accessors                                                  */
/* ------------------------------------------------------------------- */

/** @brief Test-only: reset notify subsystem state (callbacks, overflow
 *  counter) back to post-init defaults, without re-running
 *  lora_e5_notify_init()'s work-item registration. Mirrors
 *  lora_e5_mm_test_reset(). */
void lora_e5_notify_test_reset(void);

/**
 * @brief Test-only: block the calling thread until every app event
 * queued on notify_wq up to this point has been dispatched. Needed
 * because tests that inject a line directly (bypassing the real UART
 * backend) still cross two real work-queue hops before an app event is
 * observable (RX-thread-context processing -> fsm_wq -> notify_wq);
 * lora_e5_fsm_get_state_sync() only proves the fsm_wq hop has
 * completed, not the notify_wq one. Mirrors
 * lora_e5_fsm_get_state_sync()'s round-trip-through-the-queue pattern.
 */
void lora_e5_notify_test_barrier(void);

/** @brief Test-only: reset FSM state back to LORA_E5_STATE_OFF and
 *  clear provisioning/retry/recovery state, without re-registering
 *  with the Modem Manager. Call from a test's `before` hook on an
 *  already-initialized instance. */
void lora_e5_fsm_test_reset(void);

/** @brief Test-only: perform lora_e5.c's own internal wiring (sync-wait
 *  observer registration) without touching UART -- used by tests that
 *  wire the FSM/Modem Manager/notify layers directly against a mock
 *  transport (tests/modem_manager's pattern), bypassing lora_e5_init(). */
void lora_e5_test_bind(void);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_INTERNAL_H_ */
