#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Mica's cached blur with two outputs. SceneFX keeps the optimized blur's cache per output; both outputs must refresh
# theirs when the desktop changes. Two headless outputs overlap (HEADLESS-1 at 0,0, HEADLESS-2 at 640,0), so the left
# part of the corner-client blur scenario's Mica window (its windows go to HEADLESS-2, the newest output) is drawn by
# both. After the wallpaper turns blue under Mica, the same spot of the window must be blue on each output; before, the
# first output to render cleared the node's single dirty flag and the other kept its old cache. Needs SceneFX and GLES2
# (llvmpipe), as run-blur.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${WEX_CHECK_COMP_BUILD:-$REPO_DIR/build-fx}"
OUT_DIR="${1:-$REPO_DIR/tests/wex/output-mica-outputs}"
RUNS="${RUNS:-3}"
mkdir -p "$OUT_DIR"
modprobe vgem 2>/dev/null || true
umount -l /tmp/.X11-unix 2>/dev/null || true
mkdir -m 1777 -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix || true

if [ ! -d "$BUILD_DIR" ]; then meson setup "$BUILD_DIR" "$REPO_DIR" -Dscenefx=enabled -Dman-pages=disabled; fi
ninja -C "$BUILD_DIR"
make -C "$SCRIPT_DIR" wex-window-v1-client-protocol.h wex-window-v1-protocol.c
make -C "$SCRIPT_DIR"

export WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDERER_ALLOW_SOFTWARE=1 WEX_BLUR=1 WEX_BLUR_TEST_VGEM=1
export WLR_HEADLESS_OUTPUTS=2 WAYLAND_DISPLAY=wayland-0 WEX_SHELL_SOCKET=wex-shell-0
FAILED=0

run_once()   # run_once TAG
{
    local tag=$1
    export XDG_RUNTIME_DIR="$(mktemp -d /tmp/wex-mica-outputs-XXXXXX)"
    chmod 700 "$XDG_RUNTIME_DIR"
    mkdir -p "$XDG_RUNTIME_DIR/cfg"
    cat << 'EOF' > "$XDG_RUNTIME_DIR/cfg/rc.xml"
<?xml version="1.0"?>
<labwc_config>
  <core><decoration>none</decoration></core>
  <placement><policy>center</policy></placement>
</labwc_config>
EOF
    "$BUILD_DIR/labwc" -C "$XDG_RUNTIME_DIR/cfg" > "$OUT_DIR/compositor-$tag.log" 2>&1 &
    local comp=$!
    for i in {1..50}; do [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && [ -S "$XDG_RUNTIME_DIR/wex-shell-0" ] && break; sleep 0.1; done
    sleep 0.5
    WAYLAND_DISPLAY=wex-shell-0 wlr-randr --output HEADLESS-1 --custom-mode 1280x800 --pos 0,0 \
        --output HEADLESS-2 --custom-mode 1280x800 --pos 640,0 || true
    sleep 0.5
    local barriers="$XDG_RUNTIME_DIR/barriers"
    mkdir -p "$barriers"
    "$SCRIPT_DIR/corner-client" --socket wex-shell-0 --scenario blur --barrier-dir "$barriers" > "$OUT_DIR/client-$tag.log" 2>&1 &
    local client=$!
    for b in b_blur_none b_blur_active b_blur_cleared b_blur_acrylic b_blur_mica b_blur_wallpaper b_blur_corners b_blur_shadow; do
        for i in {1..100}; do [ -f "$barriers/$b" ] && break; sleep 0.05; done
        case $b in
            b_blur_mica|b_blur_wallpaper)
                sleep 0.5
                for o in HEADLESS-1 HEADLESS-2; do WAYLAND_DISPLAY=wex-shell-0 grim -o $o "$OUT_DIR/$tag-$b-$o.png"; done ;;
        esac
        touch "$barriers/${b}_cont"
    done
    wait "$client" || true
    kill "$comp" 2>/dev/null || true; wait "$comp" 2>/dev/null || true
    rm -rf "$XDG_RUNTIME_DIR"
    # layout 1180,350 is inside the Mica window's left part: HEADLESS-1's 1180,350 and HEADLESS-2's 540,350
    check() { if python3 "$SCRIPT_DIR/verify.py" "$@" >/dev/null; then echo " [PASS] $tag: $name"; else echo " [FAIL] $tag: $name"; FAILED=1; fi; }
    for o in "HEADLESS-1 1180" "HEADLESS-2 540"; do
        set -- $o
        name="$1: Mica samples the white wallpaper (green high)"
        check check-range "$OUT_DIR/$tag-b_blur_mica-$1.png" "$2" 350 140 255 1
        name="$1: the blue wallpaper reaches Mica's cache (blue high)"
        check check-range "$OUT_DIR/$tag-b_blur_wallpaper-$1.png" "$2" 350 150 255 2
        name="$1: the old white sample is gone (red low)"
        check check-range "$OUT_DIR/$tag-b_blur_wallpaper-$1.png" "$2" 350 0 180 0
    done
}

for r in $(seq 1 "$RUNS"); do run_once "run$r"; done
exit $FAILED
