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

### 1. `AT+ID` SET syntax (lora_e5_hf_commands.c: `lora_e5_hf_build_id_set_eui()`)
**[Guessing].** Modeled by analogy to `AT+KEY=APPKEY,<32-hex-chars>`
(which IS confirmed -- plain contiguous hex, no colons). No capture
in this review showed `AT+ID` used as a SET command; every capture
showed it as a bare query only. The query's own response uses
colon-separated hex (`+ID: DevAddr, 32:30:84:63`), which is a
DIFFERENT format from `KEY`'s confirmed plain-hex set syntax -- these
could plausibly use different conventions for set-vs-display, and I
have no evidence either way for the set form specifically.
**How to check:** send `AT+ID=DevEui,<hex>` on real hardware with
`AT+LOG=DEBUG` enabled, capture the actual response/whether it's
accepted at all (vs. an ERROR(-11) wrong-format rejection).

### 2. `AT+LW=LEN` max-payload query (`lora_e5_hf_build_max_payload_query()`)
**[Guessing], currently returns -ENOTSUP on purpose -- not wired to
anything.** `AT+LW` is a documented multi-purpose command with
subcommands CDR/ULDL/NET/DC/MC/THLD; whether "LEN" is a valid
subcommand of it, or the max-payload value is exposed some other way
entirely, was not re-confirmed against the primary PDF in this
review pass.
**How to check:** open the primary spec PDF directly (not a mirror/
secondary source) at the `AT+LW` section (§4.x) and read the full
subcommand list. If "LEN" isn't there, find whichever command
actually exposes max payload (it may not exist at all as a queryable
value -- Table 3-3's per-DR payload limits might be something this
library has to hardcode per region+DR combination instead of
querying).

---

## Non-blocking but flagged -- confirm before relying on exact wording

### 3. Config-command echo format (MODE, DR, PORT, CLASS, REPT set commands)
**[Likely].** Assumed to echo `"+CMD: VALUE"` back on success, by
direct analogy to the ONE confirmed example (`AT+ADR=OFF` ->
`+ADR: OFF`, from a captured Arduino library session log) plus the
spec's own general `"+CMD: RETURN DATA"` convention statement
(§2.3.3). Individual echoes for MODE/DR/PORT/CLASS/REPT were not
each independently captured. Mitigated in code: these all use
`LORA_E5_AT_MATCH_ANY_URC` (prefix-only matching), which resolves
correctly regardless of the exact remainder text -- so this is lower
risk than it would be with exact-remainder matching, but still worth
confirming the prefix itself doesn't differ from expected.
**How to check:** capture real responses for each with `AT+LOG=DEBUG`
during a real CONFIG sequence.

### 4. Six of twelve region strings (`lora_e5_hf_commands.c: region_strings[]`)
**[Likely].** `EU868`/`US915`/`AU915`/`AS923`/`KR920`/`IN865` are
**[Certain]** -- repeated across multiple independent sources in this
review (Seeed's own product spec sheet, a Hackster tutorial).
`US915HYBRID`/`CN779`/`EU433`/`AU915OLD`/`CN470`/`RU864` come from an
earlier full-spec fetch in this conversation that could not be
re-verified against visible primary-source text in this specific pass
-- the exact spelling (e.g. is it "AU915OLD" or "AU915_OLD"? Is
"US915HYBRID" one word or hyphenated?) is unconfirmed.
**How to check:** re-open the primary spec PDF, Table 3-1 (band plan
list), and copy the literal strings verbatim.

### 5. `AT+MSGHEX`/`AT+CMSGHEX` port parameter (`lora_e5_hf_build_send()`)
**Not a syntax question -- a real behavioral gap, currently
unhandled.** Every capture shows `AT+MSGHEX=<hex>` with no port
argument; port is set once via a separate `AT+PORT=` transaction.
`lora_e5_hf_build_send()`'s `port` parameter is currently a
documented no-op. **This will silently send on the wrong port** if
an application calls `lora_e5_send()` with a port differing from the
last `AT+PORT=` value, with no error surfaced anywhere. Fix belongs
in `lora_e5_modem_manager.c` (not yet written): cache last-configured
port, reissue `AT+PORT=` before `AT+MSGHEX`/`AT+CMSGHEX` if the
caller's requested port differs.

### 6. `AT+ID` query trailing terminator
**[Certain the response is 3 lines; unconfirmed whether anything
follows them.]** Resolved functionally via `required_matches = 3`
(Phase 3) rather than needing a terminator -- but this assumes
EXACTLY 3 `+ID:` lines are always returned. If a firmware revision
ever adds a 4th field, this will hang until the 3-match requirement
is met by 3 of 4 lines (probably still resolves correctly-ish since
it just needs 3 occurrences of the prefix, but the 4th line would be
consumed as a match toward a NEW transaction if one happens to be
queued right after -- a low-probability but real edge case).
**How to check:** capture `AT+ID` output against your actual firmware
version and confirm line count.

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
