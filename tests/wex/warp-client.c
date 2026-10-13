/* SPDX-License-Identifier: GPL-2.0-only */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>

#include "shm-common.h"
#include "pointer-warp-v1-client-protocol.h"

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
	int target_x = 120;
	int target_y = 240;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--x") == 0 && i + 1 < argc) {
			target_x = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
			target_y = atoi(argv[++i]);
		}
	}

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		return 1;
	}

	if (!ctx.pointer_warp) {
		fprintf(stderr, "ERROR: wp_pointer_warp_v1 not available\n");
		wex_context_finish(&ctx);
		return 1;
	}

	if (!ctx.pointer) {
		fprintf(stderr, "ERROR: wl_pointer not available (no pointer capability in seat)\n");
		wex_context_finish(&ctx);
		return 1;
	}

	/* Create 1280x800 window covering whole screen so cursor is guaranteed to be within it */
	struct wex_window_obj win;
	if (!wex_window_init(&win, &ctx, "WarpWindow", 1280, 800, 0xFF333333)) {
		fprintf(stderr, "failed to create window\n");
		wex_context_finish(&ctx);
		return 1;
	}
	wl_surface_commit(win.surface);

	wl_display_roundtrip(ctx.display);
	wex_window_commit_buffer(&win);
	wl_display_roundtrip(ctx.display);

	/* Wait for pointer enter event */
	int wait_iters = 0;
	while (!ctx.pointer_entered && wait_iters < 10) {
		wl_display_roundtrip(ctx.display);
		usleep(50000);
		wait_iters++;
	}

	if (!ctx.pointer_entered) {
		fprintf(stderr, "warning: pointer_enter not received initially, proceeding with serial 0\n");
	} else {
		printf("[warp-client] pointer entered at (%d, %d), serial=%u\n",
			ctx.pointer_x, ctx.pointer_y, ctx.last_enter_serial);
	}

	printf("[warp-client] sending warp request to (%d, %d)...\n", target_x, target_y);
	ctx.pointer_motion_received = false;
	wp_pointer_warp_v1_warp_pointer(ctx.pointer_warp, win.surface, ctx.pointer,
		wl_fixed_from_int(target_x), wl_fixed_from_int(target_y),
		ctx.last_enter_serial);
	wl_surface_commit(win.surface);

	/* Wait for motion event */
	wait_iters = 0;
	while (!ctx.pointer_motion_received && wait_iters < 10) {
		wl_display_roundtrip(ctx.display);
		usleep(50000);
		wait_iters++;
	}

	if (!ctx.pointer_motion_received) {
		fprintf(stderr, "ERROR: did not receive pointer motion after warp request!\n");
		wex_window_finish(&win);
		wex_context_finish(&ctx);
		return 2;
	}

	printf("[warp-client] received motion to (%d, %d)\n", ctx.pointer_x, ctx.pointer_y);

	if (ctx.pointer_x != target_x || ctx.pointer_y != target_y) {
		fprintf(stderr, "ERROR: motion coordinates (%d, %d) do not match target (%d, %d)\n",
			ctx.pointer_x, ctx.pointer_y, target_x, target_y);
		wex_window_finish(&win);
		wex_context_finish(&ctx);
		return 3;
	}

	printf("[warp-client] SUCCESS: pointer warp verified at exact coordinates (%d, %d)\n",
		target_x, target_y);

	wex_window_finish(&win);
	wex_context_finish(&ctx);
	return 0;
}
