/**
 * @file lora_e5_hf_commands.c
 * @brief Implementation of LoRa-E5-HF command builders. See
 * lora_e5_hf_commands.h for per-command confidence notes -- this file
 * does not repeat them, only implements what's declared there.
 */

#include "lora_e5_hf_commands.h"
#include "lora_e5_modem_manager.h" /* same directory (src/) --
					    * modem manager is internal,
					    * not under include/lora_e5/ */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <zephyr/sys/util.h> /* ARRAY_SIZE, ARG_UNUSED */

/* ------------------------------------------------------------------- */
/* Shared helpers                                                       */
/* ------------------------------------------------------------------- */

static const char hex_digits[] = "0123456789ABCDEF";

/** Encode bytes as uppercase contiguous hex (no separators) --
 *  matches the confirmed AT+KEY format and the disk91.com-captured
 *  AT+MSGHEX format (both use plain contiguous hex, not space-
 *  separated -- the spec's quoted "AA BB CC" form is an alternative
 *  input syntax, not required).
 *  @return number of chars written (2*len), or -ENOSPC if it doesn't fit.
 */
static int hex_encode(const uint8_t *data, size_t len, char *out, size_t out_cap)
{
	if (out_cap < (2 * len + 1)) {
		return -ENOSPC;
	}
	for (size_t i = 0; i < len; i++) {
		out[2 * i]     = hex_digits[(data[i] >> 4) & 0xF];
		out[2 * i + 1] = hex_digits[data[i] & 0xF];
	}
	out[2 * len] = '\0';
	return (int)(2 * len);
}

/* Every descriptor gets this catch-all so a structural/transport
 * error always resolves the transaction instead of stalling to
 * timeout -- see Phase 1 §8.1 fault classification and
 * lora_e5_at_error_is_structural() in lora_e5_types.h, which the
 * Modem Manager's result callback (not this file) uses to decide
 * whether the recovery ladder gets involved.
 */
#define ANY_ERROR_ENTRY \
	{ .prefix = NULL, .remainder = NULL, \
	  .match_mode = LORA_E5_AT_MATCH_ANY_ERROR, \
	  .result_tag = LORA_E5_MM_TAG_GENERIC_ERROR }

/* ------------------------------------------------------------------- */
/* AT (probe)                                                            */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_probe[] = {
	/* ANY_URC (prefix-only), not BARE_OK -- confirmed against real
	 * hardware (firmware V4.0.11): "AT" gets back "+AT: OK", not a
	 * bare "OK". BARE_OK requires line->prefix[0] == '\0'
	 * (line_matches_entry(), lora_e5_cmd_queue.c), which "+AT: OK"
	 * (prefix="AT") never satisfies, so the probe step silently never
	 * completed -- same pre-existing-bug class as te_reset/te_fdefault
	 * above (see docs/VERIFICATION_NEEDED.md).
	 */
	{ .prefix = "AT", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_probe(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.timeout_ms = 0,   /* caller (lora_e5_mm_probe) sets this
				    * from CONFIG_LORA_E5_CMD_TIMEOUT_MS --
				    * left at 0 here as a deliberate "you
				    * must set this" signal, not a real
				    * zero-timeout request. */
		.max_retries = 0,  /* probe retry loop lives in
				    * lora_e5_mm_probe(), which resubmits
				    * this descriptor -- not double-retried
				    * at the AT-manager layer too. */
		.terminal_events = te_probe,
		.terminal_event_count = ARRAY_SIZE(te_probe),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* MODE                                                                  */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_mode[] = {
	{ .prefix = "MODE", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_mode(struct lora_e5_at_cmd_desc *desc,
			   enum lora_e5_join_type join_type)
{
	static const char cmd_otaa[] = "AT+MODE=LWOTAA";
	static const char cmd_abp[]  = "AT+MODE=LWABP";

	if (desc == NULL) {
		return -EINVAL;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.terminal_events = te_mode,
		.terminal_event_count = ARRAY_SIZE(te_mode),
	};

	if (join_type == LORA_E5_JOIN_OTAA) {
		desc->cmd = cmd_otaa;
		desc->cmd_len = sizeof(cmd_otaa) - 1;
	} else {
		desc->cmd = cmd_abp;
		desc->cmd_len = sizeof(cmd_abp) - 1;
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* ID (set) -- confirmed real hardware, see header doc comment           */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_id[] = {
	{ .prefix = "ID", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_id_set_eui(struct lora_e5_at_cmd_desc *desc,
				 int field, const uint8_t *bytes, size_t len,
				 char *scratch, size_t scratch_len)
{
	static const char *field_names[] = { "DevEui", "AppEui", "DevAddr" };
	int hex_len;
	int n;

	if (desc == NULL || bytes == NULL || scratch == NULL) {
		return -EINVAL;
	}
	if (field < 0 || field > 2) {
		return -EINVAL;
	}

	n = snprintf(scratch, scratch_len, "AT+ID=%s,", field_names[field]);
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	hex_len = hex_encode(bytes, len, scratch + n, scratch_len - (size_t)n);
	if (hex_len < 0) {
		return hex_len;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n + (size_t)hex_len,
		.terminal_events = te_id,
		.terminal_event_count = ARRAY_SIZE(te_id),
	};
	return 0;
}

static const struct lora_e5_at_terminal_event te_id_query[] = {
	{ .prefix = "ID", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK,
	  .required_matches = 3 }, /* DevAddr, DevEui, AppEui -- see
				     * header doc comment on why 3 and the
				     * risk if a firmware revision adds a
				     * 4th field. */
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_id_query(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+ID";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.terminal_events = te_id_query,
		.terminal_event_count = ARRAY_SIZE(te_id_query),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* KEY                                                                   */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_key[] = {
	{ .prefix = "KEY", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

static int build_key_common(struct lora_e5_at_cmd_desc *desc,
			     const char *key_name,
			     const struct lora_e5_key16 *key,
			     char *scratch, size_t scratch_len)
{
	int n;
	int hex_len;

	if (desc == NULL || key == NULL || scratch == NULL) {
		return -EINVAL;
	}

	n = snprintf(scratch, scratch_len, "AT+KEY=%s,", key_name);
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	hex_len = hex_encode(key->bytes, sizeof(key->bytes),
			      scratch + n, scratch_len - (size_t)n);
	if (hex_len < 0) {
		return hex_len;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n + (size_t)hex_len,
		.terminal_events = te_key,
		.terminal_event_count = ARRAY_SIZE(te_key),
	};
	return 0;
}

int lora_e5_hf_build_key_appkey(struct lora_e5_at_cmd_desc *desc,
				 const struct lora_e5_key16 *key,
				 char *scratch, size_t scratch_len)
{
	return build_key_common(desc, "APPKEY", key, scratch, scratch_len);
}

int lora_e5_hf_build_key_nwkskey(struct lora_e5_at_cmd_desc *desc,
				  const struct lora_e5_key16 *key,
				  char *scratch, size_t scratch_len)
{
	return build_key_common(desc, "NWKSKEY", key, scratch, scratch_len);
}

int lora_e5_hf_build_key_appskey(struct lora_e5_at_cmd_desc *desc,
				  const struct lora_e5_key16 *key,
				  char *scratch, size_t scratch_len)
{
	return build_key_common(desc, "APPSKEY", key, scratch, scratch_len);
}

/* ------------------------------------------------------------------- */
/* DR (region)                                                           */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_dr[] = {
	{ .prefix = "DR", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

/* Confidence per-entry: all twelve [Certain]. EU868/US915/AU915/
 * AS923/KR920/IN865 from secondary-source captures; the remaining six
 * (US915HYBRID/CN779/EU433/AU915OLD/CN470/RU864) confirmed via real
 * hardware capture 2026-07-05, FW V4.0.11 -- see header doc comment.
 */
static const char *region_strings[] = {
	[LORA_E5_REGION_EU868]       = "EU868",
	[LORA_E5_REGION_US915]       = "US915",
	[LORA_E5_REGION_US915HYBRID] = "US915HYBRID",
	[LORA_E5_REGION_CN779]       = "CN779",
	[LORA_E5_REGION_EU433]       = "EU433",
	[LORA_E5_REGION_AU915]       = "AU915",
	[LORA_E5_REGION_AU915OLD]    = "AU915OLD",
	[LORA_E5_REGION_CN470]       = "CN470",
	[LORA_E5_REGION_AS923]       = "AS923",
	[LORA_E5_REGION_KR920]       = "KR920",
	[LORA_E5_REGION_IN865]       = "IN865",
	[LORA_E5_REGION_RU864]       = "RU864",
};

int lora_e5_hf_build_dr_region(struct lora_e5_at_cmd_desc *desc,
				enum lora_e5_region region,
				char *scratch, size_t scratch_len)
{
	int n;

	if (desc == NULL || scratch == NULL) {
		return -EINVAL;
	}
	if ((size_t)region >= ARRAY_SIZE(region_strings) ||
	    region_strings[region] == NULL) {
		return -EINVAL;
	}

	n = snprintf(scratch, scratch_len, "AT+DR=%s", region_strings[region]);
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n,
		.terminal_events = te_dr,
		.terminal_event_count = ARRAY_SIZE(te_dr),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* CLASS (A only, v1)                                                    */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_class[] = {
	{ .prefix = "CLASS", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_class_a(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+CLASS=A";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.terminal_events = te_class,
		.terminal_event_count = ARRAY_SIZE(te_class),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* PORT                                                                  */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_port[] = {
	{ .prefix = "PORT", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_port(struct lora_e5_at_cmd_desc *desc, uint8_t port,
			   char *scratch, size_t scratch_len)
{
	int n;

	if (desc == NULL || scratch == NULL || port == 0) {
		return -EINVAL;
	}

	n = snprintf(scratch, scratch_len, "AT+PORT=%u", (unsigned)port);
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n,
		.terminal_events = te_port,
		.terminal_event_count = ARRAY_SIZE(te_port),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* ADR -- confirmed exact-remainder match is safe here                   */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_adr_on[] = {
	{ .prefix = "ADR", .remainder = "ON",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};
static const struct lora_e5_at_terminal_event te_adr_off[] = {
	{ .prefix = "ADR", .remainder = "OFF",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_adr(struct lora_e5_at_cmd_desc *desc, bool enable)
{
	static const char cmd_on[]  = "AT+ADR=ON";
	static const char cmd_off[] = "AT+ADR=OFF";

	if (desc == NULL) {
		return -EINVAL;
	}

	if (enable) {
		desc->cmd = cmd_on;
		desc->cmd_len = sizeof(cmd_on) - 1;
		desc->terminal_events = te_adr_on;
		desc->terminal_event_count = ARRAY_SIZE(te_adr_on);
	} else {
		desc->cmd = cmd_off;
		desc->cmd_len = sizeof(cmd_off) - 1;
		desc->terminal_events = te_adr_off;
		desc->terminal_event_count = ARRAY_SIZE(te_adr_off);
	}
	desc->timeout_ms = 0;
	desc->max_retries = 0;
	desc->user_data = NULL;
	return 0;
}

/* ------------------------------------------------------------------- */
/* REPT / RETRY                                                          */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_rept[] = {
	{ .prefix = "REPT", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_rept(struct lora_e5_at_cmd_desc *desc, uint8_t count,
			   char *scratch, size_t scratch_len)
{
	int n;

	if (desc == NULL || scratch == NULL || count == 0 || count > 15) {
		return -EINVAL;
	}

	n = snprintf(scratch, scratch_len, "AT+REPT=%u", (unsigned)count);
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n,
		.terminal_events = te_rept,
		.terminal_event_count = ARRAY_SIZE(te_rept),
	};
	return 0;
}

static const struct lora_e5_at_terminal_event te_retry[] = {
	{ .prefix = "RETRY", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_retry(struct lora_e5_at_cmd_desc *desc, uint8_t count,
			    char *scratch, size_t scratch_len)
{
	int n;

	if (desc == NULL || scratch == NULL) {
		return -EINVAL;
	}

	n = snprintf(scratch, scratch_len, "AT+RETRY=%u", (unsigned)count);
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n,
		.terminal_events = te_retry,
		.terminal_event_count = ARRAY_SIZE(te_retry),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* JOIN                                                                   */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_join[] = {
	{ .prefix = "JOIN", .remainder = "Done",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_JOIN_DONE },
	{ .prefix = "JOIN", .remainder = "Join failed",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_JOIN_FAILED },
	{ .prefix = "JOIN", .remainder = "LoRaWAN modem is busy",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_JOIN_BUSY },
	{ .prefix = "JOIN", .remainder = "Joined already",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_JOIN_ALREADY },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_join(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+JOIN";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.max_retries = 0, /* Phase 1 §8: join retry/backoff policy
				    * lives in the FSM/recovery layer, not
				    * here. */
		.terminal_events = te_join,
		.terminal_event_count = ARRAY_SIZE(te_join),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* SEND (MSGHEX / CMSGHEX)                                               */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_msghex[] = {
	{ .prefix = "MSGHEX", .remainder = "Done",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_MSG_DONE },
	{ .prefix = "MSGHEX", .remainder = "LoRaWAN modem is busy",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_MSG_BUSY },
	{ .prefix = "MSGHEX", .remainder = "Please join network first",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_MSG_NOT_JOINED },
	{ .prefix = "MSGHEX", .remainder = "No free channel",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_NO_FREE_CHANNEL },
	{ .prefix = "MSGHEX", .remainder = "No band",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_NO_BAND },
	{ .prefix = "MSGHEX", .remainder = "DR error",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_DR_ERROR },
	{ .prefix = "MSGHEX", .remainder = "Length error",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_LENGTH_ERROR },
	ANY_ERROR_ENTRY,
};

static const struct lora_e5_at_terminal_event te_cmsghex[] = {
	{ .prefix = "CMSGHEX", .remainder = "Done",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_CMSG_DONE_ACKED },
	/* NOTE: "Done" alone does not distinguish acked-vs-not-acked --
	 * the Modem Manager's result callback must have already observed
	 * (or not) a preceding "+CMSG: ACK Received" URC to disambiguate
	 * LORA_E5_MM_TAG_CMSG_DONE_ACKED vs ...NO_ACK. This terminal
	 * table alone cannot make that distinction; it is state the
	 * Modem Manager tracks across the URC callback + this terminal
	 * match, which is exactly why that classification logic belongs
	 * one layer up (lora_e5_modem_manager.c, not yet implemented)
	 * rather than in this stateless table.
	 */
	{ .prefix = "CMSGHEX", .remainder = "LoRaWAN modem is busy",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_MSG_BUSY },
	{ .prefix = "CMSGHEX", .remainder = "Please join network first",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_MSG_NOT_JOINED },
	{ .prefix = "CMSGHEX", .remainder = "No free channel",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_NO_FREE_CHANNEL },
	{ .prefix = "CMSGHEX", .remainder = "No band",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_NO_BAND },
	{ .prefix = "CMSGHEX", .remainder = "DR error",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_DR_ERROR },
	{ .prefix = "CMSGHEX", .remainder = "Length error",
	  .match_mode = LORA_E5_AT_MATCH_STARTSWITH,
	  .result_tag = LORA_E5_MM_TAG_MSG_LENGTH_ERROR },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_send(struct lora_e5_at_cmd_desc *desc,
			   const uint8_t *data, size_t len, uint8_t port,
			   bool confirmed, char *scratch, size_t scratch_len)
{
	int n;
	int hex_len;
	const char *cmd_name = confirmed ? "AT+CMSGHEX=" : "AT+MSGHEX=";

	/* PORT is set via a separate AT+PORT= transaction during CONFIG
	 * (or re-issued if the caller changes port between sends) --
	 * MSGHEX/CMSGHEX themselves take no port parameter per every
	 * captured example ("AT+MSGHEX=<hex>" with no extra field). The
	 * `port` parameter here is accepted for API symmetry with
	 * struct lora_e5_config but currently unused in the wire format
	 * -- flagging rather than silently dropping it, since a caller
	 * passing a port different from the last AT+PORT= setting would
	 * get a send on the WRONG port with no error, which is a real
	 * correctness trap for lora_e5_mm_send()'s implementation to
	 * handle explicitly (reissue AT+PORT= first if it differs from
	 * the last-configured value).
	 */
	(void)port;

	if (desc == NULL || data == NULL || scratch == NULL) {
		return -EINVAL;
	}

	n = snprintf(scratch, scratch_len, "%s", cmd_name);
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	hex_len = hex_encode(data, len, scratch + n, scratch_len - (size_t)n);
	if (hex_len < 0) {
		return hex_len;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n + (size_t)hex_len,
		.max_retries = 0, /* Phase 1 §8: TX semantic failures are
				    * not transport faults -- no AT-manager
				    * retry. */
	};

	if (confirmed) {
		desc->terminal_events = te_cmsghex;
		desc->terminal_event_count = ARRAY_SIZE(te_cmsghex);
	} else {
		desc->terminal_events = te_msghex;
		desc->terminal_event_count = ARRAY_SIZE(te_msghex);
	}
	return 0;
}

/* ------------------------------------------------------------------- */
/* LOWPOWER                                                               */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_lowpower[] = {
	{ .prefix = "LOWPOWER", .remainder = "SLEEP",
	  .match_mode = LORA_E5_AT_MATCH_EXACT,
	  .result_tag = LORA_E5_MM_TAG_LOWPOWER_SLEEP },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_lowpower(struct lora_e5_at_cmd_desc *desc,
			       uint32_t duration_ms,
			       char *scratch, size_t scratch_len)
{
	int n;

	if (desc == NULL || scratch == NULL) {
		return -EINVAL;
	}

	if (duration_ms == 0) {
		n = snprintf(scratch, scratch_len, "AT+LOWPOWER");
	} else {
		n = snprintf(scratch, scratch_len, "AT+LOWPOWER=%u",
			     (unsigned)duration_ms);
	}
	if (n < 0 || (size_t)n >= scratch_len) {
		return -ENOSPC;
	}

	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = scratch,
		.cmd_len = (size_t)n,
		.terminal_events = te_lowpower,
		.terminal_event_count = ARRAY_SIZE(te_lowpower),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* RESET / FDEFAULT                                                       */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_reset[] = {
	/* ANY_URC (prefix-only), not EXACT -- "+RESET: OK" classifies as
	 * kind==LORA_E5_AT_LINE_OK with NO remainder text at all (see
	 * lora_e5_parser.c's "+PREFIX: OK" handling: it returns immediately
	 * without ever setting line->remainder), so an EXACT match against
	 * remainder="OK" can never fire -- line->remainder is NULL. Matches
	 * the same prefix-only convention already used by te_mode/te_port/
	 * every other simple confirm-only command in this file. Pre-existing
	 * bug found and fixed while getting tests/modem_manager building
	 * green again (see docs/VERIFICATION_NEEDED.md).
	 */
	{ .prefix = "RESET", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_RESET_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_reset(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+RESET";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.terminal_events = te_reset,
		.terminal_event_count = ARRAY_SIZE(te_reset),
	};
	return 0;
}

static const struct lora_e5_at_terminal_event te_fdefault[] = {
	/* ANY_URC, not EXACT -- same pre-existing bug/fix as te_reset above. */
	{ .prefix = "FDEFAULT", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_FDEFAULT_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_fdefault(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+FDEFAULT";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.terminal_events = te_fdefault,
		.terminal_event_count = ARRAY_SIZE(te_fdefault),
	};
	return 0;
}

/* ------------------------------------------------------------------- */
/* VER / max payload query                                               */
/* ------------------------------------------------------------------- */

static const struct lora_e5_at_terminal_event te_ver[] = {
	{ .prefix = "VER", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_ver_query(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+VER";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.terminal_events = te_ver,
		.terminal_event_count = ARRAY_SIZE(te_ver),
	};
	return 0;
}

static const struct lora_e5_at_terminal_event te_lw_len[] = {
	{ .prefix = "LW", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_max_payload_query(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+LW=LEN";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.terminal_events = te_lw_len,
		.terminal_event_count = ARRAY_SIZE(te_lw_len),
	};
	return 0;
}

static const struct lora_e5_at_terminal_event te_lw_net[] = {
	{ .prefix = "LW", .remainder = NULL,
	  .match_mode = LORA_E5_AT_MATCH_ANY_URC,
	  .result_tag = LORA_E5_MM_TAG_OK },
	ANY_ERROR_ENTRY,
};

int lora_e5_hf_build_public_network_query(struct lora_e5_at_cmd_desc *desc)
{
	static const char cmd[] = "AT+LW=NET";

	if (desc == NULL) {
		return -EINVAL;
	}
	*desc = (struct lora_e5_at_cmd_desc){
		.cmd = cmd,
		.cmd_len = sizeof(cmd) - 1,
		.terminal_events = te_lw_net,
		.terminal_event_count = ARRAY_SIZE(te_lw_net),
	};
	return 0;
}
