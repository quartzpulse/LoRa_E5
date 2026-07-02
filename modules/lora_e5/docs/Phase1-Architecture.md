# LoRa-E5 Zephyr Library — Phase 1: System Architecture

Grounding note: the AT command surface referenced below (formats, URCs, error codes) is taken from the Seeed **LoRa-E5 AT Command Specification V1.0** (2020-07-20 release, STM32WLE5JC / SX126x module). [Certain] Command *behavior* is stable across the document I fetched, but URC wording has changed across firmware majors before (e.g. `AT+LW=VER` protocol switch changes Class B beacon semantics, and `RXWIN0/1/2/3` labels are firmware-version dependent). Confirm the exact firmware version burned on your units before finalizing parser regexes — do not assume the factory-shipped firmware matches V1.0 of the doc. [Likely]

---

## 1. Overall Architecture

```
Application
   │  register_callback(), join(), send(), sleep()...
   ▼
High-Level Public API        (lora_e5.c)
   │  posts internal events, exposes sync wrappers over sem/k_poll
   ▼
LoRaWAN FSM                  (lora_e5_fsm.c)  ← owns modem state, single writer
   │  issues AT command requests via typed structs
   ▼
AT Command Manager           (lora_e5_at.c, lora_e5_cmd_queue.c)
   │  one in-flight command, timeout+retry, response matching
   ▼
AT Parser                    (lora_e5_parser.c)  ← stateless line → event
   │  consumes complete lines only
   ▼
UART Backend                 (lora_e5_uart.c)  ← byte assembly, ring buffer
   │
   ▼
LoRa-E5-HF (AT firmware over UART)
```

Layer responsibilities, restated tightly:

- **UART Backend**: turns a noisy byte stream into discrete, complete lines. Knows nothing about AT semantics.
- **AT Parser**: turns a line into a typed internal event (`AT_OK`, `AT_ERROR(code)`, `URC_JOIN(...)`, `URC_MSG(...)`, ...). Knows nothing about command sequencing or modem state.
- **AT Command Manager**: owns the *transactional* lifecycle of a single AT command (queue → send → wait → resolve). Knows nothing about LoRaWAN semantics — it would work identically for an RN2483.
- **LoRaWAN FSM**: the only module that knows what "JOINING" or "confirmed uplink" means. Issues abstract intents ("send this join command sequence") to the AT manager and reacts to its results plus asynchronous URCs.
- **Public API**: translates application intent into FSM events and exposes result delivery (callback or blocking wait) without leaking AT vocabulary.

Each layer is **replaceable independently**: a future RN2483 backend swaps the parser + a command-definition table; UART backend, command manager, event system, and FSM core are reused. This is achievable *only* if the FSM talks to the AT manager in terms of abstract command IDs/results, not raw AT strings — see §2.4 for how ownership of the command table is split out.

Design tension worth naming explicitly: strict layering forces every RX event through 4 hops (UART → parser → cmd_queue → FSM) even for the simplest exchange (`AT` → `+AT: OK`). This costs latency (bounded by k_work queue scheduling, sub-ms on typical Cortex-M) but buys testability and reuse. [Likely] For a single-vendor, single-product firmware, a flatter design would be less code — reject that trade here because the future-expansion requirement (RN2483, Type ABZ, Quectel) is explicit in the spec and retrofitting layering later is far more expensive than paying the hop cost now.

---

## 2. Module Breakdown

### 2.1 `lora_e5_uart.c` / (internal header, no public header — backend is not part of public API)

**Purpose**: Byte-level UART I/O only.

**Responsibilities**:
- Configure UART in **async API mode** (`uart_callback_set` + `uart_rx_enable`/`uart_tx`), not IRQ-driven API directly, to avoid hand-rolled ISR buffering. [Likely — recommendation, not the only valid choice; IRQ API + manual ring buffer is the fallback if the board's UART driver lacks async support]
- Feed received bytes into a `ring_buf` from the UART callback context (which on most Zephyr UART drivers *is* ISR or a driver work item — treat it as ISR-context for safety).
- Detect `\r\n` (per spec, commands/responses terminate `<CR><LF>`; `\n` alone is accepted for input, but responses are `\r\n`-terminated) [Certain, per spec §2.1/§2.3] and submit a `k_work` (or push to `k_fifo`) carrying the completed line to the RX work queue — **the line itself, copied out of the ring buffer, not a pointer into it**.
- Serialize TX: single `k_mutex`-guarded submit function; only the AT Command Manager may call it (one command in flight, so no queuing needed inside UART backend itself — the queue lives one layer up).
- Enforce a maximum line length (spec: commands ≤ 528 bytes [Certain]; responses aren't bounded but the largest realistic payload is `+MSG: PORT: n; RX: "<510 hex chars>"` for a max ~255-byte payload in hex+quotes). Overlong lines are dropped with a `UART_ERROR` event and buffer reset, not silently truncated (truncation would let a corrupted line masquerade as a valid short response — worse failure mode than an explicit drop).

**Must not**: parse content, decide OK/ERROR, run in ISR beyond the copy-and-signal step.

**Public interface (internal)**: `uart_backend_init()`, `uart_backend_send_line()`, registration of a single `line_received(const char *line, size_t len)` callback consumed by the parser/cmd_queue integration point — invoked from work-queue context, never ISR.

**Ownership**: owns the UART device handle and both ring buffers (RX assembly, TX staging).

**Dependencies**: Zephyr UART async API, `ring_buf`, `k_work`.

### 2.2 `lora_e5_parser.c` / `include/lora_e5/lora_e5_at.h` (shared line-classification types)

**Purpose**: Pure function — line in, event out. No state retained between calls except optionally a small classification table.

**Responsibilities** — classify a line into one of:
- `AT_OK` (plain `OK`, or the specific echoed `+AT: OK`, `+RESET: OK`, `+FDEFAULT: OK` — note the spec's "Return" convention `+CMD: DATA`, so a generic OK-detector must special-case commands that return `+CMD: OK` rather than a bare `OK`) [Certain, spec is inconsistent between bare `OK` shown in the outline diagram and the actual per-command returns documented in §4 — **this is a real parser hazard, not a simplification**: `AT+RESET` returns `+RESET: OK`, `AT+AT` returns `+AT: OK`, but generic multi-line sequences like `AT+MSG` terminate in `+MSG: Done`, not `OK` at all]
- `AT_ERROR(code)` — matches `ERROR(-N)` / `+CMD: ERROR(-N)` pattern; code taken from Table 2-1 (-1, -10, -11, -12, -20, -21, -22, -23, -24) [Certain]
- `URC_JOIN_*` — `+JOIN: Starting`, `+JOIN: NORMAL`, `+JOIN: NetID xxxxxx DevAddr xx:xx:xx:xx`, `+JOIN: Done`, `+JOIN: Join failed`, `+JOIN: LoRaWAN modem is busy`, `+JOIN: Joined already` [Certain]
- `URC_MSG_*` — `+MSG: Start`, `+MSG: FPENDING`, `+MSG: Link <margin>,<gwcnt>`, `+MSG: ACK Received`, `+MSG: MULTICAST`, `+MSG: PORT: n; RX: "hex"`, `+MSG: RXWIN{0,1,2,3}, RSSI x, SNR y`, `+MSG: Done`, plus the *error-status* family which are **not failures of the transport but semantic negatives**: `+MSG: LoRaWAN modem is busy`, `+MSG: Please join network first`, `+MSG: No free channel -N`, `+MSG: No band in Nms`, `+MSG: DR error`, `+MSG: Length error N` [Certain]. `+CMSG:` mirrors `+MSG:` with an added `+CMSG: Wait ACK` state marker [Certain].
- `URC_CLASS_B_*`, `URC_BEACON_*` — only relevant if Class B is in scope (see §Open Questions — recommend excluding from v1).
- `URC_LOWPOWER_*` — `+LOWPOWER: SLEEP`, `+LOWPOWER: WAKEUP` [Certain]
- `URC_INFO_TIMEOUT` — `+INFO: Input timeout`, only if `AT+UART=TIMEOUT` feature is enabled on the modem side [Certain]
- `UNRECOGNIZED` — anything not matching a known pattern is surfaced as an event, never silently dropped, so the command manager/FSM can decide whether an unexpected line during a WAIT_RESPONSE state should fail the command or just be logged.

**Design constraint**: parser must not assume it's being fed lines belonging to a single logical command — `+MSG:` sequences interleave a *result* stream for the in-flight command with *URCs* that are asynchronous by nature (e.g., `MULTICAST`, `FPENDING`). The parser's job stops at "this line means X"; deciding whether X terminates the current AT transaction or is an orthogonal notification belongs to the AT Command Manager / FSM boundary (§2.3/§2.4), because that decision requires knowing the current command context, which the parser by design does not hold.

**Public interface**: `int at_parser_parse_line(const char *line, size_t len, struct at_event *out)`.

**Dependencies**: none beyond libc string functions. Fully unit-testable with plain strings, no Zephyr kernel required beyond types — this is the highest-leverage module for the `tests/parser` suite.

### 2.3 `lora_e5_at.c` + `lora_e5_cmd_queue.c` / `include/lora_e5/lora_e5_at.h`

**Purpose**: Own the *transactional* lifecycle of AT commands — the part that is identical regardless of which LoRaWAN modem is behind the UART.

**Responsibilities**:
- Command descriptor: `{cmd_string, expected_terminal_events[], timeout_ms, retry_max, cmd_id}`. Terminal-event set matters because, per §2.2 above, not every command terminates on `OK` — `AT+MSG` terminates on `+MSG: Done` (or one of the MSG error lines), `AT+JOIN` terminates on `+JOIN: Done` or `+JOIN: Join failed`, while simple commands terminate on `+CMD: OK` or a generic `OK`. **The command manager must be told, per command, what its terminal condition is** — this cannot be hardcoded as "wait for OK" without breaking half the command set.
- Queue: `k_fifo` or a small statically-sized array + `k_msgq` of command descriptors (prefer bounded array — "minimal dynamic allocation" from Non-Goals/Design Principles rules out an unbounded fifo of heap nodes; a `k_msgq` over a fixed-size struct achieves the same effect with static memory).
- One command in flight at a time (`k_mutex` "bus busy" guard). This matches the modem's own model — the spec repeatedly returns `+MSG: LoRaWAN modem is busy` / `+JOIN: LoRaWAN modem is busy` if you race it [Certain], so the manager enforcing single-flight isn't just a design nicety, it avoids commands the modem will reject anyway.
- `k_work_delayable` per outstanding command for timeout. On expiry: increment retry counter, resend if `< max_retries`, else post `TIMEOUT` event up to caller/FSM and advance the queue.
- Response/URC routing: the manager receives *every* parsed event from the parser stage, checks it against the in-flight command's terminal-event set; if matched, resolves the command (`k_sem`/`k_poll_signal` give for sync callers, `k_work` submit of the result to FSM for async). If unmatched, it forwards the event onward as a **URC** (unsolicited) — this is the split point mentioned in §2.2.
- If synchronous API used: caller blocks on a `k_sem` with the AT manager doing the give; must support `k_sem_take` with the *same* timeout so a hung command manager can't hang the calling application thread indefinitely — belt-and-suspenders, not a substitute for the internal timeout.

**Public interface (internal, not exposed past FSM)**: `at_cmd_submit(descriptor, result_cb)`, `at_cmd_submit_sync(descriptor, timeout)`.

**Ownership**: owns the command queue, the "bus busy" mutex, and per-command timeout timers. Does **not** own UART (calls into UART backend to send) and does **not** know LoRaWAN semantics (terminal-event sets and command strings are supplied by the FSM/command-table layer, not hardcoded here — this is the seam that makes multi-module support realistic later).

**Dependencies**: UART backend (send only), AT Parser (consumes its output), Zephyr `k_msgq`/`k_fifo`, `k_work_delayable`, `k_mutex`, `k_sem`.

### 2.4 `lora_e5_fsm.c` / `include/lora_e5/lora_e5_types.h`, `lora_e5_events.h`

**Purpose**: The only module that understands *what a modem lifecycle is*. Everything else is plumbing in service of this module.

**Responsibilities**: full state ownership (§5), issuing command sequences to the AT manager (which itself is command-agnostic), reacting to command results and URCs, driving recovery policy (§8), notifying the public API layer of state transitions and terminal outcomes (join success/fail, tx success/fail, downlink received).

**Public interface**: none directly exposed to the application; internal event-driven entry point consumed only by the event dispatcher (§Event System) and by the public API layer for posting requests (`JOIN_REQUEST`, `TX_REQUEST`, etc.).

**Ownership**: the FSM state variable itself, one `k_mutex` (or rely on single-consumer work queue serialization instead of a mutex — see §4, this is the preferred approach) protecting state reads from other threads (e.g., a `lora_e5_get_state()` diagnostic API called from shell).

**Dependencies**: AT Command Manager (issues commands), Event system (produces/consumes), `k_work_delayable` for FSM-level timeouts (join timeout, ack timeout) distinct from the AT-manager's per-command timeout — these are different timescales and must not be conflated: a join sequence is several AT commands plus URC waits, so "join timeout" spans potentially multiple AT-manager transactions.

### 2.5 `lora_e5_events.c` / `include/lora_e5/lora_e5_events.h`

**Purpose**: Central definition and transport of internal events between UART→parser→cmd_queue→FSM→API stages, plus FSM→application callback delivery.

**Responsibilities**: define the event enum/union payload types (§6), provide the `k_msgq`/`k_fifo` wrappers and the FSM work-queue submission helper. This module is mostly types + thin wrappers — keep it small; resist the temptation to put FSM logic here.

**Ownership**: the FSM's dedicated work queue (`k_work_q`) and its stack.

### 2.6 `lora_e5_timer.c`

**Purpose**: Thin centralization of `k_work_delayable` patterns used identically by both the AT command manager (per-command timeout) and the FSM (join timeout, ack timeout, reconnect backoff). Whether this earns its own file versus being inlined in each caller is a judgment call — justify it only if there's real shared logic (e.g., exponential backoff computation for `CONFIG_LORA_E5_AUTO_REJOIN`); otherwise fold it into the FSM file and drop it from the layout. Flagging as **open question**, see §11.

### 2.7 `lora_e5.c` / `include/lora_e5/lora_e5.h`

**Purpose**: Public API surface (§7). Translates `lora_e5_join()` etc. into FSM events; for synchronous variants, blocks on a semaphore the FSM signals at the terminal outcome.

**Ownership**: the public callback registration table (single callback + context pointer is simplest and matches "thread-safe, minimal state" — recommend against a multi-subscriber list unless a concrete multi-consumer use case is named).

**Dependencies**: FSM (only consumer of its internal API), Event system for the result-delivery mechanism.

### 2.8 `lora_e5_internal.h`

Shared internal-only types/prototypes (event structs, FSM state enum, command descriptor struct) — deliberately not installed under `include/lora_e5/` because the application must never see these.

---

## 3. Internal Data Flow — UART byte to application event

Concrete trace for a join request, chosen because it exercises every layer including cross-command-boundary URC handling:

1. Application calls `lora_e5_join()`. Public API posts `JOIN_REQUEST` to the FSM event queue, optionally blocks on a semaphore if sync variant used.
2. FSM work queue picks up `JOIN_REQUEST`, transitions `READY → JOINING`, calls `at_cmd_submit()` with descriptor `{cmd="AT+JOIN", terminal_events={URC_JOIN_DONE, URC_JOIN_FAILED, AT_ERROR}, timeout=CONFIG_LORA_E5_JOIN_TIMEOUT_MS, retry=0}` — retry=0 at the AT-command layer because join retry policy belongs to the FSM/recovery layer (§8), not the transport-retry layer; conflating the two would double-apply backoff.
3. AT Command Manager acquires the bus-busy mutex, calls `uart_backend_send_line("AT+JOIN")`.
4. UART backend serializes the write via `uart_tx()`.
5. Module responds over several lines: `+JOIN: Starting`, `+JOIN: NORMAL`, `+JOIN: NetID 000024 DevAddr 48:00:00:01`, `+JOIN: Done` (success case) [Certain, per spec §4.24].
6. For each line: UART ISR/driver callback appends bytes to ring buffer; on `\r\n` detection, backend copies out the line and submits a `k_work` item to the RX work queue carrying the line.
7. RX work queue item runs `at_parser_parse_line()` → produces `URC_JOIN_STARTING`, then `URC_JOIN_NETID(...)`, then `URC_JOIN_DONE`, one event per line, in order (parser processes strictly one complete line at a time — no batching, so ordering is preserved for free by the single work-queue thread).
8. Each event is handed to the AT Command Manager's response-router: `URC_JOIN_STARTING` and `URC_JOIN_NETID` are **not** in the terminal-event set for this command → forwarded as URCs to the FSM. `URC_JOIN_DONE` **is** terminal → command resolved successfully, per-command timer cancelled, queue advances (nothing else queued here), and a `CMD_RESULT(cmd_id, OK)` event is submitted to the FSM work queue.
9. FSM work queue processes the intermediate URCs first (in arrival order) — `URC_JOIN_NETID` lets the FSM cache `DevAddr` for later reporting — then processes `CMD_RESULT(OK)`, transitions `JOINING → JOINED`, and submits `JOIN_SUCCESS` to the application callback dispatch (still from FSM work-queue context, or handed to yet another lower-priority queue if callback execution time is a concern — see §4).
10. Public API's sync wrapper (if used) gives the semaphore the blocked application thread is waiting on; async path simply invokes the registered callback.

Failure branch: if step 5 instead yields `+JOIN: Join failed`, or the FSM-level join timer (distinct from AT-manager per-command timer, since the whole exchange in step 5 can legitimately take multiple seconds within one AT-manager timeout window) expires without a `+JOIN: Done`, FSM transitions `JOINING → RECOVERING` and applies retry/backoff policy (§8) instead of directly to `ERROR`.

---

## 4. Thread Model

```
UART driver callback (ISR or driver-internal work item — Zephyr-driver-dependent)
        │  byte-level ring_buf push only, line-complete detection, NO parsing
        ▼
RX Work Queue (dedicated k_work_q, e.g. "lora_e5_rx_wq")
        │  runs UART backend's line-copy + submits to parser
        │  runs AT Parser (pure function, cheap, fine to run inline here)
        │  runs AT Command Manager's response routing (fast, non-blocking)
        ▼
FSM Work Queue (dedicated k_work_q, e.g. "lora_e5_fsm_wq")
        │  ALL FSM state transitions happen here, single-threaded by construction
        │  issues new AT commands (call *into* AT manager, which itself queues/sends
        │    asynchronously — this call must not block)
        ▼
Application Callback
        │  invoked from FSM work queue context UNLESS the library explicitly
        │  hands off to a lower-priority "notify" work queue (recommended — see below)
```

Key decisions and why:

- **Two dedicated work queues, not the system work queue.** Using `k_sys_work_q` for either RX processing or FSM logic risks priority inversion with unrelated system work items and makes worst-case latency unbounded from this library's perspective. Two private queues (configurable priority/stack via Kconfig, §9) keep the library's real-time behavior self-contained. [Likely — standard Zephyr practice, not vendor-specific, so confidence is high, but exact priority values need to be picked relative to the *application's* other threads, which this library cannot know in advance]
- **RX work queue does double duty (line assembly completion + parsing + routing)** rather than three separate queues, because these are all fast, non-blocking, CPU-bound operations — adding hop-to-hop queue submission between them would only add latency without any concurrency benefit (nothing here blocks).
- **FSM work queue is the single writer of FSM state** — this eliminates the need for a mutex around the state variable for the write side. A mutex is still warranted for `lora_e5_get_state()`-style diagnostic reads from arbitrary application/shell threads, OR (cleaner) make the diagnostic read itself an event round-trip through the FSM queue with `k_poll`. Recommend the latter for consistency with "no application should modify or peek FSM state outside the event system" — even reads go through the front door. [This is a design opinion — flagging as trade-off, not fact: a mutex-guarded volatile read is simpler and *adequate* if the diagnostic use case tolerates a single stale read.]
- **Application callback dispatch context matters and is easy to get wrong.** If the application's registered callback does anything blocking (logging over another UART, writing flash, taking its own semaphore), running it directly on the FSM work queue stalls the entire modem state machine — a downlink can't be processed, ack timers can't fire, nothing progresses until the callback returns. Recommend a **third, lower-priority "notify" work queue** dedicated purely to application callback invocation, decoupling application code quality from library liveness. This is a concrete recommendation, not a hedge: production field failures from "user callback blocked the driver" are a known failure class in embedded UART/modem stacks. [Likely, based on general embedded systems experience, not something citable to the LoRa-E5 spec itself]
- **No busy-waiting anywhere.** All waits are `k_sem_take`/`k_poll` with timeout, backed by `k_work_delayable` for the actual timeout mechanism, never a polling loop.
- **Synchronous application API** is implemented purely as: post event to FSM queue, block calling thread on a semaphore with a timeout slightly longer than the FSM/AT-manager's own worst-case timeout, so the sync wrapper degrades gracefully instead of hanging if something upstream misbehaves.

---

## 5. Finite State Machine

### 5.1 States (refined against the reference doc's actual command lifecycle)

| State | Meaning | Entry action |
|---|---|---|
| `OFF` | No modem interaction; initial state before `lora_e5_start()` | none |
| `RESET` | Issuing `AT+RESET` (or asserting a hardware reset line if wired — spec doesn't document a reset GPIO for LoRa-E5-HF over UART variant, only `AT+RESET`/DFU-mode repower — confirm hardware reset pin availability on your carrier board before relying on it) [Likely — flagged uncertain] | send `AT+RESET` |
| `BOOT` | Waiting for modem to become responsive after reset/power-up; no fixed "boot complete" URC is documented in the spec I retrieved — recommend a silence/settle timer rather than waiting for a specific banner | start settle timer |
| `CHECK_AT` | Probing with plain `AT` until `+AT: OK`, bounded retries | send `AT` |
| `CONFIG` | Applying `AT+MODE=LWOTAA` (or `LWABP`), `AT+ID=...`, `AT+KEY=...`, `AT+PORT=...`, `AT+CLASS=...`, `AT+ADR=...` as configured via Kconfig/API — sequential, each a separate AT-manager transaction | send first config command |
| `READY` | Configured, idle, awaiting `JOIN_REQUEST` or already-provisioned ABP send | none |
| `JOINING` | `AT+JOIN` in flight | send `AT+JOIN` (or `AT+JOIN=FORCE` if forced rejoin requested) |
| `JOINED` | Join confirmed; idle steady state for an OTAA device, or entry state directly from `CONFIG` for ABP | none |
| `TX_PENDING` | `TX_REQUEST`/`TX_CONFIRMED_REQUEST` accepted, `AT+MSG`/`AT+CMSG`/`AT+MSGHEX`/`AT+CMSGHEX` about to be sent | send msg command |
| `WAIT_ACK` | Confirmed uplink only; modem has echoed `+CMSG: Wait ACK`, awaiting `+CMSG: Done` or ack-timeout-driven `RETRY` exhaustion (module handles the confirmed-retry internally per `AT+RETRY` config — the *host* does not need to resend, only wait longer) [Certain, spec §4.17: retries happen inside the modem with 3–10s randomized delay, all under one `AT+CMSG` transaction] | none, already sent |
| `WAIT_RX1` / `WAIT_RX2` | Optional finer-grained states while an unconfirmed `AT+MSG` transaction is still open, mirroring the module's own `+MSG: RXWIN1...` / `RXWIN2...` URCs — **recommend collapsing these into a single `TX_PENDING` sub-state with a downlink-received event, rather than modeling RX1/RX2 as separate FSM states**, because the host cannot distinguish or act differently based on which RX window a downlink arrived in; only RSSI/SNR/port/payload matter to the application. Modeling RX1/RX2 literally adds states with no distinct transition behavior — see §11 recommendation to prune this from the spec's suggested state list. | — |
| `SLEEP` | `AT+LOWPOWER` issued, modem in low-power UART-wake mode | send `AT+LOWPOWER[=ms]` |
| `ERROR` | Unrecoverable-by-retry condition at current abstraction level (AT_ERROR on a config command after exhausting retries, malformed/unexpected URC sequence, UART_ERROR from backend) | notify application (`lora_e5_register_callback` error event) |
| `RECOVERING` | Executing the recovery ladder (§8) | begin recovery step |

### 5.2 Transitions — condensed table (see `02_lorawan_fsm.drawio` for the visual)

| From | Event | To | Notes |
|---|---|---|---|
| OFF | `lora_e5_start()` → internal `START` | RESET | |
| RESET | `AT_OK` (`+RESET: OK`) | BOOT | |
| RESET | `TIMEOUT` / `AT_ERROR` | RECOVERING | reset itself failed — treat as transport-level fault |
| BOOT | settle timer expiry | CHECK_AT | |
| CHECK_AT | `AT_OK` | CONFIG | |
| CHECK_AT | `TIMEOUT` (after `CONFIG_LORA_E5_MAX_RETRIES`) | ERROR | modem not responding at all — likely wiring/power fault, not worth auto-recovering indefinitely without escalating to application |
| CONFIG | all config commands `AT_OK` | READY | |
| CONFIG | any `AT_ERROR` | RECOVERING or ERROR | distinguish "transient" (-12 busy) from "structural" (-1 invalid param — a Kconfig/API misconfiguration, retrying won't help) — see §8 |
| READY | `JOIN_REQUEST` | JOINING | |
| JOINING | `URC_JOIN_DONE` | JOINED | |
| JOINING | `URC_JOIN_FAILED` | RECOVERING | apply join backoff (§8) |
| JOINING | FSM-level join timeout | RECOVERING | `+JOIN: Done` never arrived — module firmware bug, RF issue, or duty-cycle lockout (`+MSG: No band in Nms`-class condition applies to JOIN too per duty cycle rules in §3.5 of spec) |
| JOINED | `TX_REQUEST` | TX_PENDING | |
| JOINED | `SLEEP` (API call) | SLEEP | |
| TX_PENDING | unconfirmed: `URC_MSG_DONE` | JOINED | success — surface `RX_RECEIVED`/`DOWNLINK_RECEIVED` first if a `+MSG: PORT:...` URC preceded `Done` |
| TX_PENDING | confirmed: `URC_CMSG_WAIT_ACK` | WAIT_ACK | |
| TX_PENDING | any `+MSG:`/`+CMSG:` error-status line (busy/not-joined/no-channel/no-band/DR-error/length-error) | JOINED (or RECOVERING if "not joined" — implies FSM/modem state desync, worth a corrective rejoin) | these are **modem-reported semantic rejections**, not transport failures — do not apply AT-manager retry, surface as `TX_FAILED` with reason code to application |
| WAIT_ACK | `URC_CMSG_DONE` with prior `ACK Received` | JOINED | `TX_SUCCESS` (confirmed) |
| WAIT_ACK | `URC_CMSG_DONE` without `ACK Received` observed | JOINED | `TX_FAILED` (no ack) — module exhausted its internal `AT+RETRY` count |
| SLEEP | `URC_LOWPOWER_WAKEUP` or host-initiated `lora_e5_wakeup()` | JOINED (or READY if wake occurs pre-join) | |
| * (any) | `UART_ERROR` | RECOVERING | backend-level fault (overflow, framing) — always treated as needing at least a soft-reset step |
| ERROR | `lora_e5_reset()` (explicit app call) or `CONFIG_LORA_E5_AUTO_RESET` timer | RECOVERING | |
| RECOVERING | recovery ladder succeeds, reaches JOINED or READY | JOINED / READY | see §8 for ladder detail |
| RECOVERING | recovery ladder exhausted (`CONFIG_LORA_E5_MAX_RETRIES` across the whole ladder) | ERROR | stop auto-recovering, escalate — a library that retries forever silently is worse than one that gives up loudly, for a product running unattended for months: silent infinite retry hides a field fault from telemetry/ops. [Likely, product-engineering judgment] |

### 5.3 Explicitly out of scope for the FSM (push to application or later phase)

- Class B / Class C mode switching and beacon-lock state (`+BEACON:` URCs) — real added complexity (LOCKED/FAILED/DONE/LOST states, 2-hour beacon-lost timers) with no stated requirement in the spec document for Class B/C. Recommend excluding entirely from v1 and revisiting only if a concrete Class B/C use case is confirmed — see §11.
- Multicast (`AT+LW=MC`) — same reasoning.
- Beacon Sniffer / TEST mode (`AT+MODE=TEST`, `AT+TEST=...`) — this is a manufacturing/RF-bringup tool, not a runtime LoRaWAN concern; if wanted, keep it as a *separate* diagnostic entry point outside the normal FSM (would need its own state region since it explicitly disables LoRaWAN commands with error -12 while in TEST mode).

---

## 6. Event Model

All events carry a tagged union payload; producers/consumers below.

| Event | Producer | Consumer | Payload |
|---|---|---|---|
| `BOOT_COMPLETE` | FSM's own settle timer | FSM | none |
| `AT_OK` | AT Command Manager (parser-routed) | AT Command Manager internally (resolves cmd); surfaced to FSM only as `CMD_RESULT` | cmd_id |
| `AT_ERROR` | AT Command Manager | AT Command Manager (retry decision), then FSM as `CMD_RESULT(error, code)` | cmd_id, error_code (spec Table 2-1 values) |
| `JOIN_REQUEST` | Public API | FSM | none (config already applied) |
| `JOIN_SUCCESS` | FSM | Public API → application callback | dev_addr, net_id |
| `JOIN_FAILED` | FSM | Public API → application callback | reason (timeout / explicit failure / max retries) |
| `TX_REQUEST` | Public API | FSM | data ptr+len, port, confirmed flag — **must be copied into a FSM-owned buffer before the API call returns**, since the application buffer's lifetime is not guaranteed past the call (no dynamic allocation preferred → a small static/Kconfig-sized TX staging buffer, size bounded by `AT+LW=LEN` max payload, itself DR-dependent — see Kconfig §9 and risk in §12) |
| `TX_SUCCESS` | FSM | Public API → application callback | confirmed flag, rssi/snr if available |
| `TX_FAILED` | FSM | Public API → application callback | reason code (busy/not-joined/no-channel/no-band/dr-error/length-error/ack-timeout/modem-error) |
| `RX_RECEIVED` / `DOWNLINK_RECEIVED` | FSM (from `+MSG:/+CMSG: PORT: n; RX: "hex"` URC) | Public API → application callback | port, data, len, rssi, snr, rx_window |
| `TIMEOUT` | AT Command Manager (per-cmd) or FSM (join/ack-level) | same layer that raised it, or escalated up | cmd_id or none |
| `UART_ERROR` | UART Backend | FSM (via event bus, not directly — backend has no FSM reference, only posts a generic event) | error subtype (overflow/framing) |
| `RESET` | Public API (`lora_e5_reset()`) or internal recovery | FSM | none |
| `SLEEP` / `WAKEUP` | Public API | FSM | optional duration_ms for timed sleep |

Transport: FSM-bound events use a `k_msgq` of small fixed-size structs (bounded depth, e.g. `CONFIG_LORA_E5_EVENT_QUEUE_DEPTH`) submitted via `k_work` to the FSM work queue — using `k_work` rather than a raw `k_msgq` blocking-receive loop lets the FSM queue also service its own internal timer-driven work items (join timeout, ack timeout) on the same thread without a second polling construct. Application-callback events use a **separate** notify queue (§4) so a slow application callback cannot back up the FSM's own event processing.

---

## 7. Public API Proposal

The target API from the spec is largely right; annotating *why* each exists and flagging two gaps:

- `lora_e5_init(void)` — allocates/initializes internal structures, registers UART backend, does **not** talk to the modem yet (no I/O in init — Zephyr convention, keeps init deterministic and testable without hardware attached).
- `lora_e5_start(void)` — begins the boot/config sequence (`OFF → ... → READY`). Split from `init()` because a real product may want to defer modem power-up (rail sequencing, board power budget) independently of software init.
- `lora_e5_join(void)` — posts `JOIN_REQUEST`. Async by default; needs a **stated sync/async policy** — see gap below.
- `lora_e5_leave(void)` — not directly backed by an AT command in the spec I retrieved (LoRaWAN OTAA has no explicit "leave" AT command; typical implementation is a local state reset without notifying the network, since LoRaWAN itself has no network-initiated leave in Class A). Document this clearly in the header: it clears local join state and returns FSM to `READY`, it does **not** perform a protocol-level deregistration. [Certain about LoRaWAN's lack of a leave message; the exact local-state-reset behavior is an implementation choice, not a spec mandate]
- `lora_e5_send()` / `lora_e5_send_confirmed()` — map to unconfirmed/confirmed uplink (`AT+MSG`/`AT+CMSG`, or the HEX variants — **API should probably take raw bytes and internally choose MSGHEX/CMSGHEX**, since the string variants require additional escaping/quoting logic per spec §2.2 that adds no value for a byte-oriented sensor payload use case). This is a concrete deviation worth calling out from a literal reading of the spec's command list.
- `lora_e5_sleep()` / `lora_e5_wakeup()` — map to `AT+LOWPOWER` / any-byte wake per spec §4.30, noting the required ≥5ms host-side settle delay after wake before sending the next command [Certain] — this belongs inside the library's SLEEP→JOINED transition handling, not left to the application to remember.
- `lora_e5_reset()` — maps to `AT+RESET`, routes through RECOVERING rather than a raw reset-and-hope, so config gets reapplied.
- `lora_e5_factory_reset()` — `AT+FDEFAULT` — note this wipes keys/IDs per Table 4-2; after this call the FSM must re-run `CONFIG` with the application's provisioned values, not assume the modem retained them.
- `lora_e5_get_version()` / `lora_e5_get_ids()` — thin queries (`AT+VER`, `AT+ID`), reasonable as-is; should be synchronous-only (no meaningful async use case) unless the library commits to a fully async-only API for consistency's sake — **architectural decision needed**, see gap below.
- `lora_e5_register_callback(...)` — single callback + event-type filter or a single "everything" callback with a tagged event union; recommend the latter for simplicity unless multiple independent subscribers are a real requirement.

**Two concrete API gaps in the spec as given, needing a decision before Phase 2 code:**

1. **Sync/async policy is unstated per-call.** "Support both synchronous and asynchronous application APIs" doesn't specify whether that means two separate function names (`lora_e5_join()` vs `lora_e5_join_sync(timeout)`) or one function with a mode flag. Recommend explicit suffixed sync variants (`_sync`) so the header signature itself documents blocking behavior — silent blocking behind an innocuous-looking function name is a common source of production stack-overflow/priority-inversion bugs when called from the wrong thread context.
2. **No documented way to report RSSI/SNR/downlink outside of the TX result.** The sample "Uplink Example" wants to "Print RSSI/SNR" and "Receive downlinks" — needs a `DOWNLINK_RECEIVED` payload structure exposed through the public event header, which the spec's flat function list doesn't show a getter for. Add it to `lora_e5_events.h` explicitly rather than leaving it implicit.

---

## 8. Error Handling

### 8.1 Classification (drives *which* recovery step applies — this is the part the original ladder diagram glosses over)

| Fault class | Example | Response |
|---|---|---|
| Transport timeout | UART line never completes, or no response line at all within `CONFIG_LORA_E5_CMD_TIMEOUT_MS` | AT-manager retries up to `CONFIG_LORA_E5_MAX_RETRIES`, then escalates `TIMEOUT` to FSM |
| Modem-reported transient busy | `-12` in TEST mode context, `+MSG:/+JOIN: ... modem is busy` | do **not** blind-retry immediately (the modem is mid-transaction, not idle) — requeue after a short delay, bounded retry count |
| Modem-reported structural error | `-1` invalid params, `-11` wrong format | **do not retry** — this indicates a firmware/Kconfig/API-usage bug (wrong key length, bad port number), retrying is pointless and masks the real defect; surface immediately as `ERROR` with the code, so it's visible in logs/telemetry during development and won't silently loop in the field either |
| Semantic rejection (not a transport fault) | `+MSG: DR error`, `+MSG: Length error N`, `+MSG: No band in Nms` | surface `TX_FAILED(reason)` to application without touching FSM connectivity state — the modem is fine, the *request* was invalid or rate-limited; retrying with the same parameters will fail again (DR error, length error) or needs a wait, not a reset (duty-cycle/no-band) |
| UART-level fault | ring buffer overflow, framing garbage, no bytes at all when expected | `UART_ERROR` → RECOVERING → at minimum `AT+RESET`, at worst reconfigure |
| Join failure | `+JOIN: Join failed`, join timeout | RECOVERING with the rejoin backoff policy below |

### 8.2 Recovery ladder (as posed in the spec, refined with a stop condition)

```
Retry command (bounded by CONFIG_LORA_E5_MAX_RETRIES, only for transport
  timeouts and transient-busy classes — never for structural/semantic faults)
        │ still failing
        ▼
Reset modem (AT+RESET)
        │ still failing / UART-level fault suspected
        ▼
Reconfigure modem (re-run CONFIG state's command sequence from stored
  provisioning — MODE, ID, KEY, PORT, CLASS, ADR)
        │ config succeeded, was previously joined
        ▼
Rejoin network (if CONFIG_LORA_E5_AUTO_REJOIN, else stop at READY and let
  the application decide — some products intentionally don't want silent
  rejoin, e.g. regulatory duty-cycle-sensitive deployments)
        │ join succeeded
        ▼
Resume operation (JOINED, any pending TX_REQUEST re-attempted once, not
  silently retried forever — if the pending TX also fails post-recovery,
  surface TX_FAILED rather than looping the whole ladder again)
```

Stop condition, explicit: the ladder itself is retried at most `CONFIG_LORA_E5_MAX_RETRIES` full passes before landing in `ERROR` and staying there until an explicit `lora_e5_reset()` from the application (or `CONFIG_LORA_E5_AUTO_RESET` on a much longer timer, e.g. minutes not seconds, as a last-resort watchdog-style self-heal for genuinely unattended deployments). Without this stop condition, a persistently faulty module (bad antenna, dead RF front-end, wrong region config) causes an infinite reset/rejoin loop that looks alive in logs but is not actually functioning — worse for field diagnostics than a clean ERROR state that a remote monitoring system can alert on.

### 8.3 Join backoff

`CONFIG_LORA_E5_JOIN_TIMEOUT_MS` bounds a single join attempt. Retry count is `CONFIG_LORA_E5_MAX_RETRIES` (or a dedicated `CONFIG_LORA_E5_JOIN_MAX_RETRIES` if join retry semantics should differ from generic transport retry — recommend the dedicated Kconfig, since join failures interact with the modem's own join duty-cycle limitation, §3.5 of spec: 1% duty cycle for first hour, 0.1% for next 10 hours, 0.01% after — a naive fixed-interval retry can itself trip the modem's internal `+MSG: No band in Nms`-class throttling on the *join* channel too). Exponential backoff (configurable via `CONFIG_LORA_E5_AUTO_REJOIN` + a backoff base/cap pair) is the right default specifically *because* of this regulatory duty-cycle interaction — it's not just a generic "backoff is good practice" recommendation, it's necessitated by the module's own documented behavior.

---

## 9. Configuration (Kconfig)

Beyond the spec's example list, with rationale:

| Symbol | Purpose |
|---|---|
| `CONFIG_LORA_E5` | master enable |
| `CONFIG_LORA_E5_UART` | UART device selection (devicetree chosen node, or explicit label) |
| `CONFIG_LORA_E5_STACK_SIZE` | shared or per-queue? — recommend **separate** `CONFIG_LORA_E5_RX_STACK_SIZE` and `CONFIG_LORA_E5_FSM_STACK_SIZE` (and a third for the notify queue, §4) since their call depths differ (parser+routing vs FSM logic vs application callback, which is unbounded from the library's perspective) |
| `CONFIG_LORA_E5_RX_BUFFER_SIZE` | ring buffer size; must exceed the largest realistic single line (max payload hex string + URC framing) — should be validated against `AT+LW=LEN` max at runtime with a warning log if a configured payload could exceed it, not just left as a silent truncation risk |
| `CONFIG_LORA_E5_CMD_TIMEOUT_MS` | generic AT-manager per-command timeout for short commands (config, query) |
| `CONFIG_LORA_E5_TX_TIMEOUT_MS` | **separate from CMD_TIMEOUT** — an `AT+MSG`/`AT+CMSG` transaction legitimately takes longer (RX1/RX2 windows, confirmed retries with 3-10s randomized delay per attempt up to `AT+RETRY` count) than a config query; using one generic timeout for both risks either false-timeout on TX or absurdly long waits on trivial queries |
| `CONFIG_LORA_E5_JOIN_TIMEOUT_MS` | single join attempt bound |
| `CONFIG_LORA_E5_MAX_RETRIES` | generic transport retry bound |
| `CONFIG_LORA_E5_JOIN_MAX_RETRIES` | join-specific retry bound (§8.3 rationale) |
| `CONFIG_LORA_E5_AUTO_REJOIN` | enable automatic rejoin after recovery |
| `CONFIG_LORA_E5_AUTO_RESET` | enable last-resort watchdog-style self-reset on persistent ERROR |
| `CONFIG_LORA_E5_AUTO_RESET_TIMEOUT_MS` | how long ERROR persists before auto-reset fires — should default large (minutes) |
| `CONFIG_LORA_E5_EVENT_QUEUE_DEPTH` | bounded event queue depth — sizing this too small under bursty downlink+URC conditions silently drops events; needs an overflow counter exposed for diagnostics, not just a silent drop |
| `CONFIG_LORA_E5_TX_BUFFER_SIZE` | static TX staging buffer for the "copy application data before returning" requirement (§6) — must be sized against max LoRaWAN payload for the target region/DR, itself variable, so this needs to be the library's stated worst-case (242 bytes per Table 3-3 for most bands at max DR) unless the application commits to smaller |
| `CONFIG_LORA_E5_REGION` | if the library takes ownership of applying regional defaults via `AT+DR=band` rather than leaving it entirely to `AT+FDEFAULT`'s baked-in default — open question, see §11 |
| `CONFIG_LORA_E5_CLASS_B` / `_CLASS_C` | gate the extra FSM complexity discussed in §5.3 — default OFF |
| `CONFIG_LORA_E5_DEBUG` | verbose logging, maps to library's own `LOG_DBG` usage, independent of whether the *modem's* own `AT+LOG` verbosity is also toggled (two separate debug surfaces — host-side driver logs vs modem-side firmware logs — should not be conflated under one Kconfig symbol) |
| `CONFIG_LORA_E5_SHELL` | gate the shell sample/subsystem integration |

---

## 10. Testing Strategy

- **Parser (`tests/parser`)**: pure host-buildable (Zephyr's native_sim or ztest on QEMU) unit tests feeding literal captured line sequences from the spec's own documented examples (§4.5, §4.6, §4.24 "Return" blocks are effectively ready-made test vectors) into `at_parser_parse_line()`, asserting exact event type + payload. Include the "unrecognized line" and malformed/partial-line cases explicitly, since those are the ones hand-written parsers get wrong first.
- **Command queue / timeout / retry (`tests/fsm` or a dedicated `tests/cmd_queue`)**: mock UART backend that can be scripted to (a) respond correctly, (b) respond late (trigger timeout), (c) not respond at all, (d) respond with an unexpected/interleaved URC mid-transaction, (e) return a structural error code. Assert retry counts, timer cancellation, and correct terminal-event matching per the "not every command ends in OK" hazard from §2.2 — this is the single highest-value test given how easy that assumption is to get subtly wrong.
- **FSM transitions (`tests/fsm`)**: drive the FSM purely through the event API with a mocked AT-manager response (no real UART), asserting state sequences for: happy-path boot→join→send→sleep→wake, join failure→recovery→rejoin, structural config error→ERROR (no retry loop), confirmed-uplink no-ack→TX_FAILED, recovery ladder exhaustion→ERROR-and-stay. Property to explicitly assert: **no transport-timeout-classified fault should ever cause a retry loop with a *structural*-error-classified fault** (regression guard for the §8.1 classification table being respected in code, not just documented).
- **Recovery / backoff logic**: time-mocked (Zephyr's `ztest` supports fake/mocked kernel timing in native builds) test asserting the exponential backoff sequence for join retries stays within the modem's own duty-cycle envelope (§3.5) as a design invariant, not just "backoff increases."
- **Mock UART (`tests/mock_uart`)**: a fixture, not a standalone test suite — provides a scriptable fake `line_received()` producer and a TX-capture sink so higher-layer tests never touch real hardware. This is the shared infrastructure the above three suites depend on; build it first.
- **Integration tests**: on real hardware or a hardware-in-the-loop bench with an actual LoRa-E5-HF, at minimum: boot→join→uplink→sleep→wake cycle against a real LoRaWAN network server (TTN or private), verifying the library survives an actual multi-second RX-window round trip and a real confirmed-uplink retry sequence — the mocked tests validate logic correctness, only real hardware validates the *timing* assumptions (`CMD_TIMEOUT_MS`, `TX_TIMEOUT_MS` values) are actually sufficient, since UART turnaround and RF timing aren't something a mock can meaningfully fake.
- **Long-run soak test**: not explicitly in the spec's list but necessary given the "months of unattended operation" requirement — a bench setup left running an uplink-every-N-minutes cycle for days, tracking memory (should be flat, given no dynamic allocation), event-queue overflow counters, and recovery-ladder trigger counts as the actual acceptance criteria for "production-grade," rather than relying on unit tests alone to prove that claim.

---

## Open Questions

1. **Class B/C scope** — spec text mentions "Expected states include... SLEEP... ERROR" but the module supports full Class A/B/C. Is Class B/C in scope for v1, or explicitly deferred? Recommend deferred (§5.3) — confirm.
2. **RX1/RX2/RX3 window as literal FSM states vs a single TX_PENDING sub-phase** — recommend collapsing (§5.1); confirm the application doesn't need to *see* which RX window a downlink arrived in as a distinct top-level event (it can still be in the payload without being a state).
3. **ABP vs OTAA** — the public API (`join()`, `leave()`) reads OTAA-only. Is ABP support (no join phase, `CONFIG` → `JOINED` directly) required for v1? Changes the FSM's entry transitions.
4. **Hardware reset line** — is there a GPIO wired to the module's reset pin on the target board, or is `AT+RESET` the only reset mechanism available? This matters directly for the RECOVERING ladder's reliability: a UART-level fault severe enough that the modem can't parse `AT+RESET` at all has no recovery path without a hardware reset. Needs a board-specific answer, not answerable from the AT spec alone.
5. **Region/band plan ownership** — does the library set `AT+DR=<band>` / channel plan explicitly via Kconfig, or is region configuration entirely the application's responsibility via a raw passthrough command? Given "no application-specific logic" as a non-goal, leaning toward the library owning it via `CONFIG_LORA_E5_REGION`, but this needs confirmation since it's the one config area genuinely regulatory/deployment-specific rather than modem-generic.
6. **Command table extensibility for future modules** — should the AT Command Manager's command descriptors be structured as a data table the FSM populates (enabling a future RN2483 port to supply an entirely different table against the same manager code), or is that abstraction premature for v1 and worth deferring until a second module is actually being ported? Leaning toward keeping the *interface* generic now (cheap) while not over-engineering a plugin registry (expensive) until there's a second concrete consumer.
7. **`lora_e5_timer.c` file-level justification** (§2.6) — confirm whether shared backoff/timeout logic is substantial enough to warrant its own translation unit, or should fold into FSM.

## Assumptions

- Target firmware matches the behavior documented in Seeed's LoRa-E5 AT Command Specification V1.0 (2020-07-20). Firmware version actually shipped on procured units should be checked with `AT+VER` at bring-up and any URC-format deltas patched into the parser before relying on this document's line-by-line detail. [Likely — stated as assumption precisely because I cannot verify the exact firmware revision on your hardware]
- Default UART framing (9600 8N1) is either acceptable or will be reconfigured via `AT+UART=BR` as a one-time provisioning step outside the normal boot FSM (baud change requires a modem reset to take effect per spec §4.35.2, which has FSM implications if done at runtime — assumed out of scope for the *runtime* FSM, handled as a provisioning/manufacturing step instead).
- No hard real-time deadline shorter than typical LoRaWAN RX-window timing (~1-2s RX1 delay per default `AT+DELAY` config) applies to this library's own internal processing latency — the layered work-queue hop cost (§1) is acceptable against that timing budget.
- Single LoRa-E5 instance per device (no multi-instance/multi-UART requirement implied anywhere in the spec) — simplifies to a single static context rather than a driver-instance-array pattern, unless told otherwise.

## Risks

- **Parser fragility against the "not everything ends in OK" hazard (§2.2)** — this is the single most likely source of subtle production bugs: a command whose terminal-event set is misconfigured will either time out spuriously (terminal event never arrives because it was miscategorized as a URC) or resolve early on an unrelated URC that happens to share a prefix. Mitigate with the exhaustive command-table test coverage called out in §10, driven directly from the spec's own documented "Return" examples.
- **Max payload length is DR-dependent and can shrink at runtime** (ADR changes data rate, changing max payload per Table 3-3) — a fixed `CONFIG_LORA_E5_TX_BUFFER_SIZE` sized for the largest case is safe for buffering but the *library* must still surface `+MSG: Length error N` correctly (already covered) rather than allow an oversized payload to be silently truncated before send.
- **Firmware version drift** — URC format stability across firmware majors is not something I can verify without your specific shipped firmware version; treat every parser regex in Phase 3 as needing validation against `AT+VER` output from real hardware, not just this spec document.
- **Recovery ladder interacting with regulatory duty cycle** (§8.3) — an overly aggressive retry/reset policy risks the *host* effectively causing the modem to violate its own documented join duty-cycle envelope by resetting state that the modem uses to track duty-cycle windows (unconfirmed from the spec whether `AT+RESET` clears the duty-cycle accounting — worth explicitly testing on hardware before finalizing `CONFIG_LORA_E5_AUTO_RESET` defaults, since a reset-heavy recovery policy could make an already-struggling deployment worse, not better).
- **Application callback blocking risk** (§4) — without the recommended third notify-queue, a naive integration could silently create a class of bugs (frozen modem driver) that only manifests under specific application-side conditions (e.g., a callback that logs to a congested UART) — recommend this be a hard requirement, not optional, in Phase 2/3 implementation.

## Recommended Next Phase

Proceed to **Phase 2: Module Interface Design** — concrete header-level definitions (structs, enums, function signatures, Doxygen contracts) for `lora_e5_at.h`, `lora_e5_events.h`, `lora_e5_types.h`, still without implementation bodies, so the command-descriptor/terminal-event-set mechanism (§2.3, §10 risk) and the event payload unions (§6) can be reviewed and locked before any `.c` file is written. Recommend resolving Open Questions #1, #3, #4, and #5 first, since they change the FSM's state-entry topology (ABP support, hardware reset availability, region ownership) rather than just its internals.
