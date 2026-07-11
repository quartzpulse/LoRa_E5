/**
 * @file lora_e5_at.h
 * @brief Protocol-generic AT transaction engine.
 *
 * This module knows NOTHING about LoRaWAN or the LoRa-E5 command set.
 * It executes whatever command descriptor it is handed: send a string,
 * wait for a matching terminal line, retry/timeout per the descriptor,
 * forward everything else upward as an unsolicited line. All LoRa-E5
 * specific knowledge (what "+JOIN: Done" means) lives in the Modem
 * Manager (lora_e5_modem_manager.h), which populates these descriptors.
 *
 * This split is what makes a future RN2483/Type-ABZ/Quectel port
 * realistic: only the Modem Manager implementation and parser need to
 * change; this file and its .c do not.
 */
#ifndef LORA_E5_AT_H_
#define LORA_E5_AT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#include "lora_e5_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */
/* Parser output -- generic line classification                        */
/* ------------------------------------------------------------------- */

/** Maximum length of a classified line's prefix token, e.g. "JOIN",
 *  "MSG", "CMSG", "RESET". Sized generously against the longest AT
 *  command name in the spec ("LOWPOWER", "FDEFAULT" -- 8 chars); 15
 *  leaves headroom without inviting stack bloat. */
#define LORA_E5_AT_PREFIX_MAX 15

/**
 * @brief Kind of line the parser identified.
 *
 * NOTE: not every command terminates on OK. AT+MSG terminates on
 * "+MSG: Done", AT+JOIN on "+JOIN: Done"/"+JOIN: Join failed". A line
 * of kind LORA_E5_AT_LINE_URC may still be the terminal condition for
 * the in-flight command -- that's the terminal_events[] match's job,
 * not this classification's.
 */
enum lora_e5_at_line_kind {
	LORA_E5_AT_LINE_OK,             /**< Bare "OK" or "+CMD: OK". */
	LORA_E5_AT_LINE_ERROR,          /**< "ERROR(-N)" or "+CMD: ERROR(-N)". */
	LORA_E5_AT_LINE_URC,            /**< Any other "+PREFIX: remainder"
	                                  *   or "+PREFIX remainder" line. */
	LORA_E5_AT_LINE_UNRECOGNIZED,   /**< Did not match any known AT
	                                  *   line shape at all. Never
	                                  *   silently dropped -- always
	                                  *   surfaced so higher layers can
	                                  *   decide whether it's fatal. */
};

/**
 * @brief A single classified line, produced by the AT Parser.
 *
 * The parser's job stops here: it recognizes shape and extracts prefix/
 * remainder/error_code. It does NOT decide whether this line is a URC
 * or the terminal response for the in-flight command -- that requires
 * command context the parser deliberately does not hold.
 */
struct lora_e5_at_line {
	enum lora_e5_at_line_kind kind;
	char prefix[LORA_E5_AT_PREFIX_MAX + 1];   /**< Without leading '+'.
	                                            *   Empty for bare OK/
	                                            *   ERROR with no prefix. */
	const char *remainder;    /**< Points into the backing line buffer
	                            *   owned by the caller for the
	                            *   duration of the callback only --
	                            *   copy out if retaining beyond the
	                            *   callback. NULL if not applicable. */
	size_t remainder_len;
	enum lora_e5_at_error error_code; /**< Valid only when
	                                    *   kind == LORA_E5_AT_LINE_ERROR. */
};

/**
 * @brief Parse one complete line (no CR/LF) into a classified line.
 *
 * Pure function, no state, no UART interaction. Fully unit-testable
 * with literal strings -- see Phase 1 §10 testing strategy.
 *
 * @param line   Line text, NUL-terminated, CR/LF already stripped.
 * @param len    Length of line, excluding NUL.
 * @param out    Output classification. remainder/prefix point into
 *               `line`'s storage -- caller must keep `line` alive for
 *               as long as `out` is used.
 * @return 0 on success, -EINVAL if line is empty/malformed beyond
 *         what LORA_E5_AT_LINE_UNRECOGNIZED can represent.
 */
int lora_e5_at_parse_line(const char *line, size_t len,
			   struct lora_e5_at_line *out);

/* ------------------------------------------------------------------- */
/* Terminal event matching                                              */
/* ------------------------------------------------------------------- */

/** @brief How a terminal_event entry matches against an incoming line. */
enum lora_e5_at_match_mode {
	LORA_E5_AT_MATCH_ANY_URC,        /**< Match any line with this
	                                   *   prefix, EXCLUDING error-kind
	                                   *   lines -- i.e. kind == OK or
	                                   *   kind == URC, never kind ==
	                                   *   ERROR. (Corrected during
	                                   *   Phase 3 implementation: the
	                                   *   original doc said "any URC/
	                                   *   OK/ERROR line," which is
	                                   *   wrong -- if this also matched
	                                   *   ERROR-kind lines, a genuine
	                                   *   "+MODE: ERROR(-1)" would
	                                   *   match this entry before ever
	                                   *   reaching the table's
	                                   *   catch-all ANY_ERROR entry,
	                                   *   silently reporting a
	                                   *   configuration failure as
	                                   *   success. Errors are always
	                                   *   handled by a separate
	                                   *   ANY_ERROR entry in the same
	                                   *   table.) Used where the exact
	                                   *   success remainder text isn't
	                                   *   confirmed (see [Likely]/
	                                   *   [Guessing] notes in
	                                   *   lora_e5_hf_commands.c) but
	                                   *   the prefix itself is reliable. */
	LORA_E5_AT_MATCH_EXACT,          /**< prefix AND remainder must
	                                   *   match exactly (e.g. prefix=
	                                   *   "JOIN", remainder="Done"). */
	LORA_E5_AT_MATCH_STARTSWITH,     /**< prefix matches, remainder
	                                   *   starts with the given text
	                                   *   (e.g. prefix="MSG", remainder
	                                   *   ="Length error" matches both
	                                   *   "Length error 0" and
	                                   *   "Length error 12"). */
	LORA_E5_AT_MATCH_BARE_OK,        /**< Bare "OK" with no prefix
	                                   *   (e.g. plain "AT" command's
	                                   *   simplest possible reply
	                                   *   shape, if firmware uses it). */
	LORA_E5_AT_MATCH_ANY_ERROR,      /**< Any ERROR(-N) regardless of
	                                   *   prefix -- typical catch-all
	                                   *   entry every descriptor should
	                                   *   include so structural/
	                                   *   transport errors always
	                                   *   resolve the command instead
	                                   *   of stalling to timeout. */
};

/**
 * @brief One terminal-condition entry in a command descriptor.
 *
 * The AT Command Manager walks a descriptor's terminal_events[] against
 * each classified line; on first match the command resolves with
 * result_tag as the caller-visible outcome code. result_tag is opaque
 * to the AT Command Manager -- only the Modem Manager (which populated
 * it) interprets its meaning.
 */
struct lora_e5_at_terminal_event {
	const char *prefix;              /**< e.g. "JOIN". NULL matches any
	                                   *   prefix (used with
	                                   *   MATCH_ANY_ERROR / BARE_OK). */
	const char *remainder;           /**< NULL for MATCH_ANY_URC /
	                                   *   BARE_OK / ANY_ERROR. */
	enum lora_e5_at_match_mode match_mode;
	int result_tag;                  /**< Returned via
	                                   *   lora_e5_at_result.result_tag
	                                   *   on match. Modem-Manager-
	                                   *   defined, e.g. an
	                                   *   LORA_E5_MM_RESULT_* value. */
	uint8_t required_matches;        /**< Added Phase 3: number of times
	                                   *   this entry must match before
	                                   *   the transaction resolves. 0
	                                   *   and 1 are equivalent (resolve
	                                   *   on first match) -- existing
	                                   *   static tables that don't set
	                                   *   this field default to 0 via
	                                   *   designated-initializer zero-
	                                   *   fill, which is backward
	                                   *   compatible. Exists specifically
	                                   *   for AT+ID's bare-query response
	                                   *   (three "+ID:" lines, no
	                                   *   observed trailing terminator --
	                                   *   see lora_e5_hf_commands.c). A
	                                   *   line that matches an entry
	                                   *   with required_matches > 1 but
	                                   *   hasn't reached the count yet is
	                                   *   consumed (counted) and NOT
	                                   *   forwarded to the URC callback
	                                   *   -- it's a known part of the
	                                   *   expected sequence, not an
	                                   *   unsolicited notification. */
};

/** Max length of a matched terminal line's captured remainder text
 *  (see struct lora_e5_at_result.captured_text) -- sized against the
 *  query responses this exists for, longest confirmed real capture
 *  being "CDR, TXDR(0,7), RXDR(0,7)" (25 chars, VERIFICATION_NEEDED.md)
 *  with headroom to 31. Longer remainders are truncated, not rejected
 *  -- a truncated-but-present value is more useful to a caller than an
 *  outright failure.
 *
 *  DO NOT casually enlarge this: struct lora_e5_at_result is embedded
 *  in a fixed struct cb_invocation cascade[MAX_CASCADE] array that is
 *  a STACK-LOCAL variable in lora_e5_cmd_queue.c's RX-work-queue
 *  functions (lora_e5_cmd_queue_submit(), timeout_handler(),
 *  lora_e5_cmd_queue_process_line()) -- every byte added here is
 *  multiplied by MAX_CASCADE (CONFIG_LORA_E5_CMD_QUEUE_DEPTH + 1, 9 by
 *  default) against CONFIG_LORA_E5_RX_STACK_SIZE's budget. Confirmed
 *  on real hardware: raising this to 63 without also raising the RX
 *  stack budget overflowed it and crashed with EXCCAUSE 28 (load
 *  prohibited) during a context restore -- a stack overflow, not a
 *  null-pointer bug, and non-obvious from the crash trace alone. If
 *  this ever needs to grow, CONFIG_LORA_E5_RX_STACK_SIZE's default
 *  must grow with it (roughly: new_default_max * MAX_CASCADE more
 *  bytes, plus margin). */
#define LORA_E5_AT_CAPTURED_TEXT_MAX 31

/** Hard cap on terminal_events[] length per descriptor. The AT Command
 *  Manager allocates a fixed-size match-progress counter array of this
 *  size per active transaction (no dynamic allocation) --
 *  lora_e5_at_submit() returns -EINVAL if terminal_event_count exceeds
 *  this. 8 covers every command table in lora_e5_hf_commands.c today
 *  (largest is MSGHEX/CMSGHEX at 8 including the catch-all). */
#define LORA_E5_AT_MAX_TERMINAL_EVENTS 8

/* ------------------------------------------------------------------- */
/* Command descriptor and result                                        */
/* ------------------------------------------------------------------- */

/**
 * @brief A single AT command transaction, fully specified by the
 * caller (Modem Manager). The AT Command Manager executes it verbatim.
 */
struct lora_e5_at_cmd_desc {
	const char *cmd;          /**< Fully formatted command text, no
	                            *   CR/LF, no trailing NUL required
	                            *   (cmd_len is authoritative). Buffer
	                            *   must remain valid until the command
	                            *   resolves -- Modem Manager typically
	                            *   owns a static per-transaction scratch
	                            *   buffer since only one command is
	                            *   in flight at a time. */
	size_t cmd_len;
	uint32_t timeout_ms;      /**< Overall transaction timeout. For
	                            *   multi-line exchanges (AT+JOIN,
	                            *   AT+MSG/CMSG) this must cover the
	                            *   full exchange, not just the first
	                            *   line -- see Phase 1 §9,
	                            *   CONFIG_LORA_E5_TX_TIMEOUT_MS vs
	                            *   CONFIG_LORA_E5_CMD_TIMEOUT_MS. */
	uint8_t max_retries;      /**< Transport-level retries only. Must
	                            *   be 0 for commands whose failure
	                            *   modes are LoRaWAN-semantic (join,
	                            *   TX) -- see Phase 1 §8, retry policy
	                            *   belongs to the FSM/recovery layer
	                            *   for those, not here. */
	const struct lora_e5_at_terminal_event *terminal_events;
	size_t terminal_event_count;
	void *user_data;          /**< Opaque, returned in the result
	                            *   callback unchanged. */
};

/** @brief Why a command transaction ended. */
enum lora_e5_at_outcome {
	LORA_E5_AT_OUTCOME_MATCHED,   /**< A terminal_events[] entry
	                                *   matched; see result_tag. */
	LORA_E5_AT_OUTCOME_TIMEOUT,   /**< No terminal line arrived within
	                                *   timeout_ms across all retries. */
	LORA_E5_AT_OUTCOME_UART_ERROR,/**< Backend-level fault aborted the
	                                *   transaction. */
};

/** @brief Result delivered to the submitter when a command resolves.
 *  Invoked from the RX work queue context (the same context
 *  lora_e5_at_process_line() runs on) -- must not block. If the
 *  Modem Manager needs to hand off to FSM work-queue context, that
 *  hop happens on its side via k_work, not here. */
struct lora_e5_at_result {
	enum lora_e5_at_outcome outcome;
	int result_tag;           /**< Valid only when outcome ==
	                            *   MATCHED; copied from the matching
	                            *   terminal_events[] entry. */
	enum lora_e5_at_error error_code; /**< Valid only when the match
	                                    *   was an ERROR line. */
	void *user_data;          /**< Echoed from the descriptor. */

	/** The matched line's remainder text (everything after
	 *  "+PREFIX: ", or after bare "OK"/"ERROR(-N)" with no prefix --
	 *  empty in that case). Only valid when outcome == MATCHED; empty
	 *  ("" / captured_text_len == 0) on TIMEOUT/UART_ERROR, or when
	 *  required_matches > 1 resolved on a line whose remainder was
	 *  NULL. Truncated (not an error) if the real remainder exceeds
	 *  LORA_E5_AT_CAPTURED_TEXT_MAX. Added specifically so single-line
	 *  query commands (AT+VER, AT+LW=<subcommand>, etc.) can report a
	 *  real value back to the caller instead of the -ENOTSUP refusal
	 *  this engine had no way to avoid before this field existed --
	 *  see lora_e5_mm_get_version()'s prior doc comment. Does NOT
	 *  solve multi-line responses (AT+ID's required_matches=3 still
	 *  only captures the line that satisfied the count, not all
	 *  three) -- that needs a separate mechanism if ever needed. */
	char captured_text[LORA_E5_AT_CAPTURED_TEXT_MAX + 1];
	size_t captured_text_len;
};

typedef void (*lora_e5_at_result_cb_t)(const struct lora_e5_at_result *result);

/** @brief URC callback: any line that did NOT match a terminal_events[]
 *  entry for the in-flight command (or arrived with no command in
 *  flight) is forwarded here, unmodified. Invoked from the RX work
 *  queue context -- must not block. */
typedef void (*lora_e5_at_urc_cb_t)(const struct lora_e5_at_line *line);

/* ------------------------------------------------------------------- */
/* Public (internal-library-scope) API                                  */
/* ------------------------------------------------------------------- */

/**
 * @brief Transport send function, supplied by whatever owns the UART
 * (lora_e5_uart.c in production, a mock in tests). MUST be non-
 * blocking -- queues bytes for async UART TX and returns; does not
 * wait for the write to complete. CR/LF framing is the transport's
 * job, not the caller's -- `cmd`/`len` here is the bare command text
 * with no line terminator.
 *
 * @return 0 if accepted for transmission, negative errno otherwise
 *         (e.g. -EIO if the UART device isn't ready). A non-zero
 *         return here resolves the in-flight transaction as
 *         LORA_E5_AT_OUTCOME_UART_ERROR immediately, no retry.
 */
typedef int (*lora_e5_at_send_fn_t)(const char *cmd, size_t len);

/**
 * @brief Initialize the AT Command Manager.
 *
 * @param rx_work_q  Work queue that lora_e5_at_process_line() and this
 *                    module's internal timeout/dispatch work run on.
 *                    Passed explicitly (not a hidden global) so tests
 *                    can supply their own queue instead of depending
 *                    on the real UART backend's queue existing.
 *                    Per Phase 1 §4, this must be a dedicated queue,
 *                    not CONFIG_SYS_WORKQUEUE -- caller's
 *                    responsibility to create it with an appropriate
 *                    CONFIG_LORA_E5_RX_STACK_SIZE-sized stack.
 */
int lora_e5_at_init(struct k_work_q *rx_work_q);

/**
 * @brief Register the transport. Must be called before any
 * lora_e5_at_submit() -- submitting with no transport registered
 * resolves every command as UART_ERROR immediately rather than
 * silently hanging until timeout.
 */
void lora_e5_at_set_transport(lora_e5_at_send_fn_t send_fn);

/**
 * @brief Feed one classified line into the transaction engine. Called
 * by whoever owns line assembly + parsing (the RX work queue, after
 * lora_e5_at_parse_line()) -- NOT called by application code.
 *
 * Runs the terminal_events[] match against the active command (if
 * any); on match, resolves the transaction and dispatches the next
 * queued command. Non-matching lines (and lines arriving with no
 * command active) are forwarded to the registered URC callback.
 *
 * Must be called from the same work queue passed to
 * lora_e5_at_init() -- this function is not internally locked against
 * concurrent callers, only against concurrent lora_e5_at_submit()
 * calls from other threads (which IS internally locked).
 */
void lora_e5_at_process_line(const struct lora_e5_at_line *line);

/**
 * @brief Register the single URC consumer. Only the Modem Manager
 * should call this, exactly once, during its own init.
 */
int lora_e5_at_set_urc_callback(lora_e5_at_urc_cb_t cb);

/**
 * @brief Queue a command for execution (async).
 *
 * Enforces single-in-flight (bus-busy) internally -- callers do not
 * need their own queuing beyond what CONFIG_LORA_E5_EVENT_QUEUE_DEPTH
 * / the underlying k_msgq capacity allows.
 *
 * @param desc   Descriptor. Must remain valid (cmd buffer + terminal_
 *               events array) until result_cb fires.
 * @param result_cb  Invoked exactly once when the transaction resolves.
 * @return 0 if queued, -ENOMEM if the queue is full (see
 *         CONFIG_LORA_E5_CMD_QUEUE_DEPTH), -EINVAL on malformed
 *         descriptor.
 */
int lora_e5_at_submit(const struct lora_e5_at_cmd_desc *desc,
		       lora_e5_at_result_cb_t result_cb);

/**
 * @brief Synchronous wrapper: submit and block the calling thread.
 *
 * Blocks on an internal semaphore with `timeout` as an outer bound
 * slightly larger than desc->timeout_ms * (desc->max_retries + 1), so
 * this degrades gracefully rather than hanging if the AT manager
 * itself misbehaves. Must NOT be called from the FSM work queue or
 * RX work queue -- see Phase 1 §4 threading notes; calling this from
 * either queue deadlocks the library.
 *
 * @return 0 on MATCHED outcome, -ETIMEDOUT on TIMEOUT, -EIO on
 *         UART_ERROR. On 0, *result is populated for result_tag
 *         inspection.
 */
int lora_e5_at_submit_sync(const struct lora_e5_at_cmd_desc *desc,
			    k_timeout_t timeout,
			    struct lora_e5_at_result *result);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E5_AT_H_ */
