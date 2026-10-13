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

#define MAX_WINDOWS 16

static volatile sig_atomic_t g_running = 1;

static void
sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

struct zone_state {
	char handle[256];
	int width;
	int height;
	bool has_size;
	bool has_handle;
	bool done;
};

struct item_state {
	int id;
	int frame_top, frame_bottom, frame_left, frame_right;
	int x, y;
	bool got_extents;
	bool got_position;
	bool got_failed;
	bool got_closed;
};

static void
handle_zone_size(void *data, struct xx_zone_v1 *zone, int32_t width, int32_t height)
{
	struct zone_state *zs = data;
	zs->width = width;
	zs->height = height;
	zs->has_size = true;
	printf("[zones-client] zone: size %dx%d\n", width, height);
	fflush(stdout);
}

static void
handle_zone_handle(void *data, struct xx_zone_v1 *zone, const char *handle)
{
	struct zone_state *zs = data;
	snprintf(zs->handle, sizeof(zs->handle), "%s", handle ? handle : "");
	zs->has_handle = true;
	printf("[zones-client] zone: handle '%s'\n", zs->handle);
	fflush(stdout);
}

static void
handle_zone_done(void *data, struct xx_zone_v1 *zone)
{
	struct zone_state *zs = data;
	zs->done = true;
	printf("[zones-client] zone: done\n");
	fflush(stdout);
}

static void
handle_zone_item_entered(void *data, struct xx_zone_v1 *zone, struct xx_zone_item_v1 *item)
{
	printf("[zones-client] zone: item_entered\n");
	fflush(stdout);
}

static void
handle_zone_item_left(void *data, struct xx_zone_v1 *zone, struct xx_zone_item_v1 *item)
{
	printf("[zones-client] zone: item_left\n");
	fflush(stdout);
}

static void
handle_zone_item_blocked(void *data, struct xx_zone_v1 *zone, struct xx_zone_item_v1 *item)
{
	printf("[zones-client] zone: item_blocked\n");
	fflush(stdout);
}

static const struct xx_zone_v1_listener zone_listener = {
	.size = handle_zone_size,
	.handle = handle_zone_handle,
	.done = handle_zone_done,
	.item_entered = handle_zone_item_entered,
	.item_left = handle_zone_item_left,
	.item_blocked = handle_zone_item_blocked,
};

static void
handle_item_frame_extents(void *data, struct xx_zone_item_v1 *item,
		int32_t top, int32_t bottom, int32_t left, int32_t right)
{
	struct item_state *is = data;
	is->frame_top = top;
	is->frame_bottom = bottom;
	is->frame_left = left;
	is->frame_right = right;
	is->got_extents = true;
	printf("[zones-client] window %d: frame_extents top=%d bottom=%d left=%d right=%d\n",
		is->id, top, bottom, left, right);
	fflush(stdout);
}

static void
handle_item_position(void *data, struct xx_zone_item_v1 *item, int32_t x, int32_t y)
{
	struct item_state *is = data;
	is->x = x;
	is->y = y;
	is->got_position = true;
	printf("[zones-client] window %d: position x=%d y=%d\n", is->id, x, y);
	fflush(stdout);
}

static void
handle_item_position_failed(void *data, struct xx_zone_item_v1 *item)
{
	struct item_state *is = data;
	is->got_failed = true;
	printf("[zones-client] window %d: position_failed\n", is->id);
	fflush(stdout);
}

static void
handle_item_closed(void *data, struct xx_zone_item_v1 *item)
{
	struct item_state *is = data;
	is->got_closed = true;
	printf("[zones-client] window %d: closed\n", is->id);
	fflush(stdout);
}

static const struct xx_zone_item_v1_listener item_listener = {
	.frame_extents = handle_item_frame_extents,
	.position = handle_item_position,
	.position_failed = handle_item_position_failed,
	.closed = handle_item_closed,
};

struct win_config {
	int x, y;
	int width, height;
	uint32_t color;
};

int
main(int argc, char **argv)
{
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	const char *socket_name = NULL;
	const char *join_handle = NULL;
	const char *export_file = NULL;
	int wait_secs = 2;
	bool test_fail = false;
	bool test_unknown_handle = false;
	int fail_x = 2000, fail_y = 2000;

	struct win_config win_configs[MAX_WINDOWS];
	int win_count = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (strcmp(argv[i], "--join-handle") == 0 && i + 1 < argc) {
			join_handle = argv[++i];
		} else if (strcmp(argv[i], "--export-handle") == 0 && i + 1 < argc) {
			export_file = argv[++i];
		} else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
			wait_secs = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--test-unknown-handle") == 0) {
			test_unknown_handle = true;
		} else if (strcmp(argv[i], "--test-fail") == 0 && i + 2 < argc) {
			test_fail = true;
			fail_x = atoi(argv[++i]);
			fail_y = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--win") == 0 && i + 5 < argc) {
			if (win_count < MAX_WINDOWS) {
				win_configs[win_count].x = atoi(argv[++i]);
				win_configs[win_count].y = atoi(argv[++i]);
				win_configs[win_count].width = atoi(argv[++i]);
				win_configs[win_count].height = atoi(argv[++i]);
				win_configs[win_count].color = (uint32_t)strtoul(argv[++i], NULL, 0);
				win_count++;
			} else {
				i += 5;
			}
		}
	}

	/* Default window if none specified */
	if (win_count == 0 && !test_fail && !test_unknown_handle) {
		win_configs[0] = (struct win_config){
			.x = 100, .y = 150, .width = 200, .height = 200, .color = 0xFFFF0000
		};
		win_count = 1;
	}

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		return 1;
	}

	if (!ctx.zone_manager) {
		fprintf(stderr, "xx_zone_manager_v1 not available\n");
		wex_context_finish(&ctx);
		return 1;
	}

	struct zone_state zs = { 0 };
	struct xx_zone_v1 *zone = NULL;

	if (test_unknown_handle) {
		printf("[zones-client] testing get_zone_from_handle with unknown handle\n");
		zone = xx_zone_manager_v1_get_zone_from_handle(ctx.zone_manager, "unknown-invalid-token");
		if (!zone) {
			fprintf(stderr, "failed to get zone for unknown handle\n");
			wex_context_finish(&ctx);
			return 1;
		}
		xx_zone_v1_add_listener(zone, &zone_listener, &zs);
		wl_display_roundtrip(ctx.display);
		if (zs.width < 0 && zs.height < 0 && strlen(zs.handle) == 0) {
			printf("[zones-client] SUCCESS: received invalid size %dx%d and empty handle for unknown handle\n",
				zs.width, zs.height);
			xx_zone_v1_destroy(zone);
			wex_context_finish(&ctx);
			return 0;
		} else {
			fprintf(stderr, "ERROR: expected invalid size and empty handle, got size %dx%d handle '%s'\n",
				zs.width, zs.height, zs.handle);
			xx_zone_v1_destroy(zone);
			wex_context_finish(&ctx);
			return 1;
		}
	} else if (join_handle) {
		printf("[zones-client] joining zone via handle '%s'\n", join_handle);
		zone = xx_zone_manager_v1_get_zone_from_handle(ctx.zone_manager, join_handle);
	} else {
		printf("[zones-client] creating new zone\n");
		zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
	}

	if (!zone) {
		fprintf(stderr, "failed to get zone\n");
		wex_context_finish(&ctx);
		return 1;
	}
	xx_zone_v1_add_listener(zone, &zone_listener, &zs);

	/* Wait for initial zone events (size, handle, done) */
	wl_display_roundtrip(ctx.display);

	if (export_file && zs.has_handle) {
		FILE *f = fopen(export_file, "w");
		if (f) {
			fprintf(f, "%s\n", zs.handle);
			fclose(f);
			printf("[zones-client] exported handle to %s\n", export_file);
		}
	}

	struct wex_window_obj windows[MAX_WINDOWS];
	struct item_state item_states[MAX_WINDOWS];
	memset(windows, 0, sizeof(windows));
	memset(item_states, 0, sizeof(item_states));

	for (int i = 0; i < win_count; i++) {
		char title[64];
		snprintf(title, sizeof(title), "ZoneWindow-%d", i);

		if (!wex_window_init(&windows[i], &ctx, title,
				win_configs[i].width, win_configs[i].height,
				win_configs[i].color)) {
			fprintf(stderr, "failed to create window %d\n", i);
			return 1;
		}

		item_states[i].id = i;
		windows[i].zone_item = xx_zone_manager_v1_get_zone_item(
			ctx.zone_manager, windows[i].xdg_toplevel);
		xx_zone_item_v1_add_listener(windows[i].zone_item, &item_listener, &item_states[i]);

		xx_zone_v1_add_item(zone, windows[i].zone_item);
		xx_zone_item_v1_set_position(windows[i].zone_item,
			win_configs[i].x, win_configs[i].y);

		wl_surface_commit(windows[i].surface);
	}

	/* Wait for configure events and attach buffers */
	wl_display_roundtrip(ctx.display);

	for (int i = 0; i < win_count; i++) {
		wex_window_commit_buffer(&windows[i]);
	}
	wl_display_roundtrip(ctx.display);

	/* If test_fail mode requested */
	if (test_fail) {
		struct wex_window_obj fail_win;
		struct item_state fail_is = { .id = 99 };
		if (!wex_window_init(&fail_win, &ctx, "FailWin", 100, 100, 0xFF00FF00)) {
			fprintf(stderr, "failed to create fail_win\n");
			return 1;
		}
		fail_win.zone_item = xx_zone_manager_v1_get_zone_item(
			ctx.zone_manager, fail_win.xdg_toplevel);
		xx_zone_item_v1_add_listener(fail_win.zone_item, &item_listener, &fail_is);

		xx_zone_v1_add_item(zone, fail_win.zone_item);
		xx_zone_item_v1_set_position(fail_win.zone_item, fail_x, fail_y);
		wl_surface_commit(fail_win.surface);

		wl_display_roundtrip(ctx.display);
		wex_window_commit_buffer(&fail_win);
		wl_display_roundtrip(ctx.display);

		if (!fail_is.got_failed) {
			fprintf(stderr, "ERROR: expected position_failed for off-zone (%d,%d), but did not get it\n",
				fail_x, fail_y);
			return 2;
		}
		printf("[zones-client] SUCCESS: received position_failed as expected for (%d,%d)\n",
			fail_x, fail_y);
		wex_window_finish(&fail_win);
	}

	/* Keep alive for wait_secs so screenshots or verification can take place */
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

	for (int i = 0; i < win_count; i++) {
		wex_window_finish(&windows[i]);
	}
	xx_zone_v1_destroy(zone);
	wex_context_finish(&ctx);

	printf("[zones-client] finished cleanly\n");
	return 0;
}
