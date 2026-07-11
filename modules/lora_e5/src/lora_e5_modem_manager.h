/**
 * @file lora_e5_modem_manager.h
 * @brief Internal-only: translates abstract FSM intent into AT Command
 * Manager transactions, and classifies incoming URCs before they reach
 * the FSM.
 *
 * NOT part of the public API (lives under src/, not include/lora_e5/).
 * The FSM is the only consumer. This is the single module that knows
 * both "what AT+JOIN means for LoRaWAN" and "how the AT Command
 * Manager's terminal_events[] mechanism works" -- see Phase 1's Modem
 * Manager addendum for the layering rationale.
 *
 * Threading: synchronous translation layer invoked from FSM work-queue
 * context. Does NOT own a work queue of its own. Calls into
 * lora_e5_at_submit() (async, non-blocking) and returns immediately;
 * results arrive later via the registered lora_e5_at_result_cb_t,
 * which this module translates again into a struct lora_e5_mm_event
 * and forwards to the FSM's event queue.
 */
#ifndef LORA_E5_MODEM_MANAGER_H_
#define LORA_E5_MODEM_MANAGER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#include "lora_e5/lora_e5_types.h"
#include "lora_e5/lora_e5_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */
/* Reset backend abstraction (Phase 1 decision: never hardcode)         */
/* ------------------------------------------------------------------- */

/**
 * @brief How lora_e5_mm_reset() actually resets the modem.
 *
 * Selected at init time based on whether a `reset-gpios` devicetree
 * property is present and CONFIG_LORA_E5_HAS_RESET_GPIO is enabled.
 * Never hardcoded to one path -- see Phase 1 decision #4.
 */
enum lora_e5_reset_backend {
	LORA_E5_RESET_BACKEND_AT_COMMAND,  /**< AT+RESET only. Default when
	                                     *   no reset-gpios property is
	                                     *   present. Cannot recover a
	                                     *   modem that can't parse AT
	                                     *   commands at all. */
	LORA_E5_RESET_BACKEND_GPIO,        /**< Hardware NRST toggle via
	                                     *   devicetree reset-gpios.
	                                     *   Preferred when available --
	                                     *   works even if UART/firmware
	                                     *   state is wedged. */
};

/* ------------------------------------------------------------------- */
/* Modem Manager result tags                                            */
/* ------------------------------------------------------------------- */

/**
 * @brief result_tag values used in this module's terminal_events[]
 * tables (lora_e5_hf_commands.c, private, not declared here). Opaque
 * to the AT Command Manager; meaningful only to this module's result
 * callback.
 */
enum lora_e5_mm_result_tag {
	LORA_E5_MM_TAG_OK = 0,
	LORA_E5_MM_TAG_GENERIC_ERROR,       /**< Used by the ANY_ERROR_ENTRY
					      *   catch-all in every
					      *   lora_e5_hf_commands.c
					      *   terminal_events[] table --
					      *   any ERROR(-N) line not
					      *   otherwise classified by a
					      *   more specific entry. Was
					      *   referenced by
					      *   ANY_ERROR_ENTRY before this
					      *   enum defined it; added here
					      *   to fix that (pre-existing
					      *   undeclared-identifier bug,
					      *   not a new design choice). */
	LORA_E5_MM_TAG_JOIN_DONE,
	LORA_E5_MM_TAG_JOIN_FAILED,
	LORA_E5_MM_TAG_JOIN_BUSY,
	LORA_E5_MM_TAG_JOIN_ALREADY,
	LORA_E5_MM_TAG_MSG_DONE,
	LORA_E5_MM_TAG_MSG_BUSY,
	LORA_E5_MM_TAG_MSG_NOT_JOINED,
	LORA_E5_MM_TAG_MSG_NO_FREE_CHANNEL,
	LORA_E5_MM_TAG_MSG_NO_BAND,
	LORA_E5_MM_TAG_MSG_DR_ERROR,
	LORA_E5_MM_TAG_MSG_LENGTH_ERROR,
	LORA_E5_MM_TAG_CMSG_WAIT_ACK,      /**< Non-terminal marker some
	                                     *   callers may want visibility
	                                     *   into; typically forwarded
	                                     *   as a URC rather than a
	                                     *   terminal match. */
	LORA_E5_MM_TAG_CMSG_DONE_ACKED,
	LORA_E5_MM_TAG_CMSG_DONE_NO_ACK,
	LORA_E5_MM_TAG_LOWPOWER_SLEEP,
	LORA_E5_MM_TAG_LOWPOWER_WAKEUP,
	LORA_E5_MM_TAG_RESET_OK,
	LORA_E5_MM_TAG_FDEFAULT_OK,
};

/* ------------------------------------------------------------------- */
/* Init / lifecycle                                                     */
/* ------------------------------------------------------------------- */

struct lora_e5_mm_init_params {
	enum lora_e5_reset_backend reset_backend;
	const struct gpio_dt_spec *reset_gpio; /**< Valid only when
	                                         *   reset_backend ==
	                                         *   LORA_E5_RESET_BACKEND_
	                                         *   GPIO. */
};

int lora_e5_mm_init(const struct lora_e5_mm_init_params *params);

/**
 * @brief Register the callback the Modem Manager uses to push
 * classified events (join results, TX results, downlinks, link
 * checks, UART faults) to the FSM. Must be called once, by the FSM,
 * before lora_e5_mm_init() completes.
 */
typedef void (*lora_e5_mm_event_cb_t)(const struct lora_e5_fsm_event *event,
				       void *user_data);
int lora_e5_mm_set_event_callback(lora_e5_mm_event_cb_t cb, void *user_data);

/* ------------------------------------------------------------------- */
/* Boot / configuration                                                 */
/* ------------------------------------------------------------------- */

/**
 * @brief Issue AT and wait (async, result via event callback with a
 * synthetic PROBE_RESULT-shaped event) for +AT: OK, bounded by
 * CONFIG_LORA_E5_MAX_RETRIES.
 */
int lora_e5_mm_probe(void);

/**
 * @brief Drive the CONFIG sequence: AT+MODE, AT+ID, AT+KEY, AT+DR
 * (region), AT+CLASS=A, AT+PORT, AT+ADR, AT+REPT, AT+RETRY -- one
 * sub-command at a time, each a separate AT Command Manager
 * transaction. Emits one CONFIG_STEP_RESULT-shaped event per
 * sub-command via the event callback; the FSM decides whether to
 * proceed to the next step or abort into RECOVERING/ERROR based on
 * lora_e5_at_error_is_structural() on any failure.
 *
 * @param cfg  Copied internally; cfg's storage does not need to
 *             outlive this call.
 */
int lora_e5_mm_configure(const struct lora_e5_config *cfg);

/* ------------------------------------------------------------------- */
/* Join / send / sleep / reset                                          */
/* ------------------------------------------------------------------- */

/**
 * @brief Trigger activation.
 *
 * For join_type == LORA_E5_JOIN_OTAA: issues AT+JOIN, result arrives
 * async as LORA_E5_FSM_EVT_JOIN_RESULT with outcome SUCCESS/FAILED.
 * For join_type == LORA_E5_JOIN_ABP: issues NO AT command at all --
 * synthesizes an immediate LORA_E5_FSM_EVT_JOIN_RESULT with outcome
 * ABP_SKIP, delivered via the same event callback path so the FSM's
 * handling is uniform regardless of activation method (see Phase 1
 * decision #3 -- the join_type branch lives here, not duplicated in
 * the FSM).
 */
int lora_e5_mm_join(enum lora_e5_join_type join_type);

/**
 * @brief Send a frame. Chooses AT+MSGHEX/AT+CMSGHEX internally (raw
 * bytes in, no string-escaping burden pushed to caller -- see Phase 1
 * §7 API deviation rationale). Result arrives async as
 * LORA_E5_FSM_EVT_TX_RESULT; any downlink piggybacked on the RX
 * window arrives as a separate LORA_E5_FSM_EVT_DOWNLINK before the
 * TX_RESULT event, in wire order.
 *
 * @param data       Must remain valid until the result callback fires.
 *                    Caller (FSM, via Public API) is responsible for
 *                    having already copied into library-owned storage
 *                    per Phase 1 §6 TX_REQUEST payload note.
 * @param port       1..255.
 * @param confirmed  true -> AT+CMSGHEX, false -> AT+MSGHEX.
 */
int lora_e5_mm_send(const uint8_t *data, size_t len, uint8_t port,
		     bool confirmed);

/**
 * @brief AT+LOWPOWER[=duration_ms]. duration_ms == 0 means sleep until
 * woken by UART activity (spec §4.30); non-zero arms a timed wake.
 */
int lora_e5_mm_sleep(uint32_t duration_ms);

/**
 * @brief Send the host-side wake byte and observe the mandatory
 * settle delay (>=5ms per spec §4.30) before the next command may be
 * issued -- enforced here, not left to FSM/application to remember.
 */
int lora_e5_mm_wakeup(void);

/**
 * @brief Reset via whichever backend was selected at init (GPIO or
 * AT+RESET). See Phase 1 decision #4 -- never hardcoded.
 */
int lora_e5_mm_reset(void);

/**
 * @brief AT+FDEFAULT. NOTE: factory default MODE is LWABP, not LWOTAA
 * [Certain, spec Table 4-2]. If the application's configured
 * join_type is OTAA, the FSM's post-factory-reset CONFIG pass must
 * reissue AT+MODE=LWOTAA -- lora_e5_mm_configure() already does this
 * unconditionally as part of its normal sequence, so no special-casing
 * is needed here, but this is flagged because it is a real correctness
 * hazard if lora_e5_mm_configure() is ever "optimized" to skip
 * AT+MODE when it looks unchanged.
 */
int lora_e5_mm_factory_reset(void);

/* ------------------------------------------------------------------- */
/* Capability / identity queries (synchronous helpers)                  */
/* ------------------------------------------------------------------- */

/**
 * @brief AT+VER, blocking wrapper (no meaningful async use case).
 * Must not be called from the FSM work queue -- see
 * lora_e5_at_submit_sync() threading note.
 */
int lora_e5_mm_get_version(struct lora_e5_version *out, k_timeout_t timeout);

/** @brief AT+ID, blocking wrapper. Same threading caveat. */
int lora_e5_mm_get_ids(struct lora_e5_ids *out, k_timeout_t timeout);

/**
 * @brief AT+LW=LEN, blocking wrapper. Max payload is DR-dependent and
 * can change at runtime under ADR -- callers must not cache this
 * value across a TX_FAIL(LENGTH_ERROR) without re-querying.
 */
int lora_e5_mm_get_max_payload(size_t *out, k_timeout_t timeout);

/**
 * @brief AT+LW=NET, blocking wrapper. Reports whether the modem is set
 * to use the public-network sync word (ON) or a private one (OFF) --
 * spec §4.28.4. NOT a join-status query despite how the name reads;
 * this codebase briefly mislabeled it that way before checking the
 * primary spec PDF -- see lora_e5_hf_build_public_network_query()'s
 * doc comment and docs/VERIFICATION_NEEDED.md for the correction and
 * for where the real join-status signal lives (AT+JOIN's own "+JOIN:
 * Joined already" response).
 *
 * Can be called before lora_e5_start()/start_sync() has ever run this
 * power cycle (the AT Command Manager only needs lora_e5_init() to
 * have completed) -- independent of this library's FSM state, which
 * starts at OFF every boot regardless of what this reports.
 *
 * @param out  Set true if the modem reports "NET, ON" (public network),
 *             false if "NET, OFF" (private network). Left unchanged
 *             (not zeroed) on error, since callers should check the
 *             return code, not infer failure from *out.
 */
int lora_e5_mm_get_public_network_mode(bool *out, k_timeout_t timeout);

/* ------------------------------------------------------------------- */
/* Raw passthrough (debug escape hatch)                                 */
/* ------------------------------------------------------------------- */

/**
 * @brief Constrained raw AT passthrough.
 *
 * Deliberately NOT general-purpose -- see Phase 1 review. Waits for
 * the first line matching bare OK, "+<ANY>: OK", or ERROR(-N) within
 * `timeout`. NOT SAFE for commands with multi-line or URC-interleaved
 * responses (AT+JOIN, AT+MSG family, AT+CMSG family, AT+TEST=RXLRPKT,
 * AT+BEACON) -- those have first-class API calls for a reason; this
 * function will resolve early on the first line and desynchronize
 * from the rest of the exchange if used for them.
 *
 * Only queued while FSM state is READY or JOINED -- rejected with
 * -EBUSY otherwise, to avoid racing a state-owned transaction (the
 * same "modem is busy" collision the AT manager's single-in-flight
 * design exists to prevent).
 */
int lora_e5_mm_at_raw(const char *cmd, size_t cmd_len,
		       char *resp_buf, size_t resp_buf_len,
		       k_timeout_t timeout);

/* ------------------------------------------------------------------- */
/* Test-only accessors                                                  */
/* ------------------------------------------------------------------- */

/**
 * @brief Test-only: reset all transient runtime state (port cache, JOIN
 * URC cache, pending-send state, CONFIG sequence progress, registered
 * event callback) back to post-init defaults. Mirrors the role
 * lora_e5_cmd_queue_test_reset() plays for tests/cmd_queue -- call from
 * a test's `before` hook on an already-initialized instance, not
 * lora_e5_mm_init() again (which also re-touches the reset-GPIO pin).
 */
void lora_e5_mm_test_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_MODEM_MANAGER_H_ */
