/**
 * @file mock_uart.c
 * @brief See mock_uart.h for the fixture contract and threading notes.
 */

#include "mock_uart.h"

#include "lora_e5/lora_e5_at.h"

#include <string.h>

static struct k_work_q *feed_wq;

static int send_count;
static char last_cmd[MOCK_UART_LAST_CMD_BUF_SIZE];
static size_t last_cmd_len;
static int forced_rc;

static const char *script_lines[MOCK_UART_SCRIPT_MAX];
static size_t script_count;
static struct k_work script_work;

static const char *(*auto_responder)(const char *cmd, size_t len);
static char auto_response_buf[MOCK_UART_LAST_CMD_BUF_SIZE];
static struct k_work auto_response_work;

void mock_uart_feed_line(const char *line)
{
	struct lora_e5_at_line parsed;

	if (lora_e5_at_parse_line(line, strlen(line), &parsed) != 0) {
		return;
	}
	lora_e5_at_process_line(&parsed);
}

static void script_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	size_t n = script_count;

	script_count = 0;
	for (size_t i = 0; i < n; i++) {
		mock_uart_feed_line(script_lines[i]);
	}
}

static void auto_response_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	mock_uart_feed_line(auto_response_buf);
}

void mock_uart_init(struct k_work_q *wq)
{
	feed_wq = wq;
	k_work_init(&script_work, script_work_handler);
	k_work_init(&auto_response_work, auto_response_work_handler);
	mock_uart_reset();
}

void mock_uart_reset(void)
{
	send_count = 0;
	memset(last_cmd, 0, sizeof(last_cmd));
	last_cmd_len = 0;
	forced_rc = 0;
	memset(script_lines, 0, sizeof(script_lines));
	script_count = 0;
	auto_responder = NULL;
}

int mock_uart_send(const char *cmd, size_t len)
{
	send_count++;
	last_cmd_len = len < sizeof(last_cmd) - 1 ? len : sizeof(last_cmd) - 1;
	memcpy(last_cmd, cmd, last_cmd_len);
	last_cmd[last_cmd_len] = '\0';

	if (script_count > 0 && feed_wq != NULL) {
		k_work_submit_to_queue(feed_wq, &script_work);
	} else if (auto_responder != NULL && feed_wq != NULL) {
		const char *resp = auto_responder(last_cmd, last_cmd_len);

		if (resp != NULL) {
			strncpy(auto_response_buf, resp, sizeof(auto_response_buf) - 1);
			auto_response_buf[sizeof(auto_response_buf) - 1] = '\0';
			k_work_submit_to_queue(feed_wq, &auto_response_work);
		}
	}

	return forced_rc;
}

void mock_uart_script(const char *line)
{
	if (script_count < MOCK_UART_SCRIPT_MAX) {
		script_lines[script_count++] = line;
	}
}

void mock_uart_set_auto_responder(const char *(*fn)(const char *cmd, size_t len))
{
	auto_responder = fn;
}

void mock_uart_set_forced_rc(int rc)
{
	forced_rc = rc;
}

int mock_uart_send_count(void)
{
	return send_count;
}

const char *mock_uart_last_cmd(void)
{
	return last_cmd;
}

size_t mock_uart_last_cmd_len(void)
{
	return last_cmd_len;
}
