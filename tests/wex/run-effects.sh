#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${WEX_CHECK_COMP_BUILD:-$REPO_DIR/build}"
OUT_DIR="${1:-$REPO_DIR/tests/wex/output}"

mkdir -p "$OUT_DIR"

# Ensure X11 sticky bit is set (WSLg mounts ro /tmp/.X11-unix)
umount -l /tmp/.X11-unix 2>/dev/null || true
mkdir -m 1777 -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix || true

echo "=========================================================="
echo " Building labwc compositor and test clients"
echo "=========================================================="
ninja -C "$BUILD_DIR"
make -C "$SCRIPT_DIR" wex-window-v1-client-protocol.h wex-window-v1-protocol.c
make -C "$SCRIPT_DIR"

echo "=========================================================="
echo " Initializing Headless Wayland Environment"
echo "=========================================================="
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_HEADLESS_OUTPUTS=1
export XDG_RUNTIME_DIR="$(mktemp -d /tmp/wex-run-XXXXXX)"
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

echo "Starting labwc compositor in headless mode (1280x800)..."
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

# Configure output to 1280x800 as specified in work order
wlr-randr --output HEADLESS-1 --custom-mode 1280x800 || true
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

# TEST 1: Socket Privileges & Isolation
run_check "Socket Isolation (wex_window_manager_v1 privileged)" \
    "$SCRIPT_DIR/stack-client" --scenario check-sockets --socket wex-shell-0 --normal-socket wayland-0

    "$SCRIPT_DIR/policy-client" --socket wex-shell-0 --test 7 --wait 0

# TEST 16: wex-window-v1 corner radius version negotiation
run_check "Corner Radius: Version negotiation (advertised v12, old peer v11 compatibility)" \
    "$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario version-negotiation

# TEST 17: wex-window-v1 corner radius denial on unprivileged socket
run_check "Corner Radius: Denial on unprivileged socket" \
    "$SCRIPT_DIR/corner-client" --normal-socket wayland-0 --scenario denial

# TEST 18: wex-window-v1 corner radius surface destruction
run_check "Corner Radius: Surface destruction while radius is active" \
    "$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario surface-destroy

# TEST 19: wex-window-v1 visual corner rendering, clear and clamping
rm -f "$BARRIER_DIR"/*
echo "Testing wex-window-v1 corner clipping, clearing and clamping..."
"$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario render --barrier-dir "$BARRIER_DIR" &
CLIENT_PID=$!

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_radius_16" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/11_corner_radius_16.png"
touch "$BARRIER_DIR/b_radius_16_cont"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_radius_0" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/12_corner_radius_0.png"
touch "$BARRIER_DIR/b_radius_0_cont"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_radius_clamp" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/13_corner_radius_clamp.png"
touch "$BARRIER_DIR/b_radius_clamp_cont"

wait "$CLIENT_PID"

run_check "Corner Radius 16: Top-left corner (200, 200) is clipped to background Navy" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/11_corner_radius_16.png" 200 200 112233

run_check "Corner Radius 16: Interior pixel (300, 300) is Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/11_corner_radius_16.png" 300 300 FF0000

# all four corners are clipped and the 200x200 window keeps its size (no stretched slices)
run_check "Corner Radius 16: Top-right corner (399, 200) is clipped" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/11_corner_radius_16.png" 399 200 112233
run_check "Corner Radius 16: Bottom-left corner (200, 399) is clipped" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/11_corner_radius_16.png" 200 399 112233
run_check "Corner Radius 16: Bottom-right corner (399, 399) is clipped" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/11_corner_radius_16.png" 399 399 112233
run_check "Corner Radius 16: Right edge middle (399, 300) is Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/11_corner_radius_16.png" 399 300 FF0000
run_check "Corner Radius 16: Nothing below the window (300, 420)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/11_corner_radius_16.png" 300 420 112233

run_check "Corner Radius Cleared: Top-left corner (200, 200) is restored to sharp Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/12_corner_radius_0.png" 200 200 FF0000

run_check "Corner Radius Cleared: Interior pixel (300, 300) remains Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/12_corner_radius_0.png" 300 300 FF0000

run_check "Corner Radius Clamped: Top-left corner (200, 200) is clipped to background Navy" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/13_corner_radius_clamp.png" 200 200 112233

run_check "Corner Radius Clamped: Point (240, 240) outside 32px corner is Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/13_corner_radius_clamp.png" 240 240 FF0000

run_check "Corner Radius Clamped: Interior pixel (300, 300) remains Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/13_corner_radius_clamp.png" 300 300 FF0000

# TEST 20: wex_window_manager_v1 v13 set_effects (shadow, backdrop, clear, stale/destroyed handle)
rm -f "$BARRIER_DIR"/*
echo "Testing wex-window-v1 v13 set_effects (shadow, backdrop, clear)..."
"$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario effects --barrier-dir "$BARRIER_DIR" &
EFFECTS_PID=$!

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_effects_shadow" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/14_effects_shadow.png"
touch "$BARRIER_DIR/b_effects_shadow_cont"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_effects_backdrop" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/15_effects_backdrop.png"
touch "$BARRIER_DIR/b_effects_backdrop_cont"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_effects_cleared" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/16_effects_cleared.png"
touch "$BARRIER_DIR/b_effects_cleared_cont"

wait "$EFFECTS_PID"

run_check "Effects Shadow: Pixel outside right edge (403, 300) is darker than background" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/14_effects_shadow.png" 403 300 0F1E2D 5

run_check "Effects Backdrop: Interior pixel (300, 300) shows red tint blended with half-transparent surface" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/15_effects_backdrop.png" 300 300 7F0000 6

run_check "Effects Cleared: Shadow pixel (403, 300) is restored to background Navy" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/16_effects_cleared.png" 403 300 112233 5

run_check "Effects Cleared: Interior pixel (300, 300) returns to translucent without backdrop" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/16_effects_cleared.png" 300 300 081119 5

# TEST 21: wex_window_manager_v1 v14 set_thumbnails (scaled, minimized source, clear, robustness)
rm -f "$BARRIER_DIR"/*
echo "Testing wex-window-v1 v14 set_thumbnails (scaled, minimized source, cleared, robust)..."
"$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario thumbnails --barrier-dir "$BARRIER_DIR" &
THUMB_PID=$!

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_thumb_active" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/21_thumb_active.png"
touch "$BARRIER_DIR/b_thumb_active_cont"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_thumb_minimized" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/22_thumb_minimized.png"
touch "$BARRIER_DIR/b_thumb_minimized_cont"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_thumb_cleared" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/23_thumb_cleared.png"
touch "$BARRIER_DIR/b_thumb_cleared_cont"

wait "$THUMB_PID"

run_check "Thumbnail Active: Centre of thumbnail in dest B (500, 200) shows scaled green source A" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/21_thumb_active.png" 500 200 00FF00 5

run_check "Thumbnail Active: Outside thumbnail rect in dest B (420, 120) remains navy" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/21_thumb_active.png" 420 120 112233 5

run_check "Thumbnail Minimized: Minimized source window A position (150, 150) shows background" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/22_thumb_minimized.png" 150 150 222222 5

run_check "Thumbnail Minimized: Dest B thumbnail rect (500, 200) still displays source buffer" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/22_thumb_minimized.png" 500 200 00FF00 5

run_check "Thumbnail Cleared: Dest B thumbnail rect (500, 200) restored to navy" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/23_thumb_cleared.png" 500 200 112233 5

# TEST 22: wex_window_manager_v1 v14 set_peek (Aero Peek)
rm -f "$BARRIER_DIR"/*
echo "Testing wex-window-v1 v14 set_peek (Aero Peek target and restore)..."
"$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario peek --barrier-dir "$BARRIER_DIR" &
PEEK_PID=$!

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_peek_active" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/24_peek_active.png"
touch "$BARRIER_DIR/b_peek_active_cont"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b_peek_ended" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/25_peek_ended.png"
touch "$BARRIER_DIR/b_peek_ended_cont"

wait "$PEEK_PID"

run_check "Peek Active: Window A position (150, 250) is hidden, showing background navy" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/24_peek_active.png" 150 250 112233 5

run_check "Peek Active: Target window B (350, 250) remains visible green" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/24_peek_active.png" 350 250 00FF00 5

run_check "Peek Active: Window C position (550, 250) is hidden, showing background navy" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/24_peek_active.png" 550 250 112233 5

run_check "Peek Ended: Window A (150, 250) is restored to red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/25_peek_ended.png" 150 250 FF0000 5

run_check "Peek Ended: Window B (350, 250) remains visible green" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/25_peek_ended.png" 350 250 00FF00 5

run_check "Peek Ended: Window C (550, 250) is restored to blue" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/25_peek_ended.png" 550 250 0000FF 5

# TEST 23: wex_window_manager_v1 v14 frame event (clock commit notifications)
run_check "Frame Events: Increasing counter and monotonic clock on output commit" \
    "$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario frame

echo "=========================================================="
echo " ALL TESTS PASSED SUCCESSFULLY ($TESTS_PASSED/$TESTS_PASSED)"
echo " Screenshots saved in: $OUT_DIR"
echo "=========================================================="
exit 0

echo "effects run: passed $TESTS_PASSED failed $TESTS_FAILED"
