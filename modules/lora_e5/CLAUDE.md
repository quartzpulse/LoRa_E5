# CLAUDE.md

Instructions for Claude Code working in this repository
(`modules/lora_e5/` — a Zephyr LoRaWAN library for the Seeed
LoRa-E5-HF module). Read this before touching any file. If something
here conflicts with what you find in the code, the code is more
current — this file describes intent and constraints, not a snapshot
to enforce blindly. If you find a real conflict, flag it in
`docs/VERIFICATION_NEEDED.md` rather than silently picking a side.

## What this project is

A production-grade Zephyr library that manages the full LoRa-E5-HF
modem lifecycle through an event-driven FSM over the module's AT
command firmware. Full requirements: `docs/Phase1-Architecture.md`.
Full header rationale: `docs/Phase2-Design-Notes.md`.

**Read `docs/VERIFICATION_NEEDED.md` before relying on any AT command
syntax, response format, or protocol behavior in this codebase.**
Several things are marked `[Likely]` or `[Guessing]` there rather than
`[Certain]` — they're implemented but not hardware-verified. Do not
"fix" a `[Guessing]` item by making it look more confident without
actually verifying it against real hardware or the primary AT Command
Specification V1.0 PDF. If you verify something in that file, move it
to the "Resolved" section with the confirming source, don't just
delete the entry.

## Non-negotiable architectural decisions

These were arrived at through explicit design review, not defaults —
do not revert them:

1. **Class A only in v1.** No Class B/C states in the FSM, not even
   dormant/Kconfig-gated ones. Don't add `LORA_E5_STATE_CLASS_B_*` or
   similar without an explicit instruction to do so.
2. **`JOINED` is the sole steady state.** TX, sleep, reset, and leave
   are all transitions out of and back into `JOINED`, never through
   `READY`. `READY` only exists between `CONFIG` and `JOINING`/`JOINED`
   (awaiting the application's explicit `lora_e5_join()` call — v1
   does NOT auto-join after CONFIG).
3. **RX1/RX2/ACK are event metadata, not FSM states.** Single
   `WAIT_TX_RESULT` state; `enum lora_e5_rx_window` in
   `lora_e5_types.h` carries which window a downlink arrived in.
4. **ABP and OTAA both ship in v1.** The branch lives in
   `lora_e5_mm_join()` (Modem Manager), not the FSM — the FSM only
   ever sees a `JOIN_RESULT` event with `outcome` distinguishing
   `SUCCESS` vs `ABP_SKIP`. Do not add an `if (join_type == ABP)`
   branch to `lora_e5_fsm.c` when you write it.
5. **Reset backend is abstracted, never hardcoded.**
   `CONFIG_LORA_E5_HAS_RESET_GPIO` selects GPIO-toggle vs `AT+RESET`
   at compile time; defaults to AT+RESET-only until a board's
   `reset-gpios` devicetree property is confirmed present.
6. **Layering is strict and the Modem Manager is the semantic
   boundary.** `lora_e5_fsm.c` must never contain an AT command
   string or a `+PREFIX:` literal — that knowledge belongs
   exclusively in `src/lora_e5_hf_commands.c` (command tables) and
   `src/lora_e5_modem_manager.c` (translation + URC classification).
   The AT Command Manager (`lora_e5_at.c` / `lora_e5_cmd_queue.c`)
   must never contain LoRaWAN-specific logic — it executes whatever
   descriptor it's handed, generically. This split is what makes the
   stated future-expansion goal (RN2483, Type ABZ, Quectel) real
   instead of aspirational — don't erode it for convenience.
7. **No dynamic allocation.** Static buffers, `K_MSGQ_DEFINE`, fixed
   caps (`LORA_E5_AT_MAX_TERMINAL_EVENTS = 8`,
   `CONFIG_LORA_E5_CMD_QUEUE_DEPTH`). If you hit a case that seems to
   need dynamic sizing, that's a signal to raise it, not to reach for
   `k_malloc`.
8. **Every terminal-event table needs a catch-all `ANY_ERROR` entry.**
   See `ANY_ERROR_ENTRY` macro in `lora_e5_hf_commands.c`. Without it,
   a structural AT error on that command hangs until timeout instead
   of resolving immediately.

## A bug already fixed here — don't reintroduce it

`LORA_E5_AT_MATCH_ANY_URC` in `lora_e5_at.h` explicitly excludes
error-kind lines (`line->kind == LORA_E5_AT_LINE_ERROR` short-circuits
to `false` in `line_matches_entry()`, `lora_e5_cmd_queue.c`). If you
ever touch that matcher: without this exclusion, a line like
`+MODE: ERROR(-1)` matches a same-prefixed `ANY_URC` entry *before*
the table's `ANY_ERROR` catch-all is checked, and gets silently
reported as **success**. Regression test:
`test_any_urc_does_not_swallow_error` in
`tests/cmd_queue/src/main.c` — if you change matcher semantics, that
test must still pass, and if it doesn't, the matcher is wrong, not
the test.

## Known gaps / TODOs, in priority order

Check `docs/VERIFICATION_NEEDED.md` for the full list with sources.
Summary of what blocks real use:

1. **Port caching not implemented.** `lora_e5_hf_build_send()`'s
   `port` parameter is currently a no-op in the wire format (MSGHEX/
   CMSGHEX carry no port argument; it's set once via `AT+PORT=`).
   Whoever writes `lora_e5_modem_manager.c`'s `lora_e5_mm_send()` MUST
   cache the last-configured port and reissue `AT+PORT=` first if the
   caller's requested port differs — otherwise sends silently go out
   on the wrong port with no error.
2. **`AT+ID` SET syntax unverified** (`lora_e5_hf_build_id_set_eui()`
   in `lora_e5_hf_commands.c`) — modeled by analogy to `AT+KEY`, not
   directly confirmed. Do not treat as working until checked against
   real hardware.
3. **`AT+LW=LEN` max-payload query is stubbed to return `-ENOTSUP`
   on purpose** (`lora_e5_hf_build_max_payload_query()`). Do not
   implement a guessed subcommand syntax here — confirm against the
   primary spec PDF's `AT+LW` section first.
4. **`lora_e5_at_submit_sync()` serializes all synchronous callers
   app-wide** (one static context + one mutex held for the full call
   duration — see the design-note comment at the top of that function
   in `lora_e5_at.c`). This is deliberate, not a bug, but if you're
   asked to improve concurrency here, the fix is a small static
   context pool sized to `CONFIG_LORA_E5_CMD_QUEUE_DEPTH`, not a
   dynamic allocation.
5. **`g_sync_lock` lazy-init has a narrow first-call race** (noted
   TODO in `lora_e5_at.c`). Preferred fix: `K_MUTEX_DEFINE` static
   initializer instead of lazy `k_mutex_init()`. Low priority but
   flagged so it doesn't get "cleaned up" the wrong way.

## Not yet implemented at all

- `src/lora_e5_modem_manager.c` — the runtime. Header contract is in
  `src/lora_e5_modem_manager.h` and `include/lora_e5/lora_e5_at.h`.
  This is next in the implementation order.
- `src/lora_e5_fsm.c`
- `src/lora_e5_uart.c` (UART Async API backend — nothing has been
  built against real UART yet; all current tests use a mock
  `lora_e5_at_send_fn_t` function pointer in place of it)
- `src/lora_e5_events.c`
- `src/lora_e5.c` (public API implementation)
- `src/lora_e5_internal.h`
- Module-root `CMakeLists.txt`, `Kconfig`, `README.md` — until these
  exist, `CONFIG_LORA_E5_CMD_QUEUE_DEPTH` and
  `CONFIG_LORA_E5_LOG_LEVEL` fall back to `#define`s with a
  `#warning` at the top of `lora_e5_cmd_queue.c`. Replace those with
  real Kconfig symbols when you write Kconfig, don't leave both
  defined.
- `tests/fsm/`, `tests/mock_uart/` (a real reusable mock-UART fixture,
  not just the inline function pointer current tests use)
- `samples/join/`, `samples/uplink/`, `samples/shell/`

## Build gotchas (found by an actual build, not review)

- **Zephyr builds with `-Werror`.** Never use `#warning` as an
  in-code reminder (e.g. for a Kconfig-not-written-yet fallback) — it
  fails the build. Use a plain comment instead. (Found in
  `lora_e5_cmd_queue.c`'s `CONFIG_LORA_E5_CMD_QUEUE_DEPTH` fallback.)
- **Double-check relative path depth in test `CMakeLists.txt` files.**
  `tests/<suite>/` is two directories under the module root
  (`tests/<suite>/../../` → module root), not three. Verify with
  `realpath -e` against the real tree before trusting a path looks
  right by eye.
- **`LOG_MODULE_REGISTER`/`LOG_MODULE_DECLARE` argument order matters
  at the preprocessor level.** If a file defines a fallback for
  `CONFIG_LORA_E5_LOG_LEVEL` (until Kconfig exists), that `#ifndef`
  block must appear *before* the `LOG_MODULE_REGISTER`/`_DECLARE` line
  that uses it, not after — C preprocessing is single-pass. This bug
  hit both `lora_e5_cmd_queue.c` and `lora_e5_at.c` independently
  (each `.c` file needs its own fallback; a `#define` in one
  translation unit isn't visible in another).

## Testing conventions

- ztest on `native_sim` — no hardware required for `tests/parser` or
  `tests/cmd_queue`. Run with:
  ```
  west twister -p native_sim -T modules/lora_e5/tests -v
  ```
- Every AT response/URC string used as a test vector must be traceable
  to either the primary spec PDF or a cited captured real-device log
  (see comments in `tests/parser/src/main.c` for the citation style
  already in use). Don't invent a plausible-looking response string as
  a test vector — if you need one you don't have a source for, add it
  to `VERIFICATION_NEEDED.md` instead of asserting against a guess.
- New terminal-event tables in `lora_e5_hf_commands.c` need a
  corresponding test in `tests/cmd_queue/` exercising both the
  success path and at least one error/edge path (busy, not-joined,
  timeout — whichever apply).
- `lora_e5_cmd_queue_test_reset()` / `_test_is_active()` /
  `_test_pending_count()` in `lora_e5_cmd_queue.h` are test-only
  accessors into otherwise-static module state — call
  `lora_e5_cmd_queue_test_reset()` in every test's `before` hook, see
  `case_before()` in `tests/cmd_queue/src/main.c`.

## Code style

- Zephyr coding style (see `.clang-format` if one exists in the tree;
  if not, match the existing files' style — tabs, brace placement,
  Doxygen `/** */` on every public declaration).
- Confidence tagging in comments: when documenting something derived
  from the AT spec or a captured log rather than directly observed in
  this session, tag it `[Certain]` / `[Likely]` / `[Guessing]` inline,
  matching the convention already used throughout `lora_e5_hf_commands.c`
  and `lora_e5_hf_commands.h`. This is not decoration — it's what
  `VERIFICATION_NEEDED.md` is built from. Don't drop the tags to make
  a comment read more cleanly.
- No file should grow into a dumping ground — if
  `lora_e5_modem_manager.c` starts exceeding a few hundred lines,
  split URC classification, config-sequencing, and send/join/sleep
  logic into separate static functions within the file before
  considering further file splits (don't create new files without a
  reason beyond size).

## When you're not sure

Prefer stopping and asking, or adding an entry to
`VERIFICATION_NEEDED.md` and continuing with an explicitly-flagged
placeholder (see `lora_e5_hf_build_max_payload_query()` for the
pattern: return `-ENOTSUP` rather than a guessed implementation),
over silently picking the more-confident-sounding option. This
codebase's actual owner reviews and hardware-verifies flagged items
personally — an honest gap they can check is strictly more valuable
to them than a confident guess they have to first discover is wrong.