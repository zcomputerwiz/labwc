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

	printf("[corner-client] advertised wex_window_manager_v1 version: %u (expected >= 14)\n",
		tracker.wex_mgr_version);
	if (tracker.wex_mgr_version < 14) {
		fprintf(stderr, "ERROR: expected advertised version >= 14, got %u\n",
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

	/* Test 4: Bind at version 14 (v14 peer with set_thumbnails, set_peek, frame) */
	printf("[corner-client] binding wex_window_manager_v1 at version 14 (v14 peer)...\n");
	struct wex_window_manager_v1 *mgr14 = wl_registry_bind(registry,
		tracker.wex_mgr_name, &wex_window_manager_v1_interface, 14);
	if (!mgr14) {
		fprintf(stderr, "ERROR: failed to bind at version 14\n");
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	uint32_t bound_v14 = wl_proxy_get_version((struct wl_proxy *)mgr14);
	if (bound_v14 != 14) {
		fprintf(stderr, "ERROR: proxy version is %u, expected 14\n", bound_v14);
		wex_window_manager_v1_destroy(mgr14);
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}
	wl_display_roundtrip(display);
	wex_window_manager_v1_destroy(mgr14);
	printf("[corner-client] SUCCESS: version 14 binding succeeded\n");

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

static void
handle_capture_cb(void *data, struct wex_window_v1 *window, const char *handle)
{
	(void)window;
	char *buf = data;
	if (buf) {
		snprintf(buf, 33, "%s", handle);
	}
}
static const struct wex_window_v1_listener capture_listener = {
	.handle = handle_capture_cb,
};

static int
test_blur(const char *socket_name, const char *barrier_dir)
{
	printf("[corner-client] running visual backdrop blur scenario...\n");

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

	/* Background window: 1280x800 with hard vertical edge at x=640 (left white, right black) */
	struct wex_window_obj bg_win;
	if (!wex_window_init(&bg_win, &ctx, "BgBlurWindow", 1280, 800, 0xFF000000)) {
		fprintf(stderr, "failed to create bg_win\n");
		wex_context_finish(&ctx);
		return 1;
	}
	uint32_t *bg_px = (uint32_t *)bg_win.buffer.data;
	for (int y = 0; y < 800; y++) {
		for (int x = 0; x < 1280; x++) {
			bg_px[y * 1280 + x] = (x < 640) ? 0xFFFFFFFF : 0xFF000000;
		}
	}
	bg_win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, bg_win.xdg_toplevel);
	wex_window_v1_set_layer(bg_win.wex_win, WEX_WINDOW_V1_LAYER_DESKTOP);
	if (zone) {
		bg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, bg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, bg_win.zone_item);
		xx_zone_item_v1_set_position(bg_win.zone_item, 0, 0);
	}
	wl_surface_commit(bg_win.surface);

	/* A normal window between the wallpaper and the glass. Acrylic/Blur may
	 * sample it; Mica must not, because its optimized node lives in desktop_tree. */
	struct wex_window_obj blocker_win;
	if (!wex_window_init(&blocker_win, &ctx, "BlurBlockerWindow", 80, 80, 0xFFFF0000)) {
		fprintf(stderr, "failed to create blocker_win\n");
		wex_window_finish(&bg_win);
		wex_context_finish(&ctx);
		return 1;
	}
	blocker_win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager,
		blocker_win.xdg_toplevel);
	wex_window_v1_set_layer(blocker_win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		blocker_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager,
			blocker_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, blocker_win.zone_item);
		xx_zone_item_v1_set_position(blocker_win.zone_item, 540, 320);
	}
	wl_surface_commit(blocker_win.surface);
	wl_display_roundtrip(ctx.display); /* configure before the first buffer */
	wex_window_commit_buffer(&blocker_win);

	/* Foreground glass window: 400x300 at (440, 250) */
	/* Screen x=640 corresponds to window x=200 */
	/* Left half white with alpha 0x40 (premultiplied 0x40404040), right half black with alpha 0x40 (0x40000000) */
	struct wex_window_obj fg_win;
	if (!wex_window_init(&fg_win, &ctx, "FgBlurWindow", 400, 300, 0x00000000)) {
		fprintf(stderr, "failed to create fg_win\n");
		wex_window_finish(&blocker_win);
		wex_window_finish(&bg_win);
		wex_context_finish(&ctx);
		return 1;
	}
	uint32_t *fg_px = (uint32_t *)fg_win.buffer.data;
	for (int y = 0; y < 300; y++) {
		for (int x = 0; x < 400; x++) {
			fg_px[y * 400 + x] = (x < 200) ? 0x40404040 : 0x40000000;
		}
	}

	char g_blur_handle[33] = {0};
	fg_win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, fg_win.xdg_toplevel);
	wex_window_v1_add_listener(fg_win.wex_win, &capture_listener, g_blur_handle);
	wex_window_v1_set_layer(fg_win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		fg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, fg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, fg_win.zone_item);
		xx_zone_item_v1_set_position(fg_win.zone_item, 440, 250);
	}
	wl_surface_commit(fg_win.surface);

	wl_display_roundtrip(ctx.display);

	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&fg_win);
	wl_display_roundtrip(ctx.display);

	if (strlen(g_blur_handle) != 32) {
		fprintf(stderr, "ERROR: invalid window handle received: '%s'\n", g_blur_handle);
		wex_window_finish(&fg_win);
		wex_window_finish(&blocker_win);
		wex_window_finish(&bg_win);
		if (zone) xx_zone_v1_destroy(zone);
		wex_context_finish(&ctx);
		return 1;
	}

	/* Step 1: Backdrop NONE */
	printf("[corner-client] Step 1: backdrop NONE...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_blur_handle,
		0, WEX_WINDOW_MANAGER_V1_BACKDROP_NONE,
		0, 0, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_blur_none", "b_blur_none_cont");

	/* Step 2: Backdrop BLUR */
	printf("[corner-client] Step 2: backdrop BLUR...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_blur_handle,
		0, WEX_WINDOW_MANAGER_V1_BACKDROP_BLUR,
		0, 0, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_blur_active", "b_blur_active_cont");

	/* Step 3: All-zero set_effects clears blur */
	printf("[corner-client] Step 3: clearing effects...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_blur_handle,
		0, 0, 0, 0, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_blur_cleared", "b_blur_cleared_cont");

	/* Step 4: Acrylic uses the shared strong/noisy/saturated SceneFX preset. */
	printf("[corner-client] Step 4: backdrop ACRYLIC...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_blur_handle,
		0, WEX_WINDOW_MANAGER_V1_BACKDROP_ACRYLIC,
		0, 0, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_blur_acrylic", "b_blur_acrylic_cont");

	/* Step 5: Mica is sampled from desktop_tree and must ignore blocker_win. */
	printf("[corner-client] Step 5: backdrop MICA...\n");
	wex_window_manager_v1_set_effects(ctx.wex_window_manager, g_blur_handle,
		0, WEX_WINDOW_MANAGER_V1_BACKDROP_MICA,
		0, 0, 0, 0, 0, 0);
	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_blur_mica", "b_blur_mica_cont");

	/* Step 6: a new wallpaper must reach Mica's cached blur. */
	printf("[corner-client] Step 6: wallpaper turns blue under MICA...\n");
	for (int i = 0; i < 1280 * 800; i++) {
		bg_px[i] = 0xFF0000FF;
	}
	wex_window_commit_buffer(&bg_win);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_blur_wallpaper", "b_blur_wallpaper_cont");

	wex_window_finish(&fg_win);
	wex_window_finish(&blocker_win);
	wex_window_finish(&bg_win);
	if (zone) {
		xx_zone_v1_destroy(zone);
	}
	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: blur scenario completed successfully\n");
	return 0;
}

struct wex_thumbnail_record {
	char dest[32];
	char src[32];
	uint32_t flags;
	uint32_t opacity;
	int32_t dest_rect[4];
	int32_t source_rect[4];
};

static int
test_thumbnails(const char *socket_name, const char *barrier_dir)
{
	printf("[corner-client] running visual thumbnails (v14 set_thumbnails) tests...\n");

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

	/* Background window: 1280x800 solid dark gray 0xFF222222 */
	struct wex_window_obj bg_win;
	if (!wex_window_init(&bg_win, &ctx, "BgWindow", 1280, 800, 0xFF222222)) {
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

	/* Source window A: 200x200 solid green 0xFF00FF00 at (100, 100) */
	char handle_A[33] = {0};
	struct wex_window_obj win_A;
	if (!wex_window_init(&win_A, &ctx, "SrcA", 200, 200, 0xFF00FF00)) {
		fprintf(stderr, "failed to create win_A\n");
		return 1;
	}
	win_A.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_A.xdg_toplevel);
	wex_window_v1_add_listener(win_A.wex_win, &capture_listener, handle_A);
	wex_window_v1_set_layer(win_A.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_A.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_A.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_A.zone_item);
		xx_zone_item_v1_set_position(win_A.zone_item, 100, 100);
	}
	wl_surface_commit(win_A.surface);

	/* Destination window B: 300x300 solid navy 0xFF112233 at (400, 100) */
	char handle_B[33] = {0};
	struct wex_window_obj win_B;
	if (!wex_window_init(&win_B, &ctx, "DestB", 300, 300, 0xFF112233)) {
		fprintf(stderr, "failed to create win_B\n");
		return 1;
	}
	win_B.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_B.xdg_toplevel);
	wex_window_v1_add_listener(win_B.wex_win, &capture_listener, handle_B);
	wex_window_v1_set_layer(win_B.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_B.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_B.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_B.zone_item);
		xx_zone_item_v1_set_position(win_B.zone_item, 400, 100);
	}
	wl_surface_commit(win_B.surface);

	wl_display_roundtrip(ctx.display);
	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&win_A);
	wex_window_commit_buffer(&win_B);
	wl_display_roundtrip(ctx.display);

	if (strlen(handle_A) != 32 || strlen(handle_B) != 32) {
		fprintf(stderr, "ERROR: invalid handles A='%s' B='%s'\n", handle_A, handle_B);
		return 1;
	}

	/* Step 1: Set thumbnail on B showing source A scaled at (50, 50, 150, 150) */
	printf("[corner-client] Step 1: setting thumbnail on Dest B (rect 50,50,150,150) showing Src A...\n");
	struct wex_thumbnail_record rec = {0};
	memcpy(rec.dest, handle_B, 32);
	memcpy(rec.src, handle_A, 32);
	rec.flags = WEX_WINDOW_MANAGER_V1_THUMBNAIL_FLAGS_VISIBLE;
	rec.opacity = 255;
	rec.dest_rect[0] = 50;
	rec.dest_rect[1] = 50;
	rec.dest_rect[2] = 150;
	rec.dest_rect[3] = 150;

	struct wl_array arr;
	wl_array_init(&arr);
	void *p = wl_array_add(&arr, sizeof(rec));
	memcpy(p, &rec, sizeof(rec));
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_array_release(&arr);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_thumb_active", "b_thumb_active_cont");

	/* Step 2: Minimize source A: thumbnail inside B must still be visible */
	printf("[corner-client] Step 2: minimizing source window A (thumbnail should keep showing buffer)...\n");
	xdg_toplevel_set_minimized(win_A.xdg_toplevel);
	wl_surface_commit(win_A.surface);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_thumb_minimized", "b_thumb_minimized_cont");

	/* Step 3: Clear thumbnail with empty array: B becomes all navy again */
	printf("[corner-client] Step 3: clearing thumbnails with empty array...\n");
	wl_array_init(&arr);
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_thumb_cleared", "b_thumb_cleared_cont");

	/* Step 4: Stale handle + destroyed source + malformed array */
	printf("[corner-client] Step 4: testing robustness (malformed array, stale handle, destroyed source)...\n");

	/* 4a: Malformed array (not multiple of 104) */
	char malformed[77] = {0};
	wl_array_init(&arr);
	arr.data = malformed;
	arr.size = sizeof(malformed);
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_display_roundtrip(ctx.display);
	if (wl_display_get_error(ctx.display) != 0) {
		fprintf(stderr, "ERROR: malformed array caused display error\n");
		return 1;
	}

	/* 4b: Stale handle */
	memset(&rec, 0, sizeof(rec));
	memcpy(rec.dest, handle_B, 32);
	memcpy(rec.src, "stale_handle_nonexistent_0000000", 32);
	rec.flags = WEX_WINDOW_MANAGER_V1_THUMBNAIL_FLAGS_VISIBLE;
	rec.dest_rect[0] = 10; rec.dest_rect[1] = 10;
	rec.dest_rect[2] = 50; rec.dest_rect[3] = 50;
	wl_array_init(&arr);
	p = wl_array_add(&arr, sizeof(rec));
	memcpy(p, &rec, sizeof(rec));
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_array_release(&arr);
	wl_display_roundtrip(ctx.display);
	if (wl_display_get_error(ctx.display) != 0) {
		fprintf(stderr, "ERROR: stale handle caused display error\n");
		return 1;
	}

	/* 4c: Destroyed source */
	char handle_temp[33] = {0};
	struct wex_window_obj win_temp;
	if (!wex_window_init(&win_temp, &ctx, "TempSrc", 100, 100, 0xFFFF00FF)) return 1;
	win_temp.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_temp.xdg_toplevel);
	wex_window_v1_add_listener(win_temp.wex_win, &capture_listener, handle_temp);
	wl_surface_commit(win_temp.surface);
	wl_display_roundtrip(ctx.display);
	wex_window_finish(&win_temp);
	wl_display_roundtrip(ctx.display);

	memset(&rec, 0, sizeof(rec));
	memcpy(rec.dest, handle_B, 32);
	memcpy(rec.src, handle_temp, 32);
	rec.flags = WEX_WINDOW_MANAGER_V1_THUMBNAIL_FLAGS_VISIBLE;
	rec.dest_rect[0] = 10; rec.dest_rect[1] = 10;
	rec.dest_rect[2] = 50; rec.dest_rect[3] = 50;
	wl_array_init(&arr);
	p = wl_array_add(&arr, sizeof(rec));
	memcpy(p, &rec, sizeof(rec));
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_array_release(&arr);
	wl_display_roundtrip(ctx.display);
	if (wl_display_get_error(ctx.display) != 0) {
		fprintf(stderr, "ERROR: destroyed source handle caused display error\n");
		return 1;
	}

	wex_window_finish(&win_A);
	wex_window_finish(&win_B);
	wex_window_finish(&bg_win);
	if (zone) xx_zone_v1_destroy(zone);
	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: thumbnails scenario completed successfully\n");
	return 0;
}

static int
test_thumbnails_scale(const char *socket_name, const char *barrier_dir)
{
	printf("[corner-client] running visual thumbnails scale filter (1/4 downscale checker) tests...\n");

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

	/* Background window: 1280x800 solid dark gray 0xFF222222 */
	struct wex_window_obj bg_win;
	if (!wex_window_init(&bg_win, &ctx, "BgWindow", 1280, 800, 0xFF222222)) {
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

	/* Source window A: 200x200 1-pixel checker pattern at (100, 100) */
	char handle_A[33] = {0};
	struct wex_window_obj win_A;
	if (!wex_window_init(&win_A, &ctx, "SrcA", 200, 200, 0xFF000000)) {
		fprintf(stderr, "failed to create win_A\n");
		return 1;
	}
	uint32_t *pixels_a = (uint32_t *)win_A.buffer.data;
	for (int y = 0; y < 200; y++) {
		for (int x = 0; x < 200; x++) {
			pixels_a[y * 200 + x] = ((x + y) & 1) ? 0xFFFFFFFF : 0xFF000000;
		}
	}
	win_A.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_A.xdg_toplevel);
	wex_window_v1_add_listener(win_A.wex_win, &capture_listener, handle_A);
	wex_window_v1_set_layer(win_A.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_A.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_A.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_A.zone_item);
		xx_zone_item_v1_set_position(win_A.zone_item, 100, 100);
	}
	wl_surface_commit(win_A.surface);

	/* Destination window B: 300x300 solid navy 0xFF112233 at (400, 100) */
	char handle_B[33] = {0};
	struct wex_window_obj win_B;
	if (!wex_window_init(&win_B, &ctx, "DestB", 300, 300, 0xFF112233)) {
		fprintf(stderr, "failed to create win_B\n");
		return 1;
	}
	win_B.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_B.xdg_toplevel);
	wex_window_v1_add_listener(win_B.wex_win, &capture_listener, handle_B);
	wex_window_v1_set_layer(win_B.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_B.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_B.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_B.zone_item);
		xx_zone_item_v1_set_position(win_B.zone_item, 400, 100);
	}
	wl_surface_commit(win_B.surface);

	wl_display_roundtrip(ctx.display);
	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&win_A);
	wex_window_commit_buffer(&win_B);
	wl_display_roundtrip(ctx.display);

	if (strlen(handle_A) != 32 || strlen(handle_B) != 32) {
		fprintf(stderr, "ERROR: invalid handles A='%s' B='%s'\n", handle_A, handle_B);
		return 1;
	}

	/* Set thumbnail on B showing source A scaled 1/4 at (50, 50, 100, 100) */
	printf("[corner-client] setting 1/4 downscaled thumbnail on Dest B (rect 50,50,100,100) showing Src A...\n");
	struct wex_thumbnail_record rec = {0};
	memcpy(rec.dest, handle_B, 32);
	memcpy(rec.src, handle_A, 32);
	rec.flags = WEX_WINDOW_MANAGER_V1_THUMBNAIL_FLAGS_VISIBLE;
	rec.opacity = 255;
	rec.dest_rect[0] = 50;
	rec.dest_rect[1] = 50;
	rec.dest_rect[2] = 100;
	rec.dest_rect[3] = 100;

	struct wl_array arr;
	wl_array_init(&arr);
	void *p = wl_array_add(&arr, sizeof(rec));
	memcpy(p, &rec, sizeof(rec));
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_array_release(&arr);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_thumb_scale", "b_thumb_scale_cont");

	wex_window_finish(&win_A);
	wex_window_finish(&win_B);
	wex_window_finish(&bg_win);
	if (zone) xx_zone_v1_destroy(zone);
	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: thumbnails-scale scenario completed successfully\n");
	return 0;
}

/* TEST 24: wex_window_manager_v1 v14 set_thumbnails with a multi-surface
 * source: source window A has a wl_subsurface child, and the thumbnail must
 * mirror both the parent surface and the subsurface (thumbnail_collect /
 * apply_thumbnail_geometry in wex-window.c). */
static int
test_thumbnails_subsurface(const char *socket_name, const char *barrier_dir)
{
	printf("[corner-client] running visual thumbnails with subsurface source (multi-surface mirror) tests...\n");

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
	if (!ctx.subcompositor) {
		fprintf(stderr, "wl_subcompositor not available\n");
		wex_context_finish(&ctx);
		return 1;
	}

	struct xx_zone_v1 *zone = NULL;
	if (ctx.zone_manager) {
		zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
		wl_display_roundtrip(ctx.display);
	}

	/* Background window: 1280x800 solid dark gray 0xFF222222 */
	struct wex_window_obj bg_win;
	if (!wex_window_init(&bg_win, &ctx, "BgWindow", 1280, 800, 0xFF222222)) {
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

	/* Source window A: 200x200 solid red 0xFFFF0000 at (100, 100) */
	char handle_A[33] = {0};
	struct wex_window_obj win_A;
	if (!wex_window_init(&win_A, &ctx, "SrcA", 200, 200, 0xFFFF0000)) {
		fprintf(stderr, "failed to create win_A\n");
		return 1;
	}
	win_A.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_A.xdg_toplevel);
	wex_window_v1_add_listener(win_A.wex_win, &capture_listener, handle_A);
	wex_window_v1_set_layer(win_A.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_A.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_A.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_A.zone_item);
		xx_zone_item_v1_set_position(win_A.zone_item, 100, 100);
	}
	wl_surface_commit(win_A.surface);

	/* Destination window B: 400x400 solid navy 0xFF112233 at (400, 100) */
	char handle_B[33] = {0};
	struct wex_window_obj win_B;
	if (!wex_window_init(&win_B, &ctx, "DestB", 400, 400, 0xFF112233)) {
		fprintf(stderr, "failed to create win_B\n");
		return 1;
	}
	win_B.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_B.xdg_toplevel);
	wex_window_v1_add_listener(win_B.wex_win, &capture_listener, handle_B);
	wex_window_v1_set_layer(win_B.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_B.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_B.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_B.zone_item);
		xx_zone_item_v1_set_position(win_B.zone_item, 400, 100);
	}
	wl_surface_commit(win_B.surface);

	wl_display_roundtrip(ctx.display);
	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&win_A);
	wex_window_commit_buffer(&win_B);
	wl_display_roundtrip(ctx.display);

	if (strlen(handle_A) != 32 || strlen(handle_B) != 32) {
		fprintf(stderr, "ERROR: invalid handles A='%s' B='%s'\n", handle_A, handle_B);
		return 1;
	}

	/* Subsurface of A: 100x100 solid green 0xFF00FF00 at (50, 50) inside A */
	struct wl_surface *sub_surface = wl_compositor_create_surface(ctx.compositor);
	if (!sub_surface) {
		fprintf(stderr, "failed to create subsurface surface\n");
		return 1;
	}
	struct wl_subsurface *sub = wl_subcompositor_get_subsurface(ctx.subcompositor,
		sub_surface, win_A.surface);
	if (!sub) {
		fprintf(stderr, "failed to create wl_subsurface\n");
		wl_surface_destroy(sub_surface);
		return 1;
	}
	struct wex_buffer sub_buf;
	if (!wex_buffer_create(&ctx, &sub_buf, 100, 100, 0xFF00FF00)) {
		fprintf(stderr, "failed to create subsurface buffer\n");
		wl_subsurface_destroy(sub);
		wl_surface_destroy(sub_surface);
		return 1;
	}
	wl_subsurface_set_position(sub, 50, 50);
	wl_surface_attach(sub_surface, sub_buf.wl_buffer, 0, 0);
	wl_surface_damage(sub_surface, 0, 0, 100, 100);
	wl_surface_commit(sub_surface);
	/* Parent commit applies the subsurface position */
	wex_window_commit_buffer(&win_A);
	wl_display_roundtrip(ctx.display);

	/* Step 1: thumbnail on B showing A, dest rect (0,0,400,400) => 2x scale.
	 * The green subsurface at A-local (50,50) maps to B-local (100,100)..(300,300). */
	printf("[corner-client] Step 1: setting thumbnail on Dest B (rect 0,0,400,400, 2x) showing Src A + green subsurface...\n");
	struct wex_thumbnail_record rec = {0};
	memcpy(rec.dest, handle_B, 32);
	memcpy(rec.src, handle_A, 32);
	rec.flags = WEX_WINDOW_MANAGER_V1_THUMBNAIL_FLAGS_VISIBLE;
	rec.opacity = 255;
	rec.dest_rect[0] = 0;
	rec.dest_rect[1] = 0;
	rec.dest_rect[2] = 400;
	rec.dest_rect[3] = 400;

	struct wl_array arr;
	wl_array_init(&arr);
	void *p = wl_array_add(&arr, sizeof(rec));
	memcpy(p, &rec, sizeof(rec));
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_array_release(&arr);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_thumb_sub_active", "b_thumb_sub_active_cont");

	/* Step 2: move the subsurface to A-local (0,0); the thumbnail must follow
	 * after the next output frame. Green now maps to B-local (0,0)..(200,200). */
	printf("[corner-client] Step 2: moving subsurface to (0,0) inside A (thumbnail should follow)...\n");
	wl_subsurface_set_position(sub, 0, 0);
	wex_window_commit_buffer(&win_A);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_thumb_sub_moved", "b_thumb_sub_moved_cont");

	wl_subsurface_destroy(sub);
	wl_surface_destroy(sub_surface);
	wex_buffer_destroy(&sub_buf);
	wex_window_finish(&win_A);
	wex_window_finish(&win_B);
	wex_window_finish(&bg_win);
	if (zone) xx_zone_v1_destroy(zone);
	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: thumbnails-subsurface scenario completed successfully\n");
	return 0;
}

static int
test_peek(const char *socket_name, const char *barrier_dir)
{
	printf("[corner-client] running visual peek (v14 set_peek) tests...\n");

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

	/* Window A: 100x100 solid Red 0xFFFF0000 at (100, 200) */
	char handle_A[33] = {0};
	struct wex_window_obj win_A;
	if (!wex_window_init(&win_A, &ctx, "WinA", 100, 100, 0xFFFF0000)) return 1;
	win_A.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_A.xdg_toplevel);
	wex_window_v1_add_listener(win_A.wex_win, &capture_listener, handle_A);
	wex_window_v1_set_layer(win_A.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_A.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_A.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_A.zone_item);
		xx_zone_item_v1_set_position(win_A.zone_item, 100, 200);
	}
	wl_surface_commit(win_A.surface);

	/* Window B: 100x100 solid Green 0xFF00FF00 at (300, 200) */
	char handle_B[33] = {0};
	struct wex_window_obj win_B;
	if (!wex_window_init(&win_B, &ctx, "WinB", 100, 100, 0xFF00FF00)) return 1;
	win_B.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_B.xdg_toplevel);
	wex_window_v1_add_listener(win_B.wex_win, &capture_listener, handle_B);
	wex_window_v1_set_layer(win_B.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_B.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_B.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_B.zone_item);
		xx_zone_item_v1_set_position(win_B.zone_item, 300, 200);
	}
	wl_surface_commit(win_B.surface);

	/* Window C: 100x100 solid Blue 0xFF0000FF at (500, 200) */
	char handle_C[33] = {0};
	struct wex_window_obj win_C;
	if (!wex_window_init(&win_C, &ctx, "WinC", 100, 100, 0xFF0000FF)) return 1;
	win_C.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_C.xdg_toplevel);
	wex_window_v1_add_listener(win_C.wex_win, &capture_listener, handle_C);
	wex_window_v1_set_layer(win_C.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	if (zone) {
		win_C.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_C.xdg_toplevel);
		xx_zone_v1_add_item(zone, win_C.zone_item);
		xx_zone_item_v1_set_position(win_C.zone_item, 500, 200);
	}
	wl_surface_commit(win_C.surface);

	wl_display_roundtrip(ctx.display);
	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&win_A);
	wex_window_commit_buffer(&win_B);
	wex_window_commit_buffer(&win_C);
	wl_display_roundtrip(ctx.display);

	if (strlen(handle_A) != 32 || strlen(handle_B) != 32 || strlen(handle_C) != 32) {
		fprintf(stderr, "ERROR: invalid handles A='%s' B='%s' C='%s'\n", handle_A, handle_B, handle_C);
		return 1;
	}

	/* Step 1: Set peek with target = B, keep = empty */
	printf("[corner-client] Step 1: set_peek(target=B, keep=[]): A and C should be hidden, B visible...\n");
	struct wl_array keep;
	wl_array_init(&keep);
	wex_window_manager_v1_set_peek(ctx.wex_window_manager, handle_B, &keep);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_peek_active", "b_peek_active_cont");

	/* Step 2: Set peek with empty target: all windows restored */
	printf("[corner-client] Step 2: set_peek(target='', keep=[]): all windows should be restored...\n");
	wex_window_manager_v1_set_peek(ctx.wex_window_manager, "", &keep);
	wl_array_release(&keep);
	wl_display_roundtrip(ctx.display);
	barrier_signal_and_wait(barrier_dir, "b_peek_ended", "b_peek_ended_cont");

	wex_window_finish(&win_A);
	wex_window_finish(&win_B);
	wex_window_finish(&win_C);
	wex_window_finish(&bg_win);
	if (zone) xx_zone_v1_destroy(zone);
	wex_context_finish(&ctx);
	printf("[corner-client] SUCCESS: peek scenario completed successfully\n");
	return 0;
}

struct frame_stats {
	unsigned count;
	uint32_t frames[16];
	uint64_t times_ns[16];
};

static void
mgr_frame_cb(void *data, struct wex_window_manager_v1 *mgr,
	uint32_t frame, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	(void)mgr;
	struct frame_stats *stats = data;
	if (stats->count < 16) {
		stats->frames[stats->count] = frame;
		stats->times_ns[stats->count] = ((uint64_t)sec_hi << 32 | sec_lo) * 1000000000ULL + nsec;
	}
	stats->count++;
}

static const struct wex_window_manager_v1_listener frame_listener = {
	.native_environment = NULL,
	.frame = mgr_frame_cb,
};

static int
test_frame(const char *socket_name)
{
	printf("[corner-client] running frame event (v14 frame) test...\n");

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

	struct frame_stats stats = {0};
	wex_window_manager_v1_add_listener(ctx.wex_window_manager, &frame_listener, &stats);

	/* Map a window and commit initial buffer */
	struct wex_window_obj win;
	if (!wex_window_init(&win, &ctx, "FrameWin", 200, 200, 0xFFFF0000)) return 1;
	win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win.xdg_toplevel);
	wex_window_v1_set_layer(win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	wl_surface_commit(win.surface);
	wl_display_roundtrip(ctx.display);
	wex_window_commit_buffer(&win);
	wl_display_roundtrip(ctx.display);

	/* Wait briefly for initial frame commit to settle */
	usleep(50000);
	wl_display_roundtrip(ctx.display);

	/* Buffer change 1: fill with green and commit */
	printf("[corner-client] Frame test: committing buffer change 1...\n");
	unsigned count_before = stats.count;
	uint32_t *pix = win.buffer.data;
	for (int i = 0; i < 200 * 200; i++) pix[i] = 0xFF00FF00;
	wl_surface_attach(win.surface, win.buffer.wl_buffer, 0, 0);
	wl_surface_damage_buffer(win.surface, 0, 0, 200, 200);
	wl_surface_commit(win.surface);
	wl_display_flush(ctx.display);

	for (int i = 0; i < 50 && stats.count == count_before; i++) {
		usleep(20000);
		wl_display_roundtrip(ctx.display);
	}

	/* Buffer change 2: fill with blue and commit */
	printf("[corner-client] Frame test: committing buffer change 2...\n");
	count_before = stats.count;
	for (int i = 0; i < 200 * 200; i++) pix[i] = 0xFF0000FF;
	wl_surface_attach(win.surface, win.buffer.wl_buffer, 0, 0);
	wl_surface_damage_buffer(win.surface, 0, 0, 200, 200);
	wl_surface_commit(win.surface);
	wl_display_flush(ctx.display);

	for (int i = 0; i < 50 && stats.count == count_before; i++) {
		usleep(20000);
		wl_display_roundtrip(ctx.display);
	}

	printf("[corner-client] total frame events received: %u\n", stats.count);
	if (stats.count < 2) {
		fprintf(stderr, "ERROR: expected at least 2 frame events, received %u\n", stats.count);
		wex_window_finish(&win);
		wex_context_finish(&ctx);
		return 1;
	}

	/* Verify increasing frame counters and monotonic timestamps */
	for (unsigned i = 1; i < stats.count && i < 16; i++) {
		if (stats.frames[i] <= stats.frames[i - 1]) {
			fprintf(stderr, "ERROR: frame counter not strictly increasing: frame[%u]=%u <= frame[%u]=%u\n",
				i, stats.frames[i], i - 1, stats.frames[i - 1]);
			wex_window_finish(&win);
			wex_context_finish(&ctx);
			return 1;
		}
		if (stats.times_ns[i] < stats.times_ns[i - 1]) {
			fprintf(stderr, "ERROR: monotonic time went backwards: time[%u]=%llu < time[%u]=%llu\n",
				i, (unsigned long long)stats.times_ns[i],
				i - 1, (unsigned long long)stats.times_ns[i - 1]);
			wex_window_finish(&win);
			wex_context_finish(&ctx);
			return 1;
		}
	}

	if (stats.times_ns[0] == 0) {
		fprintf(stderr, "ERROR: zero monotonic timestamp received\n");
		wex_window_finish(&win);
		wex_context_finish(&ctx);
		return 1;
	}

	printf("[corner-client] SUCCESS: received %u frame events with strictly increasing counter (%u -> %u) and monotonic timestamps\n",
		stats.count, stats.frames[0], stats.frames[stats.count - 1]);

	wex_window_finish(&win);
	wex_context_finish(&ctx);
	return 0;
}

static int
test_idle(const char *socket_name)
{
	printf("[corner-client] running idle frame pacing test (checks zero commits over ~1s idle)...\n");

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

	struct frame_stats stats = {0};
	wex_window_manager_v1_add_listener(ctx.wex_window_manager, &frame_listener, &stats);

	/* Map source window A (red, 200x200) and dest window B (navy, 400x400) */
	char handle_A[33] = {0};
	struct wex_window_obj win_A;
	if (!wex_window_init(&win_A, &ctx, "IdleSrcA", 200, 200, 0xFFFF0000)) return 1;
	win_A.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_A.xdg_toplevel);
	wex_window_v1_add_listener(win_A.wex_win, &capture_listener, handle_A);
	wex_window_v1_set_layer(win_A.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	wl_surface_commit(win_A.surface);

	char handle_B[33] = {0};
	struct wex_window_obj win_B;
	if (!wex_window_init(&win_B, &ctx, "IdleDestB", 400, 400, 0xFF112233)) return 1;
	win_B.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_B.xdg_toplevel);
	wex_window_v1_add_listener(win_B.wex_win, &capture_listener, handle_B);
	wex_window_v1_set_layer(win_B.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	wl_surface_commit(win_B.surface);

	wl_display_roundtrip(ctx.display);
	wex_window_commit_buffer(&win_A);
	wex_window_commit_buffer(&win_B);
	wl_display_roundtrip(ctx.display);

	/* Configure thumbnail on B showing source A */
	struct wex_thumbnail_record rec = {0};
	memcpy(rec.dest, handle_B, 32);
	memcpy(rec.src, handle_A, 32);
	rec.flags = WEX_WINDOW_MANAGER_V1_THUMBNAIL_FLAGS_VISIBLE;
	rec.opacity = 255;
	rec.dest_rect[0] = 50; rec.dest_rect[1] = 50;
	rec.dest_rect[2] = 150; rec.dest_rect[3] = 150;

	struct wl_array arr;
	wl_array_init(&arr);
	void *p = wl_array_add(&arr, sizeof(rec));
	memcpy(p, &rec, sizeof(rec));
	wex_window_manager_v1_set_thumbnails(ctx.wex_window_manager, &arr);
	wl_array_release(&arr);
	wl_display_roundtrip(ctx.display);

	/* Wait 100ms for initial setup commits to settle */
	usleep(100000);
	wl_display_roundtrip(ctx.display);

	unsigned initial_count = stats.count;
	printf("[corner-client] settled frame count: %u; monitoring idle for 1.0s...\n", initial_count);

	/* Sleep ~1.0 second, polling Wayland display roundtrips */
	for (int i = 0; i < 20; i++) {
		usleep(50000);
		wl_display_roundtrip(ctx.display);
	}

	unsigned commits_during_idle = stats.count - initial_count;
	if (commits_during_idle != 0) {
		fprintf(stderr, "ERROR: compositor busy-rendered %u frames over 1.0s while completely idle!\n",
			commits_during_idle);
		wex_window_finish(&win_A);
		wex_window_finish(&win_B);
		wex_context_finish(&ctx);
		return 1;
	}

	printf("[corner-client] SUCCESS: 0 output commits detected over 1.0s idle window (no busy rendering)\n");

	/* Now damage window A to verify frame scheduling still wakes up on damage */
	printf("[corner-client] verifying frame scheduling wakes up on damage...\n");
	uint32_t *pix = win_A.buffer.data;
	for (int i = 0; i < 200 * 200; i++) pix[i] = 0xFF00FF00;
	wl_surface_attach(win_A.surface, win_A.buffer.wl_buffer, 0, 0);
	wl_surface_damage_buffer(win_A.surface, 0, 0, 200, 200);
	wl_surface_commit(win_A.surface);
	wl_display_flush(ctx.display);

	for (int i = 0; i < 50 && stats.count == initial_count; i++) {
		usleep(20000);
		wl_display_roundtrip(ctx.display);
	}

	if (stats.count <= initial_count) {
		fprintf(stderr, "ERROR: compositor failed to schedule a frame after surface damage\n");
		wex_window_finish(&win_A);
		wex_window_finish(&win_B);
		wex_context_finish(&ctx);
		return 1;
	}

	printf("[corner-client] SUCCESS: damage woke compositor; new frame count %u (was %u)\n",
		stats.count, initial_count);

	wex_window_finish(&win_A);
	wex_window_finish(&win_B);
	wex_context_finish(&ctx);
	return 0;
}

static void
content_type_registry_handle_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *interface, uint32_t version)
{
	(void)registry;
	(void)name;
	bool *found = data;
	if (strcmp(interface, "wp_content_type_manager_v1") == 0) {
		printf("[corner-client] found global %s version %u\n", interface, version);
		*found = true;
	}
}

static const struct wl_registry_listener content_type_registry_listener = {
	.global = content_type_registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static int
test_content_type(const char *normal_socket)
{
	printf("[corner-client] checking wp_content_type_manager_v1 on '%s'...\n",
		normal_socket ? normal_socket : "default");

	struct wl_display *display = wl_display_connect(normal_socket);
	if (!display) {
		fprintf(stderr, "failed to connect to socket '%s'\n", normal_socket ? normal_socket : "default");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	bool found = false;
	wl_registry_add_listener(registry, &content_type_registry_listener, &found);
	wl_display_roundtrip(display);

	if (!found) {
		fprintf(stderr, "ERROR: wp_content_type_manager_v1 global not advertised on '%s'\n",
			normal_socket ? normal_socket : "default");
		wl_registry_destroy(registry);
		wl_display_disconnect(display);
		return 1;
	}

	printf("[corner-client] SUCCESS: wp_content_type_manager_v1 advertised and available\n");
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
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
	} else if (strcmp(scenario, "blur") == 0) {
		return test_blur(socket_name, barrier_dir);
	} else if (strcmp(scenario, "thumbnails") == 0) {
		return test_thumbnails(socket_name, barrier_dir);
	} else if (strcmp(scenario, "thumbnails-scale") == 0) {
		return test_thumbnails_scale(socket_name, barrier_dir);
	} else if (strcmp(scenario, "thumbnails-subsurface") == 0) {
		return test_thumbnails_subsurface(socket_name, barrier_dir);
	} else if (strcmp(scenario, "peek") == 0) {
		return test_peek(socket_name, barrier_dir);
	} else if (strcmp(scenario, "frame") == 0) {
		return test_frame(socket_name);
	} else if (strcmp(scenario, "idle") == 0) {
		return test_idle(socket_name);
	} else if (strcmp(scenario, "content-type") == 0) {
		return test_content_type(normal_socket);
	} else {
		fprintf(stderr, "Unknown scenario '%s'\n", scenario);
		return 1;
	}
}
