/**
 * @file lora_e5.h
 * @brief Public API. This is the only header a typical application
 * needs to include directly.
 *
 * Sync/async policy (Phase 1 §7 gap, resolved): every operation with a
 * meaningful async use case has both a fire-and-forget async form and
 * an explicit `_sync` suffixed blocking form. The suffix is mandatory
 * in the name -- a function that can block should never look identical
 * to one that can't. `_sync` variants must not be called from the
 * library's own FSM/RX/notify work queues (self-deadlock); they exist
 * for application threads.
 */
#ifndef LORA_E5_H_
#define LORA_E5_H_

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>

#include "lora_e5/lora_e5_types.h"
#include "lora_e5/lora_e5_events.h"
#include "lora_e5/lora_e5_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */
/* Init / lifecycle                                                     */
/* ------------------------------------------------------------------- */

/**
 * @brief Allocate/initialize internal structures. No modem I/O occurs
 * here (Zephyr init convention -- deterministic, testable without
 * hardware attached).
 *
 * @param hw  Board wiring. Copied internally; caller's storage does
 *            not need to outlive this call.
 */
int lora_e5_init(const struct lora_e5_hw_config *hw);

/**
 * @brief Begin the boot/probe/config sequence (OFF -> ... -> READY).
 * Split from lora_e5_init() so board power-rail sequencing can be
 * controlled independently of software init.
 *
 * Requires lora_e5_set_otaa()/lora_e5_set_abp() and
 * lora_e5_set_region() to have been called first -- returns -EINVAL
 * if activation credentials are unset.
 */
int lora_e5_start(void);
int lora_e5_start_sync(k_timeout_t timeout);

/* ------------------------------------------------------------------- */
/* Activation configuration (must be set before lora_e5_start())        */
/* ------------------------------------------------------------------- */

int lora_e5_set_otaa(const struct lora_e5_otaa_config *otaa);
int lora_e5_set_abp(const struct lora_e5_abp_config *abp);
int lora_e5_set_region(enum lora_e5_region region);

/**
 * @brief v1 only accepts LORA_E5_CLASS_A. Returns -ENOTSUP for B/C --
 * see Phase 1 decision #1. The enum values exist for forward
 * compatibility; their existence is not a support claim.
 */
int lora_e5_set_class(enum lora_e5_class dev_class);

/* ------------------------------------------------------------------- */
/* Join / leave                                                         */
/* ------------------------------------------------------------------- */

/**
 * @brief Trigger activation. For ABP-configured devices this
 * transitions READY -> JOINED immediately with no AT+JOIN issued
 * (LoRaWAN ABP has no join handshake) -- the call is still required
 * for API uniformity across both activation methods (Phase 1
 * decision #3).
 */
int lora_e5_join(void);
int lora_e5_join_sync(k_timeout_t timeout);

/**
 * @brief Clears local join state, returns FSM to READY. Does NOT
 * perform a protocol-level deregistration -- LoRaWAN itself has no
 * network-initiated leave for Class A. [Certain on the protocol
 * limitation]
 */
int lora_e5_leave(void);

/* ------------------------------------------------------------------- */
/* Send / receive                                                       */
/* ------------------------------------------------------------------- */

/**
 * @brief Unconfirmed uplink. `data` is copied into library-owned
 * static staging storage before this returns (Phase 1 §6) -- caller's
 * buffer does not need to outlive the call.
 *
 * @return 0 if queued. Actual TX outcome (including modem-reported
 *         semantic rejections -- DR error, length error, no free
 *         channel, no band available) arrives via the registered
 *         callback as LORA_E5_APP_EVT_TX_SUCCESS/TX_FAILED, not as
 *         this call's return value.
 */
int lora_e5_send(const uint8_t *data, size_t len);
int lora_e5_send_sync(const uint8_t *data, size_t len, k_timeout_t timeout);

int lora_e5_send_confirmed(const uint8_t *data, size_t len);
int lora_e5_send_confirmed_sync(const uint8_t *data, size_t len,
				 k_timeout_t timeout);

/* ------------------------------------------------------------------- */
/* Power management                                                     */
/* ------------------------------------------------------------------- */

/** @param duration_ms  0 = sleep until woken by UART activity. */
int lora_e5_sleep(uint32_t duration_ms);
int lora_e5_wakeup(void);
int lora_e5_wakeup_sync(k_timeout_t timeout);

/* ------------------------------------------------------------------- */
/* Reset / recovery                                                     */
/* ------------------------------------------------------------------- */

/**
 * @brief Explicit application-triggered reset, routed through
 * RECOVERING so configuration is reapplied afterward -- never a raw
 * reset-and-hope.
 */
int lora_e5_reset(void);
int lora_e5_reset_sync(k_timeout_t timeout);

/**
 * @brief AT+FDEFAULT. NOTE: wipes keys/IDs and reverts MODE to LWABP
 * regardless of prior configuration [Certain, Table 4-2]. The library
 * reapplies the application's last-set OTAA/ABP config and region
 * automatically as part of the subsequent CONFIG pass -- but if the
 * application intends to provision *different* credentials after a
 * factory reset, call lora_e5_set_otaa()/lora_e5_set_abp() again
 * before the next lora_e5_start()/join().
 */
int lora_e5_factory_reset(void);
int lora_e5_factory_reset_sync(k_timeout_t timeout);

/* ------------------------------------------------------------------- */
/* Identity / capability (synchronous only -- no async use case)        */
/* ------------------------------------------------------------------- */

int lora_e5_get_version(struct lora_e5_version *out, k_timeout_t timeout);
int lora_e5_get_ids(struct lora_e5_ids *out, k_timeout_t timeout);

/**
 * @brief Query whether the modem is set to use the public-network sync
 * word (AT+LW=NET, spec Sec 4.28.4) -- ON = public LoRaWAN network,
 * OFF = private. This is a static configuration setting, NOT a
 * join/session status indicator -- despite the tempting name, do not
 * use this to decide whether a rejoin is needed. This codebase briefly
 * shipped it mislabeled as "get_join_status()" before checking the
 * primary spec PDF; corrected 2026-07-11, see
 * docs/VERIFICATION_NEEDED.md for the full story and for where the
 * real "already joined" signal actually lives (AT+JOIN's own "+JOIN:
 * Joined already" response, not a separate query).
 *
 * Unlike every other query in this header, this can be called right
 * after lora_e5_init() -- before lora_e5_start()/start_sync() has ever
 * run this power cycle -- since it only needs the AT Command Manager
 * (set up by lora_e5_init()) to be alive, not a completed boot/config
 * sequence.
 *
 * Does NOT itself change FSM state -- lora_e5_get_state() still
 * reports LORA_E5_STATE_OFF until lora_e5_start() runs.
 */
int lora_e5_get_public_network_mode(bool *out, k_timeout_t timeout);

/**
 * @brief Max payload for the CURRENT data rate. DR-dependent, changes
 * under ADR -- do not cache across a TX_FAILED(LENGTH_ERROR) without
 * re-querying.
 */
int lora_e5_get_max_payload(size_t *out, k_timeout_t timeout);

/** @brief Diagnostic read. Round-trips through the FSM queue rather
 *  than a raw mutex-guarded peek, so it never returns a state value
 *  the FSM itself is mid-transition on. */
enum lora_e5_state lora_e5_get_state(void);

/* ------------------------------------------------------------------- */
/* Callback registration                                                */
/* ------------------------------------------------------------------- */

/**
 * @brief Register the single application event consumer. Replaces any
 * previously registered callback -- this library does not support
 * multiple independent subscribers (Phase 1 §2.7; add a fan-out
 * wrapper at the application layer if multiple consumers are
 * genuinely needed).
 */
int lora_e5_register_callback(lora_e5_event_cb_t cb, void *user_data);

/* ------------------------------------------------------------------- */
/* Debug escape hatch                                                   */
/* ------------------------------------------------------------------- */

/**
 * @brief Constrained raw AT passthrough for debugging only.
 *
 * NOT SAFE for AT+JOIN, AT+MSG/AT+CMSG (and HEX variants), AT+TEST=
 * RXLRPKT, or AT+BEACON -- these have multi-line/URC-interleaved
 * responses this passthrough cannot correctly terminate on. Use the
 * first-class API calls for those. Rejected with -EBUSY if the FSM is
 * not in READY or JOINED state.
 *
 * @param resp_buf  Filled with the first matching OK/ERROR line's
 *                  text (without CR/LF). May be NULL if the caller
 *                  doesn't need the response text.
 */
int lora_e5_at_raw(const char *cmd, char *resp_buf, size_t resp_buf_len,
		    k_timeout_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_H_ */
