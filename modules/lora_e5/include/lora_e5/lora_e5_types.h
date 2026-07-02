/**
 * @file lora_e5_types.h
 * @brief Core types shared across the public API and internal modules.
 *
 * These types are intentionally free of AT-command vocabulary. Anything
 * that looks like "+JOIN" or "AT+MSGHEX" belongs in the Modem Manager's
 * internal command table (lora_e5_hf_commands.c, private), not here.
 */
#ifndef LORA_E5_TYPES_H_
#define LORA_E5_TYPES_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */
/* FSM state                                                            */
/* ------------------------------------------------------------------- */

/**
 * @brief Modem lifecycle state as owned by the LoRaWAN FSM.
 *
 * Class B/C are deliberately NOT represented here for v1 (see Phase 1
 * decision: no dormant/gated states for unimplemented classes). Adding
 * Class B/C later is expected to require new states; this enum is not
 * guaranteed binary-stable across that future change.
 */
enum lora_e5_state {
	LORA_E5_STATE_OFF = 0,
	LORA_E5_STATE_RESET,
	LORA_E5_STATE_BOOT,
	LORA_E5_STATE_CHECK_AT,
	LORA_E5_STATE_CONFIG,
	LORA_E5_STATE_READY,        /**< Configured, not yet joined (OTAA) or
	                              *   not yet activated (ABP). */
	LORA_E5_STATE_JOINING,      /**< OTAA only. ABP skips this state. */
	LORA_E5_STATE_JOINED,       /**< Single idle state for both
	                              *   post-join (OTAA) and post-activation
	                              *   (ABP). See Phase 1 review note: TX
	                              *   always returns here, never to
	                              *   READY. */
	LORA_E5_STATE_TX_PENDING,
	LORA_E5_STATE_WAIT_TX_RESULT, /**< Single state covering RX1/RX2/ACK
	                                *   wait. Window and ack outcome are
	                                *   event metadata, not FSM states. */
	LORA_E5_STATE_SLEEP,
	LORA_E5_STATE_ERROR,
	LORA_E5_STATE_RECOVERING,
};

/* ------------------------------------------------------------------- */
/* Region / band plan                                                   */
/* ------------------------------------------------------------------- */

/**
 * @brief LoRaWAN region / band plan, mapped 1:1 to AT+DR=<band> argument.
 *
 * Full firmware-supported list per AT Command Specification V1.0 Table
 * 3-1 / §4.13.2. Kconfig (CONFIG_LORA_E5_DEFAULT_REGION) should expose
 * only the subset relevant to a given product's regulatory market --
 * do not assume every board needs every region compiled in.
 */
enum lora_e5_region {
	LORA_E5_REGION_EU868 = 0,
	LORA_E5_REGION_US915,
	LORA_E5_REGION_US915HYBRID,
	LORA_E5_REGION_CN779,
	LORA_E5_REGION_EU433,
	LORA_E5_REGION_AU915,
	LORA_E5_REGION_AU915OLD,
	LORA_E5_REGION_CN470,
	LORA_E5_REGION_AS923,
	LORA_E5_REGION_KR920,
	LORA_E5_REGION_IN865,
	LORA_E5_REGION_RU864,
	/* CN470PREQUEL, STE920 intentionally omitted from v1 -- customized
	 * band plans, no stated requirement. Add only if a concrete
	 * deployment needs them. */
};

/**
 * @brief LoRaWAN device class.
 *
 * v1 implements Class A only. LORA_E5_CLASS_B and _C are declared so
 * the enum is stable for future extension, but lora_e5_set_class() must
 * reject them with -ENOTSUP until Class B/C support is actually
 * implemented -- do not let the enum's mere existence imply the
 * feature works.
 */
enum lora_e5_class {
	LORA_E5_CLASS_A = 0,
	LORA_E5_CLASS_B, /**< Not implemented in v1. Reserved. */
	LORA_E5_CLASS_C, /**< Not implemented in v1. Reserved. */
};

/** @brief Activation method. */
enum lora_e5_join_type {
	LORA_E5_JOIN_OTAA = 0,
	LORA_E5_JOIN_ABP,
};

/* ------------------------------------------------------------------- */
/* Activation credentials                                               */
/* ------------------------------------------------------------------- */

/** @brief 8-byte EUI (DevEUI, AppEUI), big-endian, per AT+ID. */
struct lora_e5_eui8 {
	uint8_t bytes[8];
};

/** @brief 16-byte AES-128 key (AppKey, NwkSKey, AppSKey), per AT+KEY. */
struct lora_e5_key16 {
	uint8_t bytes[16];
};

/** @brief 4-byte device address (DevAddr), per AT+ID. */
struct lora_e5_devaddr {
	uint8_t bytes[4];
};

/**
 * @brief OTAA activation credentials.
 *
 * Maps to AT+ID=DevEui/AppEui and AT+KEY=APPKEY during CONFIG.
 */
struct lora_e5_otaa_config {
	struct lora_e5_eui8 dev_eui;
	struct lora_e5_eui8 app_eui;
	struct lora_e5_key16 app_key;
};

/**
 * @brief ABP activation credentials.
 *
 * Maps to AT+ID=DevAddr and AT+KEY=NWKSKEY/APPSKEY during CONFIG.
 * No AT+JOIN is ever issued for ABP -- LoRaWAN ABP has no join
 * handshake by protocol definition. [Certain]
 */
struct lora_e5_abp_config {
	struct lora_e5_devaddr dev_addr;
	struct lora_e5_key16 nwk_skey;
	struct lora_e5_key16 app_skey;
};

/**
 * @brief Full device configuration, supplied by the application before
 * lora_e5_start() completes CONFIG.
 *
 * Ownership: library copies this by value during configuration; the
 * application's storage does not need to outlive the call.
 */
struct lora_e5_config {
	enum lora_e5_region region;
	enum lora_e5_class dev_class;      /**< Must be LORA_E5_CLASS_A in v1. */
	enum lora_e5_join_type join_type;
	union {
		struct lora_e5_otaa_config otaa;
		struct lora_e5_abp_config abp;
	};
	bool adr_enable;                   /**< AT+ADR. Recommended: true. */
	uint8_t port;                      /**< AT+PORT, 1..255. */
	uint8_t unconfirmed_repeats;       /**< AT+REPT, 1..15. */
	uint8_t confirmed_retries;         /**< AT+RETRY, 0..254. Values <2
	                                     *   mean only one attempt --
	                                     *   see spec §4.17. */
};

/* ------------------------------------------------------------------- */
/* Error codes (verbatim from AT Command Specification V1.0 Table 2-1)  */
/* ------------------------------------------------------------------- */

/**
 * @brief Modem-reported AT error codes.
 *
 * These are the codes the modem itself returns in "ERROR(-N)" /
 * "+CMD: ERROR(-N)" lines. Do not confuse with host-side errno values
 * returned by the public API (which are standard negative errno).
 */
enum lora_e5_at_error {
	LORA_E5_AT_ERR_INVALID_PARAM = -1,
	LORA_E5_AT_ERR_UNKNOWN_CMD = -10,
	LORA_E5_AT_ERR_WRONG_FORMAT = -11,
	LORA_E5_AT_ERR_WRONG_MODE = -12,   /**< Command unavailable in current
	                                     *   AT+MODE. Check mode before
	                                     *   retrying -- retrying blind
	                                     *   will not help. */
	LORA_E5_AT_ERR_TOO_MANY_PARAMS = -20,
	LORA_E5_AT_ERR_CMD_TOO_LONG = -21, /**< Exceeds 528-byte command
	                                     *   limit. */
	LORA_E5_AT_ERR_END_SYMBOL_TIMEOUT = -22,
	LORA_E5_AT_ERR_INVALID_CHAR = -23,
	LORA_E5_AT_ERR_COMPOSITE = -24,    /**< One of -21/-22/-23; modem did
	                                     *   not disambiguate further. */
};

/**
 * @brief Classification used by the recovery ladder (Phase 1 §8.1).
 *
 * Structural errors must never be retried by the AT Command Manager --
 * retrying a -1/-11 is retrying a bug, not a transient fault.
 */
static inline bool lora_e5_at_error_is_structural(enum lora_e5_at_error err)
{
	switch (err) {
	case LORA_E5_AT_ERR_INVALID_PARAM:
	case LORA_E5_AT_ERR_WRONG_FORMAT:
	case LORA_E5_AT_ERR_TOO_MANY_PARAMS:
	case LORA_E5_AT_ERR_CMD_TOO_LONG:
	case LORA_E5_AT_ERR_INVALID_CHAR:
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------- */
/* TX / RX metadata                                                     */
/* ------------------------------------------------------------------- */

/** @brief Which RX opportunity a downlink arrived in. Metadata only --
 *  never a state. Per spec §4.5/§4.26.2/§4.26.3, RXWIN0 is Class-C
 *  extra RXWIN2 and RXWIN3 is a Class-B ping slot; both are reachable
 *  labels from firmware even though v1 doesn't drive B/C, so the
 *  parser must still recognize them and the application/FSM should
 *  treat any non-RXWIN1/RXWIN2 value defensively (log + ignore) since
 *  v1 does not request B/C).
 */
enum lora_e5_rx_window {
	LORA_E5_RX_WINDOW_UNKNOWN = 0,
	LORA_E5_RX_WINDOW_RX1,
	LORA_E5_RX_WINDOW_RX2,
	LORA_E5_RX_WINDOW_RX0,   /**< Class C extra RXWIN2. Not expected in v1. */
	LORA_E5_RX_WINDOW_RX3,   /**< Class B ping slot. Not expected in v1. */
};

/** @brief Reason a TX request did not result in TX_SUCCESS. */
enum lora_e5_tx_fail_reason {
	LORA_E5_TX_FAIL_NONE = 0,          /**< Not a failure; TX succeeded. */
	LORA_E5_TX_FAIL_NO_ACK,            /**< Confirmed uplink, no ACK
	                                     *   after modem's internal
	                                     *   AT+RETRY attempts. */
	LORA_E5_TX_FAIL_BUSY,              /**< "+MSG: LoRaWAN modem is busy" */
	LORA_E5_TX_FAIL_NOT_JOINED,        /**< "+MSG: Please join network
	                                     *   first" -- indicates FSM/modem
	                                     *   state desync; corrective
	                                     *   rejoin recommended. */
	LORA_E5_TX_FAIL_NO_FREE_CHANNEL,   /**< "+MSG: No free channel -N" */
	LORA_E5_TX_FAIL_NO_BAND,           /**< "+MSG: No band in Nms" --
	                                     *   duty-cycle throttled, not a
	                                     *   fault. */
	LORA_E5_TX_FAIL_DR_ERROR,          /**< Current DR unsupported for
	                                     *   this payload/channel. */
	LORA_E5_TX_FAIL_LENGTH_ERROR,      /**< Payload exceeds max for
	                                     *   current DR (see
	                                     *   lora_e5_get_max_payload()). */
	LORA_E5_TX_FAIL_MODEM_ERROR,       /**< Generic AT-level ERROR(-N)
	                                     *   during the TX transaction. */
	LORA_E5_TX_FAIL_TIMEOUT,           /**< Transport-level: no terminal
	                                     *   line arrived at all within
	                                     *   CONFIG_LORA_E5_TX_TIMEOUT_MS. */
};

/** @brief A received downlink frame. */
struct lora_e5_downlink {
	uint8_t port;
	const uint8_t *data;
	size_t len;
	int16_t rssi_dbm;
	int8_t snr_db;
	enum lora_e5_rx_window window;
	bool is_multicast;
};

/** @brief LinkCheckReq/Ans result, optional URC during any MSG/CMSG. */
struct lora_e5_link_check {
	uint8_t margin_db;   /**< 0..254, per spec §4.5.1. 255 reserved. */
	uint8_t gateway_count;
};

/* ------------------------------------------------------------------- */
/* Modem identity / capability queries                                  */
/* ------------------------------------------------------------------- */

/** @brief AT+VER result (Semantic Versioning 2.0.0 per spec §4.2). */
struct lora_e5_version {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
};

/** @brief AT+ID result -- DevAddr always valid; DevEui/AppEui valid for
 *  OTAA-provisioned devices (meaningless but populated for ABP). */
struct lora_e5_ids {
	struct lora_e5_devaddr dev_addr;
	struct lora_e5_eui8 dev_eui;
	struct lora_e5_eui8 app_eui;
};

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_TYPES_H_ */
