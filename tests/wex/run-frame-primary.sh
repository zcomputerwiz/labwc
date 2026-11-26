#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Two headless outputs side by side (HEADLESS-1 at 0,0 and HEADLESS-2 at 1280,0): the shell's frame clock (the v14
# `frame` event, DwmFlush) follows the primary output, the one at the layout origin, not the output plugged in last.
# A window animating on the primary output gives many frame events; one animating on the other gives none.
# Run as root in a private mount namespace where /tmp/.X11-unix can be written (see run.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${WEX_CHECK_COMP_BUILD:-$REPO_DIR/build}"

# XWayland needs a sticky /tmp/.X11-unix (WSLg mounts it read-only without the bit); a caller's own mount is kept
if [ ! -k /tmp/.X11-unix ]; then
    umount -l /tmp/.X11-unix 2>/dev/null || true
    mkdir -m 1777 -p /tmp/.X11-unix
    chmod 1777 /tmp/.X11-unix || true
fi

ninja -C "$BUILD_DIR"
make -C "$SCRIPT_DIR" frame-client

export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_HEADLESS_OUTPUTS=2
export XDG_RUNTIME_DIR="$(mktemp -d /tmp/wex-frame-XXXXXX)"
chmod 700 "$XDG_RUNTIME_DIR"
export WAYLAND_DISPLAY="wayland-0"
export WEX_SHELL_SOCKET="wex-shell-0"

CONFIG_DIR="$XDG_RUNTIME_DIR/labwc-cfg"
mkdir -p "$CONFIG_DIR"
cat << 'EOF' > "$CONFIG_DIR/rc.xml"
<?xml version="1.0"?>
<labwc_config>
  <core><decoration>none</decoration></core>
  <placement><policy>center</policy></placement>
</labwc_config>
EOF

"$BUILD_DIR/labwc" -C "$CONFIG_DIR" > "$XDG_RUNTIME_DIR/labwc.log" 2>&1 &
COMPOSITOR_PID=$!
cleanup() {
    kill "$COMPOSITOR_PID" 2>/dev/null || true
    wait "$COMPOSITOR_PID" 2>/dev/null || true
    rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT

for i in {1..50}; do
    [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && [ -S "$XDG_RUNTIME_DIR/wex-shell-0" ] && break
    sleep 0.1
done
[ -S "$XDG_RUNTIME_DIR/wex-shell-0" ] || { echo "ERROR: compositor sockets missing"; cat "$XDG_RUNTIME_DIR/labwc.log"; exit 1; }
sleep 0.5

frames_on() {   # frames_on NAME: the frame events while a window animates fullscreen on the output NAME
    "$SCRIPT_DIR/frame-client" --socket wex-shell-0 --output "$1" --seconds 2 | tee /dev/stderr | sed -n 's/^frames=//p'
}

RC=0
# Both layouts: which output is primary must not depend on the order the outputs were created or listed in
check_layout() {   # check_layout PRIMARY OTHER: PRIMARY at the layout origin, OTHER to its right
    local primary_name=$1 other_name=$2 primary other
    WAYLAND_DISPLAY=wex-shell-0 wlr-randr \
        --output "$primary_name" --custom-mode 1280x800 --pos 0,0 \
        --output "$other_name" --custom-mode 1280x800 --pos 1280,0
    sleep 0.5
    primary=$(frames_on "$primary_name")
    other=$(frames_on "$other_name")
    echo "$primary_name at 0,0: frame events with it animating: $primary, with $other_name (1280,0) animating: $other"
    if [ "${primary:-0}" -ge 20 ]; then echo "PASS $primary_name primary: the clock runs with it animating"
    else RC=1; echo "FAIL $primary_name primary: expected at least 20 frame events, got ${primary:-none}"; fi
    if [ "${other:-99}" -le 2 ]; then echo "PASS $primary_name primary: the clock ignores $other_name"
    else RC=1; echo "FAIL $primary_name primary: expected at most 2 frame events from $other_name, got ${other:-none}"; fi
}
check_layout HEADLESS-1 HEADLESS-2
check_layout HEADLESS-2 HEADLESS-1
exit $RC
