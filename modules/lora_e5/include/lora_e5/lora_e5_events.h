/**
 * @file lora_e5_events.h
 * @brief Event types for both the internal FSM event bus and the
 * application-facing callback.
 *
 * Two distinct event vocabularies live here, kept in separate enums
 * deliberately:
 *   - lora_e5_fsm_event: internal, drives FSM transitions. Never seen
 *     by the application.
 *   - lora_e5_app_event: what lora_e5_register_callback() delivers.
 *     Stable, minimal, AT-vocabulary-free.
 */
#ifndef LORA_E5_EVENTS_H_
#define LORA_E5_EVENTS_H_

#include <stdint.h>
#include <stddef.h>

#include "lora_e5_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */
/* Internal FSM event vocabulary                                        */
/* ------------------------------------------------------------------- */

/**
 * @brief Events consumed by the FSM work queue.
 *
 * Producers: Public API (REQUEST-suffixed events), Modem Manager
 * (RESULT/classified-URC events), FSM's own delayable-work timers
 * (TIMEOUT variants).
 */
enum lora_e5_fsm_event_type {
	LORA_E5_FSM_EVT_START_REQUEST,
	LORA_E5_FSM_EVT_BOOT_SETTLE_EXPIRED,
	LORA_E5_FSM_EVT_PROBE_RESULT,       /**< CHECK_AT step result. */
	LORA_E5_FSM_EVT_CONFIG_STEP_RESULT, /**< One CONFIG sub-command
	                                      *   resolved; FSM advances to
	                                      *   next sub-command or to
	                                      *   READY. */
	LORA_E5_FSM_EVT_JOIN_REQUEST,
	LORA_E5_FSM_EVT_JOIN_RESULT,        /**< From Modem Manager;
	                                      *   payload distinguishes
	                                      *   OTAA-success/failure vs
	                                      *   ABP-immediate-success. */
	LORA_E5_FSM_EVT_JOIN_TIMEOUT,       /**< FSM-level timer, distinct
	                                      *   from the AT Command
	                                      *   Manager's per-command
	                                      *   timeout -- see Phase 1
	                                      *   §3 step 2 rationale. */
	LORA_E5_FSM_EVT_TX_REQUEST,
	LORA_E5_FSM_EVT_TX_RESULT,          /**< From Modem Manager. */
	LORA_E5_FSM_EVT_DOWNLINK,           /**< Classified downlink,
	                                      *   forwarded through
	                                      *   regardless of TX result --
	                                      *   a downlink can arrive
	                                      *   piggybacked on an
	                                      *   unconfirmed uplink's RX
	                                      *   window. */
	LORA_E5_FSM_EVT_SLEEP_REQUEST,
	LORA_E5_FSM_EVT_WAKE_REQUEST,
	LORA_E5_FSM_EVT_WAKE_RESULT,
	LORA_E5_FSM_EVT_RESET_REQUEST,      /**< Explicit application call
	                                      *   or internal recovery-
	                                      *   ladder step. */
	LORA_E5_FSM_EVT_RESET_RESULT,
	LORA_E5_FSM_EVT_UART_FAULT,         /**< From UART backend, routed
	                                      *   through Modem Manager
	                                      *   without semantic
	                                      *   translation. */
	LORA_E5_FSM_EVT_RECOVERY_LADDER_EXHAUSTED,
};

/** @brief Outcome tag for LORA_E5_FSM_EVT_JOIN_RESULT. */
enum lora_e5_join_outcome {
	LORA_E5_JOIN_OUTCOME_SUCCESS,
	LORA_E5_JOIN_OUTCOME_FAILED,     /**< "+JOIN: Join failed" */
	LORA_E5_JOIN_OUTCOME_TIMEOUT,
	LORA_E5_JOIN_OUTCOME_ABP_SKIP,   /**< ABP: no AT+JOIN issued, this
	                                   *   is an immediate synthetic
	                                   *   success. */
};

struct lora_e5_fsm_join_result {
	enum lora_e5_join_outcome outcome;
	struct lora_e5_devaddr dev_addr;  /**< Valid on SUCCESS/ABP_SKIP. */
	uint32_t net_id;                  /**< Valid on SUCCESS (OTAA only,
	                                    *   from "+JOIN: NetID..."). */
};

struct lora_e5_fsm_tx_result {
	bool confirmed;
	enum lora_e5_tx_fail_reason fail_reason; /**< NONE on success. */
};

/**
 * @brief Payload for LORA_E5_FSM_EVT_CONFIG_STEP_RESULT.
 *
 * Modem Manager emits one of these per CONFIG sub-command (9 total,
 * see CFG_STEP_COUNT in lora_e5_modem_manager.c) -- error is
 * meaningful only on failure. is_last_step lets the FSM tell "one
 * step just succeeded, more coming" from "all steps succeeded, move
 * to READY" without the FSM hardcoding the step count itself, which
 * would duplicate knowledge Modem Manager already owns and silently
 * desync if that step list ever changes.
 */
struct lora_e5_fsm_config_step_result {
	enum lora_e5_at_error error;
	bool is_last_step;
};

/**
 * @brief Tagged-union event as submitted to the FSM work queue.
 *
 * Fixed-size, no dynamic allocation -- sized for the largest payload
 * variant (currently lora_e5_downlink's embedded pointer/len pair;
 * the actual frame bytes live in a Modem-Manager-owned static buffer,
 * not copied into this struct).
 */
struct lora_e5_fsm_event {
	enum lora_e5_fsm_event_type type;
	union {
		struct lora_e5_fsm_join_result join_result;
		struct lora_e5_fsm_tx_result tx_result;
		struct lora_e5_downlink downlink;
		struct lora_e5_fsm_config_step_result config_step_result; /**< Valid
		                                    *   on CONFIG_STEP_RESULT. */
		int reset_result;   /**< 0 on success, negative errno
		                      *   otherwise. */
	};
};

/* ------------------------------------------------------------------- */
/* Application-facing event vocabulary                                  */
/* ------------------------------------------------------------------- */

/**
 * @brief Events delivered via lora_e5_register_callback().
 *
 * Deliberately smaller and more stable than lora_e5_fsm_event_type --
 * this is the library's external contract and should not need to
 * change when internal FSM/Modem-Manager plumbing changes.
 */
enum lora_e5_app_event_type {
	LORA_E5_APP_EVT_JOIN_SUCCESS,
	LORA_E5_APP_EVT_JOIN_FAILED,
	LORA_E5_APP_EVT_TX_SUCCESS,
	LORA_E5_APP_EVT_TX_FAILED,
	LORA_E5_APP_EVT_DOWNLINK_RECEIVED,
	LORA_E5_APP_EVT_SLEEP_ENTERED,
	LORA_E5_APP_EVT_WAKE_COMPLETE,
	LORA_E5_APP_EVT_STATE_CHANGED,   /**< Diagnostic/telemetry event;
	                                   *   application is not required
	                                   *   to act on it. */
	LORA_E5_APP_EVT_ERROR,           /**< FSM entered ERROR state --
	                                   *   recovery ladder exhausted or
	                                   *   a structural fault occurred.
	                                   *   Library will not self-heal
	                                   *   further without
	                                   *   lora_e5_reset() (see Phase 1
	                                   *   §8.2 stop condition). */
};

struct lora_e5_app_event {
	enum lora_e5_app_event_type type;
	union {
		struct lora_e5_fsm_join_result join;
		struct lora_e5_fsm_tx_result tx;
		struct lora_e5_downlink downlink;
		enum lora_e5_state state_changed; /**< New state, for
		                                    *   STATE_CHANGED. */
		int error_errno;      /**< Negative errno context for
		                        *   ERROR, where available. */
	};
};

/**
 * @brief Application callback signature.
 *
 * Invoked from the dedicated notify work queue (Phase 1 §4), never
 * from the FSM work queue directly -- a slow/blocking callback here
 * cannot stall modem processing. Even so, this callback should not
 * perform long blocking work; if it needs to, hand off to yet another
 * application-owned queue.
 */
typedef void (*lora_e5_event_cb_t)(const struct lora_e5_app_event *event,
				    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_EVENTS_H_ */
