/* SPDX-License-Identifier: GPL-2.0-only */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>

#include "shm-common.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "xx-zones-v1-client-protocol.h"

static volatile sig_atomic_t g_running = 1;

static void
sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

int
main(int argc, char **argv)
{
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	const char *socket_name = NULL;
	int wait_secs = 2;
	bool test_subsurface = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
			wait_secs = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--subsurface") == 0) {
			test_subsurface = true;
		}
	}

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		return 1;
	}

	if (!ctx.alpha_modifier) {
		fprintf(stderr, "ERROR: wp_alpha_modifier_v1 not available\n");
		wex_context_finish(&ctx);
		return 1;
	}

	/*
	 * Background window: Solid white (1280x800)
	 * Foreground window: Solid pure red 0xFFFF0000 (400x300) with 50% alpha modifier
	 * Expected blended color over white:
	 * R: 255 * 0.5 + 255 * 0.5 = 255
	 * G: 0 * 0.5 + 255 * 0.5 = 127/128
	 * B: 0 * 0.5 + 255 * 0.5 = 127/128
	 */
	struct wex_window_obj bg_win, fg_win;

	struct xx_zone_v1 *zone = NULL;
	if (ctx.zone_manager) {
		zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
		wl_display_roundtrip(ctx.display);
	}

	if (!wex_window_init(&bg_win, &ctx, "AlphaBG", 1280, 800, 0xFFFFFFFF)) {
		fprintf(stderr, "failed to create bg_win\n");
		wex_context_finish(&ctx);
		return 1;
	}
	if (zone) {
		bg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, bg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, bg_win.zone_item);
		xx_zone_item_v1_set_position(bg_win.zone_item, 0, 0);
	}
	wl_surface_commit(bg_win.surface);

	uint32_t fg_color = test_subsurface ? 0xFF00FF00 : 0xFFFF0000;
	if (!wex_window_init(&fg_win, &ctx, "AlphaFG", 400, 300, fg_color)) {
		fprintf(stderr, "failed to create fg_win\n");
		wex_window_finish(&bg_win);
		wex_context_finish(&ctx);
		return 1;
	}
	if (zone) {
		fg_win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, fg_win.xdg_toplevel);
		xx_zone_v1_add_item(zone, fg_win.zone_item);
		xx_zone_item_v1_set_position(fg_win.zone_item, 100, 100);
	}

	struct wp_alpha_modifier_surface_v1 *alpha_surf = NULL;
	struct wl_surface *sub_surface = NULL;
	struct wl_subsurface *subsurface = NULL;
	struct wex_buffer sub_buf;
	uint32_t multiplier = UINT32_MAX / 2;

	if (test_subsurface) {
		if (!ctx.subcompositor) {
			fprintf(stderr, "wl_subcompositor not available\n");
			return 1;
		}
		sub_surface = wl_compositor_create_surface(ctx.compositor);
		subsurface = wl_subcompositor_get_subsurface(ctx.subcompositor, sub_surface, fg_win.surface);
		wl_subsurface_set_position(subsurface, 50, 50);
		wl_subsurface_set_desync(subsurface);
		wex_buffer_create(&ctx, &sub_buf, 200, 200, 0xFFFF0000);
		alpha_surf = wp_alpha_modifier_v1_get_surface(ctx.alpha_modifier, sub_surface);
		wp_alpha_modifier_surface_v1_set_multiplier(alpha_surf, multiplier);
		printf("[alpha-client] set subsurface alpha multiplier to 50%% (0x%08x)\n", multiplier);
	} else {
		alpha_surf = wp_alpha_modifier_v1_get_surface(ctx.alpha_modifier, fg_win.surface);
		wp_alpha_modifier_surface_v1_set_multiplier(alpha_surf, multiplier);
		printf("[alpha-client] set alpha multiplier to 50%% (0x%08x)\n", multiplier);
	}

	wl_surface_commit(fg_win.surface);
	wl_display_roundtrip(ctx.display);

	wex_window_commit_buffer(&bg_win);
	wex_window_commit_buffer(&fg_win);
	if (test_subsurface) {
		wl_surface_attach(sub_surface, sub_buf.wl_buffer, 0, 0);
		wl_surface_damage_buffer(sub_surface, 0, 0, 200, 200);
		wl_surface_commit(sub_surface);
	}
	wl_display_roundtrip(ctx.display);

	printf("[alpha-client] displayed alpha window, waiting %d seconds\n", wait_secs);

	int elapsed = 0;
	while (g_running && elapsed < wait_secs) {
		struct pollfd pfd = {
			.fd = wl_display_get_fd(ctx.display),
			.events = POLLIN,
		};
		wl_display_flush(ctx.display);
		if (poll(&pfd, 1, 1000) > 0) {
			wl_display_dispatch(ctx.display);
		}
		elapsed++;
	}

	if (test_subsurface) {
		wp_alpha_modifier_surface_v1_destroy(alpha_surf);
		wex_buffer_destroy(&sub_buf);
		wl_subsurface_destroy(subsurface);
		wl_surface_destroy(sub_surface);
	} else {
		wp_alpha_modifier_surface_v1_destroy(alpha_surf);
	}
	wex_window_finish(&fg_win);
	wex_window_finish(&bg_win);
	if (zone) {
		xx_zone_v1_destroy(zone);
	}
	wex_context_finish(&ctx);

	printf("[alpha-client] finished cleanly\n");
	return 0;
}
