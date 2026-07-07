/**
 * @file lora_e5_uart.c
 * @brief UART Async API backend. Generic Zephyr UART Async API code --
 * nothing ESP32-specific here, only the board devicetree overlay is
 * chip-specific (Phase 1 §2.1).
 *
 * Split per Phase 1 §4: the uart_callback_set() callback (may run in
 * ISR/driver-internal context -- treated as ISR-context throughout
 * this file for safety) only accumulates bytes and detects line
 * completion ("\r\n", spec §2.1/§2.3) into a small fixed-size assembly
 * buffer -- no parsing here. Each completed line is copied into a
 * CONFIG_LORA_E5_UART_LINE_QUEUE_DEPTH-deep k_msgq of fixed-size line
 * structs; a k_work item on the caller-supplied rx_wq drains that
 * queue, calling lora_e5_at_parse_line() then lora_e5_at_process_line()
 * for each line.
 *
 * TX: implements the lora_e5_at_send_fn_t-conforming send function,
 * appends "\r\n" (not part of any lora_e5_hf_commands.c command string
 * -- confirmed by reading lora_e5_hf_build_mode() etc., so the
 * transport owns the terminator) into a static scratch buffer, calls
 * uart_tx(), and tracks single-in-flight TX state with an atomic flag
 * (not a mutex -- UART_TX_DONE/UART_TX_ABORTED clear it from the same
 * ISR-context callback that handles RX, where a mutex would be unsafe).
 *
 * RX re-arming uses the same ping-pong UART_RX_BUF_REQUEST pattern as
 * external/zephyr/samples/drivers/uart/async_api/src/main.c.
 *
 * Known gap, flagged rather than silently worked around (see
 * docs/VERIFICATION_NEEDED.md): an overlong or otherwise malformed RX
 * line is dropped and the assembly buffer reset, but there is no path
 * in the current lora_e5_at.h contract for this backend (which has no
 * FSM/Modem-Manager reference by design, per Phase 1 §2.1) to signal
 * that fault upward the way a TX-side uart_tx() failure naturally does
 * via lora_e5_at_send_fn_t's return value. Logged only.
 */

#include "lora_e5_internal.h"
#include "lora_e5/lora_e5_at.h"

#include <zephyr/drivers/uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(lora_e5_uart, CONFIG_LORA_E5_LOG_LEVEL);

/* Spec §2.1: command length never exceeds 528 characters; +2 for the
 * "\r\n" terminator this transport is responsible for appending.
 */
#define UART_TX_BUF_SIZE (528 + 2)

/* Chunk size for the async RX ping-pong buffers -- independent of
 * CONFIG_LORA_E5_RX_BUFFER_SIZE (the *line* assembly buffer): this is
 * just how much raw, possibly line-fragment, data the driver hands us
 * per UART_RX_RDY event.
 */
#define UART_RX_CHUNK_SIZE 64

/* Inactivity timeout (microseconds, per uart_rx_enable()'s documented
 * unit) before the driver delivers whatever it has, even if the chunk
 * buffer isn't full -- matches
 * external/zephyr/samples/drivers/uart/async_api's own value.
 */
#define UART_RX_INACTIVITY_TIMEOUT_US 100

struct uart_line {
	char text[CONFIG_LORA_E5_RX_BUFFER_SIZE];
	size_t len;
};

K_MSGQ_DEFINE(g_line_msgq, sizeof(struct uart_line), CONFIG_LORA_E5_UART_LINE_QUEUE_DEPTH, 4);

static const struct device *g_uart_dev;
static struct k_work_q *g_rx_wq;
static struct k_work g_line_drain_work;

/* Line assembly state -- touched only from uart_callback() (ISR/driver
 * context); no lock needed since it has exactly one writer.
 */
static char g_assembly_buf[CONFIG_LORA_E5_RX_BUFFER_SIZE];
static size_t g_assembly_len;
static bool g_discarding_overlong_line;
static uint32_t g_overlong_line_count;

static uint8_t g_rx_chunk_buf[2][UART_RX_CHUNK_SIZE];
static uint8_t g_rx_chunk_idx;

static char g_tx_buf[UART_TX_BUF_SIZE];
static atomic_t g_tx_busy = ATOMIC_INIT(0);

/* ------------------------------------------------------------------- */
/* Line assembly (ISR/driver context)                                   */
/* ------------------------------------------------------------------- */

static void line_drain_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct uart_line line;
	struct lora_e5_at_line parsed;

	while (k_msgq_get(&g_line_msgq, &line, K_NO_WAIT) == 0) {
		if (lora_e5_at_parse_line(line.text, line.len, &parsed) == 0) {
			lora_e5_at_process_line(&parsed);
		}
	}
}

static void complete_line(void)
{
	struct uart_line line;
	size_t len = g_assembly_len;

	/* Strip a trailing '\r' -- lines terminate "\r\n" per spec §2.1;
	 * we detect completion on '\n' alone (accepted on input per the
	 * same section) so a bare '\n' with no preceding '\r' is also
	 * handled.
	 */
	if (len > 0 && g_assembly_buf[len - 1] == '\r') {
		len--;
	}

	if (len == 0) {
		/* Blank line (e.g. a lone "\r\n" as line-ending noise) --
		 * nothing to hand off.
		 */
		return;
	}

	line.len = len < sizeof(line.text) ? len : sizeof(line.text);
	memcpy(line.text, g_assembly_buf, line.len);

	if (k_msgq_put(&g_line_msgq, &line, K_NO_WAIT) == 0) {
		k_work_submit_to_queue(g_rx_wq, &g_line_drain_work);
	}
	/* else: line queue full (CONFIG_LORA_E5_UART_LINE_QUEUE_DEPTH) --
	 * drop. No logging from ISR-context; this is a sizing problem to
	 * diagnose via CONFIG_LORA_E5_UART_LINE_QUEUE_DEPTH tuning, not
	 * something to (unsafely) log here.
	 */
}

static void handle_rx_byte(uint8_t c)
{
	if (g_discarding_overlong_line) {
		if (c == '\n') {
			g_discarding_overlong_line = false;
			g_assembly_len = 0;
		}
		return;
	}

	if (c == '\n') {
		complete_line();
		g_assembly_len = 0;
		return;
	}

	if (g_assembly_len >= sizeof(g_assembly_buf)) {
		/* Overlong line -- drop rather than silently truncate (a
		 * truncated line could masquerade as a valid short
		 * response). Discard the remainder up to the next '\n'.
		 */
		g_discarding_overlong_line = true;
		g_assembly_len = 0;
		g_overlong_line_count++;
		return;
	}

	g_assembly_buf[g_assembly_len++] = (char)c;
}

/* ------------------------------------------------------------------- */
/* UART async event callback (ISR/driver context)                       */
/* ------------------------------------------------------------------- */

static void uart_callback(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		atomic_set(&g_tx_busy, 0);
		break;

	case UART_RX_BUF_REQUEST:
		/* Best-effort: a failure here just means the driver has
		 * nowhere to put the next chunk until re-armed elsewhere;
		 * there is no crash-safe recovery available from this
		 * context.
		 */
		(void)uart_rx_buf_rsp(dev, g_rx_chunk_buf[g_rx_chunk_idx],
				      sizeof(g_rx_chunk_buf[0]));
		g_rx_chunk_idx = g_rx_chunk_idx ? 0 : 1;
		break;

	case UART_RX_RDY:
		for (size_t i = 0; i < evt->data.rx.len; i++) {
			handle_rx_byte(evt->data.rx.buf[evt->data.rx.offset + i]);
		}
		break;

	case UART_RX_BUF_RELEASED:
	case UART_RX_DISABLED:
	default:
		break;
	}
}

/* ------------------------------------------------------------------- */
/* TX (lora_e5_at_send_fn_t-conforming)                                  */
/* ------------------------------------------------------------------- */

static int lora_e5_uart_send(const char *cmd, size_t len)
{
	if (cmd == NULL || len == 0 || len + 2 > sizeof(g_tx_buf)) {
		return -EINVAL;
	}

	if (!atomic_cas(&g_tx_busy, 0, 1)) {
		/* Single-in-flight is already enforced one layer up (the AT
		 * Command Manager), so this should not normally happen --
		 * defensive, not the primary guard.
		 */
		return -EBUSY;
	}

	memcpy(g_tx_buf, cmd, len);
	g_tx_buf[len] = '\r';
	g_tx_buf[len + 1] = '\n';

	int rc = uart_tx(g_uart_dev, g_tx_buf, len + 2, SYS_FOREVER_US);

	if (rc != 0) {
		atomic_set(&g_tx_busy, 0);
	}
	return rc;
}

/* ------------------------------------------------------------------- */
/* Init                                                                  */
/* ------------------------------------------------------------------- */

int lora_e5_uart_init(const struct device *uart_dev, struct k_work_q *rx_wq)
{
	if (uart_dev == NULL || rx_wq == NULL) {
		return -EINVAL;
	}
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}

	g_uart_dev = uart_dev;
	g_rx_wq = rx_wq;
	atomic_set(&g_tx_busy, 0);
	g_assembly_len = 0;
	g_discarding_overlong_line = false;
	g_overlong_line_count = 0;
	g_rx_chunk_idx = 1;
	k_msgq_purge(&g_line_msgq);
	k_work_init(&g_line_drain_work, line_drain_work_handler);

	int rc = uart_callback_set(uart_dev, uart_callback, NULL);

	if (rc != 0) {
		return rc;
	}

	rc = uart_rx_enable(uart_dev, g_rx_chunk_buf[0], sizeof(g_rx_chunk_buf[0]),
			     UART_RX_INACTIVITY_TIMEOUT_US);
	if (rc != 0) {
		return rc;
	}

	lora_e5_at_set_transport(lora_e5_uart_send);
	return 0;
}
