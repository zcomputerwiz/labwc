/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * frame-client: which output paces the shell's frame clock.
 *
 *   frame-client --socket wex-shell-0 --output NAME [--seconds S]
 *
 * Binds the window manager on the private socket (v14 `frame` events), puts a window fullscreen on the output named
 * NAME (HEADLESS-1, ...), redraws it every 16 ms for S seconds (default 2) and prints "frames=N", the frame events
 * received. The clock follows the primary output (the one at the layout origin), so a window animating there gives
 * many events and one animating on another output gives none.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "shm-common.h"
#include "wex-window-v1-client-protocol.h"

#define MAX_OUTPUTS 8

struct outputs {
	struct wl_output *output[MAX_OUTPUTS];
	char name[MAX_OUTPUTS][64];
	int count;
};

static void
output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw, int32_t ph,
	int32_t subpixel, const char *make, const char *model, int32_t transform)
{
	(void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel; (void)make; (void)model;
	(void)transform;
}

static void
output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	(void)data; (void)output; (void)flags; (void)w; (void)h; (void)refresh;
}

static void
output_done(void *data, struct wl_output *output)
{
	(void)data; (void)output;
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
	(void)data; (void)output; (void)factor;
}

static void
output_name(void *data, struct wl_output *output, const char *name)
{
	struct outputs *outs = data;
	for (int i = 0; i < outs->count; i++) {
		if (outs->output[i] == output) {
			snprintf(outs->name[i], sizeof(outs->name[i]), "%s", name);
		}
	}
}

static void
output_description(void *data, struct wl_output *output, const char *description)
{
	(void)data; (void)output; (void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct outputs *outs = data;
	if (!strcmp(interface, wl_output_interface.name) && version >= 4 && outs->count < MAX_OUTPUTS) {
		struct wl_output *output = wl_registry_bind(registry, name, &wl_output_interface, 4);
		outs->output[outs->count] = output;
		outs->name[outs->count][0] = 0;
		wl_output_add_listener(output, &output_listener, outs);
		outs->count++;
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
manager_frame(void *data, struct wex_window_manager_v1 *manager, uint32_t frame, uint32_t sec_hi,
	uint32_t sec_lo, uint32_t nsec)
{
	(void)manager; (void)frame; (void)sec_hi; (void)sec_lo; (void)nsec;
	unsigned *count = data;
	(*count)++;
}

static const struct wex_window_manager_v1_listener manager_listener = {
	.native_environment = NULL,
	.frame = manager_frame,
};

int
main(int argc, char **argv)
{
	const char *socket_name = "wex-shell-0";
	const char *output_name_wanted = "HEADLESS-1";
	int seconds = 2;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
			output_name_wanted = argv[++i];
		} else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = atoi(argv[++i]);
		}
	}

	struct wex_context ctx;
	if (!wex_context_init(&ctx, socket_name)) {
		fprintf(stderr, "frame-client: cannot connect to '%s'\n", socket_name);
		return 1;
	}
	if (!ctx.wex_window_manager) {
		fprintf(stderr, "frame-client: no wex_window_manager_v1 on '%s'\n", socket_name);
		return 1;
	}

	struct outputs outs = { .count = 0 };
	struct wl_registry *registry = wl_display_get_registry(ctx.display);
	wl_registry_add_listener(registry, &registry_listener, &outs);
	wl_display_roundtrip(ctx.display);
	wl_display_roundtrip(ctx.display);

	struct wl_output *target = NULL;
	for (int i = 0; i < outs.count; i++) {
		printf("frame-client: output %d is %s\n", i, outs.name[i]);
		if (!strcmp(outs.name[i], output_name_wanted)) {
			target = outs.output[i];
		}
	}
	if (!target) {
		fprintf(stderr, "frame-client: no output named %s\n", output_name_wanted);
		return 1;
	}

	unsigned frames = 0;
	wex_window_manager_v1_add_listener(ctx.wex_window_manager, &manager_listener, &frames);

	struct wex_window_obj win;
	if (!wex_window_init(&win, &ctx, "FrameOut", 200, 200, 0xFFFF0000)) {
		fprintf(stderr, "frame-client: cannot create a window\n");
		return 1;
	}
	/* wait for the first configure, then ask for the output, then attach a buffer (never before a configure) */
	for (int i = 0; i < 20 && !win.configured; i++) {
		wl_display_roundtrip(ctx.display);
		usleep(20000);
	}
	xdg_toplevel_set_fullscreen(win.xdg_toplevel, target);
	wl_surface_commit(win.surface);
	for (int i = 0; i < 10; i++) {
		wl_display_roundtrip(ctx.display);
		usleep(20000);
	}
	wex_window_commit_buffer(&win);
	for (int i = 0; i < 10; i++) {
		wl_display_roundtrip(ctx.display);
		usleep(20000);
	}

	/* the window is up on its output; only frames from here on count */
	frames = 0;
	uint32_t *pixels = win.buffer.data;
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (unsigned n = 0;; n++) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= seconds) {
			break;
		}
		uint32_t color = (n & 1) ? 0xFF00FF00 : 0xFF0000FF;
		for (int i = 0; i < win.buffer.width * win.buffer.height; i++) {
			pixels[i] = color;
		}
		wl_surface_attach(win.surface, win.buffer.wl_buffer, 0, 0);
		wl_surface_damage_buffer(win.surface, 0, 0, win.buffer.width, win.buffer.height);
		wl_surface_commit(win.surface);
		wl_display_roundtrip(ctx.display);
		usleep(16000);
	}
	printf("frames=%u\n", frames);

	wex_window_finish(&win);
	wex_context_finish(&ctx);
	return 0;
}
