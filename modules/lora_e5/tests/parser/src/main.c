/**
 * @file tests/parser/src/main.c
 * @brief Unit tests for lora_e5_at_parse_line().
 *
 * Test vectors are drawn either directly from the AT Command
 * Specification V1.0's own documented "Return" examples, or from
 * captured real-device session logs cited inline -- none of these
 * are invented response shapes. Where a vector's provenance is a
 * secondary source rather than the primary spec PDF, that is noted.
 *
 * Runs on native_sim -- no hardware, no UART, no Zephyr kernel
 * primitives beyond what ztest itself pulls in.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include "lora_e5/lora_e5_at.h"

static void expect(const char *line, enum lora_e5_at_line_kind kind,
		    const char *prefix, const char *remainder,
		    enum lora_e5_at_error error_code)
{
	struct lora_e5_at_line out;
	int rc;

	rc = lora_e5_at_parse_line(line, strlen(line), &out);
	zassert_equal(rc, 0, "parse_line returned %d for \"%s\"", rc, line);
	zassert_equal(out.kind, kind, "wrong kind for \"%s\"", line);

	if (prefix != NULL) {
		zassert_str_equal(out.prefix, prefix,
				   "wrong prefix for \"%s\": got \"%s\"",
				   line, out.prefix);
	}

	if (remainder != NULL) {
		zassert_equal(out.remainder_len, strlen(remainder),
			      "wrong remainder length for \"%s\"", line);
		zassert_mem_equal(out.remainder, remainder, strlen(remainder),
				   "wrong remainder for \"%s\"", line);
	}

	if (kind == LORA_E5_AT_LINE_ERROR) {
		zassert_equal(out.error_code, error_code,
			      "wrong error code for \"%s\"", line);
	}
}

/* ------------------------------------------------------------------- */
/* Bare OK / ERROR                                                      */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_parser, test_bare_ok)
{
	expect("OK", LORA_E5_AT_LINE_OK, "", NULL, 0);
}

ZTEST(lora_e5_parser, test_ok_with_trailing_garbage_is_unrecognized)
{
	/* Regression guard: "OK" must be an EXACT match. A corrupted
	 * line like "OKX" must never resolve a command as successful --
	 * see lora_e5_parser.c comment on why partial-match OK is unsafe.
	 */
	struct lora_e5_at_line out;

	zassert_equal(lora_e5_at_parse_line("OKX", 3, &out), 0, NULL);
	zassert_equal(out.kind, LORA_E5_AT_LINE_UNRECOGNIZED, NULL);
}

ZTEST(lora_e5_parser, test_bare_error_code)
{
	/* Table 2-1, error code -1 "Parameters is invalid". */
	expect("ERROR(-1)", LORA_E5_AT_LINE_ERROR, "", NULL,
	       LORA_E5_AT_ERR_INVALID_PARAM);
}

ZTEST(lora_e5_parser, test_all_documented_error_codes)
{
	/* Full Table 2-1 -- every code the modem can return. */
	static const struct {
		const char *line;
		enum lora_e5_at_error code;
	} vectors[] = {
		{ "ERROR(-1)",  LORA_E5_AT_ERR_INVALID_PARAM },
		{ "ERROR(-10)", LORA_E5_AT_ERR_UNKNOWN_CMD },
		{ "ERROR(-11)", LORA_E5_AT_ERR_WRONG_FORMAT },
		{ "ERROR(-12)", LORA_E5_AT_ERR_WRONG_MODE },
		{ "ERROR(-20)", LORA_E5_AT_ERR_TOO_MANY_PARAMS },
		{ "ERROR(-21)", LORA_E5_AT_ERR_CMD_TOO_LONG },
		{ "ERROR(-22)", LORA_E5_AT_ERR_END_SYMBOL_TIMEOUT },
		{ "ERROR(-23)", LORA_E5_AT_ERR_INVALID_CHAR },
		{ "ERROR(-24)", LORA_E5_AT_ERR_COMPOSITE },
	};

	for (size_t i = 0; i < ARRAY_SIZE(vectors); i++) {
		expect(vectors[i].line, LORA_E5_AT_LINE_ERROR, "", NULL,
		       vectors[i].code);
	}
}

/* ------------------------------------------------------------------- */
/* "+CMD: OK" / "+CMD: ERROR(-N)" shapes                                */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_parser, test_prefixed_ok)
{
	/* AT+RESET's documented return is "+RESET: OK", not bare OK --
	 * this is the exact hazard Phase 1 flagged: not every command
	 * terminates the same way.
	 */
	expect("+RESET: OK", LORA_E5_AT_LINE_OK, "RESET", NULL, 0);
}

ZTEST(lora_e5_parser, test_prefixed_at_ok)
{
	expect("+AT: OK", LORA_E5_AT_LINE_OK, "AT", NULL, 0);
}

/* ------------------------------------------------------------------- */
/* JOIN sequence -- captured from Seeed wiki relay-function log and
 * the andresoliva/LoRa-E5 Arduino library's captured serial session
 * (see Phase 3 review notes for URLs). This is real device output,
 * not a documentation example.
 */

ZTEST(lora_e5_parser, test_join_success_sequence)
{
	expect("+JOIN: Start", LORA_E5_AT_LINE_URC, "JOIN", "Start", 0);
	expect("+JOIN: NORMAL", LORA_E5_AT_LINE_URC, "JOIN", "NORMAL", 0);
	expect("+JOIN: Network joined", LORA_E5_AT_LINE_URC, "JOIN",
	       "Network joined", 0);
	expect("+JOIN: NetID 000000 DevAddr 00:C9:F4:5F", LORA_E5_AT_LINE_URC,
	       "JOIN", "NetID 000000 DevAddr 00:C9:F4:5F", 0);
	expect("+JOIN: Done", LORA_E5_AT_LINE_URC, "JOIN", "Done", 0);
}

ZTEST(lora_e5_parser, test_join_failed)
{
	expect("+JOIN: Join failed", LORA_E5_AT_LINE_URC, "JOIN",
	       "Join failed", 0);
}

/* ------------------------------------------------------------------- */
/* CMSG (confirmed uplink) sequence -- captured from andresoliva/
 * LoRa-E5 Arduino library's real serial log.
 */

ZTEST(lora_e5_parser, test_cmsg_confirmed_uplink_sequence)
{
	expect("+CMSG: Start", LORA_E5_AT_LINE_URC, "CMSG", "Start", 0);
	expect("+CMSG: Wait ACK", LORA_E5_AT_LINE_URC, "CMSG", "Wait ACK", 0);
	expect("+CMSG: ACK Received", LORA_E5_AT_LINE_URC, "CMSG",
	       "ACK Received", 0);
	expect("+CMSG: RXWIN1, RSSI -82, SNR 10.0", LORA_E5_AT_LINE_URC,
	       "CMSG", "RXWIN1, RSSI -82, SNR 10.0", 0);
	expect("+CMSG: Done", LORA_E5_AT_LINE_URC, "CMSG", "Done", 0);
}

/* ------------------------------------------------------------------- */
/* MSGHEX / CMSGHEX -- REGRESSION GUARD for the prefix bug caught
 * during Phase 3 review: hex-variant commands echo with "MSGHEX"/
 * "CMSGHEX" as the prefix, NOT "MSG"/"CMSG". Captured from
 * disk91.com's real serial log.
 */

ZTEST(lora_e5_parser, test_msghex_uses_its_own_prefix_not_msg)
{
	struct lora_e5_at_line out;

	zassert_equal(lora_e5_at_parse_line("+MSGHEX: Start", 14, &out), 0, NULL);
	zassert_equal(out.kind, LORA_E5_AT_LINE_URC, NULL);
	zassert_str_equal(out.prefix, "MSGHEX",
			   "MSGHEX response must NOT be classified under "
			   "the MSG prefix -- got \"%s\"", out.prefix);

	zassert_equal(lora_e5_at_parse_line("+MSGHEX: Done", 13, &out), 0, NULL);
	zassert_str_equal(out.prefix, "MSGHEX", NULL);
}

/* ------------------------------------------------------------------- */
/* ADR -- captured andresoliva log: "AT+ADR=OFF" -> "+ADR: OFF"        */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_parser, test_adr_echo)
{
	expect("+ADR: OFF", LORA_E5_AT_LINE_URC, "ADR", "OFF", 0);
}

/* ------------------------------------------------------------------- */
/* ID query -- three-line response, spec §4.x + CampusIoT captured log */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_parser, test_id_query_three_lines)
{
	expect("+ID: DevAddr, 32:30:84:63", LORA_E5_AT_LINE_URC, "ID",
	       "DevAddr, 32:30:84:63", 0);
	expect("+ID: DevEui, 2C:F7:F1:20:32:30:84:63", LORA_E5_AT_LINE_URC,
	       "ID", "DevEui, 2C:F7:F1:20:32:30:84:63", 0);
	expect("+ID: AppEui, 80:00:00:00:00:00:00:06", LORA_E5_AT_LINE_URC,
	       "ID", "AppEui, 80:00:00:00:00:00:00:06", 0);
}

/* ------------------------------------------------------------------- */
/* Edge cases                                                            */
/* ------------------------------------------------------------------- */

ZTEST(lora_e5_parser, test_empty_line_is_unrecognized_not_crash)
{
	struct lora_e5_at_line out;

	zassert_equal(lora_e5_at_parse_line("", 0, &out), 0, NULL);
	zassert_equal(out.kind, LORA_E5_AT_LINE_UNRECOGNIZED, NULL);
}

ZTEST(lora_e5_parser, test_null_line_rejected)
{
	struct lora_e5_at_line out;

	zassert_equal(lora_e5_at_parse_line(NULL, 0, &out), -EINVAL, NULL);
}

ZTEST(lora_e5_parser, test_garbage_line_is_unrecognized)
{
	expect("this is not an AT response", LORA_E5_AT_LINE_UNRECOGNIZED,
	       NULL, NULL, 0);
}

ZTEST(lora_e5_parser, test_malformed_error_no_closing_paren)
{
	/* "ERROR(-1" with no ')' must not be misparsed as a valid code --
	 * this is exactly the kind of framing-corruption case the
	 * command manager needs to NOT silently treat as terminal.
	 */
	expect("ERROR(-1", LORA_E5_AT_LINE_UNRECOGNIZED, NULL, NULL, 0);
}

ZTEST(lora_e5_parser, test_lowpower_urcs)
{
	expect("+LOWPOWER: SLEEP", LORA_E5_AT_LINE_URC, "LOWPOWER", "SLEEP", 0);
	expect("+LOWPOWER: WAKEUP", LORA_E5_AT_LINE_URC, "LOWPOWER", "WAKEUP", 0);
}

ZTEST_SUITE(lora_e5_parser, NULL, NULL, NULL, NULL, NULL);
