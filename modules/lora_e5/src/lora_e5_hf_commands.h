/**
 * @file lora_e5_hf_commands.h
 * @brief LoRa-E5-HF specific AT command builders and terminal-event
 * tables. This is the file a future RN2483/Type-ABZ/Quectel port
 * replaces -- everything above the Modem Manager layer is reused.
 *
 * INTERNAL ONLY. Not installed under include/lora_e5/. Consumed
 * exclusively by lora_e5_modem_manager.c.
 *
 * Every builder fills a caller-supplied struct lora_e5_at_cmd_desc.
 * Commands with runtime-variable content (hex payloads, keys, EUIs)
 * write into a caller-supplied scratch buffer -- no dynamic
 * allocation, and since the AT Command Manager enforces single-
 * in-flight, one scratch buffer owned by the Modem Manager is
 * sufficient for the whole library.
 */
#ifndef LORA_E5_HF_COMMANDS_H_
#define LORA_E5_HF_COMMANDS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lora_e5/lora_e5_at.h"
#include "lora_e5/lora_e5_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Minimum scratch buffer size guaranteed sufficient for every builder
 * in this file. Sized against the worst case: AT+MSGHEX/AT+CMSGHEX
 * with a 242-byte payload (Table 3-3 max) hex-encoded (2 chars/byte)
 * plus the "AT+CMSGHEX=" command prefix (11 chars) plus NUL.
 * 242*2 + 11 + 1 = 496, round up to 512 for headroom under the
 * 528-byte command limit (spec §2.1) while leaving margin for the
 * limit check itself to fire before truncation would occur.
 */
#define LORA_E5_HF_SCRATCH_MIN 512

/* ------------------------------------------------------------------- */
/* Boot / probe                                                         */
/* ------------------------------------------------------------------- */

/** @brief "AT" -- bare liveness probe. Terminal: bare "OK" or any
 *  ERROR (probing a modem that's still booting can return -22 end-
 *  symbol-timeout if bytes are only partially received; that's still
 *  a valid "not ready yet" signal, not a structural fault, so the
 *  caller -- lora_e5_mm_probe() -- should treat ERROR here as
 *  retryable, not immediately fatal). */
int lora_e5_hf_build_probe(struct lora_e5_at_cmd_desc *desc);

/* ------------------------------------------------------------------- */
/* CONFIG sequence -- one builder per sub-command, called in sequence
 * by lora_e5_mm_configure(). Response format for all "+CMD=VALUE"
 * style set commands is assumed to echo "+CMD: VALUE" back, by
 * direct analogy to the confirmed AT+ADR=OFF -> "+ADR: OFF" capture
 * (andresoliva/LoRa-E5 Arduino library session log). [Likely --
 * pattern-consistent with the one directly observed example and with
 * spec §2.3.3's documented "+CMD: RETURN DATA" convention, but MODE/
 * DR/PORT/REPT/CLASS echoes were not individually captured and
 * observed -- verify against real hardware with AT+LOG enabled
 * before relying on exact remainder text in production; the terminal
 * matches below use LORA_E5_AT_MATCH_ANY_URC (prefix-only) rather
 * than exact-remainder matching specifically to tolerate this
 * uncertainty without risking a false non-match / hang.]
 * ------------------------------------------------------------------- */

/** @brief AT+MODE=LWOTAA or AT+MODE=LWABP. */
int lora_e5_hf_build_mode(struct lora_e5_at_cmd_desc *desc,
			   enum lora_e5_join_type join_type);

/**
 * @brief AT+ID=DevEui,<hex> / AT+ID=AppEui,<hex> (OTAA) or
 * AT+ID=DevAddr,<hex> (ABP).
 *
 * SET SYNTAX CONFIRMED [Certain, real hardware capture 2026-07-05,
 * LoRa-E5-HF firmware V4.0.11]: `AT+ID=DevEui,26C518F8EF840E5D`
 * (plain contiguous hex, no colons -- the AT+KEY analogy held) was
 * accepted and echoed back as `+ID: DevEui, 26:C5:18:F8:EF:84:0E:5D`
 * -- i.e. the SET echo uses the same colon-separated display format
 * as the QUERY response, even though the SET argument itself is
 * plain hex. Round-tripped against the device's own existing DevEui
 * value, so no identity change occurred. Only DevEui's field name was
 * exercised directly; AppEui/DevAddr are assumed to share the same
 * `AT+ID=<field>,<hex>` syntax by construction (same command, same
 * field-name/value shape) but were not independently sent.
 *
 * @param field  0 = DevEui, 1 = AppEui, 2 = DevAddr (kept as an int
 *               rather than a new enum until the set syntax itself is
 *               confirmed -- not worth stabilizing an API around an
 *               unverified wire format).
 */
int lora_e5_hf_build_id_set_eui(struct lora_e5_at_cmd_desc *desc,
				 int field, const uint8_t *bytes, size_t len,
				 char *scratch, size_t scratch_len);

/** @brief "AT+ID" bare query, no arguments. Returns exactly three
 *  "+ID:" lines (DevAddr, DevEui, AppEui) in every capture examined
 *  [Certain on the 3-line shape and field order]. Terminal condition
 *  uses the required_matches=3 mechanism (added Phase 3 to
 *  lora_e5_at.h) rather than resolving on the first "+ID:" line --
 *  see lora_e5_hf_commands.c for the implementation. No trailing
 *  terminator after the third line has been directly observed in any
 *  capture; this relies on exactly 3 occurrences always being
 *  correct. [Likely -- consistent across every capture seen, but if a
 *  firmware revision ever adds a 4th ID field this would need
 *  updating; not something to discover silently in the field, so
 *  lora_e5_mm_get_ids() should log a warning if a 4th "+ID:" line
 *  arrives as an unmatched URC after this resolves.]
 */
int lora_e5_hf_build_id_query(struct lora_e5_at_cmd_desc *desc);

/** @brief AT+KEY=APPKEY,<32 hex chars> (OTAA). Confirmed set syntax
 *  [Certain, CampusIoT captured log]. Terminal condition uses prefix-
 *  only match on "KEY" -- the spec explicitly states keys are
 *  "unreadable for security" (Table 4-2 region), so this deliberately
 *  does NOT assume the key value is echoed back in the remainder;
 *  matching on prefix alone is correct regardless of what the
 *  remainder actually contains. */
int lora_e5_hf_build_key_appkey(struct lora_e5_at_cmd_desc *desc,
				 const struct lora_e5_key16 *key,
				 char *scratch, size_t scratch_len);

/** @brief AT+KEY=NWKSKEY,<32 hex chars> (ABP). Same confidence/
 *  terminal-matching notes as lora_e5_hf_build_key_appkey(). */
int lora_e5_hf_build_key_nwkskey(struct lora_e5_at_cmd_desc *desc,
				  const struct lora_e5_key16 *key,
				  char *scratch, size_t scratch_len);

/** @brief AT+KEY=APPSKEY,<32 hex chars> (ABP). */
int lora_e5_hf_build_key_appskey(struct lora_e5_at_cmd_desc *desc,
				  const struct lora_e5_key16 *key,
				  char *scratch, size_t scratch_len);

/**
 * @brief AT+DR=<region>, e.g. "AT+DR=EU868". Confirmed set syntax and
 * ALL TWELVE region strings [Certain]: EU868/US915/AU915/AS923/
 * KR920/IN865 from Seeed product spec sheet + Hackster tutorial
 * captures; CN779/EU433/US915HYBRID/AU915OLD/CN470/RU864 spelling
 * (one word, no underscores -- "US915HYBRID" and "AU915OLD" are
 * confirmed NOT "US915_HYBRID"/"AU915_OLD", both of which the device
 * rejects with ERROR(-1)) directly confirmed via real hardware
 * capture 2026-07-05, LoRa-E5-HF firmware V4.0.11, device set to each
 * of the six in turn and read back via `AT+DR=<region>` ->
 * `+DR: <region>` echo, then reverted to the device's original IN865.
 */
int lora_e5_hf_build_dr_region(struct lora_e5_at_cmd_desc *desc,
				enum lora_e5_region region,
				char *scratch, size_t scratch_len);

/** @brief AT+CLASS=A. Fixed string -- v1 never sends B or C (Phase 1
 *  decision #1, no Class B/C support at all, not even as a
 *  parameter value). */
int lora_e5_hf_build_class_a(struct lora_e5_at_cmd_desc *desc);

/** @brief AT+PORT=<1..255>. */
int lora_e5_hf_build_port(struct lora_e5_at_cmd_desc *desc, uint8_t port,
			   char *scratch, size_t scratch_len);

/** @brief AT+ADR=ON or AT+ADR=OFF. Confirmed set syntax AND confirmed
 *  echo format "+ADR: OFF" [Certain, andresoliva captured log]. This
 *  is the one CONFIG sub-command where exact-remainder matching is
 *  actually safe to use rather than prefix-only. */
int lora_e5_hf_build_adr(struct lora_e5_at_cmd_desc *desc, bool enable);

/** @brief AT+REPT=<1..15>. Unconfirmed message repetition count. */
int lora_e5_hf_build_rept(struct lora_e5_at_cmd_desc *desc, uint8_t count,
			   char *scratch, size_t scratch_len);

/** @brief AT+RETRY=<0..254>. Confirmed set syntax [Certain, Seeed
 *  wiki relay-function captured log: "AT+RETRY=1"]. Values <2 mean
 *  only one attempt total (spec §4.17). */
int lora_e5_hf_build_retry(struct lora_e5_at_cmd_desc *desc, uint8_t count,
			    char *scratch, size_t scratch_len);

/* ------------------------------------------------------------------- */
/* Join                                                                  */
/* ------------------------------------------------------------------- */

/**
 * @brief AT+JOIN. Terminal on "+JOIN: Done" (success) or "+JOIN:
 * Join failed" (failure) -- both confirmed sequences [Certain,
 * Seeed wiki relay-function log + andresoliva captured log]. Also
 * terminates on "+JOIN: LoRaWAN modem is busy" and "+JOIN: Joined
 * already" as distinct result tags -- these are semantic outcomes,
 * not transport errors, and must not trigger AT-manager retry (see
 * desc->max_retries == 0 set by this builder; retry/backoff policy
 * for join lives in the FSM/recovery layer per Phase 1 §8).
 *
 * "+JOIN: Start", "+JOIN: NORMAL", "+JOIN: Network joined", and
 * "+JOIN: NetID ... DevAddr ..." are intermediate URCs, NOT in the
 * terminal set -- they are forwarded to the Modem Manager's URC
 * classifier and cached (DevAddr, NetID) for the eventual JOIN_RESULT
 * event payload, but do not resolve the transaction.
 */
int lora_e5_hf_build_join(struct lora_e5_at_cmd_desc *desc);

/* ------------------------------------------------------------------- */
/* Send                                                                  */
/* ------------------------------------------------------------------- */

/**
 * @brief AT+MSGHEX=<hex> (unconfirmed) or AT+CMSGHEX=<hex>
 * (confirmed). Raw bytes hex-encoded internally -- no string-
 * escaping burden pushed to the caller (Phase 1 §7 API deviation).
 *
 * CRITICAL, confirmed via disk91.com captured log: the response
 * prefix matches the COMMAND NAME used, i.e. "+MSGHEX: Start" /
 * "+MSGHEX: Done" for MSGHEX, NOT "+MSG: ...". Terminal events below
 * use "MSGHEX"/"CMSGHEX" as the prefix accordingly -- this was wrong
 * in the Phase 2 sketch and is corrected here.
 *
 * @param confirmed  Selects MSGHEX vs CMSGHEX.
 */
int lora_e5_hf_build_send(struct lora_e5_at_cmd_desc *desc,
			   const uint8_t *data, size_t len, uint8_t port,
			   bool confirmed, char *scratch, size_t scratch_len);

/* ------------------------------------------------------------------- */
/* Sleep / wake                                                          */
/* ------------------------------------------------------------------- */

/** @brief AT+LOWPOWER or AT+LOWPOWER=<ms>. Terminal on "+LOWPOWER:
 *  SLEEP". */
int lora_e5_hf_build_lowpower(struct lora_e5_at_cmd_desc *desc,
			       uint32_t duration_ms,
			       char *scratch, size_t scratch_len);

/* ------------------------------------------------------------------- */
/* Reset / factory reset                                                 */
/* ------------------------------------------------------------------- */

/** @brief AT+RESET. Terminal on "+RESET: OK" (confirmed §4.x return
 *  format convention). */
int lora_e5_hf_build_reset(struct lora_e5_at_cmd_desc *desc);

/** @brief AT+FDEFAULT. Terminal on "+FDEFAULT: OK". NOTE: wipes to
 *  factory default which is LWABP mode [Certain, Table 4-2] -- caller
 *  (lora_e5_mm_factory_reset()) must trigger a full CONFIG re-pass
 *  afterward regardless of the application's actual join_type. */
int lora_e5_hf_build_fdefault(struct lora_e5_at_cmd_desc *desc);

/* ------------------------------------------------------------------- */
/* Identity / capability queries                                         */
/* ------------------------------------------------------------------- */

/** @brief AT+VER. Terminal on any "+VER: ..." line (single-line
 *  response per every capture seen). */
int lora_e5_hf_build_ver_query(struct lora_e5_at_cmd_desc *desc);

/**
 * @brief AT+LW=LEN max-payload query.
 *
 * SUBCOMMAND SYNTAX CONFIRMED [Certain, real hardware capture
 * 2026-07-05, LoRa-E5-HF firmware V4.0.11]: `AT+LW=LEN` (bare `AT+LW`
 * with no subcommand returns `ERROR(-1)`) returned `+LW: LEN, 51` on
 * a device configured for IN865/DR0. Other AT+LW subcommands
 * (CDR/ULDL/NET/DC/MC/THLD) were also exercised and confirmed valid
 * in the same session.
 *
 * This builder now returns a real descriptor. It does NOT resolve
 * VERIFICATION_NEEDED.md item 2's implementation blocker, though: the
 * numeric value ("51") is carried in the matched line's remainder
 * text, and struct lora_e5_at_result has no field to expose that back
 * to the caller -- the same architecture gap documented on
 * lora_e5_mm_get_version() in lora_e5_modem_manager.c. Do not call
 * this builder from lora_e5_mm_get_max_payload() and trust its return
 * code as if it carries the parsed value; that function still
 * explicitly returns -ENOTSUP for that reason.
 */
int lora_e5_hf_build_max_payload_query(struct lora_e5_at_cmd_desc *desc);

/**
 * @brief AT+LW=NET public/private network query.
 *
 * CORRECTED [Certain, primary spec PDF §4.28.4, checked 2026-07-11 after
 * this codebase briefly mislabeled this as a join-status query --
 * see docs/VERIFICATION_NEEDED.md for the full correction]: "NET"
 * selects/reports the standard LoRaWAN public-vs-private network sync
 * word (`AT+LW=NET,"ON/OFF"` -- ON = public network, OFF = private), a
 * static configuration setting. It has NOTHING to do with join/session
 * status, despite the name being easy to misread that way -- do not
 * build a "is the modem joined" check on top of this query. If a
 * join-status check is ever needed, the spec-documented mechanism is
 * AT+JOIN's own "+JOIN: Joined already" response (§4.5.2,
 * LORA_E5_MM_TAG_JOIN_ALREADY in te_join[] below) -- not a bare status
 * query.
 *
 * SUBCOMMAND CONFIRMED [Certain, real hardware capture, 2026-07-05,
 * LoRa-E5-HF firmware V4.0.11]: `AT+LW=NET` returned `+LW: NET, ON`.
 * `lora_e5_mm_get_public_network_mode()` logs a warning rather than
 * guessing if a captured_text value other than "NET, ON"/"NET, OFF" is
 * ever seen, instead of silently misreporting it either way.
 */
int lora_e5_hf_build_public_network_query(struct lora_e5_at_cmd_desc *desc);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_HF_COMMANDS_H_ */
