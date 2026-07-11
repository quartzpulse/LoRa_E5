# VERIFICATION NEEDED

Maintained running list of every claim in this codebase that is not
[Certain] -- i.e. anything backed by inference, analogy, a secondary
source, or a captured log rather than the primary AT Command
Specification PDF read directly. Updated each implementation pass.
Items are removed (moved to "Resolved" at the bottom) once confirmed
against real hardware or the primary spec, with the confirming source
noted.

Confidence key: **[Likely]** = pattern-consistent inference or a
secondary-source capture, probably right but not directly confirmed
against the primary spec. **[Guessing]** = no direct evidence at all,
modeled by analogy; treat as probably wrong until checked.

---

## Blocking -- do not use in production without checking

*(none remaining -- see "Resolved" sections below. Item 5, the
port-caching gap, is now fully confirmed end-to-end over a real
network and the fix already implemented in `lora_e5_mm_send()`.)*

---

## Non-blocking but flagged -- confirm before relying on exact wording

### 6. `AT+ID` query trailing terminator
**[Certain the response is 3 lines on FW V4.0.11; unconfirmed whether
a different firmware revision could add a 4th line.]** Real hardware
capture 2026-07-05 (LoRa-E5-HF firmware V4.0.11) confirms exactly 3
`+ID:` lines (DevAddr, DevEui, AppEui, same field order as the two
prior captured logs) with nothing following. Resolved functionally via
`required_matches = 3` (Phase 3) regardless. The residual risk is
narrower now (confirmed on this specific firmware, not just inferred
from captures of unknown firmware version) but not eliminated for
*other* firmware revisions: if a future revision adds a 4th field,
this would still need updating -- see the original concern above.
**How to check:** re-capture against any firmware version other than
V4.0.11 if/when one is available.

---

## Design trade-offs made during implementation (not uncertain facts, but decisions worth your review)

### 7. `lora_e5_at_submit_sync()` serializes ALL synchronous callers app-wide
One static context + one mutex held for the full call duration, to
avoid a stack-use-after-return hazard without dynamic allocation. Real
consequence: two threads calling any `*_sync()` API (e.g.
`lora_e5_get_version()` and `lora_e5_join_sync()`) at the same moment
will serialize, not run concurrently. Documented in
`lora_e5_at.c`'s design-note comment. Flagging because this is the
kind of constraint that's easy to forget exists until a deadlock/
latency issue shows up in integration testing.

### 8. `g_sync_lock` lazy-initialization race
`lora_e5_at_submit_sync()` currently lazy-inits its static mutex on
first call rather than in `lora_e5_at_init()`. This has a real
(if narrow) first-call race if two threads call a `_sync()` API
simultaneously before either has completed init. Marked as a TODO in
the code -- needs `lora_e5_at_init()`'s signature reconsidered, or a
`K_MUTEX_DEFINE` static initializer used instead (simpler fix,
likely the right one -- flagging for your call since it changes the
file's global state slightly).

### 9. `CONFIG_LORA_E5_CMD_QUEUE_DEPTH` / `CONFIG_LORA_E5_LOG_LEVEL` fallback defines
`lora_e5_cmd_queue.c` currently `#define`s these with a `#warning` if
Kconfig hasn't provided them, since Kconfig/CMakeLists.txt for the
whole module don't exist yet (next deliverable). Not a correctness
risk, just a reminder these fallbacks need to disappear once Kconfig
is written -- the `#warning` is intentional so a real build using this
file before Kconfig exists is loud about it, not silent.

### 10. No line-text-capture path for successfully-matched terminal lines
`struct lora_e5_at_result` (returned by `lora_e5_at_submit_sync()`)
carries only `outcome`/`result_tag`/`error_code`/`user_data` -- never
the actual matched line's text. This blocks
`lora_e5_mm_get_version()`, `lora_e5_mm_get_ids()`, and (newly, this
pass) `lora_e5_mm_get_max_payload()` from ever returning real
`+VER:`/`+ID:`/`+LW: LEN,` values: the AT transaction can be confirmed
successful, but the version string / IDs / payload length it carried
back is unrecoverable under the current contract, so all three
functions explicitly return `-ENOTSUP` rather than fabricate a value.
This is a real capability gap, not a wrong-guess risk -- the wire
formats these three depend on are now all **[Certain]** (see
"Resolved this pass"). Fixing it means extending `lora_e5_at.h`'s
result contract (e.g. a caller-supplied capture buffer) -- an API
change to the AT Command Manager layer, flagging for your call since
`lora_e5_at.c`/`lora_e5_cmd_queue.c` are meant to stay LoRaWAN-agnostic
generic infrastructure (Non-negotiable decision #6 in CLAUDE.md).

### 11. `lora_e5_leave()`: CLAUDE.md decision #2 vs. `lora_e5.h`'s own doc comment
CLAUDE.md's decision #2 says "TX, sleep, reset, and **leave** are all
transitions out of and back into `JOINED`, never through `READY`."
`lora_e5.h`'s `lora_e5_leave()` doc comment says the opposite for this
specific call: "Clears local join state, returns FSM to READY." These
directly conflict on the one case they both name. Implemented per the
more specific, concrete contract (`lora_e5.h` -- `lora_e5_fsm_leave()`
in `lora_e5_fsm.c` transitions `JOINING`/`JOINED -> READY`), per
CLAUDE.md's own instruction to trust the code over the prose summary
when they disagree. Flagging rather than silently picking a side --
if CLAUDE.md's decision #2 was meant literally for `leave()` too, this
needs a real decision, not a guess.

### 12. Recovery ladder uses a fixed inter-pass retry delay, not full backoff
`lora_e5_fsm.c`'s `RECOVERY_PASS_RETRY_DELAY_MS` (2000ms, local
`#define`, not a Kconfig symbol) is a flat delay between failed
recovery-ladder passes. Phase 1 §8.3 discusses a fuller
exponential-backoff scheme specifically motivated by the modem's own
join duty-cycle accounting (1% first hour / 0.1% next 10h / 0.01%
after). This v1 pass implements the ladder's *stop condition*
(`CONFIG_LORA_E5_MAX_RETRIES` full passes -> `ERROR`) faithfully, but
not the duty-cycle-aware backoff curve. Flagged as a simplification
worth review before a real regulatory-duty-cycle-sensitive deployment,
not silently omitted.

### 13. `lora_e5_config`'s port/ADR/repeat/retry fields have no public setter
`lora_e5.h` exposes `lora_e5_set_otaa()`/`_abp()`/`_region()`/`_class()`
but nothing for `struct lora_e5_config`'s `port`, `adr_enable`,
`unconfirmed_repeats`, or `confirmed_retries` fields -- all four are
read by the CONFIG sequence (`AT+PORT`/`AT+ADR`/`AT+REPT`/`AT+RETRY`).
`lora_e5.c` fixes v1 defaults (`port=1`, `adr_enable=true`,
`unconfirmed_repeats=1`, `confirmed_retries=1`) rather than inventing a
new public setter function (a third, unauthorized header contract
addition beyond the two flagged in
`docs/Phase3-ESP32S3-Bringup-Plan.md`). If per-application control over
these is actually needed, that's a real `lora_e5.h` API gap to decide
on deliberately, not something to silently work around further.

### 14. Fixed: `te_reset`/`te_fdefault` used a terminal-match mode that could never fire
Pre-existing bug (already present in the initial commit, unrelated to
the FSM/events/public-API work in `docs/Phase3-ESP32S3-Bringup-Plan.md`)
found while getting `tests/modem_manager` building and passing again:
`te_reset`/`te_fdefault` in `lora_e5_hf_commands.c` used
`LORA_E5_AT_MATCH_EXACT` against `remainder = "OK"`, but
`lora_e5_at_parse_line()`'s `"+PREFIX: OK"` handling classifies the
line as `kind == LORA_E5_AT_LINE_OK` and returns *without ever setting
`line->remainder`* -- so `line->remainder == NULL` and the EXACT match
(which requires both sides non-NULL) could never succeed. In practice
this meant `lora_e5_mm_reset()`/`lora_e5_mm_factory_reset()` would hang
until timeout on real hardware instead of resolving on `"+RESET: OK"`/
`"+FDEFAULT: OK"` -- never previously caught because the test suite
itself had a separate, independent build break (`mock_script_lines`/
`mock_script_count` referenced but never declared -- also fixed this
pass) that prevented it from ever compiling and running. Fixed by
switching both to `LORA_E5_AT_MATCH_ANY_URC` (prefix-only), matching
the convention every other simple confirm-only command in this file
already uses (`te_mode`, `te_port`, etc.).

### 15. `tests/cmd_queue`/`tests/modem_manager` don't wire the real module Kconfig
`docs/Phase3-ESP32S3-Bringup-Plan.md`'s Phase 4 originally proposed
`list(APPEND ZEPHYR_EXTRA_MODULES ...)` + `CONFIG_LORA_E5=y` in these
two suites' `prj.conf` now that the in-source `#ifndef` fallbacks are
gone. Empirically this doesn't work: real values for symbols nested
under `if LORA_E5 ... endif` in Kconfig require `CONFIG_LORA_E5=y`
(confirmed by inspecting a generated `autoconf.h` -- with `LORA_E5`
unset, none of the nested symbols are emitted at all, not even at
their `default`), but setting `CONFIG_LORA_E5=y` unconditionally
triggers `modules/lora_e5/CMakeLists.txt`'s own `zephyr_library()` to
build every `src/*.c` file -- which duplicates whatever subset these
two tests already list directly in their own `target_sources(app ...)`
and would fail to link (multiple definition of the same global
symbols/`LOG_MODULE_REGISTER` storage). Fixed instead with
`target_compile_definitions(app PRIVATE CONFIG_LORA_E5_...=<value>)`
in each test's `CMakeLists.txt`, bypassing Kconfig for just the
handful of symbols these two narrow-scope suites' file subset still
references, while `tests/fsm` and `samples/join` (Phase 5/8, which
genuinely need the whole library) use the real
`ZEPHYR_EXTRA_MODULES`+`CONFIG_LORA_E5=y` path.

### 16. No path for an RX-side UART fault (overlong/malformed line) to reach the FSM
`lora_e5_uart.c` drops an overlong or malformed line and resets its
assembly buffer (logged via a counter only), but there is no path in
the current `lora_e5_at.h` contract for the UART backend (which has no
FSM/Modem-Manager reference by design, Phase 1 §2.1) to signal this
fault upward the way a TX-side `uart_tx()` failure naturally does via
`lora_e5_at_send_fn_t`'s return value (which `lora_e5_cmd_queue.c`
already turns into `LORA_E5_AT_OUTCOME_UART_ERROR` -> `UART_FAULT` ->
recovery, no gap there). Fixing this would mean adding a new
transport-to-command-manager reporting hook to `lora_e5_at.h` (e.g. an
`lora_e5_at_report_transport_fault()` entry point) -- a layering-
sensitive header change, flagged for review rather than added
unilaterally in this pass. In practice this only matters if the
physical link itself is actively corrupting bytes; a healthy UART link
should never hit this path.

---

## Resolved this pass

- ~~`AT+MSGHEX`/`AT+CMSGHEX` response prefix~~ -- **[Certain]**,
  confirmed via disk91.com captured log: uses its own prefix
  (`+MSGHEX:`/`+CMSGHEX:`), not `+MSG:`/`+CMSG:`. Fixed in
  `lora_e5_hf_commands.c`.
- ~~`AT+ADR` set syntax and echo~~ -- **[Certain]**, confirmed via
  andresoliva/LoRa-E5 captured log (`AT+ADR=OFF` -> `+ADR: OFF`).
- ~~`AT+RETRY` set syntax~~ -- **[Certain]**, confirmed via Seeed wiki
  relay-function captured log.
- ~~`AT+JOIN` full response sequence (success and failure)~~ --
  **[Certain]**, confirmed via two independent captured logs.
- ~~`AT+KEY` set syntax~~ -- **[Certain]**, confirmed via CampusIoT
  captured log.
- ~~Full Table 2-1 error code list (9 codes)~~ -- **[Certain]**,
  confirmed against primary spec PDF text directly.
- ~~Factory default mode is LWABP, not LWOTAA~~ -- **[Certain]**,
  confirmed against primary spec PDF Table 4-2.
- ~~`AT+ID` query 3-line shape and field order~~ -- **[Certain]**,
  confirmed via two independent captured logs, consistent field order
  (DevAddr, DevEui, AppEui) in both.

## Resolved 2026-07-05 (real hardware: LoRa-E5-HF, firmware V4.0.11, `/dev/ttyUSB1`)

- ~~`AT+ID` SET syntax~~ -- **[Certain]**. `AT+ID=DevEui,26C518F8EF840E5D`
  (plain hex, no colons) accepted; echoed as
  `+ID: DevEui, 26:C5:18:F8:EF:84:0E:5D` (colon-separated, matching the
  query display format even though the SET argument is plain hex).
  Round-tripped to the device's own existing value, no identity change.
  Fixed doc comments in `lora_e5_hf_commands.c`/`.h`.
- ~~`AT+LW=LEN` max-payload query~~ -- **[Certain]**. Returns
  `+LW: LEN, 51` (device was on IN865/DR0 at capture time). Also
  confirmed in the same session: `AT+LW=CDR` -> `+LW: CDR, TXDR(0,7),
  RXDR(0,7)`; `AT+LW=ULDL` -> `+LW: ULDL, 0, 0`; `AT+LW=NET` ->
  `+LW: NET, ON`; `AT+LW=DC` -> `+LW: DC, OFF, 0`; `AT+LW=MC` ->
  `+LW: MC, OFF, 55b8da01, 0`; `AT+LW=THLD` -> `+LW: THLD, -85`. Bare
  `AT+LW` (no subcommand) returns `ERROR(-1)`. Builder implemented in
  `lora_e5_hf_commands.c`; `lora_e5_mm_get_max_payload()` still returns
  `-ENOTSUP`, but now because of item 10 (line-capture gap), not
  syntax uncertainty.
- ~~Config-command echo format (MODE, DR, PORT, CLASS, REPT)~~ --
  **[Certain]**. All five confirmed to echo `"+CMD: VALUE"`:
  `AT+MODE=LWOTAA` -> `+MODE: LWOTAA`, `AT+PORT=8` -> `+PORT: 8`,
  `AT+CLASS=A` -> `+CLASS: A`, `AT+REPT=1` -> `+REPT: 1`,
  `AT+DR=IN865` -> `+DR: IN865` (region set, see below).
- ~~Six of twelve region strings~~ (`US915HYBRID`/`CN779`/`EU433`/
  `AU915OLD`/`CN470`/`RU864`) -- **[Certain]**. All six accepted
  verbatim as spelled in `region_strings[]` (one word, no underscore --
  `AT+DR=US915_HYBRID` and `AT+DR=AU915_OLD` were tried and both
  rejected with `ERROR(-1)`, confirming the underscore-free spelling is
  the correct one). Device reverted to its original `IN865` after the
  test sequence.
- ~~`AT+MSGHEX`/`AT+CMSGHEX` port parameter behavior~~ -- **[Certain]**,
  confirmed end-to-end over a real network (real `AT+JOIN`, IN865,
  ChirpStack). Sent 4 uplinks: `aa01` with `AT+PORT=5` just set ->
  ChirpStack decoded `FPort: 5`; `aa02` with **no** `AT+PORT` reissued
  (still 5) -> also `FPort: 5`; `aa03` with `AT+PORT=10` just set ->
  `FPort: 10`; `aa04` with **no** reissue (still 10) -> also
  `FPort: 10`. Conclusively proves the port is a persistent modem-side
  setting, not a per-`AT+MSGHEX` argument -- a stale `AT+PORT` value
  silently determines the wire port with no error surfaced. The fix
  predicted here is already implemented in
  `lora_e5_mm_send()` (`lora_e5_modem_manager.c`, `g_port_cached`/
  `g_port_valid`, reissues `AT+PORT=` only when the caller's port
  differs) -- this test empirically validates that fix was both
  necessary and sufficient.

## Resolved 2026-07-09 (real hardware: LoRa-E5-HF, ESP32-S3 UART2/GPIO39-40, firmware V4.0.11)

- ~~Command retries could permanently lose their own timeout~~ --
  **[Certain]**, confirmed by an actual flash+run with temporary
  `LOG_ERR` instrumentation logging `k_work_schedule_for_queue()`'s
  return code directly. `lora_e5_cmd_queue.c`'s `timeout_handler()`
  (the delayed-work handler for `timeout_work`) resends a timed-out
  command and rearms its own timeout by calling
  `k_work_schedule_for_queue(rx_wq, &timeout_work, ...)` -- from
  *inside* `timeout_work`'s own currently-running handler (both the
  per-command retry branch directly in `timeout_handler()`, and via
  `try_dispatch_and_cascade_locked()` when a match/timeout resolves
  and immediately dispatches the next queued command). Zephyr's
  `k_work_schedule_for_queue()` is documented as a no-op "if the work
  item is already scheduled or submitted" -- and the kernel still
  considers a `k_work_delayable` "submitted" while its own handler is
  running, so this self-rescheduling call silently did nothing
  (confirmed: return code `0`, meaning "already scheduled", not `1`).
  The retry's resend went out over the wire correctly, but with no
  timeout backing it -- if that resend's response was ever slow or
  absent, the command (and the whole boot sequence behind it) would
  hang forever with no recovery, no log, nothing, since the one
  mechanism that would have caught it never actually got armed. This
  is almost certainly why the probe step above appeared flaky rather
  than consistently broken across different capture runs before both
  bugs were found: the *first* attempt's timeout is scheduled from a
  different, unaffected call site (the initial dispatch, not from
  inside `timeout_work`), so it always fired correctly; only a retry's
  *second* timeout was silently dead, and whether that mattered
  depended on whether the retried command's response happened to
  arrive before the (nonexistent) backup timeout would have fired.
  native_sim's test suites never caught this because every mocked
  responder answers near-instantly, so a real retry-then-actually-
  time-out sequence never got exercised end-to-end -- only real UART
  timing (or a deliberately slow/absent mock response with a long
  enough wait) exposes it. Fixed: the two self-rescheduling call sites
  in `lora_e5_cmd_queue.c` (`timeout_handler()`'s retry branch, and
  `try_dispatch_and_cascade_locked()`), plus one narrower instance of
  the identical pattern in `lora_e5_fsm.c`'s `fail_recovery_pass()`
  (reachable from `recovery_retry_expired()` -> `enter_recovery()` ->
  here when `do_recovery_reset()` fails synchronously), now use
  `k_work_reschedule_for_queue()` instead, which unconditionally
  cancels and re-arms even while the work item is still marked
  running. No test added: reproducing the exact "self-reschedule from
  within your own still-running handler" kernel state reliably under
  native_sim would need either real UART-like latency or invasive
  fakery of `k_work_delayable` internals: not attempted here, flagged
  for a future pass instead.
- ~~`te_probe`'s match mode~~ -- **[Certain]**. The probe step
  (`lora_e5_hf_build_probe()`, `lora_e5_hf_commands.c`) used
  `LORA_E5_AT_MATCH_BARE_OK` (requires `line->prefix[0] == '\0'`),
  assuming plain `AT` gets back a bare `"OK"`. Captured directly off
  the real module: `AT` -> `"+AT: OK"` (prefix `"AT"`, same
  `+PREFIX: OK` convention as every other command in this file). A
  bare-`OK` match can never fire against that response, so
  `LORA_E5_FSM_EVT_PROBE_RESULT` was never emitted and boot silently
  hung in `CHECK_AT` until the caller's outer timeout -- with no
  recovery-ladder warning logged, since the command layer never saw a
  terminal failure either, just perpetual non-match. Same bug class as
  the already-documented `te_reset`/`te_fdefault` fix above. Fixed:
  `te_probe` now uses `LORA_E5_AT_MATCH_ANY_URC` with `prefix = "AT"`,
  matching the established convention. `tests/modem_manager/src/main.c`
  (`test_probe_success`) and `tests/fsm/src/main.c`'s auto-responders
  were also fixed -- they had been mocking the same wrong bare-`"OK"`
  vector, which is exactly how this got past the existing test suite.
- ~~ESP32 GDMA devicetree `dmas` channel-index parity~~ -- **[Certain]**,
  confirmed by an actual flash+run with temporary `LOG_ERR`
  instrumentation in `lora_e5_uart_send()`. `external/zephyr`'s
  `drivers/dma/dma_esp32_gdma.c` derives the physical GDMA pair as
  `channel_id = channel / 2`, and *separately* its IRQ dispatch
  hardcodes which parity is which: `dma_esp32_isr_handle_rx()`/
  `_handle_tx()` (and the per-pair ISRs generated by
  `DMA_ESP32_DEFINE_IRQ_HANDLER`) look up the completion callback via
  `dma_channel[pair*2]` for RX and `dma_channel[pair*2+1]` for TX --
  unconditionally, regardless of which direction was actually
  `dma_config()`'d onto that array index. `dma_esp32_config()` itself
  has no such restriction (it dispatches on
  `config_dma->channel_direction`, so a "wrong-parity" TX channel
  configures and `uart_tx()` returns 0 successfully) -- so the failure
  is silent and delayed: the real hardware TX-EOF interrupt fires, but
  the ISR reads the *other* array slot (never configured, `cb ==
  NULL`), drops the completion event, and the UART backend's
  single-in-flight TX-busy flag never clears. Every command after the
  first then fails immediately with our own `-EBUSY`, easy to
  misdiagnose as a DMA-channel-conflict/busy-status bug (an earlier
  pass here misdiagnosed it exactly that way before an instrumented
  run pinned it down). Fixing the parity (`dmas = <&dma 1>, <&dma 2>`)
  did resolve *this specific* symptom, but a second, harder-to-pin
  GDMA-related issue remained -- see the next entry. Moot now: the
  Async API + GDMA path was abandoned entirely (below), so this
  finding is kept only as a record of a real, independently valid
  Zephyr/hal_espressif driver-level bug, in case a future pass ever
  goes back to the Async API on this or another ESP32 board.
- ~~Intermittent "scheduled command timeout never fires" hang~~ --
  **[Certain]** it happened, root cause still only narrowed, not
  nailed down -- and no longer relevant, because the code path it
  lived in (Async API + GDMA) was replaced. After fixing the DMA
  parity bug above, boot still hung intermittently and
  unpredictably (sometimes at the first `AT` probe, sometimes one
  command later, never at a fixed point) with symptoms distinct from
  every bug already documented here: `k_work_schedule_for_queue()`/
  `k_work_reschedule_for_queue()` would report success (return code
  confirms the timeout was accepted) and the preceding TX would
  complete (`UART_TX_DONE` observed), yet `lora_e5_cmd_queue.c`'s
  `timeout_handler()` would sometimes simply never fire -- no retry,
  no error, no log, indefinitely. A same-process, real (non-DMA)
  ESP-IDF/Arduino UART passthrough sketch on the identical
  GPIO39/40 wiring, run interleaved with these tests, round-tripped
  `AT`/`AT+JOIN`/`AT+MSGHEX` reliably every time and independently
  joined + sent a real uplink (confirmed in ChirpStack) -- ruling out
  wiring, the module, and the OTAA credentials as the cause, and
  pointing squarely at the Zephyr ESP32 Async-API/GDMA stack. A
  recurring no-op `k_work_delayable` ("keep the kernel's global
  timeout list non-empty") fixed it in some but not all flash+run
  attempts -- inconsistent enough that it was **not** shipped
  (removed after failing two repeat trials); disabling
  `CONFIG_TICKLESS_KERNEL` did not help either. Root cause never
  conclusively identified. Resolved by working around it rather than
  fixing it: `lora_e5_uart.c` was rewritten to use the plain
  Interrupt-Driven UART API (`CONFIG_UART_INTERRUPT_DRIVEN`,
  `uart_irq_*`/`uart_fifo_*`) instead of the Async API, eliminating
  GDMA from the picture entirely -- the same approach the working
  Arduino sketch used. Confirmed by multiple full flash+run cycles:
  boot/config/join/send all completing cleanly with deterministic,
  repeatable timing and zero unexplained hangs, including one run
  where `AT+JOIN` legitimately timed out (real network-side timing)
  and the recovery ladder correctly recovered back to `READY` right
  on schedule -- i.e. the timeout/retry machinery itself now behaves
  exactly as designed under real hardware timing. If a future pass
  ever needs the Async API back (e.g. for DMA's lower CPU overhead
  on very high uplink rates), this bug needs to be actually
  root-caused first, not just avoided.
- ~~UART1 (GPIO17/18) vs UART2 (GPIO39/40) on esp32s3_devkitc~~ --
  switched during this bring-up pass to UART2/GPIO39-40 for the
  physical wiring in use; independent of the Async-vs-Interrupt-Driven
  UART API choice, so this is a wiring choice, not a finding. See
  `samples/join/boards/esp32s3_devkitc_procpu.overlay`.
- ~~`modules/lora_e5/Kconfig`'s `select GPIO` triggering a Kconfig
  dependency-loop error~~ -- **[Certain]**. Switching
  `menuconfig LORA_E5`'s UART dependency from `depends on
  UART_ASYNC_API` to `depends on UART_INTERRUPT_DRIVEN` (previous
  entry) exposed a pre-existing structural cycle in this Zephyr
  tree's driver Kconfig graph (`I2C -> MFD -> GPIO_ITE_IT8801 -> MFD
  -> GPIO -> ... -> I2C`, none of which this module touches or
  enables) -- kconfiglib detects this at parse time from the
  declared `select`/reverse-dependency expressions themselves,
  independent of what any symbol's final value ends up being, so no
  amount of explicitly setting individual symbols to `n` broke it.
  Confirmed via a minimal isolated probe app that the trigger is
  specifically `select GPIO` (a "hard" reverse dependency) combined
  with `depends on UART_INTERRUPT_DRIVEN` being visible -- swapping
  `select GPIO` for `depends on GPIO` (a "soft" one, and something
  the application already sets explicitly via `CONFIG_GPIO=y`
  anyway) resolves the loop with no other changes. A Zephyr Kconfig
  tree issue, not a lora_e5 logic bug, but the module's own
  `select` was the direct trigger, so fixed here rather than filed
  separately.

---

## Resolved 2026-07-11 (real hardware: LoRa-E5-HF, ESP32-S3 UART2/GPIO39-40, firmware V4.0.11 -- module continuously powered, never externally reset, across every step below)

- ~~Can the LoRa-E5's own network/join status be queried, and does a
  retained session let boot skip AT+RESET/AT+JOIN?~~ -- investigated in
  two passes; the first pass's own premise was wrong and got corrected
  by the second, so both are recorded here rather than silently
  overwritten:
  1. **First pass (WRONG, corrected below)**: assumed `AT+LW=NET`
     ("+LW: NET, ON" observed in an earlier session's capture) was a
     join/network-status query, without checking the primary spec PDF
     first. Built `lora_e5_get_join_status()` on that assumption,
     confirmed on real hardware that it read `joined=1` (i.e. `NET,
     ON`) both before AND after a full `lora_e5_start_sync()`
     (`AT+RESET` + `CONFIG`) -- true, but not evidence of anything
     join-related. A follow-up test then called `lora_e5_join_sync()`
     right after and timed it at **8015 ms**, statistically identical
     to a real fresh OTAA handshake, not an instant "already joined"
     result -- which read at the time as "the query doesn't predict
     the fast path," rather than the real explanation (below).
  2. **Corrected [Certain, primary spec PDF §4.28.4]**: `AT+LW=NET`
     selects/reports the standard LoRaWAN **public-vs-private network
     sync word** ("Set ON to choose public network, set OFF to choose
     private network") -- a static configuration flag, completely
     unrelated to join/session state. It read `ON` consistently for
     the boring reason that it's a config setting, not a live
     indicator; the timing "finding" above was correct data pointing
     at the wrong cause. Renamed throughout to
     `lora_e5_get_public_network_mode()` /
     `lora_e5_mm_get_public_network_mode()` /
     `lora_e5_hf_build_public_network_query()` to stop implying
     otherwise. **How this was caught**: cross-checking against the
     primary AT Command Specification PDF (`docs/LoRa-E5 AT Command
     Specification_V1.0 .pdf`), which had not been consulted before
     building the query -- exactly the failure mode this file's
     confidence-tagging convention exists to prevent, and a reminder
     to check the primary source before shipping a `[Certain, real
     hardware]` tag on an *interpretation* of a capture, not just the
     capture's literal text.
  3. **The real join-status mechanism, per spec §4.5.2**: `AT+JOIN`
     itself returns `"+JOIN: Joined already"` when "LoRaWAN modem
     already joined to a network previously" (note: "use AT+JOIN=FORCE
     to force join if needed") -- already modeled in this codebase as
     `LORA_E5_MM_TAG_JOIN_ALREADY` in `te_join[]`
     (`lora_e5_hf_commands.c`), mapped to a success outcome. This is
     the spec-documented way to detect/benefit from a retained
     session -- not a separate status query.
  4. **AT+MODE reissue hypothesis tested and REFUTED [Certain, real
     hardware, 2026-07-11]**. Isolated test: temporarily patched
     `submit_config_step()` (`lora_e5_modem_manager.c`) to
     unconditionally skip the `CFG_STEP_MODE` step (no `AT+MODE=LWOTAA`
     sent at all, every other CONFIG step unchanged), rebuilt/reflashed
     `samples/device_node`, and timed the resulting
     `lora_e5_join_sync()` from the `JOINING`/`JOIN_SUCCESS`
     `STATE_CHANGED` log timestamps: **8033 ms**, statistically
     identical to the un-patched 8015 ms baseline, with a new DevAddr
     again (`00096F32`) -- a full fresh OTAA handshake either way.
     Skipping the `AT+MODE` reissue made no measurable difference.
     Change fully reverted (`git checkout`) immediately after the
     measurement -- nothing from this experiment shipped.
  5. **`AT+RESET` confirmed as the cause [Certain, real hardware,
     2026-07-11]**. Isolated test: a throwaway diagnostic that calls
     `lora_e5_mm_join()` directly (bypassing the FSM/public API
     entirely, via an extra include path into the module's internal
     `src/` headers) as the **literal first AT command issued this
     power cycle** -- no `AT+RESET`, no `CONFIG`, nothing else sent
     first, module continuously powered from a prior successful join in
     an earlier test. Result: `JOIN_RESULT` arrived in **57 ms** (not
     ~8000 ms) with `dev_addr` all-zero (no DevAddr in the response,
     matching the spec's `"+JOIN: Joined already"` example, which has
     no NetID/DevAddr line) -- the fast path. A direct `lora_e5_mm_send()`
     issued immediately after (also bypassing the FSM) succeeded too
     (`fail_reason=NONE`, ~1.3 s including the automatic `AT+PORT=`
     reissue since no CONFIG had run this cycle). Combined with item 4
     above (full `CONFIG` minus `AT+MODE`, but *with* `AT+RESET`, still
     took the full ~8 s handshake): **`AT+RESET` is specifically what
     invalidates the join session** -- not `AT+MODE`, not the
     `AT+ID`/`AT+KEY` reissues. This directly confirms the original
     question ("can we skip reset and push a message directly if
     already joined") -- **yes**, provided `AT+RESET` is skipped.
  6. **Shipped as a real public API [Certain, real hardware,
     2026-07-11]**: `lora_e5_resume()`/`lora_e5_resume_sync()`
     (`lora_e5.h`) -- a new `LORA_E5_STATE_RESUMING` FSM state that
     probes without `AT+RESET`, attempts `AT+JOIN` directly on success
     (skipping `CONFIG` entirely), and falls back to the ordinary full
     `RESET`+`CONFIG`+`JOIN` sequence via the *existing, unmodified*
     `recovery_step_failed()`/recovery-ladder machinery if the probe or
     the fast join attempt fails for any reason -- no new fallback
     logic needed, no changes to `CONFIG` sequencing, no change to
     `lora_e5_start_sync()`'s behavior, and CLAUDE.md decision #2 ("v1
     does NOT auto-join after CONFIG") holds exactly as before on every
     path except this one explicitly-opted-into call. Two new
     `tests/fsm` cases cover both the fast path and the fallback path.
     Wired into `samples/device_node` in place of
     `start_sync()`+`join_sync()`; confirmed on real hardware across
     three consecutive real deep-sleep/wake cycles, **every single one**
     hit the fast path (`STATE_CHANGED` sequence `RESUMING -> JOINING
     -> JOINED`, skipping `RESET`/`BOOT`/`CHECK_AT`/`CONFIG`/`READY`
     entirely), `JOIN_SUCCESS` arriving ~58ms after boot each time --
     total wake-to-sleep time dropped from ~15.7s to ~1.5-3.4s per
     cycle. One cosmetic side effect observed and not fixed: the
     `JOIN_SUCCESS` event's `dev_addr` reads all-zero on the fast path
     (the modem's `"+JOIN: Joined already"` response has no DevAddr
     line to report, per item 5 above) -- functionally harmless (the
     modem itself still knows its real DevAddr for the actual send),
     but an application logging/displaying `dev_addr` from this event
     will see zeros on fast-path wakes. Still open, not yet tested:
     whether ESP32 boot-time UART line noise (mentioned throughout this
     file's Async-API investigation) could desync the modem's AT parser
     in a way only a real `AT+RESET` recovers from -- if so, the
     fallback path (already implemented and tested to work when the
     probe fails) is exactly what handles it, so this is a "does the
     safety net get exercised in practice" question, not a gap in the
     implementation.
  Implemented as part of this investigation, independent of the
  answer above and reusable regardless: `struct lora_e5_at_result`
  (`lora_e5_at.h`) gained a `captured_text`/`captured_text_len` field
  so a matched terminal line's actual text can finally reach a
  caller -- previously `lora_e5_mm_get_version()`/`get_ids()`/
  `get_max_payload()` were all stubbed to `-ENOTSUP` for exactly this
  missing capability (see their doc comments, not yet fixed to use the
  new field -- only `lora_e5_mm_get_public_network_mode()` does).
- ~~Adding a text-capture field to `struct lora_e5_at_result` is
  "just" a struct change~~ -- **[Certain, real hardware, the hard
  way]**. That struct is embedded in a fixed `cascade[MAX_CASCADE]`
  array (`MAX_CASCADE` = `CONFIG_LORA_E5_CMD_QUEUE_DEPTH + 1`, 9 by
  default) that is a **stack-local** variable in several of
  `lora_e5_cmd_queue.c`'s RX-work-queue functions -- every byte added
  to the struct is multiplied by 9 against
  `CONFIG_LORA_E5_RX_STACK_SIZE`'s budget. A first attempt
  (`LORA_E5_AT_CAPTURED_TEXT_MAX = 63`, stack left at the old default
  1536) crashed on real hardware with `EXCCAUSE 28 (load prohibited)`
  during a context restore -- a classic stack-overflow signature, not
  an obviously-attributable fault from the crash trace alone. Shrinking
  the cap to 31 and bumping the stack to 2048 (a reasoned-but-not-
  measured estimate) **still hung silently** at the very first
  post-boot AT transaction, with no crash at all -- worse than the
  crash, since a clean-looking hang gives no signal that stack size is
  even the right thing to suspect. Bisected empirically by rebuilding
  the same firmware with the RX stack at 4096: full real
  boot/join/send/sleep cycle completed successfully. `native_sim`'s 57
  tests kept passing throughout all of this (host-native pthread
  stacks don't reproduce embedded stack budgets), so this class of bug
  is real-hardware-only to catch. `CONFIG_LORA_E5_RX_STACK_SIZE`'s
  default is now 4096, with the empirical data points recorded in its
  Kconfig help text so a future size change gets re-verified against
  real hardware rather than re-estimated from arithmetic.
