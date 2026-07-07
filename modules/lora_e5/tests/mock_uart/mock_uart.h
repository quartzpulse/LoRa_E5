/**
 * @file mock_uart.h
 * @brief Shared scriptable fake transport fixture, standing in for
 * lora_e5_uart.c in tests -- a fixture, not a standalone test suite
 * (see docs/Phase1-Architecture.md §10: "build it first"). Generalizes
 * the inline mock_send()/feed_line() pattern already used by
 * tests/modem_manager so tests/fsm doesn't duplicate that boilerplate.
 *
 * Threading contract: mock_uart_send() runs on whatever thread calls
 * into lora_e5_at_submit()/lora_e5_at_submit_sync() (the AT Command
 * Manager's cmd_queue lock is already held at that point). Scripted
 * auto-responses (mock_uart_script()) are therefore fed from a k_work
 * item on the caller-supplied feed_wq -- NOT called directly inside
 * mock_uart_send() -- to avoid a reentrant-lock deadlock and to match
 * lora_e5_at_process_line()'s documented "must run on the RX work
 * queue" contract. mock_uart_feed_line() (arbitrary URC injection, not
 * tied to a send) is called directly from the caller's thread, same
 * caveat as tests/modem_manager's existing feed_line() helper: safe
 * only in this single-threaded cooperative ztest execution model, not
 * a pattern to copy into production code.
 */
#ifndef MOCK_UART_H_
#define MOCK_UART_H_

#include <zephyr/kernel.h>
#include <stddef.h>

#define MOCK_UART_LAST_CMD_BUF_SIZE 600
#define MOCK_UART_SCRIPT_MAX 4

/**
 * @brief One-time init. `feed_wq` must be the same work queue passed
 * to lora_e5_at_init() (the suite's test_wq).
 */
void mock_uart_init(struct k_work_q *feed_wq);

/** @brief Reset all fixture state -- call from every test's `before`
 *  hook. Does not touch feed_wq. */
void mock_uart_reset(void);

/** @brief lora_e5_at_send_fn_t-conforming transport function --
 *  register via lora_e5_at_set_transport(mock_uart_send). */
int mock_uart_send(const char *cmd, size_t len);

/** @brief Feed one line into the AT parser/command-manager pipeline
 *  immediately, from the calling thread -- for URC injection
 *  independent of any in-flight send (e.g. an unsolicited downlink, or
 *  manually driving a CONFIG sequence's per-step response). */
void mock_uart_feed_line(const char *line);

/**
 * @brief Arm up to MOCK_UART_SCRIPT_MAX lines to be auto-fed (via
 * feed_wq) the next time mock_uart_send() is invoked -- simulates an
 * instantly-arriving modem response. Required for exercising any
 * `_sync()` call under test, since it blocks the calling thread on a
 * semaphore only feed_wq's processing can give.
 */
void mock_uart_script(const char *line);

/**
 * @brief Register an always-on auto-responder, invoked on every
 * mock_uart_send() call with the just-sent command text -- needed to
 * drive a long multi-step chain (e.g. lora_e5_start_sync()'s
 * RESET->BOOT->CHECK_AT->CONFIG sequence, a dozen+ separate AT
 * transactions) end-to-end without per-step manual intervention.
 * Return the single line to feed back (fed via feed_wq, same as
 * mock_uart_script()), or NULL to feed nothing for this command.
 * Pass NULL to clear. Cleared by mock_uart_reset().
 */
void mock_uart_set_auto_responder(const char *(*fn)(const char *cmd, size_t len));

/** @brief Force every subsequent mock_uart_send() call to return `rc`
 *  instead of 0 (simulating a transport-level send failure), until
 *  changed again or cleared by mock_uart_reset(). */
void mock_uart_set_forced_rc(int rc);

int mock_uart_send_count(void);
const char *mock_uart_last_cmd(void);
size_t mock_uart_last_cmd_len(void);

#endif /* MOCK_UART_H_ */
