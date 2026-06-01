#!/usr/bin/env bash
#
# cross-build.sh - Cross-build librist plus the STB-side custom programs for
#                  the NationalChip ARMv7-A (Cortex-A7) target, and confirm the
#                  produced binaries are 32-bit ARM ELFs.
#
# Builds:
#   - librist (shared object, via meson/ninja)
#   - ristsender_marker  (links librist + pthread; runs on the box)
#   - rist_watchdog      (libc-only fork/exec supervisor; runs on the box)
#
# Deliberately NOT built:
#   - ristreceiver_with_markers - runs on the upload/headend side (x86), not
#     needed on the box.
#
# This script does not install anything to the host (no `ninja install`).
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
#
# Look only at the real versioned object in the top of the build dir
# (e.g. build-arm/librist.so.4.5.0). -maxdepth 1 excludes meson's private
# "*.p/" dir, which contains a 0-byte librist.so.*.symbols file that an
# unrestricted glob would match and mis-report as a FAIL.
# ---------------------------------------------------------------------------
overall_ok=1

check_arm_elf() {
    # $1 = label, $2 = path
    local label="$1" path="$2"
    if [ ! -f "$path" ]; then
        echo "FAIL: $label - not produced ($path)" >&2
        overall_ok=0
        return
    fi
    local out
    out="$(file "$path")"
    echo "$out"
    if echo "$out" | grep -q 'ARM'; then
        echo "PASS: $label is a 32-bit ARM ELF ($path)"
    else
        echo "FAIL: $label is not an ARM ELF ($path)" >&2
        overall_ok=0
    fi
}

SO_PATH="$(find "$BUILD_DIR" -maxdepth 1 -type f -name 'librist.so.*' -print -quit)"
echo ""
echo "=== librist shared object ==="
if [ -z "$SO_PATH" ]; then
    echo "FAIL: librist - no librist.so.* produced under $LIBRIST_DIR/$BUILD_DIR" >&2
    overall_ok=0
else
    ls -l "$SO_PATH"
    check_arm_elf "librist" "$SO_PATH"
fi

# ---------------------------------------------------------------------------
# 5. Cross-compile the STB-side custom programs against the ARM librist.
#
#    /opt/stb/env.sh (sourced above) already put the cross gcc on PATH.
#    Native build lines, from README.md:
#      gcc -o ristsender_marker ristsender_marker.c -lrist -lpthread
#      gcc -o rist_watchdog     rist_watchdog.c
#
#    ristsender_marker.c includes <librist/librist.h> and <librist/udpsocket.h>;
#    its header chain (peer.h / librist_srp.h) pulls in the generated
#    librist_config.h, which lives under build-arm/include - hence the extra -I.
#    rist_watchdog.c is libc-only (fork/exec supervisor), so no librist/pthread.
# ---------------------------------------------------------------------------
CROSS_CC="${CC:-arm-nationalchip-linux-uclibcgnueabihf-gcc}"
TARGET_FLAGS=(-mcpu=cortex-a7 -mfpu=vfpv3-d16 -mfloat-abi=hard)
MARKERS_DIR="$BUILD_DIR/markers"
rm -rf "$MARKERS_DIR"
mkdir -p "$MARKERS_DIR"

echo ""
echo "=== STB-side custom programs ==="

# ristsender_marker - links librist + pthread.
"$CROSS_CC" "${TARGET_FLAGS[@]}" -O2 -Wall \
    -I "$REPO_ROOT/librist/include" \
    -I "$REPO_ROOT/librist/$BUILD_DIR/include" \
    -I "$REPO_ROOT/librist/$BUILD_DIR/include/librist" \
    "$REPO_ROOT/ristsender_marker.c" \
    -L "$LIBRIST_DIR/$BUILD_DIR" -lrist -lpthread \
    -o "$MARKERS_DIR/ristsender_marker"
check_arm_elf "ristsender_marker" "$MARKERS_DIR/ristsender_marker"

# rist_watchdog - libc only.
"$CROSS_CC" "${TARGET_FLAGS[@]}" -O2 -Wall \
    "$REPO_ROOT/rist_watchdog.c" \
    -o "$MARKERS_DIR/rist_watchdog"
check_arm_elf "rist_watchdog" "$MARKERS_DIR/rist_watchdog"

echo ""
echo "==================================================================="
if [ "$overall_ok" -eq 1 ]; then
    echo "OVERALL: PASS - librist + STB-side programs cross-built for ARM"
else
    echo "OVERALL: FAIL - see messages above" >&2
    exit 1
fi
