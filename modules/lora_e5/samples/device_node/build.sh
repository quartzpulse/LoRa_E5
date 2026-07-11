#!/usr/bin/env bash
#
# build.sh -- build (and optionally flash) samples/device_node for a real
# ESP32-S3 board. lora_e5 and status_led aren't in the west manifest, so
# ZEPHYR_EXTRA_MODULES is required every time; this script exists so
# that doesn't have to be remembered/retyped (same pattern as
# samples/join/build.sh). Also passes boards/status_led_rgb.overlay as
# an EXTRA_DTC_OVERLAY_FILE (added on top of the auto-discovered
# boards/esp32s3_devkitc_procpu.overlay, not replacing it) to wire the
# onboard WS2812 status LED -- see status_led's README and
# docs/VERIFICATION_NEEDED.md for the GPIO38 hardware note.
#
# Usage:
#   ./build.sh                       # build only
#   ./build.sh --flash                # build, then flash /dev/ttyACM0
#   ./build.sh --flash --device /dev/ttyACM2
#   ./build.sh --board <board qualifier>   # default: esp32s3_devkitc/esp32s3/procpu
#   ./build.sh --clean                # wipe the build dir first (-p always)
#
# Build output goes to <repo_root>/build/device_node_esp32s3 (out-of-tree,
# not samples/device_node/build -- keeps the sample directory clean
# regardless of cwd when this script is invoked).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/device_node_esp32s3"
BOARD="esp32s3_devkitc/esp32s3/procpu"
DEVICE="/dev/ttyACM0"
DO_FLASH=0
PRISTINE=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	--flash)
		DO_FLASH=1
		shift
		;;
	--device)
		DEVICE="$2"
		shift 2
		;;
	--board)
		BOARD="$2"
		shift 2
		;;
	--clean)
		PRISTINE="-p always"
		shift
		;;
	-h | --help)
		grep '^#' "$0" | sed 's/^#//'
		exit 0
		;;
	*)
		echo "Unknown argument: $1" >&2
		exit 1
		;;
	esac
done

if [[ ! -x "$REPO_ROOT/.venv/bin/west" ]]; then
	echo "error: $REPO_ROOT/.venv/bin/west not found -- see CLAUDE.md (.venv must be set up first)" >&2
	exit 1
fi

source "$REPO_ROOT/.venv/bin/activate"

export ZEPHYR_EXTRA_MODULES="$REPO_ROOT/modules/lora_e5;$REPO_ROOT/modules/status_led"

echo "Building for $BOARD -> $BUILD_DIR"
west build $PRISTINE -b "$BOARD" "$SCRIPT_DIR" -d "$BUILD_DIR" -- \
	-DEXTRA_DTC_OVERLAY_FILE="$SCRIPT_DIR/boards/status_led_rgb.overlay"

if [[ "$DO_FLASH" -eq 1 ]]; then
	echo "Flashing via $DEVICE"
	west flash -d "$BUILD_DIR" --esp-device "$DEVICE"
fi
