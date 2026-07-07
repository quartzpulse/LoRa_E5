/**
 * @file lora_e5_modem_manager.c
 * @brief Implementation of the Modem Manager -- see
 * lora_e5_modem_manager.h for the full contract. Sections, in order:
 * shared state, small parsing helpers, event-emission helpers, URC
 * classification, CONFIG sequencing, JOIN, SEND (incl. the port-cache
 * fix), SLEEP/WAKE, RESET/FACTORY_RESET, synchronous queries, raw
 * passthrough, then init/lifecycle -- per CLAUDE.md's guidance to
 * split by concern within one file rather than grow a dumping ground.
 */

#include "lora_e5_modem_manager.h"
#include "lora_e5_hf_commands.h"
#include "lora_e5/lora_e5_at.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>

LOG_MODULE_REGISTER(lora_e5_mm, CONFIG_LORA_E5_LOG_LEVEL);

/* Not part of the documented Kconfig symbol list in lora_e5_config.h --
 * deliberately a plain local #define rather than a CONFIG_-prefixed
 * placeholder, so it doesn't get mistaken for a real Kconfig symbol
 * pending wiring.
 */
#define WAKEUP_SETTLE_MS 10 /* spec Sec4.30: >=5ms; small margin added */
#define RESET_GPIO_PULSE_MS 10 /* [Guessing] pulse width/polarity not
				  * confirmed against the module's
				  * datasheet -- see VERIFICATION_NEEDED.md */

/* Max LoRaWAN payload across every supported region/DR (Table 3-3) --
 * sizes the static downlink staging buffer. Matches the rationale in
 * LORA_E5_HF_SCRATCH_MIN's comment (lora_e5_hf_commands.h).
 */
#define MM_DOWNLINK_BUF_SIZE 242

/* ------------------------------------------------------------------- */
/* Shared module state                                                  */
/* ------------------------------------------------------------------- */

/* One shared descriptor + one shared scratch buffer for every AT
 * transaction this module issues. Safe because (a) the AT Command
 * Manager enforces single-in-flight and this module's own operations
 * are only ever chained sequentially from within their own result
 * callbacks, never issued concurrently from two call sites (the FSM,
 * the sole caller, is itself a single state machine issuing at most
 * one logical Modem Manager operation at a time), and (b) per
 * lora_e5_at.h's lora_e5_at_submit() contract, only desc->cmd's buffer
 * and desc->terminal_events need to outlive the submit() call -- the
 * struct lora_e5_at_cmd_desc itself is copied by value into the queue
 * at submit time, so reusing one static instance across calls is safe
 * even before the previous transaction resolves. This mirrors
 * lora_e5_hf_commands.h's own stated design intent for the scratch
 * buffer.
 */
static struct lora_e5_at_cmd_desc g_desc;
static char g_scratch[LORA_E5_HF_SCRATCH_MIN];

static enum lora_e5_reset_backend g_reset_backend;
static const struct gpio_dt_spec *g_reset_gpio;

static lora_e5_mm_event_cb_t g_event_cb;
static void *g_event_cb_user_data;

static bool g_initialized;

/* Port cache -- the VERIFICATION_NEEDED.md item 5 fix. */
static bool g_port_valid;
static uint8_t g_port_cached;

/* JOIN URC cache -- populated from "+JOIN: NetID ... DevAddr ..." while
 * an AT+JOIN transaction is in flight, consumed by the JOIN_RESULT
 * translation once the transaction resolves.
 */
static bool g_join_devaddr_valid;
static struct lora_e5_devaddr g_join_devaddr;
static bool g_join_netid_valid;
static uint32_t g_join_netid;

/* CMSG ack tracking -- disambiguates te_cmsghex's single "Done" tag
 * (LORA_E5_MM_TAG_CMSG_DONE_ACKED, always) into acked-vs-not, per the
 * NOTE on that table in lora_e5_hf_commands.c.
 */
static bool g_cmsg_ack_seen;

/* Pending-send state, needed across the (possible) AT+PORT= reissue ->
 * AT+MSGHEX/CMSGHEX two-step async chain.
 */
struct pending_send {
	const uint8_t *data;
	size_t len;
	uint8_t port;
	bool confirmed;
};
static struct pending_send g_pending_send;

/* Pending downlink RSSI/SNR/window, cached from a "RXWINn, RSSI x, SNR
 * y" URC until the following "PORT: n; RX: hex" URC arrives to
 * complete the downlink event. See handle_msg_urc().
 */
static bool g_pending_rxwin_valid;
static int16_t g_pending_rssi;
static int8_t g_pending_snr;
static enum lora_e5_rx_window g_pending_window;
static uint8_t g_downlink_buf[MM_DOWNLINK_BUF_SIZE];

/* CONFIG sequencing state. */
enum config_step {
	CFG_STEP_MODE = 0,
	CFG_STEP_ID_A,           /* DevEui (OTAA) or DevAddr (ABP) */
	CFG_STEP_ID_B_OR_KEY_A,  /* AppEui (OTAA) or NwkSKey (ABP) */
	CFG_STEP_KEY_B,          /* AppKey (OTAA) or AppSKey (ABP) */
	CFG_STEP_DR,
	CFG_STEP_CLASS,
	CFG_STEP_PORT,
	CFG_STEP_ADR,
	CFG_STEP_REPT,
	CFG_STEP_RETRY,
	CFG_STEP_COUNT,
};

struct config_state {
	struct lora_e5_config cfg;
	int step;
	bool active;
};
static struct config_state g_config;

/* Probe retry state -- lora_e5_mm_probe() owns its own retry loop, not
 * double-retried at the AT-manager layer (see hf_build_probe()'s doc
 * comment and lora_e5_hf_commands.c).
 */
static uint8_t g_probe_retry_count;

static struct k_work_delayable g_reset_gpio_work;
static struct k_work_delayable g_wakeup_settle_work;

/* ------------------------------------------------------------------- */
/* Small parsing helpers                                                */
/* ------------------------------------------------------------------- */

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

static const char *skip_seps(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == ',' || *p == ':' || *p == ';')) {
		p++;
	}
	return p;
}

/** Bounded substring search; returns a pointer just past the match, or
 *  NULL. Not a general strstr() -- deliberately bounded by hay_len
 *  since `hay` is not guaranteed NUL-terminated beyond it.
 */
static const char *find_token(const char *hay, size_t hay_len, const char *needle)
{
	size_t needle_len = strlen(needle);

	if (hay == NULL || needle_len == 0 || hay_len < needle_len) {
		return NULL;
	}
	for (size_t i = 0; i + needle_len <= hay_len; i++) {
		if (memcmp(hay + i, needle, needle_len) == 0) {
			return hay + i + needle_len;
		}
	}
	return NULL;
}

static bool parse_long(const char *p, const char *end, long *out, const char **next)
{
	char buf[16];
	size_t n = 0;
	const char *q = p;
	bool neg = false;

	if (q < end && *q == '-') {
		neg = true;
		q++;
	}
	while (q < end && *q >= '0' && *q <= '9' && n < sizeof(buf) - 1) {
		buf[n++] = *q++;
	}
	if (n == 0) {
		return false;
	}
	buf[n] = '\0';
	*out = strtol(buf, NULL, 10) * (neg ? -1 : 1);
	if (next != NULL) {
		*next = q;
	}
	return true;
}

/** [Guessing] NetID's numeric base (hex vs decimal) is not confirmed --
 *  see VERIFICATION_NEEDED.md. Parsed as hex here, consistent with the
 *  rest of this codebase's convention for ID-shaped fields.
 */
static bool parse_hex_ulong(const char *p, const char *end, unsigned long *out,
			     const char **next)
{
	char buf[16];
	size_t n = 0;
	const char *q = p;

	while (q < end && n < sizeof(buf) - 1 && hex_nibble(*q) >= 0) {
		buf[n++] = *q++;
	}
	if (n == 0) {
		return false;
	}
	buf[n] = '\0';
	*out = strtoul(buf, NULL, 16);
	if (next != NULL) {
		*next = q;
	}
	return true;
}

/** "XX:XX:XX:XX" colon-separated hex, matching the [Certain] format
 *  confirmed for AT+ID query's DevAddr field.
 */
static bool parse_hex_devaddr(const char *p, const char *end, struct lora_e5_devaddr *out)
{
	uint8_t bytes[4];
	const char *q = p;

	for (int i = 0; i < 4; i++) {
		if ((end - q) < 2) {
			return false;
		}
		int hi = hex_nibble(q[0]);
		int lo = hex_nibble(q[1]);

		if (hi < 0 || lo < 0) {
			return false;
		}
		bytes[i] = (uint8_t)((hi << 4) | lo);
		q += 2;
		if (i < 3) {
			if (q < end && *q == ':') {
				q++;
			} else {
				return false;
			}
		}
	}
	memcpy(out->bytes, bytes, sizeof(bytes));
	return true;
}

static int hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap)
{
	if (hex_len == 0 || (hex_len % 2) != 0) {
		return -EINVAL;
	}

	size_t n = hex_len / 2;

	if (n > out_cap) {
		return -ENOSPC;
	}
	for (size_t i = 0; i < n; i++) {
		int hi = hex_nibble(hex[2 * i]);
		int lo = hex_nibble(hex[2 * i + 1]);

		if (hi < 0 || lo < 0) {
			return -EINVAL;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return (int)n;
}

static enum lora_e5_rx_window rx_window_from_int(long win)
{
	switch (win) {
	case 0: return LORA_E5_RX_WINDOW_RX0;
	case 1: return LORA_E5_RX_WINDOW_RX1;
	case 2: return LORA_E5_RX_WINDOW_RX2;
	case 3: return LORA_E5_RX_WINDOW_RX3;
	default: return LORA_E5_RX_WINDOW_UNKNOWN;
	}
}

/* ------------------------------------------------------------------- */
/* Event emission helpers                                               */
/* ------------------------------------------------------------------- */

static void emit_event(const struct lora_e5_fsm_event *evt)
{
	if (g_event_cb != NULL) {
		g_event_cb(evt, g_event_cb_user_data);
	}
}

/**
 * Convention used throughout this file: struct lora_e5_fsm_event's
 * union has no dedicated payload for PROBE_RESULT or UART_FAULT --
 * both reuse the `reset_result` field (0 on success, negative errno
 * otherwise), the closest existing "generic int result" shape already
 * declared in lora_e5_events.h. Changing that is out of scope here
 * (this file implements against the given header contracts).
 *
 * Routes any non-MATCHED outcome (TIMEOUT, UART_ERROR) through
 * LORA_E5_FSM_EVT_UART_FAULT uniformly, regardless of which operation
 * was in flight -- these are transport-level faults with no
 * modem-reported error_code to carry, so they don't fit any
 * operation-specific result event cleanly; the FSM's generic fault
 * path is designed to handle exactly this ("From UART backend, routed
 * through Modem Manager without semantic translation" -- close enough
 * in spirit even though the timeout itself is detected by the AT
 * Command Manager, not literally the UART backend).
 *
 * @return true if this was a transport fault (already handled, caller
 *         should stop further tag-based translation).
 */
static bool handle_transport_fault(const struct lora_e5_at_result *result)
{
	if (result->outcome == LORA_E5_AT_OUTCOME_MATCHED) {
		return false;
	}

	struct lora_e5_fsm_event evt = {
		.type = LORA_E5_FSM_EVT_UART_FAULT,
		.reset_result = (result->outcome == LORA_E5_AT_OUTCOME_TIMEOUT)
			? -ETIMEDOUT : -EIO,
	};

	emit_event(&evt);
	return true;
}

/* ------------------------------------------------------------------- */
/* URC classification                                                   */
/* ------------------------------------------------------------------- */

static void handle_join_urc(const struct lora_e5_at_line *line)
{
	if (line->remainder == NULL) {
		return;
	}

	const char *end = line->remainder + line->remainder_len;
	const char *p = find_token(line->remainder, line->remainder_len, "NetID");

	if (p == NULL) {
		/* "+JOIN: Start" / "+JOIN: NORMAL" etc -- nothing to cache. */
		return;
	}

	p = skip_seps(p, end);
	{
		unsigned long netid;
		const char *after;

		if (parse_hex_ulong(p, end, &netid, &after)) {
			g_join_netid = (uint32_t)netid;
			g_join_netid_valid = true;
			p = after;
		}
	}

	p = find_token(p, (size_t)(end - p), "DevAddr");
	if (p == NULL) {
		return;
	}
	p = skip_seps(p, end);
	if (parse_hex_devaddr(p, end, &g_join_devaddr)) {
		g_join_devaddr_valid = true;
	}
}

/**
 * [Likely] Applies the same "response prefix matches the command name"
 * correction that lora_e5_hf_commands.c already confirmed for the
 * MSGHEX/CMSGHEX terminal Start/Done lines (disk91.com capture) to the
 * REST of that command's URC family (ACK Received, PORT/RX downlink,
 * RXWIN/RSSI/SNR, Link) -- i.e. assumes "+MSGHEX: ..."/"+CMSGHEX: ..."
 * throughout, not "+MSG: .../+CMSG: ..." as Phase1's architecture doc
 * originally assumed before that correction. This is an inference by
 * analogy (the AT+JOIN family shows the same self-consistent pattern:
 * "+JOIN: Start"/"+JOIN: NetID..."/"+JOIN: Done" all share one prefix),
 * not an independently captured confirmation for each sub-line. See
 * VERIFICATION_NEEDED.md.
 */
static void handle_msg_urc(const struct lora_e5_at_line *line)
{
	if (line->remainder == NULL) {
		return;
	}

	const char *end = line->remainder + line->remainder_len;
	static const char ack_text[] = "ACK Received";

	if (line->remainder_len == sizeof(ack_text) - 1 &&
	    memcmp(line->remainder, ack_text, sizeof(ack_text) - 1) == 0) {
		g_cmsg_ack_seen = true;
		return;
	}

	/* [Likely] "RXWINn, RSSI x, SNR y" */
	{
		const char *p = find_token(line->remainder, line->remainder_len, "RXWIN");

		if (p != NULL) {
			long win;

			if (parse_long(p, end, &win, NULL)) {
				g_pending_window = rx_window_from_int(win);
				g_pending_rxwin_valid = true;
			}

			p = find_token(line->remainder, line->remainder_len, "RSSI");
			if (p != NULL) {
				long rssi;

				p = skip_seps(p, end);
				if (parse_long(p, end, &rssi, NULL)) {
					g_pending_rssi = (int16_t)rssi;
				}
			}

			p = find_token(line->remainder, line->remainder_len, "SNR");
			if (p != NULL) {
				long snr;

				p = skip_seps(p, end);
				if (parse_long(p, end, &snr, NULL)) {
					g_pending_snr = (int8_t)snr;
				}
			}
			return;
		}
	}

	/* [Likely] "PORT: n; RX: \"hex\"" */
	{
		const char *p = find_token(line->remainder, line->remainder_len, "PORT");
		long port;
		const char *after;

		if (p == NULL) {
			return;
		}
		p = skip_seps(p, end);
		if (!parse_long(p, end, &port, &after)) {
			return;
		}

		const char *rx = find_token(after, (size_t)(end - after), "RX");

		if (rx == NULL) {
			return;
		}
		rx = skip_seps(rx, end);
		if (rx < end && *rx == '"') {
			rx++;
		}

		const char *hex_start = rx;
		const char *q = rx;

		while (q < end && *q != '"') {
			q++;
		}

		int n = hex_decode(hex_start, (size_t)(q - hex_start),
				    g_downlink_buf, sizeof(g_downlink_buf));

		if (n < 0) {
			LOG_WRN("downlink RX hex decode failed (%d)", n);
			return;
		}

		struct lora_e5_fsm_event evt = {
			.type = LORA_E5_FSM_EVT_DOWNLINK,
			.downlink = {
				.port = (uint8_t)port,
				.data = g_downlink_buf,
				.len = (size_t)n,
				.rssi_dbm = g_pending_rxwin_valid ? g_pending_rssi : 0,
				.snr_db = g_pending_rxwin_valid ? g_pending_snr : 0,
				.window = g_pending_rxwin_valid
					? g_pending_window : LORA_E5_RX_WINDOW_UNKNOWN,
				/* [Guessing] MULTICAST URC detection not
				 * implemented -- see VERIFICATION_NEEDED.md.
				 */
				.is_multicast = false,
			},
		};

		emit_event(&evt);
		g_pending_rxwin_valid = false;
	}
}

static void handle_lowpower_urc(const struct lora_e5_at_line *line)
{
	static const char wakeup_text[] = "WAKEUP";

	if (line->remainder == NULL) {
		return;
	}
	/* "+LOWPOWER: SLEEP" is the terminal response to a submitted
	 * AT+LOWPOWER command, resolved via te_lowpower's terminal_events[]
	 * entry -- this URC path only ever sees lines that did NOT resolve
	 * an in-flight command, so only WAKEUP (which can arrive with no
	 * command in flight at all, e.g. after a timed sleep the modem
	 * wakes itself from) is handled here.
	 */
	if (line->remainder_len == sizeof(wakeup_text) - 1 &&
	    memcmp(line->remainder, wakeup_text, sizeof(wakeup_text) - 1) == 0) {
		struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_WAKE_RESULT };

		emit_event(&evt);
	}
}

static void mm_urc_cb(const struct lora_e5_at_line *line)
{
	if (line->kind == LORA_E5_AT_LINE_ERROR) {
		LOG_WRN("unsolicited ERROR(%d) line, prefix=\"%s\"",
			(int)line->error_code, line->prefix);
		return;
	}

	if (strcmp(line->prefix, "JOIN") == 0) {
		handle_join_urc(line);
		return;
	}
	if (strcmp(line->prefix, "MSGHEX") == 0 || strcmp(line->prefix, "CMSGHEX") == 0) {
		handle_msg_urc(line);
		return;
	}
	if (strcmp(line->prefix, "LOWPOWER") == 0) {
		handle_lowpower_urc(line);
		return;
	}

	LOG_DBG("unclassified URC: prefix=\"%s\"", line->prefix);
}

/* ------------------------------------------------------------------- */
/* CONFIG sequencing                                                     */
/* ------------------------------------------------------------------- */

static void mm_config_step_cb(const struct lora_e5_at_result *result);

static int submit_config_step(void)
{
	int rc;

	switch (g_config.step) {
	case CFG_STEP_MODE:
		rc = lora_e5_hf_build_mode(&g_desc, g_config.cfg.join_type);
		break;
	case CFG_STEP_ID_A:
		if (g_config.cfg.join_type == LORA_E5_JOIN_OTAA) {
			rc = lora_e5_hf_build_id_set_eui(
				&g_desc, 0, g_config.cfg.otaa.dev_eui.bytes,
				sizeof(g_config.cfg.otaa.dev_eui.bytes),
				g_scratch, sizeof(g_scratch));
		} else {
			rc = lora_e5_hf_build_id_set_eui(
				&g_desc, 2, g_config.cfg.abp.dev_addr.bytes,
				sizeof(g_config.cfg.abp.dev_addr.bytes),
				g_scratch, sizeof(g_scratch));
		}
		break;
	case CFG_STEP_ID_B_OR_KEY_A:
		if (g_config.cfg.join_type == LORA_E5_JOIN_OTAA) {
			rc = lora_e5_hf_build_id_set_eui(
				&g_desc, 1, g_config.cfg.otaa.app_eui.bytes,
				sizeof(g_config.cfg.otaa.app_eui.bytes),
				g_scratch, sizeof(g_scratch));
		} else {
			rc = lora_e5_hf_build_key_nwkskey(
				&g_desc, &g_config.cfg.abp.nwk_skey,
				g_scratch, sizeof(g_scratch));
		}
		break;
	case CFG_STEP_KEY_B:
		if (g_config.cfg.join_type == LORA_E5_JOIN_OTAA) {
			rc = lora_e5_hf_build_key_appkey(
				&g_desc, &g_config.cfg.otaa.app_key,
				g_scratch, sizeof(g_scratch));
		} else {
			rc = lora_e5_hf_build_key_appskey(
				&g_desc, &g_config.cfg.abp.app_skey,
				g_scratch, sizeof(g_scratch));
		}
		break;
	case CFG_STEP_DR:
		rc = lora_e5_hf_build_dr_region(&g_desc, g_config.cfg.region,
						 g_scratch, sizeof(g_scratch));
		break;
	case CFG_STEP_CLASS:
		rc = lora_e5_hf_build_class_a(&g_desc);
		break;
	case CFG_STEP_PORT:
		rc = lora_e5_hf_build_port(&g_desc, g_config.cfg.port,
					    g_scratch, sizeof(g_scratch));
		break;
	case CFG_STEP_ADR:
		rc = lora_e5_hf_build_adr(&g_desc, g_config.cfg.adr_enable);
		break;
	case CFG_STEP_REPT:
		rc = lora_e5_hf_build_rept(&g_desc, g_config.cfg.unconfirmed_repeats,
					    g_scratch, sizeof(g_scratch));
		break;
	case CFG_STEP_RETRY:
		rc = lora_e5_hf_build_retry(&g_desc, g_config.cfg.confirmed_retries,
					     g_scratch, sizeof(g_scratch));
		break;
	default:
		g_config.active = false;
		return 0;
	}

	if (rc != 0) {
		g_config.active = false;
		return rc;
	}

	/* Config-step commands are ordinary, non-semantic AT transactions
	 * -- Phase1 Sec8.1 classifies transport timeouts on these as
	 * retryable at the AT-manager layer, unlike JOIN/MSG whose
	 * hf_build_*() functions deliberately fix max_retries=0 themselves
	 * (semantic retry belongs to the FSM for those). The builders here
	 * leave timeout_ms/max_retries at 0 as a "caller must set this"
	 * signal (same convention as hf_build_probe()) -- set them here.
	 */
	g_desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;
	g_desc.max_retries = CONFIG_LORA_E5_MAX_RETRIES;

	return lora_e5_at_submit(&g_desc, mm_config_step_cb);
}

static void mm_config_step_cb(const struct lora_e5_at_result *result)
{
	if (handle_transport_fault(result)) {
		g_config.active = false;
		return;
	}

	struct lora_e5_fsm_event evt = {
		.type = LORA_E5_FSM_EVT_CONFIG_STEP_RESULT,
		.config_step_result = {
			.error = (result->result_tag == LORA_E5_MM_TAG_OK)
				? 0 : result->error_code,
			.is_last_step = (result->result_tag == LORA_E5_MM_TAG_OK) &&
				(g_config.step + 1 >= CFG_STEP_COUNT),
		},
	};

	emit_event(&evt);

	if (result->result_tag != LORA_E5_MM_TAG_OK) {
		/* Structural/semantic config failure -- halt the sequence
		 * here rather than proceeding or auto-retrying whole-hog;
		 * recovery policy (Phase1 Sec8) is the FSM's call, which
		 * must invoke lora_e5_mm_configure() again from scratch to
		 * retry -- this module does not resume mid-sequence.
		 */
		g_config.active = false;
		return;
	}

	if (g_config.step == CFG_STEP_PORT) {
		/* VERIFICATION_NEEDED.md item 5 fix: CONFIG's own AT+PORT=
		 * step is itself an authoritative port configuration, so
		 * seed the send-time port cache here too, not only from
		 * lora_e5_mm_send()'s own reissue path.
		 */
		g_port_cached = g_config.cfg.port;
		g_port_valid = true;
	}

	g_config.step++;
	if (g_config.step >= CFG_STEP_COUNT) {
		g_config.active = false;
		return;
	}

	int rc = submit_config_step();

	if (rc != 0) {
		LOG_ERR("CONFIG step %d failed to submit (%d)", g_config.step, rc);
		g_config.active = false;
	}
}

int lora_e5_mm_configure(const struct lora_e5_config *cfg)
{
	if (!g_initialized) {
		return -EINVAL;
	}
	if (cfg == NULL) {
		return -EINVAL;
	}
	if (cfg->dev_class != LORA_E5_CLASS_A) {
		/* Phase1 decision #1 / lora_e5_types.h: Class B/C not
		 * supported in v1, not even as a parameter value --
		 * hf_build_class_a() can't even express anything else, so
		 * reject here rather than silently sending AT+CLASS=A
		 * against the caller's differing request.
		 */
		return -ENOTSUP;
	}

	g_config.cfg = *cfg;
	g_config.step = CFG_STEP_MODE;
	g_config.active = true;

	return submit_config_step();
}

/* ------------------------------------------------------------------- */
/* JOIN                                                                  */
/* ------------------------------------------------------------------- */

static void mm_join_result_cb(const struct lora_e5_at_result *result)
{
	if (handle_transport_fault(result)) {
		return;
	}

	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_JOIN_RESULT };

	switch (result->result_tag) {
	case LORA_E5_MM_TAG_JOIN_DONE:
	case LORA_E5_MM_TAG_JOIN_ALREADY: /* already joined == success state */
		evt.join_result.outcome = LORA_E5_JOIN_OUTCOME_SUCCESS;
		break;
	case LORA_E5_MM_TAG_JOIN_FAILED:
	case LORA_E5_MM_TAG_JOIN_BUSY:
	/* enum lora_e5_join_outcome has no dedicated BUSY variant -- FAILED
	 * is the closest fit available in the current event vocabulary.
	 * Design trade-off, not a protocol guess; see VERIFICATION_NEEDED.md.
	 */
	case LORA_E5_MM_TAG_GENERIC_ERROR:
	default:
		evt.join_result.outcome = LORA_E5_JOIN_OUTCOME_FAILED;
		break;
	}

	if (evt.join_result.outcome == LORA_E5_JOIN_OUTCOME_SUCCESS) {
		if (g_join_devaddr_valid) {
			evt.join_result.dev_addr = g_join_devaddr;
		}
		if (g_join_netid_valid) {
			evt.join_result.net_id = g_join_netid;
		}
	}

	emit_event(&evt);
}

int lora_e5_mm_join(enum lora_e5_join_type join_type)
{
	if (!g_initialized) {
		return -EINVAL;
	}
	if (join_type == LORA_E5_JOIN_ABP) {
		struct lora_e5_fsm_event evt = {
			.type = LORA_E5_FSM_EVT_JOIN_RESULT,
			.join_result = {
				.outcome = LORA_E5_JOIN_OUTCOME_ABP_SKIP,
				.dev_addr = g_config.cfg.abp.dev_addr,
				/* LoRaWAN ABP has no join handshake -- no
				 * NetID is ever reported for ABP activation
				 * [Certain, no join exchange occurs at all].
				 */
				.net_id = 0,
			},
		};

		emit_event(&evt);
		return 0;
	}

	g_join_devaddr_valid = false;
	g_join_netid_valid = false;

	int rc = lora_e5_hf_build_join(&g_desc);

	if (rc != 0) {
		return rc;
	}
	g_desc.timeout_ms = CONFIG_LORA_E5_JOIN_TIMEOUT_MS;

	return lora_e5_at_submit(&g_desc, mm_join_result_cb);
}

/* ------------------------------------------------------------------- */
/* SEND (MSGHEX/CMSGHEX) -- includes the port-cache fix                  */
/* ------------------------------------------------------------------- */

static void mm_send_result_cb(const struct lora_e5_at_result *result)
{
	if (handle_transport_fault(result)) {
		return;
	}

	struct lora_e5_fsm_event evt = {
		.type = LORA_E5_FSM_EVT_TX_RESULT,
		.tx_result = { .confirmed = g_pending_send.confirmed },
	};

	switch (result->result_tag) {
	case LORA_E5_MM_TAG_MSG_DONE:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_NONE;
		break;
	case LORA_E5_MM_TAG_CMSG_DONE_ACKED:
		/* te_cmsghex's "Done" entry always reports this tag
		 * regardless of ack status (see the NOTE in
		 * lora_e5_hf_commands.c) -- disambiguate using the
		 * "ACK Received" URC observed (or not) earlier in this same
		 * transaction via handle_msg_urc().
		 */
		evt.tx_result.fail_reason = g_cmsg_ack_seen
			? LORA_E5_TX_FAIL_NONE : LORA_E5_TX_FAIL_NO_ACK;
		break;
	case LORA_E5_MM_TAG_MSG_BUSY:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_BUSY;
		break;
	case LORA_E5_MM_TAG_MSG_NOT_JOINED:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_NOT_JOINED;
		break;
	case LORA_E5_MM_TAG_MSG_NO_FREE_CHANNEL:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_NO_FREE_CHANNEL;
		break;
	case LORA_E5_MM_TAG_MSG_NO_BAND:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_NO_BAND;
		break;
	case LORA_E5_MM_TAG_MSG_DR_ERROR:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_DR_ERROR;
		break;
	case LORA_E5_MM_TAG_MSG_LENGTH_ERROR:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_LENGTH_ERROR;
		break;
	case LORA_E5_MM_TAG_GENERIC_ERROR:
	default:
		evt.tx_result.fail_reason = LORA_E5_TX_FAIL_MODEM_ERROR;
		break;
	}

	emit_event(&evt);
}

static int submit_send_command(void)
{
	int rc = lora_e5_hf_build_send(&g_desc, g_pending_send.data, g_pending_send.len,
					g_pending_send.port, g_pending_send.confirmed,
					g_scratch, sizeof(g_scratch));

	if (rc != 0) {
		return rc;
	}
	g_desc.timeout_ms = CONFIG_LORA_E5_TX_TIMEOUT_MS;

	return lora_e5_at_submit(&g_desc, mm_send_result_cb);
}

static void mm_port_reissue_cb(const struct lora_e5_at_result *result)
{
	if (handle_transport_fault(result)) {
		return;
	}

	if (result->result_tag != LORA_E5_MM_TAG_OK) {
		struct lora_e5_fsm_event evt = {
			.type = LORA_E5_FSM_EVT_TX_RESULT,
			.tx_result = {
				.confirmed = g_pending_send.confirmed,
				.fail_reason = LORA_E5_TX_FAIL_MODEM_ERROR,
			},
		};

		emit_event(&evt);
		return;
	}

	g_port_cached = g_pending_send.port;
	g_port_valid = true;

	int rc = submit_send_command();

	if (rc != 0) {
		struct lora_e5_fsm_event evt = {
			.type = LORA_E5_FSM_EVT_TX_RESULT,
			.tx_result = {
				.confirmed = g_pending_send.confirmed,
				.fail_reason = LORA_E5_TX_FAIL_MODEM_ERROR,
			},
		};

		emit_event(&evt);
	}
}

int lora_e5_mm_send(const uint8_t *data, size_t len, uint8_t port, bool confirmed)
{
	if (!g_initialized) {
		return -EINVAL;
	}
	if (data == NULL || port == 0) {
		return -EINVAL;
	}

	g_pending_send.data = data;
	g_pending_send.len = len;
	g_pending_send.port = port;
	g_pending_send.confirmed = confirmed;
	g_cmsg_ack_seen = false;
	g_pending_rxwin_valid = false;

	if (!g_port_valid || g_port_cached != port) {
		/* VERIFICATION_NEEDED.md item 5 fix: AT+MSGHEX/AT+CMSGHEX
		 * carry no port argument on the wire -- port is a separate,
		 * sticky AT+PORT= transaction. Reissue it first whenever the
		 * caller's requested port differs from the last one actually
		 * confirmed on the modem (by CONFIG or a prior send), so a
		 * send never silently goes out on a stale port.
		 */
		int rc = lora_e5_hf_build_port(&g_desc, port, g_scratch, sizeof(g_scratch));

		if (rc != 0) {
			return rc;
		}
		g_desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;
		g_desc.max_retries = CONFIG_LORA_E5_MAX_RETRIES;

		return lora_e5_at_submit(&g_desc, mm_port_reissue_cb);
	}

	return submit_send_command();
}

/* ------------------------------------------------------------------- */
/* SLEEP / WAKE                                                          */
/* ------------------------------------------------------------------- */

static void mm_sleep_result_cb(const struct lora_e5_at_result *result)
{
	if (handle_transport_fault(result)) {
		return;
	}

	/* No LORA_E5_FSM_EVT_SLEEP_RESULT exists in lora_e5_events.h.
	 * Per Phase1 Sec5.2's transition table, JOINED --SLEEP(API call)-->
	 * SLEEP happens synchronously on the *request*, not on AT+LOWPOWER's
	 * own confirmation -- so a successful "+LOWPOWER: SLEEP" match
	 * needs no event here. A rejected/errored LOWPOWER has nowhere to
	 * go in the current event vocabulary; logged only. Design-trade-off
	 * gap, flagged in VERIFICATION_NEEDED.md, not silently guessed
	 * around by inventing a new event type in this file.
	 */
	if (result->result_tag != LORA_E5_MM_TAG_LOWPOWER_SLEEP) {
		LOG_ERR("AT+LOWPOWER rejected (error %d) -- no FSM event "
			"exists to carry this; see VERIFICATION_NEEDED.md",
			(int)result->error_code);
	}
}

int lora_e5_mm_sleep(uint32_t duration_ms)
{
	if (!g_initialized) {
		return -EINVAL;
	}

	int rc = lora_e5_hf_build_lowpower(&g_desc, duration_ms, g_scratch, sizeof(g_scratch));

	if (rc != 0) {
		return rc;
	}
	g_desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;

	return lora_e5_at_submit(&g_desc, mm_sleep_result_cb);
}

static void wakeup_settle_expired(struct k_work *work)
{
	ARG_UNUSED(work);

	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_WAKE_RESULT };

	emit_event(&evt);
}

static void mm_wakeup_probe_cb(const struct lora_e5_at_result *result)
{
	if (handle_transport_fault(result)) {
		return;
	}
	/* Settle delay (>=5ms, spec Sec4.30) before the next command may be
	 * issued -- enforced via delayed work rather than a blocking sleep,
	 * since this module must not block its caller's (FSM) work queue.
	 * lora_e5_mm_init()'s params carry no work queue for this module to
	 * schedule on (Modem Manager "does NOT own a work queue of its
	 * own" per this file's header doc) -- k_work_schedule() targets the
	 * system work queue, the only one available to this module; a tiny
	 * one-shot delay here is a reasonable exception to Phase1's general
	 * "don't use the system work queue" guidance, which was scoped to
	 * the larger RX/FSM processing queues, not incidental short delays
	 * like this.
	 */
	k_work_schedule(&g_wakeup_settle_work, K_MSEC(WAKEUP_SETTLE_MS));
}

int lora_e5_mm_wakeup(void)
{
	/* lora_e5_at.h exposes no raw-byte transport primitive to this
	 * layer -- lora_e5_at_send_fn_t is registered once by whoever owns
	 * the UART (lora_e5_at_set_transport()) and is never exposed back
	 * to callers for an ad-hoc single-byte write; every write this
	 * module can issue goes through lora_e5_at_submit()'s queued-
	 * command model, which expects a full AT command with a terminal
	 * response, not a single wake byte with no reply at all.
	 *
	 * "AT" is reused here as a practical wake mechanism instead: its
	 * bytes wake a UART-listening modem the same as any other byte
	 * would per spec Sec4.30, and its "+AT: OK" reply doubles as a
	 * liveness confirmation. This is a genuine architecture gap, not a
	 * wire-protocol guess -- flagged in VERIFICATION_NEEDED.md. A
	 * cleaner fix would add a raw-send hook to lora_e5_at.h.
	 */
	if (!g_initialized) {
		return -EINVAL;
	}

	int rc = lora_e5_hf_build_probe(&g_desc);

	if (rc != 0) {
		return rc;
	}
	g_desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;
	g_desc.max_retries = 0;

	return lora_e5_at_submit(&g_desc, mm_wakeup_probe_cb);
}

/* ------------------------------------------------------------------- */
/* RESET / FACTORY_RESET                                                 */
/* ------------------------------------------------------------------- */

static void mm_reset_at_cb(const struct lora_e5_at_result *result)
{
	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_RESET_RESULT };

	switch (result->outcome) {
	case LORA_E5_AT_OUTCOME_MATCHED:
		evt.reset_result = (result->result_tag == LORA_E5_MM_TAG_RESET_OK) ? 0 : -EIO;
		break;
	case LORA_E5_AT_OUTCOME_TIMEOUT:
		evt.reset_result = -ETIMEDOUT;
		break;
	case LORA_E5_AT_OUTCOME_UART_ERROR:
	default:
		evt.reset_result = -EIO;
		break;
	}

	/* Plain AT+RESET is a reboot, not a factory wipe -- provisioning
	 * (including the last-configured port) persists in NVM, so the
	 * port cache is intentionally NOT invalidated here. Contrast with
	 * mm_fdefault_cb() below.
	 */
	emit_event(&evt);
}

static void reset_gpio_deassert(struct k_work *work)
{
	ARG_UNUSED(work);

	int rc = gpio_pin_set_dt(g_reset_gpio, 0);
	struct lora_e5_fsm_event evt = {
		.type = LORA_E5_FSM_EVT_RESET_RESULT,
		.reset_result = rc,
	};

	emit_event(&evt);
}

int lora_e5_mm_reset(void)
{
	if (!g_initialized) {
		return -EINVAL;
	}
	if (g_reset_backend == LORA_E5_RESET_BACKEND_GPIO) {
		if (g_reset_gpio == NULL) {
			return -EINVAL;
		}

		int rc = gpio_pin_set_dt(g_reset_gpio, 1);

		if (rc != 0) {
			return rc;
		}
		/* [Guessing] pulse width/active polarity not confirmed
		 * against the module's datasheet -- see
		 * VERIFICATION_NEEDED.md.
		 */
		k_work_schedule(&g_reset_gpio_work, K_MSEC(RESET_GPIO_PULSE_MS));
		return 0;
	}

	int rc = lora_e5_hf_build_reset(&g_desc);

	if (rc != 0) {
		return rc;
	}
	g_desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;

	return lora_e5_at_submit(&g_desc, mm_reset_at_cb);
}

static void mm_fdefault_cb(const struct lora_e5_at_result *result)
{
	struct lora_e5_fsm_event evt = { .type = LORA_E5_FSM_EVT_RESET_RESULT };

	switch (result->outcome) {
	case LORA_E5_AT_OUTCOME_MATCHED:
		if (result->result_tag == LORA_E5_MM_TAG_FDEFAULT_OK) {
			/* AT+FDEFAULT wipes ALL provisioning, including the
			 * modem's own AT+PORT= setting -- unlike plain
			 * AT+RESET, the port cache must NOT survive this
			 * (VERIFICATION_NEEDED.md item 5: a stale cache here
			 * would suppress a needed AT+PORT= reissue after the
			 * modem's port reverted to factory default).
			 */
			g_port_valid = false;
			evt.reset_result = 0;
		} else {
			evt.reset_result = -EIO;
		}
		break;
	case LORA_E5_AT_OUTCOME_TIMEOUT:
		evt.reset_result = -ETIMEDOUT;
		break;
	case LORA_E5_AT_OUTCOME_UART_ERROR:
	default:
		evt.reset_result = -EIO;
		break;
	}

	emit_event(&evt);
}

int lora_e5_mm_factory_reset(void)
{
	if (!g_initialized) {
		return -EINVAL;
	}

	int rc = lora_e5_hf_build_fdefault(&g_desc);

	if (rc != 0) {
		return rc;
	}
	g_desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;

	return lora_e5_at_submit(&g_desc, mm_fdefault_cb);
}

/* ------------------------------------------------------------------- */
/* Synchronous capability/identity queries                               */
/* ------------------------------------------------------------------- */

int lora_e5_mm_get_version(struct lora_e5_version *out, k_timeout_t timeout)
{
	struct lora_e5_at_cmd_desc desc;
	struct lora_e5_at_result result;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}

	rc = lora_e5_hf_build_ver_query(&desc);
	if (rc != 0) {
		return rc;
	}
	desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;

	rc = lora_e5_at_submit_sync(&desc, timeout, &result);
	if (rc != 0) {
		return rc;
	}
	if (result.result_tag != LORA_E5_MM_TAG_OK) {
		return -EIO;
	}

	/* GENUINE ARCHITECTURE GAP, not a wire-format guess: struct
	 * lora_e5_at_result carries only outcome/result_tag/error_code/
	 * user_data -- no captured line text. A line that matches a
	 * terminal_events[] entry (te_ver's ANY_URC "VER" match, here) is
	 * explicitly excluded from the URC callback by
	 * lora_e5_cmd_queue_process_line() ("if (!matched && urc_cb !=
	 * NULL) urc_cb(line)"), so there is NO path in the current
	 * lora_e5_at.h contract by which this module can read the actual
	 * "+VER: x.y.z" value back. The AT transaction itself resolved
	 * successfully (checked above) -- only the value extraction is
	 * impossible against the current contract. Returning a fabricated
	 * struct lora_e5_version here would be worse than an explicit
	 * refusal -- see VERIFICATION_NEEDED.md.
	 */
	memset(out, 0, sizeof(*out));
	return -ENOTSUP;
}

int lora_e5_mm_get_ids(struct lora_e5_ids *out, k_timeout_t timeout)
{
	struct lora_e5_at_cmd_desc desc;
	struct lora_e5_at_result result;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}

	rc = lora_e5_hf_build_id_query(&desc);
	if (rc != 0) {
		return rc;
	}
	desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;

	rc = lora_e5_at_submit_sync(&desc, timeout, &result);
	if (rc != 0) {
		return rc;
	}
	if (result.result_tag != LORA_E5_MM_TAG_OK) {
		return -EIO;
	}

	/* Same architecture gap as lora_e5_mm_get_version() above, worse
	 * here since AT+ID's required_matches=3 mechanism consumes all
	 * three "+ID:" lines (DevAddr/DevEui/AppEui) as partial matches --
	 * none of the three ever reach the URC callback either. See that
	 * function's comment and VERIFICATION_NEEDED.md.
	 */
	memset(out, 0, sizeof(*out));
	return -ENOTSUP;
}

int lora_e5_mm_get_max_payload(size_t *out, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);

	if (out == NULL) {
		return -EINVAL;
	}
	*out = 0;

	/* AT+LW=LEN's wire syntax is now confirmed on real hardware (see
	 * VERIFICATION_NEEDED.md "Resolved" and
	 * lora_e5_hf_build_max_payload_query()'s doc comment) -- the
	 * builder returns a real descriptor. But extracting the numeric
	 * value from "+LW: LEN, <n>" hits the same architecture gap as
	 * lora_e5_mm_get_version() above: no path in the current
	 * lora_e5_at.h contract exposes a matched terminal line's text
	 * back to this module. Refuse explicitly rather than return a
	 * fabricated value.
	 */
	return -ENOTSUP;
}

/* ------------------------------------------------------------------- */
/* Raw passthrough                                                       */
/* ------------------------------------------------------------------- */

int lora_e5_mm_at_raw(const char *cmd, size_t cmd_len,
		       char *resp_buf, size_t resp_buf_len,
		       k_timeout_t timeout)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(cmd_len);
	ARG_UNUSED(resp_buf);
	ARG_UNUSED(resp_buf_len);
	ARG_UNUSED(timeout);

	/* NOT SAFELY IMPLEMENTABLE against the current lora_e5_at.h
	 * match-mode vocabulary. The documented terminal condition is
	 * "bare OK, +<ANY>: OK, or ERROR(-N)" -- but enum
	 * lora_e5_at_match_mode has no mode meaning "kind==OK regardless
	 * of prefix": LORA_E5_AT_MATCH_BARE_OK matches only a truly bare
	 * "OK" (empty prefix), and LORA_E5_AT_MATCH_ANY_URC with
	 * prefix=NULL matches ANY non-error line (OK- or URC-kind alike),
	 * which would incorrectly resolve this transaction on the first
	 * unrelated URC line instead of waiting for an actual OK/ERROR.
	 * Implementing this would mean guessing at wrong-but-plausible
	 * behavior for a passthrough whose whole purpose is correctness
	 * against arbitrary caller-supplied commands -- refusing
	 * explicitly instead, same spirit as
	 * lora_e5_hf_build_max_payload_query(). See VERIFICATION_NEEDED.md:
	 * either add a LORA_E5_AT_MATCH_ANY_OK mode to lora_e5_at.h, or
	 * accept the ANY_URC-with-NULL-prefix over-broad behavior as a
	 * documented limitation before implementing this.
	 *
	 * Separately: the "-EBUSY unless FSM state is READY or JOINED"
	 * gating this function's header doc describes cannot be enforced
	 * here either -- the Modem Manager does not hold FSM state (the
	 * FSM is the sole state owner per Phase1); that check belongs on
	 * the caller side.
	 */
	return -ENOTSUP;
}

/* ------------------------------------------------------------------- */
/* Probe                                                                 */
/* ------------------------------------------------------------------- */

static void mm_probe_result_cb(const struct lora_e5_at_result *result);

static int submit_probe(void)
{
	int rc = lora_e5_hf_build_probe(&g_desc);

	if (rc != 0) {
		return rc;
	}
	g_desc.timeout_ms = CONFIG_LORA_E5_CMD_TIMEOUT_MS;

	return lora_e5_at_submit(&g_desc, mm_probe_result_cb);
}

static void mm_probe_result_cb(const struct lora_e5_at_result *result)
{
	bool ok = (result->outcome == LORA_E5_AT_OUTCOME_MATCHED &&
		   result->result_tag == LORA_E5_MM_TAG_OK);

	if (ok) {
		struct lora_e5_fsm_event evt = {
			.type = LORA_E5_FSM_EVT_PROBE_RESULT,
			.reset_result = 0,
		};

		emit_event(&evt);
		return;
	}

	/* Per lora_e5_hf_build_probe()'s own doc comment: an ERROR here
	 * (commonly -22 end-symbol-timeout while the modem is still
	 * booting) is a retryable "not ready yet" signal, not a structural
	 * fault -- unlike every other operation in this file, probe
	 * retries ALL failure kinds (including transport TIMEOUT/
	 * UART_ERROR) up to CONFIG_LORA_E5_MAX_RETRIES, rather than routing
	 * them through handle_transport_fault()'s generic UART_FAULT path.
	 */
	g_probe_retry_count++;
	if (g_probe_retry_count <= CONFIG_LORA_E5_MAX_RETRIES) {
		if (submit_probe() == 0) {
			return;
		}
	}

	struct lora_e5_fsm_event evt = {
		.type = LORA_E5_FSM_EVT_PROBE_RESULT,
		.reset_result = (result->outcome == LORA_E5_AT_OUTCOME_TIMEOUT)
			? -ETIMEDOUT : -EIO,
	};

	emit_event(&evt);
}

int lora_e5_mm_probe(void)
{
	if (!g_initialized) {
		return -EINVAL;
	}
	g_probe_retry_count = 0;
	return submit_probe();
}

/* ------------------------------------------------------------------- */
/* Init / lifecycle                                                     */
/* ------------------------------------------------------------------- */

void lora_e5_mm_test_reset(void)
{
	/* Cancel before any subsequent lora_e5_mm_init() re-runs
	 * k_work_init_delayable() on these -- re-initializing a still-
	 * scheduled delayable work item corrupts the kernel's internal
	 * timeout list rather than cleanly replacing it (hit in practice by
	 * tests/fsm, whose test cases can end before a short settle/pulse
	 * timer from an earlier case has fired).
	 */
	k_work_cancel_delayable(&g_reset_gpio_work);
	k_work_cancel_delayable(&g_wakeup_settle_work);

	memset(&g_pending_send, 0, sizeof(g_pending_send));
	memset(&g_config, 0, sizeof(g_config));

	g_port_valid = false;
	g_port_cached = 0;

	g_join_devaddr_valid = false;
	g_join_netid_valid = false;

	g_cmsg_ack_seen = false;
	g_pending_rxwin_valid = false;

	g_probe_retry_count = 0;

	g_event_cb = NULL;
	g_event_cb_user_data = NULL;
}

int lora_e5_mm_init(const struct lora_e5_mm_init_params *params)
{
	if (params == NULL) {
		return -EINVAL;
	}
	if (params->reset_backend == LORA_E5_RESET_BACKEND_GPIO && params->reset_gpio == NULL) {
		return -EINVAL;
	}

	g_reset_backend = params->reset_backend;
	g_reset_gpio = params->reset_gpio;

	if (g_reset_backend == LORA_E5_RESET_BACKEND_GPIO) {
		int rc = gpio_pin_configure_dt(g_reset_gpio, GPIO_OUTPUT_INACTIVE);

		if (rc != 0) {
			return rc;
		}
	}

	/* Cancel before re-initializing -- a repeated lora_e5_mm_init() call
	 * (as tests do, between cases) must not run k_work_init_delayable()
	 * on a work item a previous call left scheduled; that corrupts the
	 * kernel's internal timeout list rather than cleanly replacing it.
	 * lora_e5_mm_test_reset() also cancels both, but only after this
	 * point if called separately -- cancel directly here too, since
	 * this function's own k_work_init_delayable() calls are what must
	 * be protected.
	 */
	k_work_cancel_delayable(&g_reset_gpio_work);
	k_work_cancel_delayable(&g_wakeup_settle_work);

	k_work_init_delayable(&g_reset_gpio_work, reset_gpio_deassert);
	k_work_init_delayable(&g_wakeup_settle_work, wakeup_settle_expired);

	lora_e5_mm_test_reset();

	/* Per this module's header doc, lora_e5_mm_set_event_callback()
	 * must be called once by the FSM "before lora_e5_mm_init()
	 * completes" -- since lora_e5_mm_init() has no async side effects
	 * of its own, calling set_event_callback() either just before or
	 * just after this function returns is equivalent in practice; the
	 * documented ordering constraint is satisfied either way as long
	 * as it happens before any operation that would emit an event.
	 * lora_e5_mm_test_reset() above clears g_event_cb, so callers must
	 * register it after this call returns, not before.
	 */
	int rc = lora_e5_at_set_urc_callback(mm_urc_cb);

	if (rc != 0) {
		return rc;
	}

	g_initialized = true;
	return 0;
}

int lora_e5_mm_set_event_callback(lora_e5_mm_event_cb_t cb, void *user_data)
{
	if (cb == NULL) {
		return -EINVAL;
	}
	g_event_cb = cb;
	g_event_cb_user_data = user_data;
	return 0;
}
