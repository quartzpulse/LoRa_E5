# Bring up LoRa-E5 library on ESP32-S3-DevKitC (UART1) — FSM + public API + real hardware test

## Context

The LoRa-E5 Zephyr library currently has a working, hardware-verified AT
layer (`lora_e5_at.c`/`lora_e5_cmd_queue.c`/`lora_e5_parser.c`) and Modem
Manager (`lora_e5_modem_manager.c`) — both tested on `native_sim` and,
for AT-command syntax, against the real LoRa-E5-HF module. Everything
above that layer is still unwritten: the FSM, the event/notify system,
the public API, the real UART backend, and any sample app. The user
wants to actually run this on an ESP32-S3-DevKitC board, wired to the
LoRa-E5 on UART1 (default pins TX=GPIO17/RX=GPIO18, already have a
pinctrl entry in the board file, just not enabled), and see a real
join+send happen end-to-end through the full library stack rather than
through hand-rolled AT commands.

This plan completes the remaining architecture (FSM, events, public
API), wires up Kconfig/CMakeLists so the module builds as a real Zephyr
module, writes the real UART Async API backend, adds a board overlay
for `esp32s3_devkitc` UART1, and builds a `samples/join/` app to prove
it end-to-end against the same ChirpStack instance already used for
hardware verification.

All of the state/event/API shapes below are the *actual* header
contracts already checked into the repo
(`include/lora_e5/lora_e5.h`, `lora_e5_events.h`, `lora_e5_types.h`,
`lora_e5_config.h`, `src/lora_e5_modem_manager.h`) and Phase 1/2 design
docs — not something invented for this plan. Two small, deliberate
additions to those contracts are called out explicitly below since
they're gaps I found by reading the actual code, not aesthetic choices.

## Two contract additions (small, justified — flagging before I touch headers)

1. **`lora_e5_fsm_event`'s `CONFIG_STEP_RESULT` payload needs a
   "last step" flag.** Confirmed by reading `mm_config_step_cb()`
   (`lora_e5_modem_manager.c:684`): it emits one `CONFIG_STEP_RESULT`
   event per CONFIG sub-command (9 total), including the final one, and
   the *only* payload is `enum lora_e5_at_error config_step_error`
   (meaningful only on failure). There is currently no way for the FSM
   to tell "one step just succeeded, more coming" from "all 9 steps
   succeeded, move to READY" — it would have to hardcode the step count
   itself, which duplicates knowledge Modem Manager already owns and
   silently desyncs if that step list ever changes. Fix: change that
   union member from a bare enum to
   `struct lora_e5_fsm_config_step_result { enum lora_e5_at_error error; bool is_last_step; }`
   in `lora_e5_events.h`, and set `is_last_step` in
   `mm_config_step_cb()` from the existing `g_config.step >=
   CFG_STEP_COUNT` check it already computes. Minimal, mechanical,
   keeps the step-count knowledge where it already lives.
2. **Two new Kconfig symbols not in the documented list** (both
   referenced by design intent but never named): `CONFIG_LORA_E5_BOOT_SETTLE_MS`
   (Phase 1 §5.1 says `BOOT`'s entry action is "start settle timer" but
   no symbol was ever assigned to its duration) and
   `CONFIG_LORA_E5_UART_LINE_QUEUE_DEPTH` (bounds the small fixed-depth
   queue of assembled lines between the UART callback and the RX work
   queue in the new `lora_e5_uart.c` — needed because Phase 1 §4's own
   diagram splits "line-complete detection" (cheap, possibly ISR
   context) from "line copy + parse" (RX work queue), which needs a
   bounded handoff structure that was never named as a Kconfig symbol).

## Implementation order

### Phase 0 — header additions
- `include/lora_e5/lora_e5_events.h`: the `CONFIG_STEP_RESULT` struct
  change above.
- `modules/lora_e5/src/lora_e5_modem_manager.c`: update
  `mm_config_step_cb()` to set `is_last_step`.
- `modules/lora_e5/src/lora_e5_internal.h` (new): shared internal
  prototypes used across `lora_e5.c`/`lora_e5_fsm.c`/`lora_e5_events.c`
  — `lora_e5_fsm_init()`, `lora_e5_fsm_post_event()`,
  `lora_e5_fsm_get_state_sync()` (round-trips through the FSM queue per
  Phase 1 §4's recommendation, not a raw mutex peek), and
  `lora_e5_notify_init()`/`lora_e5_notify_set_callback()`/
  `lora_e5_notify_post()`. Kept deliberately thin per Phase 1 §2.8.

### Phase 1 — `lora_e5_events.c`
Owns the third ("notify") work queue and its `k_msgq`
(`CONFIG_LORA_E5_EVENT_QUEUE_DEPTH`-sized) per Phase 1 §4's explicit
recommendation that application-callback dispatch never runs on the FSM
queue directly. `lora_e5_notify_post()` is non-blocking; a `k_work`
item drains the queue and invokes whatever callback
`lora_e5_register_callback()` registered. No FSM logic here — per
Phase 1 §2.5 ("resist the temptation to put FSM logic here"), the
`lora_e5_app_event` is constructed by the FSM itself (it already has
the tagged-union data from its own `lora_e5_fsm_event`), not translated
here.

### Phase 2 — `lora_e5_fsm.c` (the core of this work)
Implements the full state machine from Phase 1 §5.1/§5.2 using the
*actual* `enum lora_e5_state` (already final — `WAIT_TX_RESULT`, not
separate RX1/RX2 states, per CLAUDE.md decision #3) and the finalized
CLAUDE.md decision #2 correction (TX/sleep/reset/leave always return to
`JOINED`, never `READY` — supersedes Phase 1 §5.2's own table on this
one point). Registers itself as Modem Manager's sole event sink via
`lora_e5_mm_set_event_callback()` — since Modem Manager already emits
events pre-shaped as `struct lora_e5_fsm_event`, the callback is a
direct `lora_e5_fsm_post_event()` call, no translation layer needed
(confirms the layering is clean).

Two deliberate simplifications vs. Phase 1's original suggestion,
based on what Modem Manager actually does (confirmed by reading
`lora_e5_modem_manager.c`):
- **No separate FSM-level join timeout.** `lora_e5_mm_join()`'s OTAA
  path is one single AT-manager transaction end-to-end (the
  `Start`/`NetID`/`Done` URCs are intermediate lines *within* that one
  transaction, not separate transactions) — so the AT layer's own
  per-transaction timeout (`CONFIG_LORA_E5_JOIN_TIMEOUT_MS`) already
  bounds the whole join attempt correctly. `LORA_E5_FSM_EVT_JOIN_TIMEOUT`
  stays declared for defensive handling but isn't expected to be the
  primary path — I'll confirm the timeout→outcome mapping in
  `mm_join_result_cb()` while writing this and adjust if it turns out
  incomplete.
- **No separate FSM-level ack timeout for confirmed uplinks.** Same
  reasoning — the modem's internal confirmed-retry ladder runs inside
  one `AT+CMSG` transaction, bounded by `CONFIG_LORA_E5_TX_TIMEOUT_MS`
  at the AT layer, surfaced as `TX_RESULT` with `fail_reason` already
  distinguishing `NO_ACK` from `TIMEOUT`.

Owns: current state, provisioning config copy, retry counters
(CHECK_AT/CONFIG-step/recovery-ladder), recovery-ladder step tracking
(Phase 1 §8.2: retry → reset → reconfigure → rejoin-if-`AUTO_REJOIN`,
stop after `CONFIG_LORA_E5_MAX_RETRIES` full passes → `ERROR`), the
boot-settle `k_work_delayable`, and the static TX-request staging
(pointer/len/port/confirmed, written by `lora_e5.c` before the
`TX_REQUEST` event is posted). `lora_e5_at_error_is_structural()`
(already in `lora_e5_types.h`) is the classifier driving RECOVERING-vs-
ERROR on any CONFIG/structural failure, per Phase 1 §8.1 — never retry
a structural error.

### Phase 3 — `lora_e5.c` (public API)
Thin translation layer per Phase 1 §2.7: each function builds/posts the
matching `lora_e5_fsm_event` (or calls straight into a synchronous
Modem Manager query for `get_version`/`get_ids`/`get_max_payload`).
`lora_e5_init()` creates and starts the three dedicated work queues
(rx/fsm/notify — never `k_sys_work_q`, per Phase 1 §4) and wires
`lora_e5_at_init()` → `lora_e5_uart_init()` (registers transport) →
`lora_e5_mm_init()` → `lora_e5_fsm_init()` → `lora_e5_notify_init()`, in
that order (matches the dependency chain each layer's init doc comment
implies). `_sync` variants reuse the same serialization pattern already
established and accepted at the AT layer (`lora_e5_at_submit_sync()`'s
single static context + mutex, documented as trade-off #7 in
`VERIFICATION_NEEDED.md`): one static waiter record + one mutex
serializing all synchronous callers app-wide, signaled by
`lora_e5_events.c`'s dispatch when a matching terminal app-event fires
— this runs *alongside* the normal registered callback, not instead of
it, so a blocking sync call from one thread doesn't suppress the
app's own async callback registration. TX staging uses a single static
`CONFIG_LORA_E5_TX_BUFFER_SIZE`-sized buffer (default 242 bytes, max
LoRaWAN payload per Table 3-3) — sufficient because the FSM only
allows one TX in flight at a time by construction.

`lora_e5_get_version()`/`get_ids()`/`get_max_payload()` currently
**cannot** work regardless of this effort — `lora_e5_mm_get_version()`
etc. all return `-ENOTSUP` today because of the line-capture
architecture gap already logged as item 10 in `VERIFICATION_NEEDED.md`
(no path to read a matched terminal line's text back out of
`lora_e5_at_submit_sync()`). I'll leave these three public functions
thin-wrapping that `-ENOTSUP` for this pass rather than also fixing the
AT-layer contract gap — that's a separate, layering-sensitive change
(touches `lora_e5_at.h`) better done as its own reviewed step, not
bundled into FSM/hardware bring-up. Flagging so it's not mistaken for
an oversight.

### Phase 4 — Module build files
- `modules/lora_e5/Kconfig`: `menuconfig LORA_E5`, all symbols
  documented in `lora_e5_config.h`'s comment block plus the two new
  ones from Phase 0, following the `LORA`/`LORAWAN_SERVICES` in-tree
  pattern (`menuconfig` + `module = LORA_E5` + `source
  "subsys/logging/Kconfig.template.log_config"` for the log level, same
  as `external/zephyr/drivers/lora/Kconfig`).
- `modules/lora_e5/CMakeLists.txt`: `zephyr_library()` +
  `zephyr_library_sources()` for every `src/*.c` file, gated on
  `CONFIG_LORA_E5`, mirroring
  `external/zephyr/subsys/lorawan/services/CMakeLists.txt`'s pattern.
- `modules/lora_e5/zephyr/module.yml`: standard `name:`/`build: {cmake:
  ./, kconfig: ./Kconfig}` so `ZEPHYR_EXTRA_MODULES` picks it up.
- Update `lora_e5_cmd_queue.c` and `lora_e5_at.c` to drop their
  fallback `#define`+comment for `CONFIG_LORA_E5_CMD_QUEUE_DEPTH`/
  `CONFIG_LORA_E5_LOG_LEVEL` now that real Kconfig exists, per
  CLAUDE.md's explicit instruction not to leave both defined.
- Existing tests (`tests/parser`, `tests/cmd_queue`, `tests/modem_manager`)
  currently list source files directly in their own `CMakeLists.txt`
  and never depended on module Kconfig — add
  `list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../..)`
  before `find_package(Zephyr)` in each, plus `CONFIG_LORA_E5=y` (and
  whatever depth/timeout values each test needs) in each `prj.conf`, so
  they keep building now that the fallback defines are gone.

### Phase 5 — `tests/fsm/` + `tests/mock_uart/`
- `tests/mock_uart/` (new, small shared fixture per CLAUDE.md's
  explicit list of missing pieces, and Phase 1 §10's own note that it's
  "a fixture, not a standalone test suite, build it first"): a
  scriptable fake transport (extends the inline `mock_send`-style
  pattern already used in `tests/modem_manager/src/main.c`) plus a
  TX-capture sink, so `tests/fsm` doesn't duplicate that boilerplate.
- `tests/fsm/`: drives the FSM through real `lora_e5_fsm_post_event()`
  calls plus the real Modem Manager/AT stack underneath (same
  integration-style approach `tests/modem_manager` already uses — mock
  only at the transport boundary, everything above it is real code).
  Covers the properties Phase 1 §10 calls out explicitly: happy-path
  boot→join→send→sleep→wake, join failure→RECOVERING→rejoin,
  structural CONFIG error→ERROR with no retry loop, confirmed-uplink
  no-ack→`TX_FAILED`, recovery-ladder exhaustion→ERROR-and-stay, and the
  regression property that a transport-timeout-classified fault never
  retries a structural-classified fault.
- Fold a handful of public-API-level smoke assertions into this same
  suite (calling `lora_e5_init()`/`lora_e5_join_sync()`/etc. against
  the mock transport) rather than standing up a fourth, separate test
  directory the docs never named.

Run `west twister -p native_sim -T modules/lora_e5/tests -v` after
Phases 0–5 and confirm everything (existing + new suites) passes before
touching anything hardware-specific.

### Phase 6 — `lora_e5_uart.c` (real UART Async API backend)
Generic Zephyr UART Async API backend — nothing ESP32-specific in the
C code itself, only the board devicetree overlay is chip-specific.
Implements the split Phase 1 §4 specifies: the `uart_callback_set()`
callback (may run in ISR/driver-internal context) only accumulates
bytes and detects line completion (`\r\n` boundaries) into a small
fixed-size assembly buffer — no parsing there. Each completed line is
copied into a `CONFIG_LORA_E5_UART_LINE_QUEUE_DEPTH`-deep `k_msgq` of
fixed-size line structs, and a `k_work` item (running on the `rx_wq`
passed to `lora_e5_uart_init()`) drains that queue, calling
`lora_e5_at_parse_line()` then `lora_e5_at_process_line()` for each —
matching the existing project convention of small fixed caps instead of
dynamic sizing. TX: implements the `lora_e5_at_send_fn_t`-conforming
send function, copies the command plus an appended `"\r\n"` into a
static scratch buffer (the terminator isn't part of any
`lora_e5_hf_commands.c` command string — confirmed by reading
`lora_e5_hf_build_mode()` etc., so the transport is responsible for
it), calls `uart_tx()`, and tracks single-in-flight TX state (mirroring
the pattern in `external/zephyr/samples/drivers/uart/async_api/src/main.c`).
RX re-arming uses the same sample's ping-pong `UART_RX_BUF_REQUEST`
pattern for continuous RX. Uses `CONFIG_UART_ASYNC_API` (already
`select`-able by the ESP32-S3 driver per
`external/zephyr/drivers/serial/Kconfig.esp32`), not the interrupt-driven
API, and the two must not be mixed on the same UART instance
(`CONFIG_UART_EXCLUSIVE_API_CALLBACKS` enforces this at the driver
level anyway).

### Phase 7 — `esp32s3_devkitc` board overlay
New overlay under the sample (`samples/join/boards/esp32s3_devkitc_procpu.overlay`,
standard Zephyr per-board-per-app overlay convention) enabling `&uart1`:
`status = "okay"`, `pinctrl-0 = <&uart1_default>` (already exists in
the board's own `esp32s3_devkitc-pinctrl.dtsi`, TX=GPIO17/RX=GPIO18,
confirmed already wired per your earlier answer), `current-speed =
<9600>` (matches the LoRa-E5's fixed default UART framing per Phase 1's
own assumption, already validated live against the real module), plus
`dmas`/`dma-names = "rx", "tx"` picking two free GDMA channels — the
ESP32 async UART driver hard-requires DMA wiring in devicetree, it
isn't just a Kconfig flag (confirmed by reading `uart_esp32.c`
directly: `uart_esp32_async_tx()`/`async_rx_enable()` return
`-ENOTSUP` if the DMA channel is unconfigured).

### Phase 8 — `samples/join/` sample app
Minimal app: `lora_e5_init()` with `hw_config.uart_dev =
DEVICE_DT_GET(DT_NODELABEL(uart1))`, `lora_e5_set_otaa()` (reusing the
DevEUI/AppEUI already confirmed live on your ChirpStack instance;
AppKey needs to be supplied by you since `AT+KEY` values are
write-only, never queryable), `lora_e5_set_region(LORA_E5_REGION_IN865)`,
`lora_e5_start_sync()`, `lora_e5_join_sync()`, then
`lora_e5_send_sync()` a small test payload, logging every step and the
registered callback's events. `sample.yaml`/`prj.conf` mirror
`external/zephyr/samples/drivers/uart/async_api`'s Kconfig-aware
pattern (`CONFIG_LORA_E5=y`, `CONFIG_UART_ASYNC_API=y`) combined with
`hello_world`'s brevity.

## Verification

1. `west twister -p native_sim -T modules/lora_e5/tests -v` — all
   suites (existing + new `tests/fsm`) green, no hardware involved.
2. Real hardware: rewire the LoRa-E5 from the USB-serial adapter
   (`/dev/ttyUSB1`) to the ESP32-S3-DevKitC's GPIO17(TX)/GPIO18(RX)/GND.
   I'll need your ESP32-S3's own serial device node (its USB-CDC port,
   for `west flash` and console monitoring — separate from the E5's
   old `/dev/ttyUSB1`) and the AppKey for the OTAA credentials already
   registered on ChirpStack, at that point.
3. `west build -b esp32s3_devkitc/esp32s3/procpu modules/lora_e5/samples/join -- -DEXTRA_ZEPHYR_MODULES=<repo>/modules/lora_e5`,
   `west flash`, monitor console output for join success + send
   confirmation.
4. Cross-check the resulting uplink on ChirpStack's device event log
   (same method already used for the port-caching verification earlier
   in this project) to confirm the frame actually arrived, not just
   that the ESP32 believes it sent successfully.
