# LoRa-E5 Zephyr Library — Phase 2: Module Interface Design Notes

Headers only, no `.c` bodies, per process. Six files, listed with what each owns and why.

## Updated directory layout

```
modules/lora_e5/
├── CMakeLists.txt
├── Kconfig
├── README.md
│
├── include/
│   └── lora_e5/
│       ├── lora_e5.h              (public API — application includes this)
│       ├── lora_e5_events.h       (public — app_event vocabulary + fsm_event vocabulary)
│       ├── lora_e5_types.h        (public — states, regions, config structs, error codes)
│       ├── lora_e5_config.h       (public — hw_config: UART dev, reset GPIO, Kconfig index)
│       └── lora_e5_at.h           (public-ish — needed by anyone writing a Modem Manager
│                                    for a future modem; not something a typical
│                                    application includes)
│
├── src/
│   ├── lora_e5.c
│   ├── lora_e5_uart.c
│   ├── lora_e5_parser.c
│   ├── lora_e5_at.c
│   ├── lora_e5_cmd_queue.c
│   ├── lora_e5_modem_manager.h    (INTERNAL — Modem Manager interface, NOT installed)
│   ├── lora_e5_modem_manager.c
│   ├── lora_e5_hf_commands.c      (INTERNAL — LoRa-E5-HF specific command/terminal-event
│                                    tables; this is the file a future RN2483/Type-ABZ/
│                                    Quectel port replaces)
│   ├── lora_e5_fsm.c
│   ├── lora_e5_events.c
│   └── lora_e5_internal.h
│
├── samples/
│   ├── join/
│   ├── uplink/
│   └── shell/
│
└── tests/
    ├── parser/
    ├── at_cmd_queue/
    ├── fsm/
    └── mock_uart/
```

Deltas from the original layout: `lora_e5_timer.c` removed (decision #7 — folded into FSM/Modem Manager call sites, wasn't earning a separate file). `lora_e5_modem_manager.{h,c}` and `lora_e5_hf_commands.c` added (decision: Modem Manager layer). `lora_e5_at.h` promoted to `include/` rather than staying fully internal, since a second modem port needs to see the terminal-event matching mechanism to write its own command table against it.

## What each header owns

- **`lora_e5_types.h`** — the vocabulary layer. States, regions, error codes (verbatim from AT Command Specification V1.0 Table 2-1, all nine codes), activation config structs. Zero AT-command strings anywhere in this file.
- **`lora_e5_at.h`** — the mechanism that makes the "Modem Manager owns AT semantics, AT Command Manager stays generic" split actually work: `struct lora_e5_at_line` (parser output — prefix/remainder/kind, no interpretation) and `struct lora_e5_at_terminal_event` (prefix+remainder match rules with an opaque `result_tag`). The AT Command Manager walks a descriptor's `terminal_events[]` against classified lines using string matching only — it never needs a LoRaWAN-specific switch statement. This is the concrete form of "do it now" (decision #6): the command table is data (an array of `lora_e5_at_terminal_event`), owned by whoever populates the descriptor, not hardcoded logic in the transaction engine.
- **`lora_e5_events.h`** — two separate enums on purpose. `lora_e5_fsm_event_type` is internal, will churn as FSM internals change. `lora_e5_app_event_type` is the external contract and is deliberately smaller — collapsing OTAA/ABP distinctions, RX-window distinctions, etc. into payload fields rather than proliferating event types the application has to switch on.
- **`lora_e5_modem_manager.h`** (internal, `src/`) — every function here is a synchronous, non-blocking translation call: FSM calls `lora_e5_mm_join()`, it decides OTAA-vs-ABP internally and either calls `lora_e5_at_submit()` (OTAA) or synthesizes an immediate result event (ABP) — the FSM does not know the difference exists. This is where the ABP skip-join branch actually lives, not in the FSM (Phase 1 decision #3 said "internally the FSM can simply skip JOINING" — on reflection, it's cleaner for the FSM to not even know ABP exists as a branch; it just gets a `JOIN_RESULT` event either way, with `outcome == ABP_SKIP` vs `SUCCESS` as the only tell, and both drive the same `READY → JOINED` transition).
- **`lora_e5_config.h`** — intentionally *not* the same struct as the LoRaWAN activation config in `lora_e5_types.h`. Board wiring (UART device, optional reset GPIO) and LoRaWAN provisioning (keys, EUIs, region) are different concerns with different owners in a real product (hardware/board team vs. provisioning/backend team) — merging them into one struct would force both concerns through the same code path and the same review.
- **`lora_e5.h`** — sync/async naming resolved as flagged in Phase 1: explicit `_sync` suffix on every blocking variant, no bare function name that silently blocks depending on context.

## Corrections made against the full spec text (now fetched in full, not partial)

- Factory default mode is **LWABP**, not LWOTAA [Certain, Table 4-2] — documented directly in `lora_e5_factory_reset()`'s doc comment, since this is exactly the kind of detail a future maintainer will get wrong by assuming symmetry.
- Full nine-entry error code table now in `lora_e5_at_error` (Phase 1 only had five confirmed; `-21` command-too-long, `-22` end-symbol-timeout, `-23` invalid-character, `-24` composite were unverified guesses I explicitly declined to fabricate at the time).
- `lora_e5_at_error_is_structural()` added as a static inline helper directly in the types header — makes the Phase 1 §8.1 fault classification table enforceable in code instead of just documented, and is exactly the kind of invariant the Phase 1 testing strategy called out as needing a regression guard.

## Open items carried forward (unchanged from last review, still need your input before Phase 3)

1. `WAIT_TX_RESULT → JOINED` (not `READY`) is now baked into the header comments and the state enum's own documentation. Treat as decided unless you object.
2. `CONFIG_LORA_E5_HAS_RESET_GPIO` defaults to unset/AT+RESET-only until your board schematic confirms NRST is routed.
3. Not yet decided: should `lora_e5_hf_commands.c`'s terminal-event tables be generated from a data file (e.g., a small DSL or table literal reviewed against the spec line-by-line) or hand-written directly as C initializers? Hand-written is simpler for a single modem; a generator only pays off if you're targeting multiple modems' tables from one source of truth. Recommend hand-written for v1, revisit if/when a second modem port is actually scheduled.

## Recommended Next Phase

**Phase 3: Implementation**, starting with the two lowest-risk, highest-test-value modules first per the Phase 1 testing strategy: `lora_e5_at_parse_line()` (pure function, fully spec-vector-testable right now) and `lora_e5_hf_commands.c`'s terminal-event tables (data, reviewable against the spec independent of any runtime code). AT Command Manager (`lora_e5_at.c`/`lora_e5_cmd_queue.c`) next, with its mock-UART test harness, before touching the FSM or UART backend.
