#!/usr/bin/env bash
#
# cross-build.sh - Cross-build librist for the NationalChip ARMv7-A (Cortex-A7)
#                  STB target and confirm the produced shared object is ARM.
#
# This script ONLY verifies that librist cross-compiles. It does not install
# anything to the host (no `ninja install`) and does not touch the root-level
# marker programs (ristsender_marker, ristreceiver_with_markers, rist_watchdog).
#
# The actual toolchain only exists on the dedicated build VM, so this is meant
# to be run there, not in CI on a developer machine.

set -euo pipefail

# Resolve repo paths relative to this script so it can be run from anywhere.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CROSS_FILE="$REPO_ROOT/cross/nationalchip-armv7.txt"
LIBRIST_DIR="$REPO_ROOT/librist"
BUILD_DIR="build-arm"

# ---------------------------------------------------------------------------
# 1. Toolchain environment.
#
# /opt/stb/env.sh puts the cross gcc on PATH and, crucially, sets
# LD_LIBRARY_PATH so the compiler's cc1 stage can load libmpc/libmpfr/libgmp
# from the toolchain. Without it the build fails with:
#   "libmpc.so.3: cannot open shared object file"
# ---------------------------------------------------------------------------
if [ ! -f /opt/stb/env.sh ]; then
    echo "ERROR: /opt/stb/env.sh not found - run this on the build VM with the" >&2
    echo "       NationalChip toolchain installed." >&2
    exit 1
fi
# shellcheck source=/dev/null
source /opt/stb/env.sh

if [ ! -f "$CROSS_FILE" ]; then
    echo "ERROR: meson cross file not found: $CROSS_FILE" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Build tooling - install only what is missing.
# ---------------------------------------------------------------------------
missing=()
command -v meson      >/dev/null 2>&1 || missing+=(meson)
command -v ninja      >/dev/null 2>&1 || missing+=(ninja-build)
command -v pkg-config >/dev/null 2>&1 || missing+=(pkg-config)
if [ "${#missing[@]}" -gt 0 ]; then
    echo "Installing build tooling: ${missing[*]}"
    sudo apt-get update
    sudo apt-get install -y "${missing[@]}"
fi

# ---------------------------------------------------------------------------
# 3. Configure + build librist for ARM.
#
# Builtin mbedtls/cjson so we don't depend on ARM dev packages in a sysroot;
# tests are off because they would build host-architecture test runners.
# rm -rf keeps the script idempotent across reruns and option changes.
# ---------------------------------------------------------------------------
cd "$LIBRIST_DIR"
rm -rf "$BUILD_DIR"
meson setup "$BUILD_DIR" \
    --cross-file "$CROSS_FILE" \
    --buildtype=release \
    -Dbuiltin_mbedtls=true \
    -Dbuiltin_cjson=true \
    -Dtest=false
ninja -C "$BUILD_DIR"

# ---------------------------------------------------------------------------
# 4. Confirm the produced shared object is a 32-bit ARM ELF.
# ---------------------------------------------------------------------------
SO_PATH="$(find "$BUILD_DIR" -name 'librist.so*' -type f -print -quit)"
if [ -z "$SO_PATH" ]; then
    echo "FAIL: no librist.so* produced under $LIBRIST_DIR/$BUILD_DIR" >&2
    exit 1
fi

echo ""
echo "Produced shared object:"
ls -l "$SO_PATH"
FILE_OUT="$(file "$SO_PATH")"
echo "$FILE_OUT"
echo ""

if echo "$FILE_OUT" | grep -q 'ARM'; then
    echo "PASS: librist cross-compiled to a 32-bit ARM ELF ($SO_PATH)"
else
    echo "FAIL: $SO_PATH is not an ARM ELF - check the cross file / toolchain" >&2
    exit 1
fi
