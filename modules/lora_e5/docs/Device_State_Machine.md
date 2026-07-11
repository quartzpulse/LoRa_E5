```

                +-----------+
                |   RESET   |
                +-----------+
                      |
                      v
              +---------------+
              | BOOT/INIT HW  |
              +---------------+
                      |
                      v
            +-------------------+
            | Load Configuration|
            +-------------------+
                      |
             Config valid?
              /           \
             /             \
           Yes             No
            |               |
            |               v
            |       Provisioning Mode
            |               |
            |<--------------+
            |
            v
      +----------------+
      | LoRa Join      |
      +----------------+
        |          |
   Success       Failure
      |             |
      |        Retry Backoff
      |             |
      +------<------+
      |
      v
 +---------------------+
 | Normal Operation    |
 +---------------------+
      |
      |
      +----------------------+
      |                      |
      | Timer                |
      | Sensor Trigger        |
      | Downlink             |
      | Alarm                |
      v
   Transmit Packet
      |
      v
 Wait RX1
      |
 Wait RX2
      |
 Process Downlink
      |
      v
 Deep Sleep
      |
 Wakeup
      |
      +---------------> Normal Operation
```

## Architecture: how this maps to the real implementation

This diagram describes an **application-level** device lifecycle, not the
`lora_e5` library's own modem FSM. The library already owns the full
boot/config/join/send/sleep protocol state machine internally
(`src/lora_e5_fsm.c`: `OFF -> RESET -> BOOT -> CHECK_AT -> CONFIG -> READY
-> JOINING -> JOINED -> TX_PENDING -> WAIT_TX_RESULT -> SLEEP ->
ERROR/RECOVERING`) and CLAUDE.md's layering rule (#6) means nothing outside
that file should reimplement modem-lifecycle state. So this diagram's
states are implemented as a **thin sequence built on the public API**
(`include/lora_e5/lora_e5.h`), not as a second parallel FSM.

Decisions made when turning this diagram into `samples/device_node/`
(2026-07-11, confirmed with the project owner):

1. **"Deep Sleep" is real ESP32 MCU deep sleep**, not just
   `lora_e5_sleep()` (modem-only sleep, MCU stays live). This means Zephyr
   RAM -- every work queue, `k_msgq`, and all of `lora_e5`'s live state --
   does **not** survive a sleep/wake cycle: waking is a full reboot back
   into `main()`. Consequently "Normal Operation" is not a long-running
   event loop waiting on a work queue; it is a **linear per-wake sequence**
   that runs once per boot and ends by re-arming sleep.
2. **Trigger is Timer only for v1.** Sensor Trigger / Downlink / Alarm from
   the diagram are not wired up -- the wake itself (RTC timer wakeup
   source) *is* the timer trigger. They can be added later as additional
   `esp_sleep_enable_*_wakeup()` sources read back via
   `esp_sleep_get_wakeup_causes()`, following the same pattern, without
   restructuring anything.
3. **"Load Configuration" / "Provisioning Mode" are not implemented.**
   OTAA credentials stay hardcoded in `src/main.c`, same as
   `samples/join`. No persistent settings storage, no real provisioning
   path -- can be layered in later without changing the sequence's shape.
4. **"Transmit Packet -> Wait RX1 -> Wait RX2 -> Process Downlink"
   collapses to one `lora_e5_send_sync()` call.** Same reasoning as
   CLAUDE.md decision #3: RX1/RX2/ACK are event metadata inside the
   library's `WAIT_TX_RESULT` state, never separate states -- there is no
   reason for the application layer to re-introduce them as separate
   steps. Downlinks arrive via the registered callback as
   `LORA_E5_APP_EVT_DOWNLINK_RECEIVED` regardless of which window they
   rode in on.
5. **Retained memory and SoC deep sleep are both available on this
   board**, confirmed via
   `external/zephyr/samples/boards/espressif/deep_sleep`, which ships an
   `esp32s3_devkitc_procpu.overlay` (this project's exact board):
   - `CONFIG_RETAINED_MEM=y` + a devicetree `zephyr,retained-ram` node
     carved out of `rtc_slow_ram` (`RTC_SLOW_RAM_RETAINED_MEM` region,
     base `0x50001f00` on this SoC) survives deep sleep, read/written via
     `retained_mem_read()`/`retained_mem_write()`.
   - `CONFIG_PM=y` + `CONFIG_PM_DEVICE=y` + `CONFIG_POWEROFF=y`, then
     `esp_sleep_enable_timer_wakeup(usec)` followed by `sys_poweroff()`,
     puts the SoC into deep sleep with a timer wakeup source. Wake cause
     is read back via `esp_sleep_get_wakeup_causes()`.
6. **Retained memory does NOT let a wake skip the join call.**
   `lora_e5_start_sync()` unconditionally issues `AT+RESET` then
   `CONFIG` (`lora_e5_fsm.c`'s `handle_start_request()`/
   `handle_probe_result()`) -- there is no "resume without reset" entry
   point in the public API today. So every wake redoes the full
   init/config/join sequence regardless of whether the LoRa-E5 module
   itself kept its session across the MCU's sleep. The retained struct in
   `samples/device_node` is used for a wake/boot counter (diagnostic),
   not to skip work -- claiming otherwise would be an unverified
   optimization the project's `VERIFICATION_NEEDED.md` convention
   explicitly warns against.
7. **Resolved (was "open/unverified"): the module's VCC is on an
   always-on rail** -- confirmed by the project owner. This raised the
   natural follow-up -- "so can boot skip the reset/join if the module
   never lost its session?" -- which was investigated but not yet
   answered yes: an attempted "check join status first" query turned
   out to be built on a misread of the AT spec (`AT+LW=NET` is the
   public-vs-private network sync word, not a join indicator -- see
   `docs/VERIFICATION_NEEDED.md`'s "Resolved 2026-07-11" section for
   the correction) and got renamed to
   `lora_e5_get_public_network_mode()` accordingly. The spec's actual
   documented "already joined" signal is `AT+JOIN`'s own `"+JOIN:
   Joined already"` response (§4.5.2), already modeled in this codebase
   (`LORA_E5_MM_TAG_JOIN_ALREADY`) -- but a real-hardware timing test
   showed a post-`start_sync()` `lora_e5_join_sync()` still takes the
   full ~8s handshake, not that fast path. First candidate explanation
   (`CONFIG`'s unconditional `AT+MODE=LWOTAA` reissue every boot,
   spec §4.23) was tested directly on real hardware by temporarily
   skipping it -- **refuted**, join still took 8033ms with a new
   DevAddr. Current leading candidate: `AT+RESET` itself clearing
   volatile OTAA session state, which every test so far has had no way
   to isolate from (`lora_e5_start_sync()` is the only public path to
   `READY` and always resets first). See
   `docs/VERIFICATION_NEEDED.md`'s "Resolved 2026-07-11" section, item
   5, for the full reasoning. So point 6 above still stands as-is: there
   is currently no verified way to skip `lora_e5_start_sync()`'s
   reset+config or `lora_e5_join_sync()`'s join, even though the
   underlying session survives both an MCU reboot and our own
   `AT+RESET`+`CONFIG`. See `docs/VERIFICATION_NEEDED.md`'s
   "Resolved 2026-07-11" section for the full investigation and the
   concrete next experiment if this is revisited.

See `samples/device_node/` for the implementation.

## Confirmed against real hardware (2026-07-11)

Flashed to the real esp32s3_devkitc + LoRa-E5-HF setup and observed three
consecutive real deep-sleep/wake cycles over serial:
`RESET -> BOOT -> CHECK_AT -> CONFIG -> READY -> JOINING -> JOIN_SUCCESS ->
JOINED -> TX_PENDING -> WAIT_TX_RESULT` on every wake, a genuine `rst:0x5
(DSLEEP)` boot each time (not a plain reset), the retained-memory boot
counter correctly incrementing 1 -> 2 -> 3 across power-off, and a
different DevAddr on every cycle (confirming point 6 above: each wake
really does a fresh OTAA join, nothing is being resumed).

One real bug found and fixed by this run: `go_to_sleep()` originally
called `sys_poweroff()` immediately after arming the timer wakeup source,
which cut power before Zephyr's deferred logging thread had drained its
queue -- the TX result and "Send confirmed" log lines were silently lost
on every cycle. Fixed with `LOG_PANIC()` (flushes pending log messages
and switches logging synchronous) called just before `sys_poweroff()` --
see `src/main.c`.