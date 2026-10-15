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

TESTS_PASSED=0
TESTS_FAILED=0

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

# Version 2 keeps the v1 clients above and below unchanged.
run_check "Shared handles across three clients and layer/lifetime invariants" \
    python3 "$SCRIPT_DIR/check-handles.py"

run_check "Native supplemental observation, controls and lifetime" \
    "$SCRIPT_DIR/foreign-client"
run_check "Native maximum constraints, live updates and old observers" \
    "$SCRIPT_DIR/foreign-client" --constraints
run_check "XWayland maximum constraints and live WM_NORMAL_HINTS" \
    "$SCRIPT_DIR/foreign-client" --xwayland-constraints

run_check "Native/frame association reservation and lifetime" \
    "$SCRIPT_DIR/foreign-client" --association

run_check "Native/frame scene, input routing and recovery" \
    env WEX_SCENE_TEST_DIR="$OUT_DIR" "$SCRIPT_DIR/foreign-client" --scene
run_check "Frame ring visible" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-initial.png" 200 110 0xFFFF0000 1
run_check "Native alpha blends with background through frame cutout" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-initial.png" 150 180 0xFF007F80 1
run_check "Native transparency exposes background without proxy occlusion" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-initial.png" 240 230 0xFF00FF00 1
run_check "Frame and native move together" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-moved.png" 250 230 0xFF007F80 1
run_check "Group minimize exposes background" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-minimized.png" 205 155 0xFF00FF00 1
run_check "Group restore restores frame" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-restored.png" 205 155 0xFFFF0000 1
run_check "Wine menu subsurface stays above native content" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-menu.png" 270 220 0xFFFFFF00 1
run_check "Controller loss hides obsolete proxy" python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/scene-detached.png" 205 155 0xFF00FF00 1

# TEST 2: xx-zones-v1 Window Placement
echo "Testing xx-zones-v1 explicit placement and centering bypass..."
"$SCRIPT_DIR/zones-client" \
    --win 100 150 200 200 0xFFFF0000 \
    --win 400 200 250 180 0xFF0000FF \
    --export-handle "$XDG_RUNTIME_DIR/zone.handle" \
    --wait 3 &
CLIENT_PID=$!
sleep 1
grim "$OUT_DIR/01_zones_placement.png"
wait "$CLIENT_PID"

run_check "Zone Window 1 at (100, 150) 200x200 Red" \
    python3 "$SCRIPT_DIR/verify.py" check-rect "$OUT_DIR/01_zones_placement.png" 100 150 200 200 FF0000

run_check "Zone Window 2 at (400, 200) 250x180 Blue" \
    python3 "$SCRIPT_DIR/verify.py" check-rect "$OUT_DIR/01_zones_placement.png" 400 200 250 180 0000FF

run_check "xx-zones-v1 Off-Zone Request position_failed rejection" \
    "$SCRIPT_DIR/zones-client" --test-fail 2000 2000 --wait 0

# TEST 3: xx-zones-v1 Join by Handle from second process
ZONE_HANDLE="$(cat "$XDG_RUNTIME_DIR/zone.handle")"
echo "Joining zone via handle '$ZONE_HANDLE'..."
"$SCRIPT_DIR/zones-client" \
    --join-handle "$ZONE_HANDLE" \
    --win 700 100 200 200 0xFF00FF00 \
    --wait 3 &
CLIENT_PID=$!
sleep 1
grim "$OUT_DIR/02_zones_join_handle.png"
wait "$CLIENT_PID"

run_check "Zone Join Handle Window at (700, 100) 200x200 Green" \
    python3 "$SCRIPT_DIR/verify.py" check-rect "$OUT_DIR/02_zones_join_handle.png" 700 100 200 200 00FF00

run_check "xx-zones-v1 Unknown Handle Rejection" \
    "$SCRIPT_DIR/zones-client" --test-unknown-handle

# TEST 4: wex-window-v1 Layering
echo "Testing wex-window-v1 layers (Desktop, Normal, Topmost, Shell)..."
"$SCRIPT_DIR/stack-client" --scenario layers --wait 3 &
CLIENT_PID=$!
sleep 1
grim "$OUT_DIR/03_stack_layers.png"
wait "$CLIENT_PID"

run_check "Layer Desktop Navy at (50, 50)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_stack_layers.png" 50 50 112233

run_check "Layer Normal 1 Red at (120, 120)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_stack_layers.png" 120 120 FF0000

run_check "Layer Normal 2 Blue above Normal 1 at (160, 160)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_stack_layers.png" 160 160 0000FF

run_check "Layer Topmost Yellow above Normal 2 at (210, 210)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_stack_layers.png" 210 210 FFFF00

run_check "Layer Shell Green taskbar at bottom (640, 785)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/03_stack_layers.png" 640 785 00AA00

# TEST 5: wex-window-v1 Reordering with place_above
echo "Testing wex-window-v1 place_above reordering..."
"$SCRIPT_DIR/stack-client" --scenario reorder --wait 3 &
CLIENT_PID=$!
sleep 1
grim "$OUT_DIR/04_stack_reorder.png"
wait "$CLIENT_PID"

run_check "Reordered: Normal 1 Red placed above Normal 2 Blue at (160, 160)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/04_stack_reorder.png" 160 160 FF0000

# TEST 6: wex-window-v1 Exclusive Edge & Maximization
echo "Testing wex-window-v1 exclusive shell edge reservation & maximization..."
"$SCRIPT_DIR/stack-client" --scenario maximize --wait 3 &
CLIENT_PID=$!
sleep 1
grim "$OUT_DIR/05_stack_maximize.png"
wait "$CLIENT_PID"

run_check "Exclusive edge: Shell window at bottom (640, 785) is Green" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/05_stack_maximize.png" 640 785 00AA00

run_check "Maximized window occupies (0, 0, 1280, 770)" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/05_stack_maximize.png" 640 400 4488CC

# TEST 7: wex-window-v1 Unminimize on activate()
run_check "wex-window-v1 activate() unminimizes minimized window" \
    "$SCRIPT_DIR/stack-client" --scenario unminimize --wait 1

# TEST 8: wp_alpha_modifier_v1
echo "Testing wp_alpha_modifier_v1 50% opacity..."
"$SCRIPT_DIR/alpha-client" --wait 3 &
CLIENT_PID=$!
sleep 1
grim "$OUT_DIR/06_alpha_50.png"
wait "$CLIENT_PID"

run_check "wp_alpha_modifier_v1 renders at 50% opacity (blended R=255, G=128, B=128)" \
    python3 "$SCRIPT_DIR/verify.py" check-alpha "$OUT_DIR/06_alpha_50.png" 100 100 200 200 255 128 128 12

echo "Testing wp_alpha_modifier_v1 subsurface 50% opacity..."
"$SCRIPT_DIR/alpha-client" --subsurface --wait 3 &
CLIENT_PID=$!
sleep 1
grim "$OUT_DIR/06b_subsurface_alpha_50.png"
wait "$CLIENT_PID"

run_check "wp_alpha_modifier_v1 subsurface at 50% opacity (blended Yellow R=128, G=128, B=0)" \
    python3 "$SCRIPT_DIR/verify.py" check-alpha "$OUT_DIR/06b_subsurface_alpha_50.png" 150 150 200 200 128 128 0 12

# TEST 9: wp_pointer_warp_v1
run_check "wp_pointer_warp_v1 repositions pointer and emits motion event" \
    "$SCRIPT_DIR/warp-client" --x 350 --y 250
run_check "wp_pointer_warp_v1 respects nonzero xdg window geometry" \
    "$SCRIPT_DIR/warp-client" --geometry-offset --x 350 --y 250

# TEST 10: wex focus and stacking policy (Test 1)
BARRIER_DIR="$XDG_RUNTIME_DIR/barriers"
mkdir -p "$BARRIER_DIR"
rm -f "$BARRIER_DIR"/*

echo "Testing wex policy Test 1 (click without focus steal; activate raises)..."
"$SCRIPT_DIR/policy-client" --socket wex-shell-0 --test 1 --barrier-dir "$BARRIER_DIR" &
CLIENT_PID=$!

for i in {1..100}; do if [ -f "$BARRIER_DIR/b1_clicked" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/07_click_no_steal.png"
touch "$BARRIER_DIR/b1_continue"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b1_activated" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/08_activate_raised.png"
touch "$BARRIER_DIR/b1_finish"

wait "$CLIENT_PID"

run_check "Policy 1: Clicked Window A does not steal focus, Window B remains above Window A at (175, 175) Blue" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/07_click_no_steal.png" 175 175 0000FF

run_check "Policy 1: activate(A) raises Window A above Window B at (175, 175) Red" \
    python3 "$SCRIPT_DIR/verify.py" check-pixel "$OUT_DIR/08_activate_raised.png" 175 175 FF0000

# TEST 11: wex map unfocused policy (Test 2)
run_check "Policy 2: Newly mapped wex window has no keyboard focus until activate()" \
    "$SCRIPT_DIR/policy-client" --socket wex-shell-0 --test 2 --wait 0

# TEST 12: wex close unfocused policy (Test 3)
run_check "Policy 3: Closing focused wex window defocuses rather than stealing focus to another wex window" \
    "$SCRIPT_DIR/policy-client" --socket wex-shell-0 --test 3 --wait 0

# TEST 13: wex keybindings ignored (Test 4)
rm -f "$BARRIER_DIR"/*
echo "Testing wex policy Test 4 (keybinds W-a, A-Left, A-Space ignored on wex window)..."
"$SCRIPT_DIR/policy-client" --socket wex-shell-0 --test 4 --barrier-dir "$BARRIER_DIR" &
CLIENT_PID=$!

for i in {1..100}; do if [ -f "$BARRIER_DIR/b4_before" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/09_wex_keybinds_before.png"
touch "$BARRIER_DIR/b4_continue"

for i in {1..100}; do if [ -f "$BARRIER_DIR/b4_after" ]; then break; fi; sleep 0.05; done
grim "$OUT_DIR/10_wex_keybinds_after.png"
touch "$BARRIER_DIR/b4_finish"

wait "$CLIENT_PID"

run_check "Policy 4: Keybinds ignored on wex window (geometry unchanged, no menu screenshot compare)" \
    python3 "$SCRIPT_DIR/verify.py" compare-images "$OUT_DIR/09_wex_keybinds_before.png" "$OUT_DIR/10_wex_keybinds_after.png"

# TEST 14: wex window switcher ignored (Test 5)
run_check "Policy 5: Window switcher (A-Tab) does not select wex window" \
    "$SCRIPT_DIR/policy-client" --socket wex-shell-0 --test 5 --wait 0

# TEST 15: non-wex client retains stock behavior (Test 7)
run_check "Policy 7: Non-wex client retains stock click-to-focus and Super+A maximize" \
    "$SCRIPT_DIR/policy-client" --socket wex-shell-0 --test 7 --wait 0

echo "=========================================================="
echo " ALL TESTS PASSED SUCCESSFULLY ($TESTS_PASSED/$TESTS_PASSED)"
echo " Screenshots saved in: $OUT_DIR"
echo "=========================================================="
exit 0
