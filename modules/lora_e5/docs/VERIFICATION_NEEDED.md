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
