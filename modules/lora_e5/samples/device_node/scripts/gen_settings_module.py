#!/usr/bin/env python3
"""
gen_settings_module.py — Zephyr NVS Settings Module Generator
==============================================================

PRIMARY USAGE — driven by a YAML definition file:

    # Scaffold a new YAML (then edit it):
    python3 scripts/gen_settings_module.py --init hwinfo

    # Generate C/H files from the YAML:
    python3 scripts/gen_settings_module.py --yaml src/settings_hwinfo.yaml

    # OR let the script auto-discover src/settings_<module>.yaml:
    python3 scripts/gen_settings_module.py --module hwinfo

FALLBACK USAGE — inline entries on the command line:

    python3 scripts/gen_settings_module.py \\
        --module hwinfo \\
        --out-dir src \\
        --entries "STR:hw_version_t:hw_version:hw_version:32" \\
                  "INT:int32_t:sensor_count:sensor_count" \\
                  "BOOL:bool:debug_enabled:debug_enabled" \\
                  "FLOAT:float:calib_factor:calib_factor" \\
                  "DOUBLE:double:precision_value:precision_value"

FALLBACK USAGE — fully interactive (no args):

    python3 scripts/gen_settings_module.py

─────────────────────────────────────────────────────────────────────────────
YAML FORMAT  (settings_<module>.yaml)
─────────────────────────────────────────────────────────────────────────────

module: hwinfo           # NVS namespace & file prefix
description: "HW info settings" # (optional) human-readable description
out_dir: src                    # (optional) output directory, default: src
shell: true                     # (optional) generate per-module shell commands, default: true

entries:
  - kind: STR                   # STR | INT | BOOL | FLOAT | DOUBLE
    type: hw_version_t          # C type (for STR, also used as typedef name)
    name_var: hw_version        # struct field name
    str_key: hw_version         # NVS key segment (also shell sub-command)
    str_len: 32                 # (STR only) buffer size incl. NUL, default 64

  - kind: STR
    type: hw_serial_t
    name_var: serial_number
    str_key: serial_number
    str_len: 64

  - kind: INT
    type: int32_t               # any integer type
    name_var: sensor_count
    str_key: sensor_count

  - kind: BOOL
    # type defaults to 'bool', may be omitted
    name_var: debug_enabled
    str_key: debug_enabled

  - kind: FLOAT
    # type defaults to 'float', may be omitted
    name_var: calibration_factor
    str_key: calib_factor

  - kind: DOUBLE
    # type defaults to 'double', may be omitted
    name_var: precision_value
    str_key: precision_value

─────────────────────────────────────────────────────────────────────────────
"""

import argparse
import os
import sys
from dataclasses import dataclass, field
from typing import List, Optional

# ─────────────────────────────────────────────────────────────────────────────
# YAML import (stdlib fallback hint)
# ─────────────────────────────────────────────────────────────────────────────

try:
    import yaml as _yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False

# ─────────────────────────────────────────────────────────────────────────────
# Data model
# ─────────────────────────────────────────────────────────────────────────────

SUPPORTED_KINDS = ("STR", "INT", "BOOL", "FLOAT", "DOUBLE")

KIND_DEFAULTS = {
    "STR":    "char_buf_t",
    "INT":    "int32_t",
    "BOOL":   "bool",
    "FLOAT":  "float",
    "DOUBLE": "double",
}


@dataclass
class Entry:
    kind: str               # STR | INT | BOOL | FLOAT | DOUBLE
    ctype: str              # C type
    name_var: str           # struct field name
    str_key: str            # NVS key segment
    str_len: int = 64       # STR only
    sensitive: bool = False # mask value in get_setting responses
    readonly:  bool = False # disallow writes via set_setting


# Per-module Kconfig symbol  e.g. NVS_SETTINGS_WIFI_SHELL
def shell_symbol(module: str) -> str:
    return f"NVS_SETTINGS_{module.upper()}_SHELL"

def kconfig_file(module: str) -> str:
    return f"Kconfig.settings_{module}"


# ─────────────────────────────────────────────────────────────────────────────
# Naming helpers
# ─────────────────────────────────────────────────────────────────────────────

def P(module: str) -> str:          return f"NVS_{module.upper()}"
def guard(module: str) -> str:      return f"SETTINGS_{module.upper()}_H"
def cfg(module: str) -> str:        return f"{module}_config"
def sname(module: str) -> str:      return f"{module}_settings"
def handler(module: str) -> str:    return f"{module}_settings_handler"
def init_fn(module: str) -> str:    return f"settings_{module}_init"
def list_hdr(module: str) -> str:   return f"settings_{module}_list.h"
def hdr_file(module: str) -> str:   return f"settings_{module}.h"
def src_file(module: str) -> str:   return f"settings_{module}.c"
def yaml_file(module: str) -> str:  return f"settings_{module}.yaml"


ALL_KINDS = ["STR", "INT", "BOOL", "FLOAT", "DOUBLE"]


def undef_all(module: str) -> str:
    return "\n".join(f"#undef {P(module)}_{k}_ADD" for k in ALL_KINDS)


# ─────────────────────────────────────────────────────────────────────────────
# YAML scaffold generator
# ─────────────────────────────────────────────────────────────────────────────

YAML_TEMPLATE = """\
# settings_{module}.yaml — NVS Settings definition for module '{module}'
# ──────────────────────────────────────────────────────────────────────
# Run:  python3 scripts/gen_settings_module.py --yaml src/settings_{module}.yaml
#       to regenerate settings_{module}.c / .h / _list.h / Kconfig.settings_{module}
#
# Supported kinds:  STR | INT | BOOL | FLOAT | DOUBLE
# ──────────────────────────────────────────────────────────────────────

module: {module}
description: "{module_title} Settings"
out_dir: src                    # directory where .c/.h/_list.h are written
shell: true                     # set to false to exclude shell commands for this module

entries:
  # ── String entries ──────────────────────────────────────────────────
  - kind: STR
    type: {module}_version_t    # typedef name (a char array alias)
    name_var: version           # struct field name
    str_key: version            # NVS key & shell sub-command
    str_len: 32                 # buffer size incl. NUL terminator

  # ── Integer entries ─────────────────────────────────────────────────
  - kind: INT
    type: int32_t               # any C integer type
    name_var: count
    str_key: count

  # ── Boolean entries ─────────────────────────────────────────────────
  - kind: BOOL
    # type defaults to 'bool' when omitted
    name_var: enabled
    str_key: enabled

  # ── Float entries ───────────────────────────────────────────────────
  - kind: FLOAT
    # type defaults to 'float' when omitted
    name_var: scale_factor
    str_key: scale_factor

  # ── Double entries ──────────────────────────────────────────────────
  - kind: DOUBLE
    # type defaults to 'double' when omitted
    name_var: precision
    str_key: precision
"""


# ─────────────────────────────────────────────────────────────────────────────
# YAML loader
# ─────────────────────────────────────────────────────────────────────────────

def load_yaml(path: str):
    """Parse a settings YAML and return (module, out_dir, shell_enabled, entries)."""
    if not HAS_YAML:
        print("ERROR: PyYAML is not installed. Run:  pip install pyyaml", file=sys.stderr)
        sys.exit(1)
    with open(path) as f:
        data = _yaml.safe_load(f)

    module = data.get("module", "").strip().lower().replace(" ", "_")
    if not module:
        print(f"ERROR: 'module' key missing in {path}", file=sys.stderr)
        sys.exit(1)

    out_dir = data.get("out_dir", "src")
    shell_enabled = bool(data.get("shell", True))   # default: shell commands ON
    raw_entries = data.get("entries", [])

    entries: List[Entry] = []
    for idx, e in enumerate(raw_entries, 1):
        kind = str(e.get("kind", "")).strip().upper()
        if kind not in SUPPORTED_KINDS:
            print(f"ERROR: entry #{idx}: unknown kind '{kind}'. "
                  f"Must be one of {SUPPORTED_KINDS}", file=sys.stderr)
            sys.exit(1)

        ctype     = str(e.get("type", KIND_DEFAULTS[kind])).strip()
        name_var  = str(e.get("name_var", "")).strip()
        str_key   = str(e.get("str_key",  name_var)).strip()
        str_len   = int(e.get("str_len", 64))
        sensitive = bool(e.get("sensitive", False))
        readonly  = bool(e.get("readonly",  False))

        if not name_var:
            print(f"ERROR: entry #{idx}: 'name_var' is required", file=sys.stderr)
            sys.exit(1)

        entries.append(Entry(kind, ctype, name_var, str_key, str_len,
                             sensitive=sensitive, readonly=readonly))

    return module, out_dir, shell_enabled, entries


# ─────────────────────────────────────────────────────────────────────────────
# Code generators
# ─────────────────────────────────────────────────────────────────────────────

def gen_list_h(module: str, entries: List[Entry]) -> str:
    lines = [
        f"/* {list_hdr(module)} — AUTO-GENERATED from {yaml_file(module)} */",
        f"/* Edit the YAML, then re-run gen_settings_module.py to update. */",
        "",
    ]
    for e in entries:
        macro = f"{P(module)}_{e.kind}_ADD"
        lines.append(f"{macro}({e.ctype}, {e.name_var}, {e.str_key})")
    lines.append("")
    return "\n".join(lines)


def gen_h(module: str, entries: List[Entry]) -> str:
    # Typedef declarations for STR entries
    typedef_lines, seen = [], set()
    for e in entries:
        if e.kind == "STR" and e.ctype not in seen:
            seen.add(e.ctype)
            typedef_lines.append(f"typedef char {e.ctype}[{e.str_len}];")
    typedef_block = ("\n".join(typedef_lines) + "\n\n") if typedef_lines else ""

    def struct_macros():
        return "\n".join(
            f"#define {P(module)}_{k}_ADD(type, name_var, str_key)  type name_var;"
            for k in ALL_KINDS
        )

    def getter_setter_macros():
        lines = []
        lines += [
            f"#define {P(module)}_STR_ADD(type, name_var, str_key) \\",
            f"\tconst char* get_nvs_##type##_##str_key(void); \\",
            f"\tint set_nvs_##type##_##str_key(const char *val);",
        ]
        for k in ("INT", "FLOAT", "DOUBLE"):
            lines += [
                f"#define {P(module)}_{k}_ADD(type, name_var, str_key) \\",
                f"\ttype get_nvs_##type##_##str_key(void); \\",
                f"\tint set_nvs_##type##_##str_key(type val);",
            ]
        lines += [
            f"#define {P(module)}_BOOL_ADD(type, name_var, str_key) \\",
            f"\tbool get_nvs_##type##_##str_key(void); \\",
            f"\tint set_nvs_##type##_##str_key(bool val);",
        ]
        return "\n".join(lines)

    listh = list_hdr(module)
    return f"""\
#ifndef {guard(module)}
#define {guard(module)}

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

/* ── String type aliases (buffer sizes include NUL terminator) ─────────── */
{typedef_block}\
/* ── Settings struct ───────────────────────────────────────────────────── */
struct {sname(module)} {{
\tstruct k_mutex lock;
{struct_macros()}
#include "{listh}"
{undef_all(module)}
}};

extern struct {sname(module)} {cfg(module)};
void {init_fn(module)}(void);

/* ── Auto-generated getter / setter declarations ───────────────────────── */
{getter_setter_macros()}
#include "{listh}"
{undef_all(module)}

#endif /* {guard(module)} */
"""


# ─────────────────────────────────────────────────────────────────────────────
# Kconfig fragment generator
# ─────────────────────────────────────────────────────────────────────────────

def gen_kconfig(module: str, description: str, shell_enabled: bool) -> str:
    sym  = shell_symbol(module)
    title = module.replace("_", " ").title()
    default = "y" if shell_enabled else "n"
    return f"""\
# {kconfig_file(module)} — AUTO-GENERATED from {yaml_file(module)}
# Source this file from your root Kconfig:
#   rsource "{kconfig_file(module)}"

config {sym}
\tbool "Enable shell commands for the '{module}' settings module"
\tdefault {default}
\tdepends on SHELL
\thelp
\t  When enabled, adds '{module} <key> <value>' and '{module} show'
\t  shell commands to inspect and modify {title} settings at runtime.
\t  Disable to save flash if the {module} shell interface is not needed.
"""


def gen_c(module: str, entries: List[Entry], shell_enabled: bool = True) -> str:
    listh = list_hdr(module)
    hdr   = hdr_file(module)

    def default_macros():
        return "\n".join([
            f"#define {P(module)}_STR_ADD(type, name_var, str_key)    .name_var = {{0}},",
            f"#define {P(module)}_INT_ADD(type, name_var, str_key)    .name_var = 0,",
            f"#define {P(module)}_BOOL_ADD(type, name_var, str_key)   .name_var = false,",
            f"#define {P(module)}_FLOAT_ADD(type, name_var, str_key)  .name_var = 0.0f,",
            f"#define {P(module)}_DOUBLE_ADD(type, name_var, str_key) .name_var = 0.0,",
        ])

    def getset_macros():
        c = cfg(module)
        lines = []
        # STR
        lines += [
            f"#define {P(module)}_STR_ADD(type, name_var, str_key) \\",
            f"\tconst char* get_nvs_##type##_##str_key(void) {{ \\",
            f"\t\treturn {c}.name_var; \\",
            f"\t}} \\",
            f"\tint set_nvs_##type##_##str_key(const char *val) {{ \\",
            f"\t\tstrncpy({c}.name_var, val, sizeof({c}.name_var) - 1); \\",
            f"\t\t{c}.name_var[sizeof({c}.name_var) - 1] = '\\0'; \\",
            f'\t\treturn settings_save_one("{module}/" #str_key, {c}.name_var, strlen({c}.name_var)); \\',
            f"\t}}",
        ]
        # INT / BOOL / FLOAT / DOUBLE
        specs = [
            ("INT",    "type",  "type get_nvs_##type##_##str_key(void)"),
            ("BOOL",   "bool",  "bool get_nvs_##type##_##str_key(void)"),
            ("FLOAT",  "type",  "type get_nvs_##type##_##str_key(void)"),
            ("DOUBLE", "type",  "type get_nvs_##type##_##str_key(void)"),
        ]
        for k, _getter_type, getter_sig in specs:
            setter_arg = "bool val" if k == "BOOL" else "type val"
            lines += [
                f"#define {P(module)}_{k}_ADD(type, name_var, str_key) \\",
                f"\t{getter_sig} {{ \\",
                f"\t\tk_mutex_lock(&{c}.lock, K_FOREVER); \\",
                f"\t\ttype val = {c}.name_var; \\",
                f"\t\tk_mutex_unlock(&{c}.lock); \\",
                f"\t\treturn val; \\",
                f"\t}} \\",
                f"\tint set_nvs_##type##_##str_key({setter_arg}) {{ \\",
                f"\t\tk_mutex_lock(&{c}.lock, K_FOREVER); \\",
                f"\t\t{c}.name_var = val; \\",
                f'\t\tint rc = settings_save_one("{module}/" #str_key, &{c}.name_var, sizeof({c}.name_var)); \\',
                f"\t\tk_mutex_unlock(&{c}.lock); \\",
                f"\t\treturn rc; \\",
                f"\t}}",
            ]
        return "\n".join(lines)

    def loader_macros():
        c = cfg(module)
        lines = []
        # STR
        lines += [
            f"#define {P(module)}_STR_ADD(type, name_var, str_key) \\",
            f"\tif (settings_name_steq(name, #str_key, &next) && !next) {{ \\",
            f"\t\tif (len != 0) {{ \\",
            f"\t\t\tk_mutex_lock(&{c}.lock, K_FOREVER); \\",
            f"\t\t\tint rc = read_cb(cb_arg, &{c}.name_var, sizeof({c}.name_var)); \\",
            f"\t\t\tif (rc >= 0) {{ \\",
            f"\t\t\t\t((char *)&{c}.name_var)[sizeof({c}.name_var) - 1] = '\\0'; \\",
            f"\t\t\t}} \\",
            f"\t\t\tk_mutex_unlock(&{c}.lock); \\",
            f"\t\t}} \\",
            f"\t\treturn 0; \\",
            f"\t}}",
        ]
        for k in ("INT", "BOOL", "FLOAT", "DOUBLE"):
            lines += [
                f"#define {P(module)}_{k}_ADD(type, name_var, str_key) \\",
                f"\tif (settings_name_steq(name, #str_key, &next) && !next) {{ \\",
                f"\t\tif (len != 0) {{ \\",
                f"\t\t\tk_mutex_lock(&{c}.lock, K_FOREVER); \\",
                f"\t\t\tread_cb(cb_arg, &{c}.name_var, sizeof({c}.name_var)); \\",
                f"\t\t\tk_mutex_unlock(&{c}.lock); \\",
                f"\t\t}} \\",
                f"\t\treturn 0; \\",
                f"\t}}",
            ]
        return "\n".join(lines)

    def shell_cmd_macros():
        lines = []
        # We handle validation inside the macro expansion to keep the generated code safe.
        specs = {
            "STR":    ('<value>',              'set_nvs_##type##_##str_key(argv[1])',      '%s',  "(char*)get_nvs_##type##_##str_key()"),
            "INT":    ('<integer>',            '',                                          '%lld',  "(long long)get_nvs_##type##_##str_key()"),
            "BOOL":   ('<1|0 or true|false>',  '',                                          '%s',  'get_nvs_##type##_##str_key()?"true":"false"'),
            "FLOAT":  ('<float>',              '',                                          '%f',  "(double)get_nvs_##type##_##str_key()"),
            "DOUBLE": ('<double>',             '',                                          '%f',  "(double)get_nvs_##type##_##str_key()"),
        }
        for k, (hint, _legacy_call, fmt, print_expr) in specs.items():
            lines += [f"#define {P(module)}_{k}_ADD(type, name_var, str_key) \\"]
            lines += [f"static int cmd_{module}_##str_key(const struct shell *sh, size_t argc, char **argv) \\"]
            lines += [f"{{ \\"]
            lines += [f'\tif (argc < 2) {{ shell_error(sh, "Usage: {module} " #str_key " {hint}"); return -EINVAL; }} \\']
            
            if k == "STR":
                lines += [f"\tset_nvs_##type##_##str_key(argv[1]); \\"]
            elif k in ("FLOAT", "DOUBLE"):
                lines += [
                    f"\tchar *endptr; \\",
                    f"\tdouble d = strtod(argv[1], &endptr); \\",
                    f"\tif (*endptr != '\\0') {{ shell_error(sh, \"Invalid number: %s\", argv[1]); return -EINVAL; }} \\",
                    f"\tset_nvs_##type##_##str_key(({KIND_DEFAULTS[k]})d); \\"
                ]
            elif k == "BOOL":
                lines += [
                    f"\tbool b; \\",
                    f"\tif (strcmp(argv[1],\"true\")==0||strcmp(argv[1],\"1\")==0) b = true; \\",
                    f"\telse if (strcmp(argv[1],\"false\")==0||strcmp(argv[1],\"0\")==0) b = false; \\",
                    f"\telse {{ shell_error(sh, \"Invalid bool: %s. Use true/false/1/0\", argv[1]); return -EINVAL; }} \\",
                    f"\tset_nvs_##type##_##str_key(b); \\"
                ]
            else: # INT
                lines += [
                    f"\tchar *endptr; \\",
                    f"\tlong long ll = strtoll(argv[1], &endptr, 0); \\",
                    f"\tif (*endptr != '\\0') {{ shell_error(sh, \"Invalid integer: %s\", argv[1]); return -EINVAL; }} \\",
                    f"\tset_nvs_##type##_##str_key((type)ll); \\"
                ]

            lines += [f'\tshell_print(sh, "Saved " #str_key " [{k}]: {fmt}", {print_expr}); \\']
            lines += [f"\treturn 0; \\"]
            lines += [f"}}"]
        return "\n".join(lines)

    def show_macros():
        lines = []
        specs = {
            "STR":    ('\'%s\'', 'strlen(get_nvs_##type##_##str_key())>0?get_nvs_##type##_##str_key():"<not set>"'),
            "INT":    ('%lld',     '(long long)get_nvs_##type##_##str_key()'),
            "BOOL":   ('%s',     'get_nvs_##type##_##str_key()?"true":"false"'),
            "FLOAT":  ('%f',     '(double)get_nvs_##type##_##str_key()'),
            "DOUBLE": ('%f',     '(double)get_nvs_##type##_##str_key()'),
        }
        for k, (fmt, expr) in specs.items():
            lines += [
                f"#define {P(module)}_{k}_ADD(type, name_var, str_key) \\",
                f'\tshell_print(sh, "  " #str_key " [{k}]: {fmt}", {expr});',
            ]
        return "\n".join(lines)

    def subcmd_macros():
        return "\n".join(
            f"#define {P(module)}_{k}_ADD(type, name_var, str_key) \\\n"
            f'\tSHELL_CMD_ARG(str_key, NULL, "Set " #str_key, cmd_{module}_##str_key, 2, 0),'
            for k in ALL_KINDS
        )

    title = module.replace("_", " ").title()

    sym = shell_symbol(module)

    return f"""\
/* {src_file(module)} — AUTO-GENERATED from {yaml_file(module)} */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "{hdr}"

#include <zephyr/logging/log.h>
/* Registers its own log module rather than LOG_MODULE_DECLARE(main, ...)
 * -- the previous version of this template assumed every consuming app
 * names its own log module "main", which is not a safe assumption for
 * a generic, reusable generator (breaks the link for any app that
 * registers its own module under a different name, e.g.
 * LOG_MODULE_REGISTER(lora_e5_device_node, ...)). Self-registering
 * avoids the assumption entirely. */
LOG_MODULE_REGISTER({module}, LOG_LEVEL_INF);

/* ── Default-initialised config instance ─────────────────────────────────── */
struct {sname(module)} {cfg(module)} = {{
{default_macros()}
#include "{listh}"
{undef_all(module)}
}};

/* ── Getter / Setter implementations ────────────────────────────────────── */
{getset_macros()}
#include "{listh}"
{undef_all(module)}

/* ── Settings load callback (h_set) ─────────────────────────────────────── */
static int {module}_settings_set(const char *name, size_t len,
                                  settings_read_cb read_cb, void *cb_arg)
{{
\tconst char *next;

{loader_macros()}
#include "{listh}"
{undef_all(module)}

\tLOG_WRN("Ignoring unknown/deprecated {module} key: %s", name);
\treturn 0;
}}

static struct settings_handler {handler(module)} = {{
\t.name = "{module}",
\t.h_set = {module}_settings_set,
}};

/* ── Shell commands — controlled by CONFIG_{sym} ──── */
#ifdef CONFIG_{sym}

{shell_cmd_macros()}
#include "{listh}"
{undef_all(module)}

static int cmd_{module}_show(const struct shell *sh, size_t argc, char **argv)
{{
\tshell_print(sh, "Current {module} settings:");
{show_macros()}
#include "{listh}"
{undef_all(module)}
\treturn 0;
}}

static const struct shell_static_entry shell_sub_{module}[] = {{
{subcmd_macros()}
#include "{listh}"
{undef_all(module)}
\tSHELL_CMD_ARG(show, NULL, "Show all {module} settings", cmd_{module}_show, 1, 0),
\tSHELL_SUBCMD_SET_END
}};

static const union shell_cmd_entry sub_{module} = {{
\t.entry = shell_sub_{module}
}};

SHELL_CMD_REGISTER({module}, &sub_{module}, "{title} Configuration Commands", NULL);

#endif /* CONFIG_{sym} */

/* ── Init ────────────────────────────────────────────────────────────────── */
void {init_fn(module)}(void)
{{
\tk_mutex_init(&{cfg(module)}.lock);
\tsettings_register(&{handler(module)});
}}
"""


# ─────────────────────────────────────────────────────────────────────────────
# Interactive prompt (legacy fallback)
# ─────────────────────────────────────────────────────────────────────────────

def prompt_entries() -> List[Entry]:
    print(f"\nAdd settings entries (blank KIND to finish).")
    print(f"  Kinds: {', '.join(SUPPORTED_KINDS)}\n")
    entries: List[Entry] = []
    while True:
        kind = input("  KIND: ").strip().upper()
        if not kind:
            break
        if kind not in SUPPORTED_KINDS:
            print(f"  ✗ Unknown kind '{kind}'.")
            continue
        default_type = KIND_DEFAULTS[kind]
        ctype = (input(f"  C type (default '{default_type}'): ").strip() or default_type)
        name_var = input("  Field name: ").strip()
        str_key = input(f"  NVS key   (default '{name_var}'): ").strip() or name_var
        str_len = 64
        if kind == "STR":
            try:
                str_len = int(input("  Buffer size incl. NUL (default 64): ").strip() or "64")
            except ValueError:
                str_len = 64
        entries.append(Entry(kind, ctype, name_var, str_key, str_len))
        print(f"  ✓ Added {kind}({ctype}, {name_var}, {str_key})\n")
    return entries


def parse_entry_arg(raw: str) -> Entry:
    parts = raw.split(":")
    if len(parts) < 4:
        raise ValueError(f"Entry '{raw}' needs at least 4 colon-separated fields")
    kind = parts[0].upper()
    if kind not in SUPPORTED_KINDS:
        raise ValueError(f"Unknown KIND '{kind}'")
    ctype = parts[1] or KIND_DEFAULTS[kind]
    name_var = parts[2]
    str_key = parts[3]
    str_len = int(parts[4]) if len(parts) >= 5 else 64
    return Entry(kind, ctype, name_var, str_key, str_len)


# ─────────────────────────────────────────────────────────────────────────────
# RPC settings generators
# ─────────────────────────────────────────────────────────────────────────────

def rpc_hdr_file(module: str) -> str:  return f"rpc_settings_{module}.h"
def rpc_src_file(module: str) -> str:  return f"rpc_settings_{module}.c"
def rpc_guard(module: str) -> str:     return f"RPC_SETTINGS_{module.upper()}_H"
def rpc_get_fn(module: str) -> str:    return f"rpc_{module}_get"
def rpc_set_fn(module: str) -> str:    return f"rpc_{module}_set"


def _snprintf_ok(module: str, key: str, kind: str, value_fmt: str, *c_args: str) -> str:
    """Return C snprintf call for a successful get_setting response."""
    json = (f'{{"ok":true,"module":"{module}","key":"{key}",'
            f'"kind":"{kind}","value":{value_fmt}}}')
    c_lit = '"' + json.replace('"', '\\"') + '"'
    args  = (',\n\t\t\t' + ',\n\t\t\t'.join(c_args)) if c_args else ''
    return f'return snprintf(buf, buf_size,\n\t\t\t{c_lit}{args});'


def _snprintf_err(template: str, *c_args: str) -> str:
    """Return C snprintf call for an error response."""
    json  = f'{{"ok":false,"error":"{template}"}}'
    c_lit = '"' + json.replace('"', '\\"') + '"'
    args  = (',\n\t\t\t' + ',\n\t\t\t'.join(c_args)) if c_args else ''
    return f'return snprintf(buf, buf_size,\n\t\t\t{c_lit}{args});'


def gen_rpc_h(module: str) -> str:
    g     = rpc_guard(module)
    get_f = rpc_get_fn(module)
    set_f = rpc_set_fn(module)
    yaml  = yaml_file(module)
    return f"""\
/* {rpc_hdr_file(module)} — AUTO-GENERATED from {yaml} */
/* Edit the YAML, then re-run gen_settings_module.py to update.   */

#ifndef {g}
#define {g}

#include <stddef.h>

/* Returns bytes written to buf (snprintf semantics, no truncation check). */
int {get_f}(const char *key, char *buf, size_t buf_size);
int {set_f}(const char *key, const char *params, char *buf, size_t buf_size);

#endif /* {g} */
"""


def gen_rpc_c(module: str, entries: List[Entry]) -> str:
    get_f  = rpc_get_fn(module)
    set_f  = rpc_set_fn(module)
    yaml   = yaml_file(module)
    mod_h  = hdr_file(module)
    rpc_h  = rpc_hdr_file(module)

    # ── get function body ────────────────────────────────────────────────────
    get_cases: List[str] = []
    for e in entries:
        getter = f"get_nvs_{e.ctype}_{e.str_key}"

        if e.sensitive:
            body = '\t\t' + _snprintf_ok(module, e.str_key, e.kind, '"****"')
        elif e.kind == "STR":
            body = (f'\t\tconst char *v = {getter}();\n'
                    '\t\t' + _snprintf_ok(module, e.str_key, "STR", '"%s"', 'v ? v : ""'))
        elif e.kind == "INT":
            body = '\t\t' + _snprintf_ok(module, e.str_key, "INT", '%lld',
                                          f'(long long){getter}()')
        elif e.kind == "BOOL":
            body = '\t\t' + _snprintf_ok(module, e.str_key, "BOOL", '%s',
                                          f'{getter}() ? "true" : "false"')
        elif e.kind == "FLOAT":
            body = '\t\t' + _snprintf_ok(module, e.str_key, "FLOAT", '%.6g',
                                          f'(double){getter}()')
        elif e.kind == "DOUBLE":
            body = '\t\t' + _snprintf_ok(module, e.str_key, "DOUBLE", '%.6g',
                                          f'{getter}()')
        else:
            continue

        get_cases.append(f'\tif (strcmp(key, "{e.str_key}") == 0) {{\n{body}\n\t}}')

    get_chain = '\n\t else '.join(get_cases) if get_cases else ''
    get_fallback = '\t' + _snprintf_err(f'Unknown key: {module}.%s', 'key')

    # ── set function body ────────────────────────────────────────────────────
    set_cases: List[str] = []
    for e in entries:
        setter = f"set_nvs_{e.ctype}_{e.str_key}"

        if e.readonly:
            body = ('\t\t' +
                    _snprintf_err(f'Key {e.str_key} is read-only'))
            set_cases.append(
                f'\tif (strcmp(key, "{e.str_key}") == 0) {{\n{body}\n\t}}')
            continue

        if e.kind == "STR":
            body = (f'\t\tchar sval[{e.str_len}] = {{0}};\n'
                    f'\t\tif (rpc_str(params, "value", sval, sizeof(sval)) == 0) {{\n'
                    f'\t\t\tret = {setter}(sval);\n'
                    f'\t\t}} else {{ ret = -EINVAL; }}')
        elif e.kind == "INT":
            body = (f'\t\tint32_t ival;\n'
                    f'\t\tif (rpc_int(params, "value", &ival) == 0) {{\n'
                    f'\t\t\tret = {setter}(({e.ctype})ival);\n'
                    f'\t\t}} else {{ ret = -EINVAL; }}')
        elif e.kind == "BOOL":
            body = (f'\t\tbool bval;\n'
                    f'\t\tif (rpc_bool(params, "value", &bval) == 0) {{\n'
                    f'\t\t\tret = {setter}(bval);\n'
                    f'\t\t}} else {{ ret = -EINVAL; }}')
        elif e.kind == "FLOAT":
            body = (f'\t\tfloat fval;\n'
                    f'\t\tif (rpc_float(params, "value", &fval) == 0) {{\n'
                    f'\t\t\tret = {setter}(fval);\n'
                    f'\t\t}} else {{ ret = -EINVAL; }}')
        elif e.kind == "DOUBLE":
            body = (f'\t\tdouble dval;\n'
                    f'\t\tif (rpc_double(params, "value", &dval) == 0) {{\n'
                    f'\t\t\tret = {setter}(dval);\n'
                    f'\t\t}} else {{ ret = -EINVAL; }}')
        else:
            continue

        set_cases.append(f'\tif (strcmp(key, "{e.str_key}") == 0) {{\n{body}\n\t}}')

    set_chain = '\n\t else '.join(set_cases) if set_cases else ''

    set_ok      = '\t\t' + _snprintf_ok(module, '%s', '', 'true').replace(
                      '"kind":"","value":true',
                      '"saved":true').replace(',"key":"%s"', ',"key":"%s"')
    # Build set responses manually for clarity
    set_ok_resp = (
        '\t\t' + f'return snprintf(buf, buf_size,\n\t\t\t'
        + '"' + f'{{"ok":true,"module":"{module}","key":"%s","saved":true}}'.replace('"', '\\"') + '"'
        + ',\n\t\t\tkey);'
    )
    set_inv_resp = (
        '\t\t' + f'return snprintf(buf, buf_size,\n\t\t\t'
        + '"' + f'{{"ok":false,"error":"Invalid value for {module}.%s"}}'.replace('"', '\\"') + '"'
        + ',\n\t\t\tkey);'
    )
    set_unk_resp = (
        '\t\t' + f'return snprintf(buf, buf_size,\n\t\t\t'
        + '"' + f'{{"ok":false,"error":"Unknown key: {module}.%s"}}'.replace('"', '\\"') + '"'
        + ',\n\t\t\tkey);'
    )
    set_nvs_resp = (
        '\t' + f'return snprintf(buf, buf_size,\n\t\t'
        + '"' + f'{{"ok":false,"error":"NVS write failed: %d"}}'.replace('"', '\\"') + '"'
        + ',\n\t\tret);'
    )

    return f"""\
/* {rpc_src_file(module)} — AUTO-GENERATED from {yaml} */
/* Edit the YAML, then re-run gen_settings_module.py to update.   */

#include "rpc_helpers.h"
#include "{mod_h}"
#include "{rpc_h}"

#include <string.h>
#include <stdio.h>
#include <errno.h>

int {get_f}(const char *key, char *buf, size_t buf_size)
{{
{get_chain}
{get_fallback}
}}

int {set_f}(const char *key, const char *params, char *buf, size_t buf_size)
{{
\tint ret = -ENOENT;

{set_chain}

\tif (ret == 0) {{
{set_ok_resp}
\t}} else if (ret == -EINVAL) {{
{set_inv_resp}
\t}} else if (ret == -ENOENT) {{
{set_unk_resp}
\t}}
{set_nvs_resp}
}}
"""


# ─────────────────────────────────────────────────────────────────────────────
# File writer
# ─────────────────────────────────────────────────────────────────────────────

def write_files(module: str, out_dir: str, kconfig_dir: str, entries: List[Entry], force: bool,
                shell_enabled: bool = True, description: str = ""):
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(kconfig_dir, exist_ok=True)

    src_files = {
        list_hdr(module):    (out_dir, gen_list_h(module, entries)),
        hdr_file(module):    (out_dir, gen_h(module, entries)),
        src_file(module):    (out_dir, gen_c(module, entries, shell_enabled)),
        rpc_hdr_file(module):(out_dir, gen_rpc_h(module)),
        rpc_src_file(module):(out_dir, gen_rpc_c(module, entries)),
    }
    kconfig_name = kconfig_file(module)
    kconfig_content = gen_kconfig(module, description, shell_enabled)

    print()
    for fname, (dest, content) in src_files.items():
        path = os.path.join(dest, fname)
        if os.path.exists(path) and not force:
            ans = input(f"  '{path}' exists. Overwrite? [y/N] ").strip().lower()
            if ans != "y":
                print(f"  Skipped {fname}")
                continue
        with open(path, "w") as f:
            f.write(content)
        print(f"  ✓ Written: {path}")

    # Write Kconfig fragment
    kconfig_path = os.path.join(kconfig_dir, kconfig_name)
    if os.path.exists(kconfig_path) and not force:
        ans = input(f"  '{kconfig_path}' exists. Overwrite? [y/N] ").strip().lower()
        if ans == "y":
            with open(kconfig_path, "w") as f:
                f.write(kconfig_content)
            print(f"  ✓ Written: {kconfig_path}")
        else:
            print(f"  Skipped {kconfig_name}")
    else:
        with open(kconfig_path, "w") as f:
            f.write(kconfig_content)
        print(f"  ✓ Written: {kconfig_path}")

    sym = shell_symbol(module)
    print(f"""
Done! Next steps:
  1. Edit  {out_dir}/{list_hdr(module)}  (or the YAML) to tweak entries.
  2. Add to CMakeLists.txt:
       target_sources(app PRIVATE {out_dir}/{src_file(module)})
  3. Call  {init_fn(module)}()  from main() after settings_load().
  4. Source the generated Kconfig in your root Kconfig:
       rsource "{kconfig_dir}/{kconfig_name}"
  5. Tune shell commands per-module in prj.conf:
       CONFIG_{sym}=y                 # this module (default: {'y' if shell_enabled else 'n'})
""")


# ─────────────────────────────────────────────────────────────────────────────
# Aggregate generator — rpc_settings_all.c/.h + Kconfig.settings_all
# ─────────────────────────────────────────────────────────────────────────────

def gen_aggregate_h() -> str:
    return """\
/* rpc_settings_all.h — AUTO-GENERATED aggregator */
/* Re-run:  gen_settings_module.py --aggregate config/settings_*.yaml */

#ifndef RPC_SETTINGS_ALL_H
#define RPC_SETTINGS_ALL_H

#include <stddef.h>

/* Route get_setting to the correct per-module handler. */
int rpc_dispatch_module_get(const char *module, const char *key,
                             char *buf, size_t buf_size);

/* Route set_setting to the correct per-module handler. */
int rpc_dispatch_module_set(const char *module, const char *key,
                             const char *params, char *buf, size_t buf_size);

#endif /* RPC_SETTINGS_ALL_H */
"""


def gen_aggregate_c(modules: List[str]) -> str:
    includes = '\n'.join(f'#include "{rpc_hdr_file(m)}"' for m in modules)

    get_cases = [
        f'if (strcmp(module, "{m}") == 0) {{ return rpc_{m}_get(key, buf, buf_size); }}'
        for m in modules
    ]
    set_cases = [
        f'if (strcmp(module, "{m}") == 0) {{ return rpc_{m}_set(key, params, buf, buf_size); }}'
        for m in modules
    ]
    get_chain = '\t' + '\n\telse '.join(get_cases)
    set_chain = '\t' + '\n\telse '.join(set_cases)

    unk = '"' + '{"ok":false,"error":"Unknown module: %s"}'.replace('"', '\\"') + '"'

    return f"""\
/* rpc_settings_all.c — AUTO-GENERATED aggregator */
/* Re-run:  gen_settings_module.py --aggregate config/settings_*.yaml */

#include "rpc_settings_all.h"
{includes}

#include <stdio.h>
#include <string.h>

int rpc_dispatch_module_get(const char *module, const char *key,
                             char *buf, size_t buf_size)
{{
{get_chain}
\treturn snprintf(buf, buf_size, {unk}, module);
}}

int rpc_dispatch_module_set(const char *module, const char *key,
                             const char *params, char *buf, size_t buf_size)
{{
{set_chain}
\treturn snprintf(buf, buf_size, {unk}, module);
}}
"""


def gen_kconfig_all(modules: List[str]) -> str:
    lines = [
        '# Kconfig.settings_all — AUTO-GENERATED aggregator',
        '# Re-run:  gen_settings_module.py --aggregate config/settings_*.yaml',
        '',
    ] + [f'rsource "Kconfig.settings_{m}"' for m in modules]
    return '\n'.join(lines) + '\n'


def write_aggregate(modules: List[str], out_dir: str, kconfig_dir: str) -> None:
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(kconfig_dir, exist_ok=True)

    files = {
        os.path.join(out_dir,     "rpc_settings_all.h"):       gen_aggregate_h(),
        os.path.join(out_dir,     "rpc_settings_all.c"):       gen_aggregate_c(modules),
        os.path.join(kconfig_dir, "Kconfig.settings_all"):     gen_kconfig_all(modules),
    }
    for path, content in files.items():
        with open(path, "w") as f:
            f.write(content)
        print(f"  ✓ Written: {path}")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate Zephyr NVS settings module from a YAML file.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--yaml",    metavar="FILE",   help="Path to settings YAML file")
    parser.add_argument("--init",    metavar="MODULE",  help="Scaffold a new YAML for MODULE and exit")
    parser.add_argument("--module",  metavar="NAME",    help="Module name (auto-discovers config/settings_<name>.yaml)")
    parser.add_argument("--out-dir", default="src/autogen-code", help="Output directory for C/H (default: src/autogen-code)")
    parser.add_argument("--kconfig-dir", default="kconfig", help="Output directory for Kconfig (default: kconfig)")
    parser.add_argument("--entries", nargs="*",        metavar="KIND:type:name:key[:len]",
                        help="Inline entry specs (fallback when no YAML)")
    parser.add_argument("--force",   action="store_true", help="Overwrite existing files without prompting")
    parser.add_argument("--aggregate", nargs="+", metavar="YAML",
                        help="Aggregate mode: read module names from YAMLs and emit "
                             "rpc_settings_all.c/.h + Kconfig.settings_all")
    args = parser.parse_args()

    # ── --aggregate: emit rpc_settings_all.* and exit ───────────────────────
    if args.aggregate:
        if not HAS_YAML:
            print("ERROR: PyYAML required for --aggregate. Run: pip install pyyaml",
                  file=sys.stderr)
            sys.exit(1)
        modules = []
        for yaml_path in args.aggregate:
            mod, _, _, _ = load_yaml(yaml_path)
            modules.append(mod)
        print(f"\n  Aggregating {len(modules)} modules: {', '.join(modules)}")
        write_aggregate(modules, args.out_dir, args.kconfig_dir)
        return

    # ── --init: scaffold a YAML and exit ────────────────────────────────────
    if args.init:
        module = args.init.strip().lower().replace(" ", "_")
        out_dir = args.out_dir
        os.makedirs(out_dir, exist_ok=True)
        path = os.path.join(out_dir, yaml_file(module))
        if os.path.exists(path) and not args.force:
            ans = input(f"'{path}' already exists. Overwrite? [y/N] ").strip().lower()
            if ans != "y":
                print("Aborted.")
                return
        content = YAML_TEMPLATE.format(
            module=module,
            module_title=module.replace("_", " ").title(),
        )
        with open(path, "w") as f:
            f.write(content)
        print(f"✓ Scaffolded: {path}")
        print(f"  Edit it, then run:")
        print(f"    python3 scripts/gen_settings_module.py --yaml {path}")
        return

    # ── Determine source of entries ──────────────────────────────────────────
    module = None
    out_dir = args.out_dir
    entries: List[Entry] = []

    shell_enabled = True
    description   = ""

    # Priority 1: explicit --yaml
    if args.yaml:
        if not HAS_YAML:
            print("ERROR: PyYAML not installed. Run:  pip install pyyaml", file=sys.stderr)
            sys.exit(1)
        module, out_dir, shell_enabled, entries = load_yaml(args.yaml)

    # Priority 2: --module → auto-discover src/settings_<module>.yaml
    elif args.module:
        module = args.module.strip().lower().replace(" ", "_")
        auto = os.path.join(out_dir, yaml_file(module))
        if os.path.exists(auto) and HAS_YAML:
            print(f"  Auto-discovered YAML: {auto}")
            module, out_dir, shell_enabled, entries = load_yaml(auto)
        elif args.entries:
            entries = [parse_entry_arg(e) for e in args.entries]
        else:
            print(f"  No YAML found at '{auto}'. Entering interactive mode.\n")
            entries = prompt_entries()

    # Priority 3: inline --entries without --module
    elif args.entries:
        print("ERROR: --entries requires --module", file=sys.stderr)
        sys.exit(1)

    # Priority 4: fully interactive
    else:
        print("=== Zephyr NVS Settings Module Generator ===\n")
        print("  Tip: run with --init <module> to scaffold a settings YAML first.\n")
        module = input("Module name (e.g. hwinfo): ").strip().lower().replace(" ", "_")
        if not module:
            print("Aborted — module name is empty.", file=sys.stderr)
            sys.exit(1)
        auto = os.path.join(out_dir, yaml_file(module))
        if os.path.exists(auto) and HAS_YAML:
            ans = input(f"  Found '{auto}'. Load it? [Y/n] ").strip().lower()
            if ans != "n":
                module, out_dir, shell_enabled, entries = load_yaml(auto)
        if not entries:
            entries = prompt_entries()

    if not entries:
        print("No entries defined — aborting.", file=sys.stderr)
        sys.exit(1)

    write_files(module, out_dir, args.kconfig_dir, entries, force=args.force,
                shell_enabled=shell_enabled, description=description)


if __name__ == "__main__":
    main()
