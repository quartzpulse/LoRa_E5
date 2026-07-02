#!/usr/bin/env bash
#
# scaffold_lora_e5.sh
#
# Un-flattens a folder of individually-downloaded chat deliverables
# into the modules/lora_e5/ directory tree. See the rename table in
# chat before running this -- several source files share a basename
# (main.c, CMakeLists.txt, prj.conf, testcase.yaml across two test
# suites; lora_e5_modem_manager.h across two delivery passes) and
# MUST be saved with the flat names this script expects, or this will
# silently pick up the wrong file.
#
# Usage:
#   ./scaffold_lora_e5.sh <flat_downloads_dir> [dest_root]
#
#   <flat_downloads_dir>  Folder containing the flat, renamed files.
#   [dest_root]           Where modules/lora_e5/ gets created.
#                          Defaults to the current directory.
#
# Fails loudly and stops on the first missing expected file rather
# than scaffolding a partial/silently-broken tree -- if something is
# missing, fix the source folder and re-run; this script is
# idempotent (safe to re-run, uses cp not mv).

set -euo pipefail

if [[ $# -lt 1 ]]; then
	echo "Usage: $0 <flat_downloads_dir> [dest_root]" >&2
	exit 1
fi

SRC_DIR=$(cd "$1" && pwd)
DEST_ROOT=$(cd "${2:-.}" && pwd)
MOD_ROOT="${DEST_ROOT}/modules/lora_e5"

echo "Source (flat downloads): ${SRC_DIR}"
echo "Destination module root: ${MOD_ROOT}"
echo

# ---------------------------------------------------------------------
# 1. Create the full target directory tree, including directories for
#    modules not yet implemented (fsm, uart backend, events, top-level
#    public API .c, samples, mock_uart fixture) so the scaffold
#    reflects the complete intended layout, not just what exists today.
# ---------------------------------------------------------------------

DIRS=(
	"${MOD_ROOT}"
	"${MOD_ROOT}/include/lora_e5"
	"${MOD_ROOT}/src"
	"${MOD_ROOT}/samples/join"
	"${MOD_ROOT}/samples/uplink"
	"${MOD_ROOT}/samples/shell"
	"${MOD_ROOT}/tests/parser/src"
	"${MOD_ROOT}/tests/cmd_queue/src"
	"${MOD_ROOT}/tests/fsm/src"
	"${MOD_ROOT}/tests/mock_uart"
	"${MOD_ROOT}/docs/diagrams"
)

for d in "${DIRS[@]}"; do
	mkdir -p "${d}"
done
echo "Directory tree created."
echo

# ---------------------------------------------------------------------
# 2. Copy mapping: "flat_source_name:relative/dest/path"
#    Only files delivered so far are listed -- see the "NOT YET
#    IMPLEMENTED" section below for what's still missing.
# ---------------------------------------------------------------------

COPY_MAP=(
	"LoRa-E5-Zephyr-Architecture-Phase1.md:docs/Phase1-Architecture.md"
	"Phase2-Design-Notes.md:docs/Phase2-Design-Notes.md"
	"VERIFICATION_NEEDED.md:docs/VERIFICATION_NEEDED.md"
	"01_layered_architecture_v2.drawio:docs/diagrams/layered_architecture.drawio"
	"02_lorawan_fsm_v3.drawio:docs/diagrams/lorawan_fsm.drawio"

	"lora_e5.h:include/lora_e5/lora_e5.h"
	"lora_e5_at.h:include/lora_e5/lora_e5_at.h"
	"lora_e5_config.h:include/lora_e5/lora_e5_config.h"
	"lora_e5_events.h:include/lora_e5/lora_e5_events.h"
	"lora_e5_types.h:include/lora_e5/lora_e5_types.h"

	"lora_e5_modem_manager.h:src/lora_e5_modem_manager.h"
	"lora_e5_parser.c:src/lora_e5_parser.c"
	"lora_e5_at.c:src/lora_e5_at.c"
	"lora_e5_cmd_queue.h:src/lora_e5_cmd_queue.h"
	"lora_e5_cmd_queue.c:src/lora_e5_cmd_queue.c"
	"lora_e5_hf_commands.h:src/lora_e5_hf_commands.h"
	"lora_e5_hf_commands.c:src/lora_e5_hf_commands.c"

	"test_parser_main.c:tests/parser/src/main.c"
	"test_parser_CMakeLists.txt:tests/parser/CMakeLists.txt"
	"test_parser_prj.conf:tests/parser/prj.conf"
	"test_parser_testcase.yaml:tests/parser/testcase.yaml"

	"test_cmdq_main.c:tests/cmd_queue/src/main.c"
	"test_cmdq_CMakeLists.txt:tests/cmd_queue/CMakeLists.txt"
	"test_cmdq_prj.conf:tests/cmd_queue/prj.conf"
	"test_cmdq_testcase.yaml:tests/cmd_queue/testcase.yaml"
)

# ---------------------------------------------------------------------
# 3. Verify every expected file exists BEFORE copying anything --
#    fail loudly with the full list of what's missing rather than
#    copying half the tree and leaving it ambiguous what happened.
# ---------------------------------------------------------------------

missing=()
for entry in "${COPY_MAP[@]}"; do
	src_name="${entry%%:*}"
	if [[ ! -f "${SRC_DIR}/${src_name}" ]]; then
		missing+=("${src_name}")
	fi
done

if [[ ${#missing[@]} -gt 0 ]]; then
	echo "ERROR: ${#missing[@]} expected file(s) not found in ${SRC_DIR}:" >&2
	for f in "${missing[@]}"; do
		echo "  - ${f}" >&2
	done
	echo >&2
	echo "Check the rename table in chat -- these must be saved with" >&2
	echo "these exact flat filenames before running this script." >&2
	exit 1
fi

echo "All ${#COPY_MAP[@]} expected files found. Copying..."
echo

for entry in "${COPY_MAP[@]}"; do
	src_name="${entry%%:*}"
	dest_rel="${entry##*:}"
	dest_path="${MOD_ROOT}/${dest_rel}"
	mkdir -p "$(dirname "${dest_path}")"
	cp -v "${SRC_DIR}/${src_name}" "${dest_path}"
done

echo
echo "Scaffold complete: ${MOD_ROOT}"
echo
echo "NOT YET IMPLEMENTED (directories created, empty -- do not expect"
echo "build success until these land):"
echo "  - src/lora_e5_modem_manager.c   (Modem Manager runtime)"
echo "  - src/lora_e5_fsm.c             (LoRaWAN FSM)"
echo "  - src/lora_e5_uart.c            (UART backend)"
echo "  - src/lora_e5_events.c          (event bus wrappers)"
echo "  - src/lora_e5.c                 (public API implementation)"
echo "  - src/lora_e5_internal.h"
echo "  - CMakeLists.txt, Kconfig, README.md (module root)"
echo "  - tests/fsm/, tests/mock_uart/  (fixtures/suites)"
echo "  - samples/join/, samples/uplink/, samples/shell/"
echo
echo "This script is safe to re-run once more files are delivered --"
echo "it only copies files present in COPY_MAP and will not touch"
echo "anything already scaffolded that isn't in the map."
