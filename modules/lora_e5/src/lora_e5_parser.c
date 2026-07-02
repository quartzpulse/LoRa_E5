/**
 * @file lora_e5_parser.c
 * @brief Pure line classifier. No UART interaction, no retained state
 * between calls (aside from the static prefix-copy helper, which is
 * stateless itself). Fully unit-testable on native_sim/host without
 * any Zephyr kernel dependency beyond the types pulled in from
 * lora_e5_at.h.
 *
 * Classification rules implemented here, derived from the AT Command
 * Specification V1.0 §2.3.3 ("Return" format) and cross-checked
 * against captured real-device session logs (see call-site comments
 * for citations -- this file does not invent response shapes):
 *
 *   "OK"                     -> LORA_E5_AT_LINE_OK,   prefix=""
 *   "+PREFIX: OK"             -> LORA_E5_AT_LINE_OK,   prefix="PREFIX"
 *   "ERROR(-N)"               -> LORA_E5_AT_LINE_ERROR, prefix=""
 *   "+PREFIX: ERROR(-N)"      -> LORA_E5_AT_LINE_ERROR, prefix="PREFIX"
 *   "+PREFIX: <anything else>" -> LORA_E5_AT_LINE_URC,  prefix="PREFIX",
 *                                  remainder points past "PREFIX: "
 *   "+PREFIX <anything else>"  -> LORA_E5_AT_LINE_URC (space-separated
 *                                  variant -- not observed in captured
 *                                  logs for this firmware, but the
 *                                  §2.3.3 format description does not
 *                                  rule it out; handled defensively)
 *   anything else              -> LORA_E5_AT_LINE_UNRECOGNIZED
 *
 * NOTE: response text case is assumed to match the documented/
 * captured examples exactly (mixed case, e.g. "+JOIN: Done"). The
 * §2.1 "command is case insensitive" rule applies to commands the
 * HOST sends, not to what the modem returns -- matching is
 * case-sensitive here. [Likely -- no counter-example seen in any
 * captured log, but not a rule explicitly stated for responses.]
 */

#include "lora_e5/lora_e5_at.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* ------------------------------------------------------------------- */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------- */

static void copy_prefix(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
	size_t n = src_len < (dst_cap - 1) ? src_len : (dst_cap - 1);

	memcpy(dst, src, n);
	dst[n] = '\0';
}

/**
 * @brief Parse "ERROR(-N)" or the remainder-half of "+CMD: ERROR(-N)".
 *
 * @param text  Points at 'E' of "ERROR(...)".
 * @param len   Length of text from that point to end of line.
 * @param out   Parsed code on success.
 * @return true if this matched the ERROR(-N) shape.
 */
static bool try_parse_error_code(const char *text, size_t len, int *out)
{
	static const char prefix[] = "ERROR(";
	const size_t prefix_len = sizeof(prefix) - 1;
	const char *paren_close;
	char numbuf[8];
	size_t num_len;
	char *endptr;
	long val;

	if (len < prefix_len + 2) { /* "ERROR(" + at least "N)" */
		return false;
	}
	if (memcmp(text, prefix, prefix_len) != 0) {
		return false;
	}

	paren_close = memchr(text + prefix_len, ')', len - prefix_len);
	if (paren_close == NULL) {
		return false;
	}

	num_len = (size_t)(paren_close - (text + prefix_len));
	if (num_len == 0 || num_len >= sizeof(numbuf)) {
		return false;
	}

	memcpy(numbuf, text + prefix_len, num_len);
	numbuf[num_len] = '\0';

	val = strtol(numbuf, &endptr, 10);
	if (endptr == numbuf || *endptr != '\0') {
		return false; /* not a clean integer */
	}

	*out = (int)val;
	return true;
}

/* ------------------------------------------------------------------- */
/* Public entry point                                                   */
/* ------------------------------------------------------------------- */

int lora_e5_at_parse_line(const char *line, size_t len, struct lora_e5_at_line *out)
{
	const char *p;
	const char *colon;
	const char *remainder_start;
	size_t remainder_len;
	size_t prefix_len;
	int err_code;

	if (line == NULL || out == NULL) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));
	out->error_code = (enum lora_e5_at_error)0;

	if (len == 0) {
		out->kind = LORA_E5_AT_LINE_UNRECOGNIZED;
		return 0;
	}

	/* Bare "OK" -- exact match only. A line that merely starts with
	 * "OK" but has trailing garbage is NOT treated as OK; that would
	 * mask real protocol errors (e.g. framing corruption appending
	 * stray bytes). Surfaced as UNRECOGNIZED instead so the AT
	 * Command Manager doesn't silently resolve a corrupted exchange
	 * as success.
	 */
	if (len == 2 && line[0] == 'O' && line[1] == 'K') {
		out->kind = LORA_E5_AT_LINE_OK;
		return 0;
	}

	/* Bare "ERROR(-N)" with no '+' prefix. */
	if (line[0] == 'E') {
		if (try_parse_error_code(line, len, &err_code)) {
			out->kind = LORA_E5_AT_LINE_ERROR;
			out->error_code = (enum lora_e5_at_error)err_code;
			return 0;
		}
		out->kind = LORA_E5_AT_LINE_UNRECOGNIZED;
		return 0;
	}

	/* Everything else of interest starts with '+'. */
	if (line[0] != '+') {
		out->kind = LORA_E5_AT_LINE_UNRECOGNIZED;
		return 0;
	}

	p = line + 1;

	/* Find end of prefix token: up to ':' or ' ', whichever comes
	 * first, or end of line if neither is present (a bare "+FOO"
	 * with nothing after it -- not observed in any captured log,
	 * handled defensively as a URC with empty remainder rather than
	 * rejected outright).
	 */
	colon = memchr(p, ':', len - 1);
	{
		const char *space = memchr(p, ' ', len - 1);

		if (colon != NULL && (space == NULL || colon < space)) {
			prefix_len = (size_t)(colon - p);
			remainder_start = colon + 1;
		} else if (space != NULL) {
			prefix_len = (size_t)(space - p);
			remainder_start = space + 1;
		} else {
			prefix_len = len - 1;
			remainder_start = p + prefix_len;
		}
	}

	if (prefix_len == 0 || prefix_len > LORA_E5_AT_PREFIX_MAX) {
		/* Empty prefix ("+: ...") or absurdly long token -- neither
		 * matches any documented command name. Surface rather than
		 * guess.
		 */
		out->kind = LORA_E5_AT_LINE_UNRECOGNIZED;
		return 0;
	}

	copy_prefix(out->prefix, sizeof(out->prefix), p, prefix_len);

	/* Skip exactly one leading space after "PREFIX:" if present --
	 * §2.3.3 format is "+CMD: RETURN DATA", single space after colon
	 * in every captured example.
	 */
	{
		const char *line_end = line + len;

		if (remainder_start < line_end && *remainder_start == ' ') {
			remainder_start++;
		}
		remainder_len = (size_t)(line_end - remainder_start);
	}

	/* "+PREFIX: OK" */
	if (remainder_len == 2 && remainder_start[0] == 'O' && remainder_start[1] == 'K') {
		out->kind = LORA_E5_AT_LINE_OK;
		return 0;
	}

	/* "+PREFIX: ERROR(-N)" */
	if (remainder_len >= 1 && remainder_start[0] == 'E') {
		if (try_parse_error_code(remainder_start, remainder_len, &err_code)) {
			out->kind = LORA_E5_AT_LINE_ERROR;
			out->error_code = (enum lora_e5_at_error)err_code;
			return 0;
		}
	}

	/* Anything else is a URC candidate -- terminal-vs-unsolicited is
	 * decided one layer up (AT Command Manager), not here. This
	 * function's job stops at classification.
	 */
	out->kind = LORA_E5_AT_LINE_URC;
	out->remainder = remainder_start;
	out->remainder_len = remainder_len;
	return 0;
}
