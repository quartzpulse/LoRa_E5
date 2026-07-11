# status_led — Portable RGB Status LED Library

An out-of-tree Zephyr module providing a priority-based, event-driven RGB
status LED state machine. Applications call only four functions and never
touch GPIO/SPI/PWM directly:

```c
status_led_init();
status_led_set_state(STATUS_LED_CONNECTING_NETWORK);
status_led_clear_state(STATUS_LED_CONNECTING_NETWORK);
status_led_get_state();
```

See `include/status_led/status_led.h` for full API documentation and the
state priority table (lowest to highest priority, matching the enum order).

## LED behavior (current state -> pattern mapping)

Lowest to highest priority; a higher row always overrides a lower one while
both are active. Defined in `src/status_led_patterns.c` — edit that file to
re-color or re-time a state (see "Adding or re-coloring a state" below).

Shapes and timings are deliberately simple and borrowed as-is from this
project's original monochrome indicator (`led_indicator.c`, since retired) --
single/double/triple pulse rhythms rather than per-state breathing fades.
Color, not shape, carries most of the meaning; shape only distinguishes
severity tiers (steady vs. connecting vs. urgent).

| State | Color | Pattern | Pulse / period |
|---|---|---|---|
| `STATUS_LED_SLEEP` | off | off | n/a |
| `STATUS_LED_NETWORK_CONNECTED` | green | single pulse | 100 ms on, 2900 ms off |
| `STATUS_LED_MQTT_CONNECTED` | cyan | single pulse ("sleepy blip") | 50 ms on, 5000 ms off |
| `STATUS_LED_CONNECTING_MQTT` | cyan | double pulse | 150 ms x3 on/off, 1500 ms off |
| `STATUS_LED_CONNECTING_NETWORK` | green | fast blink | 250 ms on, 250 ms off |
| `STATUS_LED_BOOTING` | white | slow blink | 1000 ms on, 1000 ms off |
| `STATUS_LED_PROVISIONING` | blue | triple pulse | 100 ms x3 on/off, 2000 ms off |
| `STATUS_LED_OTA_PREPARING` | yellow | double pulse | 150 ms x3 on/off, 1500 ms off |
| `STATUS_LED_OTA_UPDATING` | magenta | triple pulse | 150 ms x3 on/off, 1200 ms off |
| `STATUS_LED_HW_FAILURE` | red | solid | n/a |
| `STATUS_LED_FATAL_ERROR` | red | fast blink | 100 ms on, 100 ms off |

Notes on how this reads in practice:
- `CONNECTING_MQTT` and `OTA_PREPARING` share the exact same double-pulse
  rhythm and only differ by color (cyan vs. yellow) — on a monochrome
  backend (`MONO_GPIO`/`MONO_PWM`) they render identically. Same for
  `PROVISIONING` (blue) vs. `OTA_UPDATING` (magenta), both triple-pulse.
- `BOOTING` outranks both `CONNECTING_*` states, so the whole
  init sequence (network wait, SNTP, provisioning, MQTT/OTA init) shows solid
  `BOOTING` until the app's main loop starts, even though the device may
  already be mid-connect underneath.
- No state currently uses `LED_PATTERN_BREATHING` -- the pattern engine still
  supports it (see `src/status_led_pattern_engine.c`) for anyone who wants a
  smoother look on WS2812/PWM hardware, it's just not in the default table.
- Blink/pulse patterns compute their full on/off step sequence once per state
  change and otherwise sit idle between steps -- no continuous polling.

## Architecture

```
application  ->  status_led (bitmap + mutex)  ->  priority engine
             ->  pattern engine (k_work_delayable interpreter)
             ->  hardware backend (WS2812 / GPIO / PWM / mono GPIO / mono PWM)  ->  DeviceTree
```

Multiple states may be active simultaneously; the library always renders the
single highest-priority active state and automatically falls back when it is
cleared. See `src/status_led_priority.c` and `src/status_led_pattern_engine.c`
for the two engines, and `src/status_led_patterns.c` for the data-only
state-to-pattern mapping.

## Enabling

```
CONFIG_STATUS_LED=y
CONFIG_STATUS_LED_BACKEND_WS2812=y   # or _GPIO / _PWM / _MONO_GPIO / _MONO_PWM
```

`CONFIG_STATUS_LED=n` removes the module from the build entirely.

In the parent project, the backend and its devicetree wiring are selected
per board variant rather than hardcoded: `build_lte.sh` targets the custom
EC200U board (`CONFIG_STATUS_LED_BACKEND_MONO_GPIO`, `boards/status_led_mono.overlay`),
`build_wifi.sh` targets the Waveshare ESP32-S3 DevKit
(`CONFIG_STATUS_LED_BACKEND_WS2812`, `boards/status_led_rgb.overlay`). Both
overlays are passed alongside the shared `boards/esp32s3_devkitc_procpu.overlay`
via an explicit `-DDTC_OVERLAY_FILE="...;..."` list, since setting that
variable replaces Zephyr's default board-overlay auto-discovery rather than
adding to it.

## Backends and their DeviceTree contract

| Backend | Kconfig | Devicetree alias(es) required | Notes |
|---|---|---|---|
| WS2812 (addressable) | `CONFIG_STATUS_LED_BACKEND_WS2812` | `led-strip` -> a `worldsemi,ws2812-*` node | Brightness applied by scaling RGB in software (no hardware dimming). See `boards/status_led_rgb.overlay` in the parent project for a working SPI3-based reference. |
| Discrete GPIO | `CONFIG_STATUS_LED_BACKEND_GPIO` | `red-led`, `green-led`, `blue-led` -> `gpio-leds` children | On/off only; a raw channel value >= 128 is treated as on. `CONFIG_STATUS_LED_DEFAULT_BRIGHTNESS` is ignored here -- it has no dimming capability, and scaling it in would push every "on" value below the threshold at low brightness settings. |
| PWM | `CONFIG_STATUS_LED_BACKEND_PWM` | `pwm-red-led`, `pwm-green-led`, `pwm-blue-led` -> `pwm-leds` children | True brightness via duty cycle. |
| Monochrome GPIO | `CONFIG_STATUS_LED_BACKEND_MONO_GPIO` | `mono-led` -> a `gpio-leds` child | Single on/off LED; color collapsed to `max(r, g, b)` before thresholding against the raw (not brightness-scaled) value, same reasoning as discrete GPIO. States that only differ by color look identical. |
| Monochrome PWM | `CONFIG_STATUS_LED_BACKEND_MONO_PWM` | `pwm-mono-led` -> a `pwm-leds` child | Single dimmable LED; color collapsed to `max(r, g, b)`, rendered as true duty-cycle brightness. |

Only one backend is compiled in per build (`STATUS_LED_BACKEND` is a Kconfig
choice); adding a new backend means implementing `status_led_hw_init()` /
`status_led_hw_write()` (see `include/status_led/status_led_hw.h`) in a new
`src/backends/status_led_hw_<name>.c` file and adding a choice entry to
`Kconfig` plus a `zephyr_library_sources_ifdef()` line in `CMakeLists.txt` —
no changes to the priority or pattern engines are ever required.

## Adding or re-coloring a state

Edit one line in `src/status_led_patterns.c`. The pattern engine
(`src/status_led_pattern_engine.c`) interprets the closed set of pattern
types (`LED_PATTERN_OFF/SOLID/BLINK/DOUBLE_BLINK/TRIPLE_BLINK/BREATHING/
PULSE/FLASH`) generically; it never needs to change for a new state or color.
`BLINK`/`BREATHING`/`SOLID`/`OFF` only use `period_ms`; `DOUBLE_BLINK`,
`TRIPLE_BLINK`, `PULSE`, and `FLASH` also need `pulse_ms` (the on-time of
each pulse) since those shapes are asymmetric (short pulse, long gap).

To add an entirely new *state* (not just recolor an existing one): insert it
into `enum status_led_state` in `include/status_led/status_led.h` at the
correct priority rank (numeric value = priority; higher always wins), add a
matching entry to `status_led_pattern_table[]`, and renumber any states above
the insertion point.

## Sample

`samples/basic/` is a minimal, standalone application (own `prj.conf`, no
networking/MQTT/OTA dependency) demonstrating the full lifecycle: boot ->
provisioning -> network -> MQTT -> OTA -> fatal error. Build it directly:

```bash
west build -b esp32s3_devkitc/esp32s3/procpu modules/status_led/samples/basic
```

## Future work

- Low-power integration: suspend the pattern engine when `STATUS_LED_SLEEP`
  is resolved and let the system enter deeper sleep.
- Additional pattern types (e.g. multi-LED rainbow cycle for strips with
  `chain-length > 1`).
- Shell command for manual testing (`status_led set <state>`), mirroring
  Zephyr's own `led_strip_shell.c`.
