/* SPDX-License-Identifier: GPL-2.0-only */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>

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
wait_loop(struct wex_context *ctx, int seconds)
{
	int elapsed = 0;
	while (g_running && elapsed < seconds) {
		struct pollfd pfd = {
			.fd = wl_display_get_fd(ctx->display),
			.events = POLLIN,
		};
		wl_display_flush(ctx->display);
		if (poll(&pfd, 1, 1000) > 0) {
			wl_display_dispatch(ctx->display);
		}
		elapsed++;
	}
}

static int
do_check_sockets(const char *normal_sock, const char *priv_sock)
{
	printf("[stack-client] checking socket privileges...\n");

	struct wex_context ctx_normal;
	if (!wex_context_init(&ctx_normal, normal_sock)) {
		fprintf(stderr, "failed to connect to normal socket '%s'\n", normal_sock);
		return 1;
	}
	if (ctx_normal.wex_window_manager != NULL) {
		fprintf(stderr, "ERROR: wex_window_manager_v1 is present on standard socket '%s'!\n",
			normal_sock ? normal_sock : "default");
		wex_context_finish(&ctx_normal);
		return 1;
	}
	printf("[stack-client] SUCCESS: wex_window_manager_v1 NOT advertised on standard socket\n");
	wex_context_finish(&ctx_normal);

	struct wex_context ctx_priv;
	if (!wex_context_init(&ctx_priv, priv_sock)) {
		fprintf(stderr, "failed to connect to privileged socket '%s'\n", priv_sock);
		return 1;
	}
	if (ctx_priv.wex_window_manager == NULL) {
		fprintf(stderr, "ERROR: wex_window_manager_v1 is missing on privileged socket '%s'!\n",
			priv_sock);
		wex_context_finish(&ctx_priv);
		return 1;
	}
	printf("[stack-client] SUCCESS: wex_window_manager_v1 present on privileged socket '%s'\n",
		priv_sock);
	wex_context_finish(&ctx_priv);

	return 0;
}

int
main(int argc, char **argv)
{
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	const char *socket_name = "wex-shell-0";
	const char *normal_socket = "wayland-0";
	const char *scenario = "layers";
	int wait_secs = 2;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--normal-socket") == 0 && i + 1 < argc) {
			normal_socket = argv[++i];
		} else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
			scenario = argv[++i];
		} else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
			wait_secs = atoi(argv[++i]);
		}
	}

	if (strcmp(scenario, "check-sockets") == 0) {
		return do_check_sockets(normal_socket, socket_name);
	}

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		return 1;
	}

	if (!ctx.wex_window_manager) {
		fprintf(stderr, "wex_window_manager_v1 not available on %s\n", socket_name);
		wex_context_finish(&ctx);
		return 1;
	}

	struct xx_zone_v1 *zone = NULL;
	if (ctx.zone_manager) {
		zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
		wl_display_roundtrip(ctx.display);
	}

	if (strcmp(scenario, "layers") == 0 || strcmp(scenario, "reorder") == 0) {
		/*
		 * Create windows:
		 * 1. Desktop: navy blue (1280x800)
		 * 2. Normal 1: red (300x200) at (100, 100)
		 * 3. Normal 2: blue (300x200) at (150, 150)
		 * 4. Topmost: yellow (300x200) at (200, 200)
		 * 5. Shell: green (1280x30) at bottom (0, 770) with exclusive edge 30px
		 */
		struct wex_window_obj win_desktop, win_normal1, win_normal2, win_topmost, win_shell;

		wex_window_init(&win_desktop, &ctx, "Desktop", 1280, 800, 0xFF112233);
		win_desktop.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_desktop.xdg_toplevel);
		wex_window_v1_set_layer(win_desktop.wex_win, WEX_WINDOW_V1_LAYER_DESKTOP);
		if (zone) {
			win_desktop.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_desktop.xdg_toplevel);
			xx_zone_v1_add_item(zone, win_desktop.zone_item);
			xx_zone_item_v1_set_position(win_desktop.zone_item, 0, 0);
		}
		wl_surface_commit(win_desktop.surface);

		wex_window_init(&win_normal1, &ctx, "Normal1", 300, 200, 0xFFFF0000);
		win_normal1.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_normal1.xdg_toplevel);
		wex_window_v1_set_layer(win_normal1.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
		if (zone) {
			win_normal1.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_normal1.xdg_toplevel);
			xx_zone_v1_add_item(zone, win_normal1.zone_item);
			xx_zone_item_v1_set_position(win_normal1.zone_item, 100, 100);
		}
		wl_surface_commit(win_normal1.surface);

		wex_window_init(&win_normal2, &ctx, "Normal2", 300, 200, 0xFF0000FF);
		win_normal2.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_normal2.xdg_toplevel);
		wex_window_v1_set_layer(win_normal2.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
		if (zone) {
			win_normal2.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_normal2.xdg_toplevel);
			xx_zone_v1_add_item(zone, win_normal2.zone_item);
			xx_zone_item_v1_set_position(win_normal2.zone_item, 150, 150);
		}
		wl_surface_commit(win_normal2.surface);

		wex_window_init(&win_topmost, &ctx, "Topmost", 300, 200, 0xFFFFFF00);
		win_topmost.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_topmost.xdg_toplevel);
		wex_window_v1_set_layer(win_topmost.wex_win, WEX_WINDOW_V1_LAYER_TOPMOST);
		if (zone) {
			win_topmost.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_topmost.xdg_toplevel);
			xx_zone_v1_add_item(zone, win_topmost.zone_item);
			xx_zone_item_v1_set_position(win_topmost.zone_item, 200, 200);
		}
		wl_surface_commit(win_topmost.surface);

		wex_window_init(&win_shell, &ctx, "Shell", 1280, 30, 0xFF00AA00);
		win_shell.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_shell.xdg_toplevel);
		wex_window_v1_set_layer(win_shell.wex_win, WEX_WINDOW_V1_LAYER_SHELL);
		wex_window_v1_set_exclusive_edge(win_shell.wex_win, WEX_WINDOW_V1_EDGE_BOTTOM, 30);
		if (zone) {
			win_shell.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_shell.xdg_toplevel);
			xx_zone_v1_add_item(zone, win_shell.zone_item);
			xx_zone_item_v1_set_position(win_shell.zone_item, 0, 770);
		}
		wl_surface_commit(win_shell.surface);

		wl_display_roundtrip(ctx.display);

		wex_window_commit_buffer(&win_desktop);
		wex_window_commit_buffer(&win_normal1);
		wex_window_commit_buffer(&win_normal2);
		wex_window_commit_buffer(&win_topmost);
		wex_window_commit_buffer(&win_shell);
		wl_display_roundtrip(ctx.display);

		if (strcmp(scenario, "reorder") == 0) {
			printf("[stack-client] reordering: placing normal1 (red) above normal2 (blue)\n");
			wex_window_v1_place_above(win_normal1.wex_win, win_normal2.wex_win);
			wl_display_roundtrip(ctx.display);
		}

		printf("[stack-client] scenario '%s' displayed, waiting %d seconds\n", scenario, wait_secs);
		wait_loop(&ctx, wait_secs);

		wex_window_finish(&win_shell);
		wex_window_finish(&win_topmost);
		wex_window_finish(&win_normal2);
		wex_window_finish(&win_normal1);
		wex_window_finish(&win_desktop);
	} else if (strcmp(scenario, "maximize") == 0) {
		/* Shell window with exclusive edge bottom 30px */
		struct wex_window_obj win_shell;
		wex_window_init(&win_shell, &ctx, "Shell", 1280, 30, 0xFF00AA00);
		win_shell.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_shell.xdg_toplevel);
		wex_window_v1_set_layer(win_shell.wex_win, WEX_WINDOW_V1_LAYER_SHELL);
		wex_window_v1_set_exclusive_edge(win_shell.wex_win, WEX_WINDOW_V1_EDGE_BOTTOM, 30);
		if (zone) {
			win_shell.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win_shell.xdg_toplevel);
			xx_zone_v1_add_item(zone, win_shell.zone_item);
			xx_zone_item_v1_set_position(win_shell.zone_item, 0, 770);
		}
		wl_surface_commit(win_shell.surface);
		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&win_shell);
		wl_display_roundtrip(ctx.display);

		/* Normal window requesting maximize */
		struct wex_window_obj win_norm;
		wex_window_init(&win_norm, &ctx, "MaximizeWindow", 400, 300, 0xFF4488CC);
		win_norm.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win_norm.xdg_toplevel);
		wex_window_v1_set_layer(win_norm.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
		xdg_toplevel_set_maximized(win_norm.xdg_toplevel);
		wl_surface_commit(win_norm.surface);

		wl_display_roundtrip(ctx.display);

		printf("[stack-client] maximized configure size: %dx%d\n",
			win_norm.configured_width, win_norm.configured_height);

		if (win_norm.configured_height != 770 || win_norm.configured_width != 1280) {
			fprintf(stderr, "ERROR: expected 1280x770 for maximized window, got %dx%d\n",
				win_norm.configured_width, win_norm.configured_height);
			return 2;
		}
		printf("[stack-client] SUCCESS: maximized window respects exclusive shell edge (1280x770)\n");

		/* Reallocate buffer to match maximized size */
		wex_buffer_destroy(&win_norm.buffer);
		wex_buffer_create(&ctx, &win_norm.buffer, win_norm.configured_width, win_norm.configured_height, 0xFF4488CC);
		win_norm.width = win_norm.configured_width;
		win_norm.height = win_norm.configured_height;
		wex_window_commit_buffer(&win_norm);
		wl_display_roundtrip(ctx.display);

		wait_loop(&ctx, wait_secs);

		wex_window_finish(&win_norm);
		wex_window_finish(&win_shell);
	} else if (strcmp(scenario, "unminimize") == 0) {
		struct wex_window_obj win;
		wex_window_init(&win, &ctx, "UnminimizeWindow", 300, 200, 0xFFCC3388);
		win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win.xdg_toplevel);
		wex_window_v1_set_layer(win.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
		wl_surface_commit(win.surface);
		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&win);
		wl_display_roundtrip(ctx.display);

		printf("[stack-client] minimizing window...\n");
		xdg_toplevel_set_minimized(win.xdg_toplevel);
		wl_surface_commit(win.surface);
		wl_display_roundtrip(ctx.display);

		printf("[stack-client] calling wex_window_v1.activate() to restore...\n");
		wex_window_v1_activate(win.wex_win);
		wl_display_roundtrip(ctx.display);

		printf("[stack-client] SUCCESS: activate() called on minimized window\n");
		wait_loop(&ctx, wait_secs);

		wex_window_finish(&win);
	}

	if (zone) {
		xx_zone_v1_destroy(zone);
	}
	wex_context_finish(&ctx);
	printf("[stack-client] finished cleanly\n");
	return 0;
}
