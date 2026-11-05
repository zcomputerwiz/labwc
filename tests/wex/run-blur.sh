#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${WEX_CHECK_COMP_BUILD:-$REPO_DIR/build-fx}"
OUT_DIR="${1:-$REPO_DIR/tests/wex/output-blur}"

mkdir -p "$OUT_DIR"

# Ensure vgem is loaded so DRM card node exists for llvmpipe/GBM
modprobe vgem 2>/dev/null || true

# Ensure X11 sticky bit is set (WSLg mounts ro /tmp/.X11-unix)
umount -l /tmp/.X11-unix 2>/dev/null || true
mkdir -m 1777 -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix || true

echo "=========================================================="
echo " Building labwc compositor (SceneFX) and test clients"
echo "=========================================================="
if [ ! -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" "$REPO_DIR" -Dscenefx=enabled
fi
ninja -C "$BUILD_DIR"
make -C "$SCRIPT_DIR" wex-window-v1-client-protocol.h wex-window-v1-protocol.c
make -C "$SCRIPT_DIR"

echo "=========================================================="
echo " Initializing Headless Wayland Environment (SceneFX/GLES2)"
echo "=========================================================="
export WLR_BACKENDS=headless
export WLR_RENDERER=gles2
export WLR_RENDERER_ALLOW_SOFTWARE=1
export WEX_BLUR=1
export WEX_BLUR_TEST_VGEM=1
export WLR_HEADLESS_OUTPUTS=1
export XDG_RUNTIME_DIR="$(mktemp -d /tmp/wex-run-blur-XXXXXX)"
chmod 700 "$XDG_RUNTIME_DIR"
export WAYLAND_DISPLAY="wayland-0"
export LABWC_WEX_SHELL_SOCKET="wex-shell-0"

CONFIG_DIR="$XDG_RUNTIME_DIR/labwc-cfg"
mkdir -p "$CONFIG_DIR"
cat << 'EOF' > "$CONFIG_DIR/rc.xml"
<?xml version="1.0"?>
<labwc_config>
  <core>
    <decoration>none</decoration>
  </core>
  <placement>
    <policy>center</policy>
  </placement>
</labwc_config>
EOF

echo "Starting labwc compositor in headless mode (1280x800) with SceneFX blur..."
"$BUILD_DIR/labwc" -C "$CONFIG_DIR" > "$XDG_RUNTIME_DIR/labwc.log" 2>&1 &
COMPOSITOR_PID=$!

cleanup() {
    echo "Stopping compositor (PID $COMPOSITOR_PID)..."
    kill "$COMPOSITOR_PID" 2>/dev/null || true
    wait "$COMPOSITOR_PID" 2>/dev/null || true
    cp "$XDG_RUNTIME_DIR/labwc.log" "$OUT_DIR/compositor.log"
    rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT

# Wait for sockets
SOCKET_READY=0
for i in {1..50}; do
    if [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && [ -S "$XDG_RUNTIME_DIR/wex-shell-0" ]; then
        SOCKET_READY=1
        break
    fi
    sleep 0.1
done

if [ "$SOCKET_READY" -ne 1 ]; then
    echo "ERROR: Compositor sockets failed to appear in $XDG_RUNTIME_DIR!"
    cat "$XDG_RUNTIME_DIR/labwc.log"
    exit 1
fi
echo "Compositor is ready. Sockets: wayland-0 and wex-shell-0 active."
sleep 0.5

# Configure output to 1280x800 if wlr-randr is available
if command -v wlr-randr >/dev/null 2>&1; then
    wlr-randr --output HEADLESS-1 --custom-mode 1280x800 || true
fi
sleep 0.5

BARRIER_DIR="$XDG_RUNTIME_DIR/barriers"
mkdir -p "$BARRIER_DIR"
TESTS_PASSED=0; TESTS_FAILED=0
run_check() {
    local name="$1"
    shift
    echo "----------------------------------------------------------"
    echo " Running test: $name"
    echo "----------------------------------------------------------"
    if "$@"; then
        echo " [PASS] $name"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo " [FAIL] $name"
        TESTS_FAILED=$((TESTS_FAILED + 1))
        exit 1
    fi
}

echo "=========================================================="
echo " Running Visual Backdrop Blur Scenario"
echo "=========================================================="

rm -f "$BARRIER_DIR"/*
"$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario blur --barrier-dir "$BARRIER_DIR" &
BLUR_PID=$!

# Step 1: Backdrop NONE
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_none" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/01_blur_none.png"
touch "$BARRIER_DIR/b_blur_none_cont"

# Step 2: Backdrop BLUR
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_active" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/02_blur_active.png"
touch "$BARRIER_DIR/b_blur_active_cont"

# Step 3: Backdrop Cleared
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_cleared" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/03_blur_cleared.png"
touch "$BARRIER_DIR/b_blur_cleared_cont"

# Step 4: Acrylic
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_acrylic" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/04_blur_acrylic.png"
touch "$BARRIER_DIR/b_blur_acrylic_cont"

# Step 5: Mica with a red normal window between the glass and wallpaper
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_mica" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/05_blur_mica.png"
touch "$BARRIER_DIR/b_blur_mica_cont"

# Step 6: new wallpaper under Mica
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_wallpaper" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/06_blur_mica_wallpaper.png"
touch "$BARRIER_DIR/b_blur_wallpaper_cont"

# Step 7: Corner Anti-Aliasing
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_corners" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/07_blur_corners.png"
touch "$BARRIER_DIR/b_blur_corners_cont"

# Step 8: Soft Shadow
for i in {1..100}; do if [ -f "$BARRIER_DIR/b_blur_shadow" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/08_blur_shadow.png"
touch "$BARRIER_DIR/b_blur_shadow_cont"

wait "$BLUR_PID"

echo "=========================================================="
echo " Verifying Backdrop Blur Results"
echo "=========================================================="

# Verification 1: Backdrop NONE
run_check "Blur None: Outside white pixel (635, 100) is White" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/01_blur_none.png" 635 100 FFFFFF 5

run_check "Blur None: Outside black pixel (645, 100) is Black" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/01_blur_none.png" 645 100 000000 5

run_check "Blur None: Inside glass pixel on white side (635, 350) equals outside White" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/01_blur_none.png" 635 350 FFFFFF 5

run_check "Blur None: Inside glass pixel on black side (645, 350) equals outside Black" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/01_blur_none.png" 645 350 000000 5

# Verification 2: Backdrop BLUR
run_check "Blur Active: Outside white pixel (635, 100) stays White" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/02_blur_active.png" 635 100 FFFFFF 5

run_check "Blur Active: Outside black pixel (645, 100) stays Black" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/02_blur_active.png" 645 100 000000 5

run_check "Blur Active: Inside glass white side (635, 350) is darker than pure white" \
    python3 "$SCRIPT_DIR/verify.py" check-range "$OUT_DIR/02_blur_active.png" 635 350 20 240

run_check "Blur Active: Inside glass black side (645, 350) is lighter than pure black" \
    python3 "$SCRIPT_DIR/verify.py" check-range "$OUT_DIR/02_blur_active.png" 645 350 15 235

# Verification 3: Backdrop Cleared
run_check "Blur Cleared: Inside glass white side (635, 350) restored to White" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_blur_cleared.png" 635 350 FFFFFF 5

run_check "Blur Cleared: Inside glass black side (645, 350) restored to Black" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_blur_cleared.png" 645 350 000000 5

run_check "Blur Cleared: Outside white pixel (635, 100) remains White" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_blur_cleared.png" 635 100 FFFFFF 5

run_check "Blur Cleared: Outside black pixel (645, 100) remains Black" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_blur_cleared.png" 645 100 000000 5

run_check "Acrylic: capture uses a material distinct from plain Blur" \
    bash -c '! python3 "$0" compare-images "$1" "$2" 0' "$SCRIPT_DIR/verify.py" "$OUT_DIR/02_blur_active.png" "$OUT_DIR/04_blur_acrylic.png"

run_check "Mica: wallpaper sample keeps green channel high over red blocker" \
    python3 "$SCRIPT_DIR/verify.py" check-range "$OUT_DIR/05_blur_mica.png" 600 350 140 255 1

run_check "Mica: wallpaper sample keeps blue channel high over red blocker" \
    python3 "$SCRIPT_DIR/verify.py" check-range "$OUT_DIR/05_blur_mica.png" 600 350 140 255 2

run_check "Mica: a blue wallpaper reaches the cached sample (blue channel)" \
    python3 "$SCRIPT_DIR/verify.py" check-range "$OUT_DIR/06_blur_mica_wallpaper.png" 600 350 150 255 2

run_check "Mica: a blue wallpaper replaces the old white sample (red channel)" \
    python3 "$SCRIPT_DIR/verify.py" check-range "$OUT_DIR/06_blur_mica_wallpaper.png" 600 350 0 180 0

# Verification 7: Corner Anti-Aliasing (SceneFX SDF corner shader)
run_check "Corner AA: Outside corner (440, 250) is background Blue" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/07_blur_corners.png" 440 250 0000FF 5

run_check "Corner AA: Inside corner (460, 270) is foreground Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/07_blur_corners.png" 460 270 FF0000 5

run_check "Corner AA: Edge pixel (444, 254) has intermediate colour (anti-aliased)" \
    python3 "$SCRIPT_DIR/verify.py" check-corner-aa "$OUT_DIR/07_blur_corners.png" 444 254 0000FF FF0000 15

run_check "Corner AA: Edge pixel (451, 250) has intermediate colour (anti-aliased)" \
    python3 "$SCRIPT_DIR/verify.py" check-corner-aa "$OUT_DIR/07_blur_corners.png" 451 250 0000FF FF0000 15

# Verification 8: Soft Shadow (SceneFX Gaussian blur shadow)
run_check "Soft Shadow: Right edge pixels (840..860, 400) fall off gradually and monotonically" \
    python3 "$SCRIPT_DIR/verify.py" check-shadow-falloff "$OUT_DIR/08_blur_shadow.png" 840 400 21 x 2 15 8

run_check "Soft Shadow: Bottom edge pixels (640, 550..570) fall off gradually and monotonically" \
    python3 "$SCRIPT_DIR/verify.py" check-shadow-falloff "$OUT_DIR/08_blur_shadow.png" 640 550 21 y 2 15 8

echo "=========================================================="
echo " All blur checks passed ($TESTS_PASSED/$((TESTS_PASSED + TESTS_FAILED)))"
echo "=========================================================="
