/* SPDX-License-Identifier: GPL-2.0-only */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "shm-common.h"
#include "wex-window-v1-client-protocol.h"
#include "xx-zones-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static volatile sig_atomic_t g_running = 1;

static void
sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

struct virtual_input {
	struct zwlr_virtual_pointer_v1 *vpointer;
	struct zwp_virtual_keyboard_v1 *vkeyboard;
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *keymap;
	uint32_t mod_alt;
	uint32_t mod_super;
};

static bool
virtual_input_init(struct virtual_input *vi, struct wex_context *ctx)
{
	memset(vi, 0, sizeof(*vi));

	if (!ctx->virtual_pointer_manager || !ctx->virtual_keyboard_manager) {
		fprintf(stderr, "virtual pointer or keyboard manager missing\n");
		return false;
	}

	vi->vpointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
		ctx->virtual_pointer_manager, ctx->seat);
	if (!vi->vpointer) {
		fprintf(stderr, "failed to create virtual pointer\n");
		return false;
	}

	vi->vkeyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		ctx->virtual_keyboard_manager, ctx->seat);
	if (!vi->vkeyboard) {
		fprintf(stderr, "failed to create virtual keyboard\n");
		return false;
	}

	vi->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { 0 };
	vi->keymap = xkb_keymap_new_from_names(vi->xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!vi->keymap) {
		fprintf(stderr, "failed to compile default xkb keymap\n");
		return false;
	}

	char *keymap_str = xkb_keymap_get_as_string(vi->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t size = strlen(keymap_str) + 1;
	int fd = memfd_create("test-keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, keymap_str, size) != (ssize_t)size) {
		fprintf(stderr, "failed to write keymap fd\n");
		free(keymap_str);
		if (fd >= 0) close(fd);
		return false;
	}
	free(keymap_str);

	zwp_virtual_keyboard_v1_keymap(vi->vkeyboard,
		WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);

	xkb_mod_index_t alt_idx = xkb_keymap_mod_get_index(vi->keymap, XKB_MOD_NAME_ALT);
	xkb_mod_index_t super_idx = xkb_keymap_mod_get_index(vi->keymap, XKB_MOD_NAME_LOGO);
	vi->mod_alt = (alt_idx != XKB_MOD_INVALID) ? (1 << alt_idx) : (1 << 3);
	vi->mod_super = (super_idx != XKB_MOD_INVALID) ? (1 << super_idx) : (1 << 6);

	wl_display_roundtrip(ctx->display);
	return true;
}

static void
virtual_input_finish(struct virtual_input *vi)
{
	if (vi->vpointer) {
		zwlr_virtual_pointer_v1_destroy(vi->vpointer);
	}
	if (vi->vkeyboard) {
		zwp_virtual_keyboard_v1_destroy(vi->vkeyboard);
	}
	if (vi->keymap) {
		xkb_keymap_unref(vi->keymap);
	}
	if (vi->xkb_ctx) {
		xkb_context_unref(vi->xkb_ctx);
	}
}

static void
virtual_pointer_click(struct virtual_input *vi, struct wex_context *ctx, int x, int y)
{
	zwlr_virtual_pointer_v1_motion_absolute(vi->vpointer, 0,
		x, y, 1280, 800);
	zwlr_virtual_pointer_v1_frame(vi->vpointer);
	wl_display_flush(ctx->display);
	usleep(20000);

	zwlr_virtual_pointer_v1_button(vi->vpointer, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(vi->vpointer);
	wl_display_flush(ctx->display);
	usleep(20000);

	zwlr_virtual_pointer_v1_button(vi->vpointer, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(vi->vpointer);
	wl_display_flush(ctx->display);
	usleep(20000);

	wl_display_roundtrip(ctx->display);
}

static void
virtual_keyboard_combo(struct virtual_input *vi, struct wex_context *ctx, uint32_t mods, uint32_t key)
{
	zwp_virtual_keyboard_v1_modifiers(vi->vkeyboard, mods, 0, 0, 0);
	wl_display_flush(ctx->display);
	usleep(20000);

	zwp_virtual_keyboard_v1_key(vi->vkeyboard, 0, key, WL_KEYBOARD_KEY_STATE_PRESSED);
	wl_display_flush(ctx->display);
	usleep(20000);

	zwp_virtual_keyboard_v1_key(vi->vkeyboard, 0, key, WL_KEYBOARD_KEY_STATE_RELEASED);
	zwp_virtual_keyboard_v1_modifiers(vi->vkeyboard, 0, 0, 0, 0);
	wl_display_flush(ctx->display);
	usleep(20000);

	wl_display_roundtrip(ctx->display);
}

static void
dispatch_wait(struct wex_context *ctx, int msec)
{
	int remaining = msec;
	while (g_running && remaining > 0) {
		struct pollfd pfd = {
			.fd = wl_display_get_fd(ctx->display),
			.events = POLLIN,
		};
		wl_display_flush(ctx->display);
		int timeout = remaining > 50 ? 50 : remaining;
		if (poll(&pfd, 1, timeout) > 0) {
			wl_display_dispatch(ctx->display);
		}
		remaining -= timeout;
	}
}

static void
barrier_signal_and_wait(const char *dir, const char *signal_file, const char *wait_file)
{
	if (!dir) return;
	char s_path[512], w_path[512];
	snprintf(s_path, sizeof(s_path), "%s/%s", dir, signal_file);
	snprintf(w_path, sizeof(w_path), "%s/%s", dir, wait_file);
	FILE *f = fopen(s_path, "w");
	if (f) {
		fputs("1\n", f);
		fclose(f);
	}
	for (int i = 0; i < 200; i++) {
		if (access(w_path, F_OK) == 0) {
			return;
		}
		usleep(20000);
	}
	fprintf(stderr, "barrier timeout waiting for %s\n", wait_file);
}

int
main(int argc, char **argv)
{
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	const char *test_name = "1";
	const char *socket_name = NULL;
	const char *barrier_dir = NULL;
	int wait_secs = 2;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--test") == 0 && i + 1 < argc) {
			test_name = argv[++i];
		} else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--barrier-dir") == 0 && i + 1 < argc) {
			barrier_dir = argv[++i];
		} else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
			wait_secs = atoi(argv[++i]);
		}
	}

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		return 1;
	}

	struct virtual_input vi;
	if (!virtual_input_init(&vi, &ctx)) {
		wex_context_finish(&ctx);
		return 1;
	}

	struct xx_zone_v1 *zone = NULL;
	if (ctx.zone_manager) {
		zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
		wl_display_roundtrip(ctx.display);
	}

	int ret = 0;

	/* TEST 1: Two normal wex windows A (below) and B (above), B activated.
	 * Click A: A receives wl_pointer.button; NO wl_keyboard.enter for A;
	 * pixels show B still above A. Then activate(A): A gets keyboard enter and is raised. */
	if (strcmp(test_name, "1") == 0 || strcmp(test_name, "click-focus-stack") == 0) {
		printf("[policy-client] Running Test 1: click-focus-stack...\n");

		struct wex_window_obj winA, winB;
		/* Window A: Red 200x200 at (100, 100) */
		wex_window_init(&winA, &ctx, "WinA", 200, 200, 0xFFFF0000);
		if (zone) {
			winA.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, winA.xdg_toplevel);
			xx_zone_v1_add_item(zone, winA.zone_item);
			xx_zone_item_v1_set_position(winA.zone_item, 100, 100);
		}
		winA.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, winA.xdg_toplevel);
		wex_window_v1_set_layer(winA.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
		wl_surface_commit(winA.surface);

		/* Window B: Blue 200x200 at (150, 150) */
		wex_window_init(&winB, &ctx, "WinB", 200, 200, 0xFF0000FF);
		if (zone) {
			winB.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, winB.xdg_toplevel);
			xx_zone_v1_add_item(zone, winB.zone_item);
			xx_zone_item_v1_set_position(winB.zone_item, 150, 150);
		}
		winB.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, winB.xdg_toplevel);
		wex_window_v1_set_layer(winB.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
		wl_surface_commit(winB.surface);

		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&winA);
		wex_window_commit_buffer(&winB);
		wl_display_roundtrip(ctx.display);

		/* Activate B */
		wex_window_v1_activate(winB.wex_win);
		wl_display_roundtrip(ctx.display);

		if (ctx.keyboard_focus_surface != winB.surface) {
			fprintf(stderr, "FAIL: Window B did not get initial keyboard focus\n");
			ret = 1;
		}

		/* Click Window A at non-overlapping area (120, 120) */
		ctx.button_event_count = 0;
		virtual_pointer_click(&vi, &ctx, 120, 120);

		if (ctx.button_event_count == 0 || ctx.pointer_focus_surface != winA.surface) {
			fprintf(stderr, "FAIL: Window A did not receive pointer button event\n");
			ret = 1;
		}

		if (ctx.keyboard_focus_surface == winA.surface) {
			fprintf(stderr, "FAIL: Window A stole keyboard focus on click!\n");
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: Window A received button click without stealing keyboard focus\n");
		}

		if (barrier_dir) {
			barrier_signal_and_wait(barrier_dir, "b1_clicked", "b1_continue");
		} else if (wait_secs > 0) {
			dispatch_wait(&ctx, wait_secs * 1000);
		}

		/* Now activate A */
		printf("[policy-client] Activating Window A...\n");
		wex_window_v1_activate(winA.wex_win);
		wl_display_roundtrip(ctx.display);

		if (ctx.keyboard_focus_surface != winA.surface) {
			fprintf(stderr, "FAIL: Window A did not receive keyboard focus on activate()\n");
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: Window A received keyboard focus on activate()\n");
		}

		if (barrier_dir) {
			barrier_signal_and_wait(barrier_dir, "b1_activated", "b1_finish");
		} else if (wait_secs > 0) {
			dispatch_wait(&ctx, wait_secs * 1000);
		}

		wex_window_finish(&winB);
		wex_window_finish(&winA);
	}
	/* TEST 2: A new wex window maps: no keyboard enter until activate(). */
	else if (strcmp(test_name, "2") == 0 || strcmp(test_name, "map-unfocused") == 0) {
		printf("[policy-client] Running Test 2: map-unfocused...\n");

		struct wex_window_obj win;
		wex_window_init(&win, &ctx, "MapTest", 200, 200, 0xFF00AA00);
		if (zone) {
			win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win.xdg_toplevel);
			xx_zone_v1_add_item(zone, win.zone_item);
			xx_zone_item_v1_set_position(win.zone_item, 100, 100);
		}
		win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win.xdg_toplevel);
		wex_window_v1_set_layer(win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
		wl_surface_commit(win.surface);

		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&win);
		wl_display_roundtrip(ctx.display);

		if (ctx.keyboard_focus_surface == win.surface) {
			fprintf(stderr, "FAIL: Newly mapped wex window received keyboard focus immediately!\n");
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: Newly mapped wex window has no keyboard focus\n");
		}

		wex_window_v1_activate(win.wex_win);
		wl_display_roundtrip(ctx.display);

		if (ctx.keyboard_focus_surface != win.surface) {
			fprintf(stderr, "FAIL: wex window did not get keyboard focus on activate()\n");
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: wex window received keyboard focus after activate()\n");
		}

		wex_window_finish(&win);
	}
	/* TEST 3: The focused wex window closes: labwc does not focus another wex window. */
	else if (strcmp(test_name, "3") == 0 || strcmp(test_name, "close-unfocused") == 0) {
		printf("[policy-client] Running Test 3: close-unfocused...\n");

		struct wex_window_obj win1, win2;
		wex_window_init(&win1, &ctx, "Win1", 200, 200, 0xFFFF0000);
		win1.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win1.xdg_toplevel);
		wl_surface_commit(win1.surface);

		wex_window_init(&win2, &ctx, "Win2", 200, 200, 0xFF0000FF);
		win2.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win2.xdg_toplevel);
		wl_surface_commit(win2.surface);

		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&win1);
		wex_window_commit_buffer(&win2);
		wl_display_roundtrip(ctx.display);

		wex_window_v1_activate(win1.wex_win);
		wl_display_roundtrip(ctx.display);

		if (ctx.keyboard_focus_surface != win1.surface) {
			fprintf(stderr, "FAIL: Win1 not focused\n");
			ret = 1;
		}

		printf("[policy-client] Closing focused Win1...\n");
		wex_window_finish(&win1);
		wl_display_roundtrip(ctx.display);

		if (ctx.keyboard_focus_surface == win2.surface) {
			fprintf(stderr, "FAIL: labwc focused Win2 after Win1 closed!\n");
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: labwc did not focus Win2 when Win1 closed\n");
		}

		wex_window_finish(&win2);
	}
	/* TEST 4: With a wex window activated, send labwc's default bindings for
	 * maximize (W-a), snap (A-Left) and the window menu (A-Space):
	 * geometry unchanged, no menu. */
	else if (strcmp(test_name, "4") == 0 || strcmp(test_name, "keybinds-ignored") == 0) {
		printf("[policy-client] Running Test 4: keybinds-ignored...\n");

		struct wex_window_obj win;
		wex_window_init(&win, &ctx, "KeybindTest", 300, 200, 0xFF8844AA);
		if (zone) {
			win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win.xdg_toplevel);
			xx_zone_v1_add_item(zone, win.zone_item);
			xx_zone_item_v1_set_position(win.zone_item, 200, 200);
		}
		win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win.xdg_toplevel);
		wl_surface_commit(win.surface);

		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&win);
		wl_display_roundtrip(ctx.display);

		wex_window_v1_activate(win.wex_win);
		wl_display_roundtrip(ctx.display);

		if (barrier_dir) {
			barrier_signal_and_wait(barrier_dir, "b4_before", "b4_continue");
		}

		int orig_w = win.configured_width > 0 ? win.configured_width : win.width;
		int orig_h = win.configured_height > 0 ? win.configured_height : win.height;

		/* Send Super+A (Maximize) */
		printf("[policy-client] Sending Super+A...\n");
		virtual_keyboard_combo(&vi, &ctx, vi.mod_super, KEY_A);
		dispatch_wait(&ctx, 200);

		if (win.configured_width != orig_w || win.configured_height != orig_h) {
			fprintf(stderr, "FAIL: Window maximized via Super+A keybinding (%dx%d -> %dx%d)\n",
				orig_w, orig_h, win.configured_width, win.configured_height);
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: Super+A ignored, geometry unchanged (%dx%d)\n",
				orig_w, orig_h);
		}

		/* Send Alt+Left (Snap to left edge) */
		printf("[policy-client] Sending Alt+Left...\n");
		virtual_keyboard_combo(&vi, &ctx, vi.mod_alt, KEY_LEFT);
		dispatch_wait(&ctx, 200);

		if (win.configured_width != orig_w || win.configured_height != orig_h) {
			fprintf(stderr, "FAIL: Window snapped via Alt+Left keybinding\n");
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: Alt+Left ignored, geometry unchanged\n");
		}

		/* Send Alt+Space (Window menu) */
		printf("[policy-client] Sending Alt+Space...\n");
		virtual_keyboard_combo(&vi, &ctx, vi.mod_alt, KEY_SPACE);
		dispatch_wait(&ctx, 200);

		if (barrier_dir) {
			barrier_signal_and_wait(barrier_dir, "b4_after", "b4_finish");
		} else if (wait_secs > 0) {
			dispatch_wait(&ctx, wait_secs * 1000);
		}

		printf("[policy-client] SUCCESS: Keybindings test completed\n");
		wex_window_finish(&win);
	}
	/* TEST 5: Window switcher (A-Tab) does not select or raise a wex window. */
	else if (strcmp(test_name, "5") == 0 || strcmp(test_name, "switcher-ignored") == 0) {
		printf("[policy-client] Running Test 5: switcher-ignored...\n");

		struct wex_window_obj win1, win2;
		wex_window_init(&win1, &ctx, "Win1", 200, 200, 0xFFFF0000);
		win1.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win1.xdg_toplevel);
		wl_surface_commit(win1.surface);

		wex_window_init(&win2, &ctx, "Win2", 200, 200, 0xFF0000FF);
		win2.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win2.xdg_toplevel);
		wl_surface_commit(win2.surface);

		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&win1);
		wex_window_commit_buffer(&win2);
		wl_display_roundtrip(ctx.display);

		wex_window_v1_activate(win1.wex_win);
		wl_display_roundtrip(ctx.display);

		/* Send Alt+Tab */
		printf("[policy-client] Sending Alt+Tab...\n");
		virtual_keyboard_combo(&vi, &ctx, vi.mod_alt, KEY_TAB);
		dispatch_wait(&ctx, 300);

		/* Win2 must not have been selected */
		if (ctx.keyboard_focus_surface == win2.surface) {
			fprintf(stderr, "FAIL: Window switcher selected wex window Win2!\n");
			ret = 1;
		} else {
			printf("[policy-client] SUCCESS: Window switcher did not select wex window\n");
		}

		wex_window_finish(&win2);
		wex_window_finish(&win1);
	}
	/* TEST 7: A non-wex client (plain test client without wex_window_v1):
	 * click-to-focus and bindings still behave as stock labwc. */
	else if (strcmp(test_name, "7") == 0 || strcmp(test_name, "non-wex") == 0) {
		printf("[policy-client] Running Test 7: non-wex client...\n");

		struct wex_window_obj plain1, plain2;
		/* Create two plain windows WITHOUT wex_window_v1 */
		wex_window_init(&plain1, &ctx, "Plain1", 200, 200, 0xFF112233);
		if (zone) {
			plain1.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, plain1.xdg_toplevel);
			xx_zone_v1_add_item(zone, plain1.zone_item);
			xx_zone_item_v1_set_position(plain1.zone_item, 100, 100);
		}
		wl_surface_commit(plain1.surface);

		wex_window_init(&plain2, &ctx, "Plain2", 200, 200, 0xFF445566);
		if (zone) {
			plain2.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, plain2.xdg_toplevel);
			xx_zone_v1_add_item(zone, plain2.zone_item);
			xx_zone_item_v1_set_position(plain2.zone_item, 350, 100);
		}
		wl_surface_commit(plain2.surface);

		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&plain1);
		wex_window_commit_buffer(&plain2);
		wl_display_roundtrip(ctx.display);

		/* Click on Plain1 at (150, 150) -> stock click-to-focus */
		printf("[policy-client] Clicking Plain1 at (150, 150)...\n");
		virtual_pointer_click(&vi, &ctx, 150, 150);
		dispatch_wait(&ctx, 200);

		if (ctx.keyboard_focus_surface == plain1.surface) {
			printf("[policy-client] SUCCESS: Non-wex window Plain1 focused on click (stock behavior)\n");
		} else {
			fprintf(stderr, "FAIL: Non-wex window Plain1 did not focus on click!\n");
			ret = 1;
		}

		/* Click on Plain2 at (400, 150) -> stock click-to-focus */
		printf("[policy-client] Clicking Plain2 at (400, 150)...\n");
		virtual_pointer_click(&vi, &ctx, 400, 150);
		dispatch_wait(&ctx, 200);

		if (ctx.keyboard_focus_surface == plain2.surface) {
			printf("[policy-client] SUCCESS: Non-wex window Plain2 focused on click (stock behavior)\n");
		} else {
			fprintf(stderr, "FAIL: Non-wex window Plain2 did not focus on click!\n");
			ret = 1;
		}

		/* Now test Super+A (Maximize) on focused plain window Plain2 */
		int orig_w = plain2.configured_width > 0 ? plain2.configured_width : plain2.width;
		printf("[policy-client] Sending Super+A on Plain2...\n");
		virtual_keyboard_combo(&vi, &ctx, vi.mod_super, KEY_A);
		dispatch_wait(&ctx, 300);

		if (plain2.configured_width > orig_w) {
			printf("[policy-client] SUCCESS: Non-wex window maximized via Super+A (%d -> %d)\n",
				orig_w, plain2.configured_width);
		} else {
			fprintf(stderr, "FAIL: Non-wex window did not maximize via Super+A\n");
			ret = 1;
		}

		wex_window_finish(&plain2);
		wex_window_finish(&plain1);
	}

	virtual_input_finish(&vi);
	if (zone) {
		xx_zone_v1_destroy(zone);
	}
	wex_context_finish(&ctx);
	return ret;
}
