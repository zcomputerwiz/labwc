/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 WineExplorer contributors */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>

#include "shm-common.h"
#include "xx-zones-v1-client-protocol.h"
#include "wex-window-v1-client-protocol.h"

static volatile sig_atomic_t g_running = 1;

static void
sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static void
barrier_signal_and_wait(const char *dir, const char *signal_file, const char *wait_file)
{
	if (!dir) {
		return;
	}
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

struct registry_tracker {
	uint32_t wex_mgr_name;
	uint32_t wex_mgr_version;
	bool found_wex_mgr;
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	(void)registry;
	struct registry_tracker *tracker = data;
	if (strcmp(interface, wex_window_manager_v1_interface.name) == 0) {
		tracker->wex_mgr_name = name;
		tracker->wex_mgr_version = version;
		tracker->found_wex_mgr = true;
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener tracker_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static int
test_version_negotiation(const char *socket_name)
{
	printf("[corner-client] testing version negotiation on socket '%s'...\n",
		socket_name ? socket_name : "default");

	struct wl_display *display = wl_display_connect(socket_name);
	if (!display) {
		fprintf(stderr, "failed to connect to socket '%s'\n", socket_name ? socket_name : "default");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	struct registry_tracker tracker = { 0 };
	wl_registry_add_listener(registry, &tracker_listener, &tracker);
	wl_display_roundtrip(display);

	if (!tracker.found_wex_mgr) {
		fprintf(stderr, "ERROR: wex_window_manager_v1 global not found\n");
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}

	printf("[corner-client] advertised wex_window_manager_v1 version: %u (expected >= 12)\n",
		tracker.wex_mgr_version);
	if (tracker.wex_mgr_version < 12) {
		fprintf(stderr, "ERROR: expected advertised version >= 12, got %u\n",
			tracker.wex_mgr_version);
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}

	/* Test 1: Bind at version 11 (old peer simulation) */
	printf("[corner-client] binding wex_window_manager_v1 at version 11 (old peer)...\n");
	struct wex_window_manager_v1 *mgr11 = wl_registry_bind(registry,
		tracker.wex_mgr_name, &wex_window_manager_v1_interface, 11);
	if (!mgr11) {
		fprintf(stderr, "ERROR: failed to bind at version 11\n");
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	uint32_t bound_v11 = wl_proxy_get_version((struct wl_proxy *)mgr11);
	if (bound_v11 != 11) {
		fprintf(stderr, "ERROR: proxy version is %u, expected 11\n", bound_v11);
		wex_window_manager_v1_destroy(mgr11);
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	wl_display_roundtrip(display);
	wex_window_manager_v1_destroy(mgr11);
	printf("[corner-client] SUCCESS: version 11 binding succeeded\n");

	/* Test 2: Bind at version 12 (new peer) */
	printf("[corner-client] binding wex_window_manager_v1 at version 12 (new peer)...\n");
	struct wex_window_manager_v1 *mgr12 = wl_registry_bind(registry,
		tracker.wex_mgr_name, &wex_window_manager_v1_interface, 12);
	if (!mgr12) {
		fprintf(stderr, "ERROR: failed to bind at version 12\n");
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	uint32_t bound_v12 = wl_proxy_get_version((struct wl_proxy *)mgr12);
	if (bound_v12 != 12) {
		fprintf(stderr, "ERROR: proxy version is %u, expected 12\n", bound_v12);
		wex_window_manager_v1_destroy(mgr12);
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	wl_display_roundtrip(display);
	wex_window_manager_v1_destroy(mgr12);
	printf("[corner-client] SUCCESS: version 12 binding succeeded\n");

	/* Test 3: Bind at version 13 (v13 peer with set_effects) */
	printf("[corner-client] binding wex_window_manager_v1 at version 13 (v13 peer)...\n");
	struct wex_window_manager_v1 *mgr13 = wl_registry_bind(registry,
		tracker.wex_mgr_name, &wex_window_manager_v1_interface, 13);
	if (!mgr13) {
		fprintf(stderr, "ERROR: failed to bind at version 13\n");
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	uint32_t bound_v13 = wl_proxy_get_version((struct wl_proxy *)mgr13);
	if (bound_v13 != 13) {
		fprintf(stderr, "ERROR: proxy version is %u, expected 13\n", bound_v13);
		wex_window_manager_v1_destroy(mgr13);
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	wl_display_roundtrip(display);
	wex_window_manager_v1_destroy(mgr13);
	printf("[corner-client] SUCCESS: version 13 binding succeeded\n");

	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	return 0;
}

static int
test_denial(const char *unprivileged_socket)
{
	printf("[corner-client] testing unprivileged socket denial on '%s'...\n",
		unprivileged_socket ? unprivileged_socket : "default");

	struct wl_display *display = wl_display_connect(unprivileged_socket);
	if (!display) {
		fprintf(stderr, "failed to connect to socket '%s'\n", unprivileged_socket);
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	struct registry_tracker tracker = { 0 };
	wl_registry_add_listener(registry, &tracker_listener, &tracker);
	wl_display_roundtrip(display);

	if (tracker.found_wex_mgr) {
		fprintf(stderr, "ERROR: wex_window_manager_v1 should NOT be advertised on unprivileged socket '%s'\n",
			unprivileged_socket);
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}

	printf("[corner-client] SUCCESS: wex_window_manager_v1 is withheld from unprivileged socket '%s'\n",
		unprivileged_socket);

	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	return 0;
}

static int
test_surface_destroy(const char *socket_name)
{
	printf("[corner-client] testing surface destruction while corner radius is active...\n");

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		fprintf(stderr, "failed to init wex_context on '%s'\n", socket_name);
		return 1;
	}
	if (!ctx.wex_window_manager) {
		fprintf(stderr, "wex_window_manager not available\n");
		wex_context_finish(&ctx);
		return 1;
	}

	struct wex_window_obj win;
	if (!wex_window_init(&win, &ctx, "DestroyTest", 200, 200, 0xFF00FF00)) {
		fprintf(stderr, "failed to init window\n");
		wex_context_finish(&ctx);
		return 1;
	}
	win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win.xdg_toplevel);
	wex_window_v1_set_layer(win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	wl_surface_commit(win.surface);
	wl_display_roundtrip(ctx.display);

	wex_window_commit_buffer(&win);
	wex_window_v1_set_corner_radius(win.wex_win, 16);
	wl_surface_commit(win.surface);
	wl_display_roundtrip(ctx.display);

	/* Destroy surface and toplevel while corner radius is active */
	printf("[corner-client] destroying window while radius 16 is active...\n");
	wex_window_finish(&win);
	wl_display_roundtrip(ctx.display);

	if (wl_display_get_error(ctx.display) != 0) {
		fprintf(stderr, "ERROR: display in error state after window destruction\n");
		wex_context_finish(&ctx);
		return 1;
	}

	/* Verify subsequent window creation works cleanly */
	struct wex_window_obj win2;
	if (!wex_window_init(&win2, &ctx, "DestroyTest2", 100, 100, 0xFF0000FF)) {
		fprintf(stderr, "failed to init second window\n");
		wex_context_finish(&ctx);
		return 1;
	}
	win2.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win2.xdg_toplevel);
	wl_surface_commit(win2.surface);
	wl_display_roundtrip(ctx.display);

	wex_window_commit_buffer(&win2);
	wex_window_v1_set_corner_radius(win2.wex_win, 8);
	wl_surface_commit(win2.surface);
	wl_display_roundtrip(ctx.display);
	wex_window_finish(&win2);
	wl_display_roundtrip(ctx.display);

	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: surface destruction while radius active passed without fault\n");
	return 0;
}

static int
test_render(const char *socket_name, const char *barrier_dir)
{
	printf("[corner-client] running visual corner rendering tests...\n");

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		fprintf(stderr, "failed to init wex_context on '%s'\n", socket_name);
		return 1;
	}
	if (!ctx.wex_window_manager) {
		fprintf(stderr, "wex_window_manager not available\n");
		wex_context_finish(&ctx);
		return 1;
	}

	struct xx_zone_v1 *zone = NULL;
	if (ctx.zone_manager) {
		zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
		wl_display_roundtrip(ctx.display);
	}

	/* Background window: 1280x800 solid navy 0xFF112233 */
	struct wex_window_obj bg_win;
	if (!wex_window_init(&bg_win, &ctx, "BgWindow", 1280, 800, 0xFF112233)) {
		fprintf(stderr, "failed to create bg_win\n");
		wex_context_finish(&ctx);
		return 1;
	}
	bg_win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, bg_win.xdg_toplevel);
	wex_window_v1_set_layer(bg_win.wex_win, WEX_WINDOW_V1_LAYER_DESKTOP);
	if (zone) {
		bg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, bg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, bg_win.zone_item);
		xx_zone_item_v1_set_position(bg_win.zone_item, 0, 0);
	}
	wl_surface_commit(bg_win.surface);

	/* Foreground window: 200x200 solid red 0xFFFF0000 at (200, 200) */
	struct wex_window_obj fg_win;
	if (!wex_window_init(&fg_win, &ctx, "FgWindow", 200, 200, 0xFFFF0000)) {
		fprintf(stderr, "failed to create fg_win\n");
		wex_window_finish(&bg_win);
		wex_context_finish(&ctx);
		return 1;
	}
	fg_win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, fg_win.xdg_toplevel);
	wex_window_v1_set_layer(fg_win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		fg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, fg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, fg_win.zone_item);
		xx_zone_item_v1_set_position(fg_win.zone_item, 200, 200);
	}
	wl_surface_commit(fg_win.surface);

	wl_display_roundtrip(ctx.display);

	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&fg_win);
	wl_display_roundtrip(ctx.display);

	/* Step 1: Set radius 16 */
	printf("[corner-client] Step 1: setting corner radius to 16...\n");
	wex_window_v1_set_corner_radius(fg_win.wex_win, 16);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_radius_16", "b_radius_16_cont");

	/* Step 2: Clear radius to 0 */
	printf("[corner-client] Step 2: clearing corner radius to 0...\n");
	wex_window_v1_set_corner_radius(fg_win.wex_win, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_radius_0", "b_radius_0_cont");

	/* Step 3: Clamping test (set radius to 100, clamped to 32) */
	printf("[corner-client] Step 3: setting corner radius to 100 (should clamp to 32)...\n");
	wex_window_v1_set_corner_radius(fg_win.wex_win, 100);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_radius_clamp", "b_radius_clamp_cont");

	wex_window_finish(&fg_win);
	wex_window_finish(&bg_win);
	if (zone) {
		xx_zone_v1_destroy(zone);
	}
	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: render test finished\n");
	return 0;
}

static char g_effects_handle[33];
static void
effects_handle_cb(void *data, struct wex_window_v1 *window, const char *handle)
{
	(void)data;
	(void)window;
	snprintf(g_effects_handle, sizeof(g_effects_handle), "%s", handle);
}
static const struct wex_window_v1_listener effects_listener = {
	.handle = effects_handle_cb,
};

static int
test_effects(const char *socket_name, const char *barrier_dir)
{
	printf("[corner-client] running visual effects (v13 set_effects) tests...\n");

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		fprintf(stderr, "failed to init wex_context on '%s'\n", socket_name);
		return 1;
	}
	if (!ctx.wex_window_manager) {
		fprintf(stderr, "wex_window_manager not available\n");
		wex_context_finish(&ctx);
		return 1;
	}

	struct xx_zone_v1 *zone = NULL;
	if (ctx.zone_manager) {
		zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
		wl_display_roundtrip(ctx.display);
	}

	/* Background window: 1280x800 solid navy 0xFF112233 */
	struct wex_window_obj bg_win;
	if (!wex_window_init(&bg_win, &ctx, "BgWindow", 1280, 800, 0xFF112233)) {
		fprintf(stderr, "failed to create bg_win\n");
		wex_context_finish(&ctx);
		return 1;
	}
	bg_win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, bg_win.xdg_toplevel);
	wex_window_v1_set_layer(bg_win.wex_win, WEX_WINDOW_V1_LAYER_DESKTOP);
	if (zone) {
		bg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, bg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, bg_win.zone_item);
		xx_zone_item_v1_set_position(bg_win.zone_item, 0, 0);
	}
	wl_surface_commit(bg_win.surface);

	/* Foreground window: 200x200 half-transparent (alpha 0x80) at (200, 200) */
	/* 0x80000000: 50% translucent black */
	struct wex_window_obj fg_win;
	if (!wex_window_init(&fg_win, &ctx, "FgEffectsWindow", 200, 200, 0x80000000)) {
		fprintf(stderr, "failed to create fg_win\n");
		wex_window_finish(&bg_win);
		wex_context_finish(&ctx);
		return 1;
	}
	fg_win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, fg_win.xdg_toplevel);
	wex_window_v1_add_listener(fg_win.wex_win, &effects_listener, NULL);
	wex_window_v1_set_layer(fg_win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		fg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, fg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, fg_win.zone_item);
		xx_zone_item_v1_set_position(fg_win.zone_item, 200, 200);
	}
	wl_surface_commit(fg_win.surface);

	wl_display_roundtrip(ctx.display);

	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&fg_win);
	wl_display_roundtrip(ctx.display);

	if (strlen(g_effects_handle) != 32) {
		fprintf(stderr, "ERROR: invalid window handle received: '%s'\n", g_effects_handle);
		wex_window_finish(&fg_win);
		wex_window_finish(&bg_win);
		if (zone) xx_zone_v1_destroy(zone);
		wex_context_finish(&ctx);
		return 1;
	}

	/* Step 1: Shadow flag (a) */
	printf("[corner-client] Step 1: setting shadow flag...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_effects_handle,
		0, WEX_WINDOW_MANAGER_V1_BACKDROP_NONE,
		WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_SHADOW,
		0, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_effects_shadow", "b_effects_shadow_cont");

	/* Step 2: Half-transparent shm buffer + backdrop gradient + tint 0xFFFF0000 + tint flag (b) */
	printf("[corner-client] Step 2: setting backdrop gradient + opaque red tint + shadow...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_effects_handle,
		0, WEX_WINDOW_MANAGER_V1_BACKDROP_GRADIENT,
		WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_SHADOW | WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_TINT,
		0xFFFF0000, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_effects_backdrop", "b_effects_backdrop_cont");

	/* Step 3: All-zero set_effects clears both (c) */
	printf("[corner-client] Step 3: clearing all effects with all-zero arguments...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_effects_handle,
		0, 0, 0, 0, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_effects_cleared", "b_effects_cleared_cont");

	/* Step 4: Stale handle and destroyed handle tests (d) */
	printf("[corner-client] Step 4: testing stale handle and destroyed window handle...\n");
	/* Unknown / stale handle: harmless no-op */
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, "stale_handle_deadbeef_00000000",
		16, WEX_WINDOW_MANAGER_V1_BACKDROP_MICA,
		WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_SHADOW,
		0xFFFFFFFF, 0, 0, 100, 100);
	wl_display_roundtrip(ctx.display);
	if (wl_display_get_error(ctx.display) != 0) {
		fprintf(stderr, "ERROR: stale handle caused display error\n");
		return 1;
	}

	/* Window that was destroyed: harmless no-op */
	static char temp_handle[33];
	struct wex_window_obj win3;
	if (!wex_window_init(&win3, &ctx, "TempWindow", 100, 100, 0xFF00FF00)) {
		fprintf(stderr, "failed to create temp window\n");
		return 1;
	}
	win3.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win3.xdg_toplevel);
	wex_window_v1_add_listener(win3.wex_win, &effects_listener, NULL);
	wl_surface_commit(win3.surface);
	wl_display_roundtrip(ctx.display);
	snprintf(temp_handle, sizeof(temp_handle), "%s", g_effects_handle);

	/* Destroy win3 */
	wex_window_finish(&win3);
	wl_display_roundtrip(ctx.display);

	/* Calling set_effects on the destroyed window's handle must do nothing and not crash */
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, temp_handle,
		16, WEX_WINDOW_MANAGER_V1_BACKDROP_ACRYLIC,
		WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_SHADOW,
		0xFF112233, 0, 0, 50, 50);
	wl_display_roundtrip(ctx.display);
	if (wl_display_get_error(ctx.display) != 0) {
		fprintf(stderr, "ERROR: destroyed window handle caused display error\n");
		return 1;
	}

	wex_window_finish(&fg_win);
	wex_window_finish(&bg_win);
	if (zone) {
		xx_zone_v1_destroy(zone);
	}
	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: effects scenario completed successfully\n");
	return 0;
}

int
main(int argc, char **argv)
{
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	const char *socket_name = "wex-shell-0";
	const char *normal_socket = "wayland-0";
	const char *scenario = "version-negotiation";
	const char *barrier_dir = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--normal-socket") == 0 && i + 1 < argc) {
			normal_socket = argv[++i];
		} else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
			scenario = argv[++i];
		} else if (strcmp(argv[i], "--barrier-dir") == 0 && i + 1 < argc) {
			barrier_dir = argv[++i];
		}
	}

	if (strcmp(scenario, "version-negotiation") == 0) {
		return test_version_negotiation(socket_name);
	} else if (strcmp(scenario, "denial") == 0) {
		return test_denial(normal_socket);
	} else if (strcmp(scenario, "surface-destroy") == 0) {
		return test_surface_destroy(socket_name);
	} else if (strcmp(scenario, "render") == 0) {
		return test_render(socket_name, barrier_dir);
	} else if (strcmp(scenario, "effects") == 0) {
		return test_effects(socket_name, barrier_dir);
	} else {
		fprintf(stderr, "Unknown scenario '%s'\n", scenario);
		return 1;
	}
}
