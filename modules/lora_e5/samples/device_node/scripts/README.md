# scripts/ — NVS Settings Code Generator

This directory holds `gen_settings_module.py`, the code generator behind the
project's NVS-backed settings system. Settings modules (`hwconfig`, `lte`,
`mqtt_cfg`, `ota`, `shared`, `wifi`, ...) are **not** hand-written — they are
generated at CMake configure time from YAML definitions in `config/`, and the
generated output in `src/autogen-code/` is overwritten (`--force`) on every
build. Never edit `src/autogen-code/*` or `kconfig/Kconfig.settings_*` by
hand; edit the YAML instead.

## Why generated code

Every NVS-backed setting needs the same boilerplate: a mutex-guarded struct
field, a getter/setter pair, a `settings_handler` load callback, an optional
shell command, and an RPC `get_setting`/`set_setting` case. Writing that by
hand for every field across six+ modules is repetitive and error-prone.
`gen_settings_module.py` takes a declarative list of fields (kind, C type,
NVS key) and emits all of it consistently.

## Quick start — adding a setting to an existing module

1. Edit the module's YAML in `config/settings_<module>.yaml`, e.g. add an
   entry under `entries:`.
2. Rebuild (`./build_lte.sh` / `./build_wifi.sh`, or `west build`). CMake
   globs `config/settings_*.yaml` on every configure and regenerates
   `src/autogen-code/` — no manual generator invocation needed.
3. If you added a field, use the generated accessor:
   `get_nvs_<type>_<key>()` / `set_nvs_<type>_<key>(val)`.

## Quick start — creating a brand-new module

```bash
# Scaffold a new YAML (writes config/settings_<module>.yaml)
python3 scripts/gen_settings_module.py --init <module> --out-dir config
```

The `--out-dir config` matters — only `config/*.yaml` is picked up by
CMake's glob. Edit the scaffolded YAML (module name, description, entries),
then just rebuild; CMake auto-discovers the new file. Finally, call
`settings_<module>_init()` from `main()` after `settings_load()` (see
[Wiring into `main()`](#wiring-into-main) below).

No CMakeLists.txt edit is required — adding `config/settings_<module>.yaml`
is sufficient.

## YAML format

```yaml
module: hwinfo                  # NVS namespace & file prefix
description: "HW info settings" # optional, used in generated Kconfig help text
out_dir: src/autogen-code        # optional, output dir for generated .c/.h (default: src/autogen-code)
shell: true                      # optional, generate per-module shell commands (default: true)

entries:
  - kind: STR                   # STR | INT | BOOL | FLOAT | DOUBLE
    type: hw_version_t          # C type; for STR also becomes the typedef name
    name_var: hw_version        # struct field name
    str_key: hw_version         # NVS key segment (also the shell sub-command name)
    str_len: 32                 # STR only: buffer size incl. NUL, default 64
    sensitive: false            # optional: mask value as "****" in RPC get responses
    readonly: false             # optional: reject RPC set_setting for this key

  - kind: INT
    type: int32_t               # any integer type
    name_var: sensor_count
    str_key: sensor_count

  - kind: BOOL
    name_var: debug_enabled     # 'type' defaults to bool, may be omitted
    str_key: debug_enabled

  - kind: FLOAT
    name_var: calibration_factor  # 'type' defaults to float
    str_key: calib_factor

  - kind: DOUBLE
    name_var: precision_value     # 'type' defaults to double
    str_key: precision_value
```

See `config/settings_ota.yaml` for a real example with `sensitive`/comment
usage patterns.

## What gets generated

For a module named `<module>`, in `<out_dir>` (default `src/autogen-code`):

| File | Contents |
|---|---|
| `settings_<module>.h` | `struct <module>_settings` (mutex + one field per entry), `<module>_config` extern, `settings_<module>_init()` decl, getter/setter decls |
| `settings_<module>.c` | Default-initialized `<module>_config` instance, getter/setter bodies (mutex-guarded for non-STR, `settings_save_one()` on every set), the `settings_handler` load callback, optional shell commands (`<module> <key> <value>`, `<module> show`), `settings_<module>_init()` (registers the handler) |
| `settings_<module>_list.h` | X-macro list of entries — `#include`d multiple times inside `.c`/`.h` with different macro definitions to avoid repeating the entry list |
| `rpc_settings_<module>.h/.c` | `rpc_<module>_get(key, buf, buf_size)` / `rpc_<module>_set(key, params, buf, buf_size)` — JSON in/out RPC handlers for this module's keys |

And in `<kconfig_dir>` (default `kconfig`):

| File | Contents |
|---|---|
| `Kconfig.settings_<module>.h` → `Kconfig.settings_<module>` | `CONFIG_NVS_SETTINGS_<MODULE>_SHELL` — per-module toggle for the generated shell commands |

A second pass, `--aggregate`, is run once after all per-module files exist
(driven by CMake — see below) and emits cross-module dispatch:

| File | Contents |
|---|---|
| `src/autogen-code/rpc_settings_all.c/.h` | `rpc_dispatch_module_get()` / `rpc_dispatch_module_set()` — routes an RPC call by module name string to the right `rpc_<module>_get/set()` |
| `kconfig/Kconfig.settings_all` | `rsource`s every per-module `Kconfig.settings_<module>` |

### The X-macro pattern

`settings_<module>_list.h` contains one line per entry:

```c
NVS_<MODULE>_STR_ADD(ota_server_t, server_url, server_url)
NVS_<MODULE>_INT_ADD(int32_t, retry_count, retry_count)
```

`settings_<module>.h`/`.c` `#include` this file repeatedly, each time with
`NVS_<MODULE>_{STR,INT,BOOL,FLOAT,DOUBLE}_ADD` redefined as a different
macro (struct field, getter/setter, load-callback case, shell command,
`show` line, ...). This is why the entry list lives in its own header: it's
the single source of truth that every generated code section expands
against.

## CLI reference

```bash
# Scaffold a new YAML (then edit it)
python3 scripts/gen_settings_module.py --init <module> --out-dir config

# Generate from an explicit YAML path
python3 scripts/gen_settings_module.py --yaml config/settings_<module>.yaml \
    --out-dir src/autogen-code --kconfig-dir kconfig --force

# Auto-discover config/settings_<module>.yaml by module name
python3 scripts/gen_settings_module.py --module <module>

# Aggregate pass — run after all per-module YAMLs are generated
python3 scripts/gen_settings_module.py --aggregate config/settings_*.yaml \
    --out-dir src/autogen-code --kconfig-dir kconfig

# Fallback: inline entries, no YAML
python3 scripts/gen_settings_module.py --module <module> \
    --entries "STR:hw_version_t:hw_version:hw_version:32" \
              "INT:int32_t:sensor_count:sensor_count" \
              "BOOL:bool:debug_enabled:debug_enabled"

# Fallback: fully interactive prompts
python3 scripts/gen_settings_module.py
```

`--force` overwrites existing generated files without a confirmation
prompt — this is what CMake passes, since regeneration on every configure
is expected. Without it, the script asks per-file before overwriting
(useful when running by hand to inspect a diff).

Requires `pyyaml` (`pip install pyyaml`) for any YAML-driven mode; the
`--entries`/interactive fallback modes work without it.

## Wiring into a new project via CMakeLists.txt

This is already wired up in this project's `CMakeLists.txt` (top of the
file, before `find_package(Zephyr REQUIRED ...)`). To port the pipeline
into a new project:

```cmake
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Discover every module YAML — adding config/settings_<mod>.yaml is
# sufficient; no further CMakeLists changes needed.
file(GLOB SETTINGS_YAML_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/settings_*.yaml")

foreach(yaml ${SETTINGS_YAML_FILES})
    get_filename_component(module_name ${yaml} NAME_WE)
    string(REPLACE "settings_" "" module_name ${module_name})

    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_settings_module.py
                --yaml ${yaml}
                --out-dir ${CMAKE_CURRENT_SOURCE_DIR}/src/autogen-code
                --kconfig-dir ${CMAKE_CURRENT_SOURCE_DIR}/kconfig
                --force
        RESULT_VARIABLE gen_res
    )
    if(NOT gen_res EQUAL 0)
        message(FATAL_ERROR "Failed to generate settings for ${yaml}")
    endif()

    list(APPEND GENERATED_SETTINGS_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/src/autogen-code/settings_${module_name}.c
         ${CMAKE_CURRENT_SOURCE_DIR}/src/autogen-code/rpc_settings_${module_name}.c)
endforeach()

# Aggregate pass — must run after all per-module files exist.
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_settings_module.py
            --aggregate ${SETTINGS_YAML_FILES}
            --out-dir ${CMAKE_CURRENT_SOURCE_DIR}/src/autogen-code
            --kconfig-dir ${CMAKE_CURRENT_SOURCE_DIR}/kconfig
    RESULT_VARIABLE agg_res
)
if(NOT agg_res EQUAL 0)
    message(FATAL_ERROR "Failed to generate RPC settings aggregator")
endif()

list(APPEND GENERATED_SETTINGS_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/src/autogen-code/rpc_settings_all.c)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(<your_project>)

target_sources(app PRIVATE
    src/main.c
    # ... other sources ...
    ${GENERATED_SETTINGS_SOURCES}
)
```

Key points for porting:

- The `execute_process()` calls run at **CMake configure time**, before
  `project()` — this is why `find_package(Python3 ...)` comes first and
  the generator must not depend on any Zephyr/Python3-target CMake state.
  Builds are pristine by convention (`west build -p always`), so
  regeneration reliably runs; if you rely on incremental builds, note that
  `CONFIGURE_DEPENDS` on the glob re-triggers CMake reconfiguration when a
  YAML file's mtime changes, but adding/removing YAMLs still needs a
  reconfigure to be picked up.
- `GENERATED_SETTINGS_SOURCES` collects one `settings_<module>.c` +
  `rpc_settings_<module>.c` pair per YAML, plus the single aggregator
  `rpc_settings_all.c`, and gets appended to `target_sources(app PRIVATE ...)`.
- Also source the aggregator Kconfig from your project's root `Kconfig`:
  ```
  rsource "kconfig/Kconfig.settings_all"
  ```
- `rpc_helpers.h` (the `rpc_str`/`rpc_int`/`rpc_bool`/`rpc_float`/`rpc_double`
  JSON-param parsers used by generated `rpc_settings_<module>.c`) must exist
  in the include path — see `src/rpc_helpers.h` in this project.

## Wiring into `main()`

```c
ret = settings_subsys_init();
settings_<module>_init();   /* once per module, before settings_load() */
...
settings_load();            /* restores all registered modules from NVS */
```

Call `settings_<module>_init()` for every module before `settings_load()`
so each module's `settings_handler` is registered in time to receive its
NVS records. See `src/main.c`'s `main()` for the full sequence across all
six current modules — note `shared` sets compile-time fallback defaults
between its `_init()` call and `settings_load()`, since NVS values (if
present) should override those defaults, not the reverse.

## RPC integration

`src/rpc_handler.c` calls `rpc_dispatch_module_get(module, key, buf, sizeof(buf))`
and `rpc_dispatch_module_set(module, key, params, buf, sizeof(buf))` — both
from the generated `rpc_settings_all.h` — to route a `get_setting`/
`set_setting` RPC request by module name to the right per-module handler.
No per-module RPC wiring is needed beyond adding the module's YAML; the
aggregate pass handles dispatch.

## Current modules

`hwconfig`, `lte`, `mqtt_cfg`, `ota`, `shared`, `wifi` — see their YAMLs in
`config/` for real-world examples of each entry kind, `sensitive`, and
multi-line comments documenting non-obvious fields.

## In this project (`samples/device_node`)

This generator was brought in from a different, larger (RPC/cloud-connected,
multi-module) project template. Only one module is actually wired up here:

- **`lorawan`** (`config/settings_lorawan.yaml`) — OTAA credentials + region
  for `src/main.c`, replacing what used to be compile-time constants. Not
  every field in the YAML is live: `sub_band`/`adr_enabled`/`data_rate`/
  `tx_power`/`confirmed_uplink`/`uplink_interval_sec` are commented out
  because `lora_e5.h` has no public setter for any of them yet
  (`CLAUDE.md`'s "Known gaps" #2) — re-enable once the library grows real
  setters, not before (an NVS-backed field an application can "set" that
  silently does nothing is worse than not having it).
- **RPC dispatch is deliberately not used.** `CMakeLists.txt` here does not
  run the `--aggregate` pass and does not add `rpc_settings_lorawan.c` to
  `target_sources()` (`rpc_helpers.h`, which that file needs, doesn't exist
  in this project) — this sample has one settings module and no RPC layer,
  so that machinery would be unused complexity. The generator still writes
  `rpc_settings_lorawan.{c,h}` to `src/autogen-code/`; they just aren't
  compiled.
- **Shell commands are off by default** (`CONFIG_NVS_SETTINGS_LORAWAN_SHELL`
  needs `CONFIG_SHELL=y`, not set in `prj.conf`) — this sample races through
  its wake→join→send→sleep sequence in ~1-3s with no interactive window, so
  a shell isn't useful yet without also adding a way to pause that race
  (e.g. a boot-button-held provisioning mode). Storage is NVS-backed now;
  field reprovisioning without a reflash is a separate, not-yet-done step.
- **`gen_settings_module.py` was patched** (see its own header comment)
  to `LOG_MODULE_REGISTER()` each settings module's own log module instead
  of `LOG_MODULE_DECLARE(main, ...)` — the original template assumed the
  consuming app's log module is literally named `main`, which isn't true
  here (`lora_e5_device_node`) and would have been a link failure.
- Two real Kconfig/NVS gotchas hit while wiring this up, with fixes
  documented in `prj.conf` itself and in `docs/VERIFICATION_NEEDED.md`'s
  "Resolved 2026-07-12" section: `CONFIG_SETTINGS_NVS` silently falls back
  to no backend at all without also `CONFIG_FLASH=y` + `CONFIG_FLASH_MAP=y`
  (undocumented by the "trio" of `NVS`/`SETTINGS`/`SETTINGS_NVS` alone), and
  a real ESP32-S3 board's flash isn't guaranteed to already be a valid NVS
  filesystem the first time you point `CONFIG_SETTINGS_NVS` at it
  (`CONFIG_NVS_INIT_BAD_MEMORY_REGION=y` self-heals that).
