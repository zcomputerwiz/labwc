/* SPDX-License-Identifier: GPL-2.0-only */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shm-common.h"
#include "xx-zones-v1-client-protocol.h"
#include "wex-window-v1-client-protocol.h"
#include "pointer-warp-v1-client-protocol.h"
#include "alpha-modifier-v1-client-protocol.h"

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t sx, wl_fixed_t sy)
{
	struct wex_context *ctx = data;
	ctx->last_enter_serial = serial;
	ctx->pointer_entered = true;
	ctx->pointer_x = wl_fixed_to_int(sx);
	ctx->pointer_y = wl_fixed_to_int(sy);
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface)
{
	(void)pointer;
	(void)serial;
	(void)surface;
	struct wex_context *ctx = data;
	ctx->pointer_entered = false;
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)pointer;
	(void)time;
	struct wex_context *ctx = data;
	ctx->pointer_motion_received = true;
	ctx->pointer_x = wl_fixed_to_int(sx);
	ctx->pointer_y = wl_fixed_to_int(sy);
}

static void
pointer_handle_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)time;
	(void)button;
	(void)state;
}

static void
pointer_handle_axis(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
	(void)value;
}

static void
pointer_handle_frame(void *data, struct wl_pointer *pointer)
{
	(void)data;
	(void)pointer;
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
	(void)data;
	(void)pointer;
	(void)axis_source;
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
	(void)data;
	(void)pointer;
	(void)axis;
	(void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_handle_enter,
	.leave = pointer_handle_leave,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = pointer_handle_axis,
	.frame = pointer_handle_frame,
	.axis_source = pointer_handle_axis_source,
	.axis_stop = pointer_handle_axis_stop,
	.axis_discrete = pointer_handle_axis_discrete,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	struct wex_context *ctx = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !ctx->pointer) {
		ctx->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ctx->pointer, &pointer_listener, ctx);
	} else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && ctx->pointer) {
		wl_pointer_destroy(ctx->pointer);
		ctx->pointer = NULL;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct wex_context *ctx = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		ctx->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, version < 4 ? version : 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		ctx->shm = wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		ctx->wm_base = wl_registry_bind(registry, name,
			&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(ctx->wm_base, &wm_base_listener, ctx);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		ctx->seat = wl_registry_bind(registry, name,
			&wl_seat_interface, version < 7 ? version : 7);
		wl_seat_add_listener(ctx->seat, &seat_listener, ctx);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		ctx->deco_manager = wl_registry_bind(registry, name,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(interface, xx_zone_manager_v1_interface.name) == 0) {
		ctx->zone_manager = wl_registry_bind(registry, name,
			&xx_zone_manager_v1_interface, 1);
	} else if (strcmp(interface, wex_window_manager_v1_interface.name) == 0) {
		ctx->wex_window_manager = wl_registry_bind(registry, name,
			&wex_window_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_alpha_modifier_v1_interface.name) == 0) {
		ctx->alpha_modifier = wl_registry_bind(registry, name,
			&wp_alpha_modifier_v1_interface, 1);
	} else if (strcmp(interface, wp_pointer_warp_v1_interface.name) == 0) {
		ctx->pointer_warp = wl_registry_bind(registry, name,
			&wp_pointer_warp_v1_interface, 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

bool
wex_context_init(struct wex_context *ctx, const char *socket_name)
{
	memset(ctx, 0, sizeof(*ctx));

	ctx->display = wl_display_connect(socket_name);
	if (!ctx->display) {
		fprintf(stderr, "failed to connect to wayland display (%s)\n",
			socket_name ? socket_name : "default");
		return false;
	}

	ctx->registry = wl_display_get_registry(ctx->display);
	wl_registry_add_listener(ctx->registry, &registry_listener, ctx);

	if (wl_display_roundtrip(ctx->display) < 0) {
		fprintf(stderr, "failed roundtrip after registry bind\n");
		wex_context_finish(ctx);
		return false;
	}

	/* Second roundtrip processes initial events from bound objects (e.g. seat capabilities) */
	if (wl_display_roundtrip(ctx->display) < 0) {
		fprintf(stderr, "failed second roundtrip after binding globals\n");
		wex_context_finish(ctx);
		return false;
	}

	return true;
}

void
wex_context_finish(struct wex_context *ctx)
{
	if (!ctx) {
		return;
	}
	if (ctx->pointer) {
		wl_pointer_destroy(ctx->pointer);
	}
	if (ctx->seat) {
		wl_seat_destroy(ctx->seat);
	}
	if (ctx->pointer_warp) {
		wp_pointer_warp_v1_destroy(ctx->pointer_warp);
	}
	if (ctx->alpha_modifier) {
		wp_alpha_modifier_v1_destroy(ctx->alpha_modifier);
	}
	if (ctx->wex_window_manager) {
		wex_window_manager_v1_destroy(ctx->wex_window_manager);
	}
	if (ctx->zone_manager) {
		xx_zone_manager_v1_destroy(ctx->zone_manager);
	}
	if (ctx->deco_manager) {
		zxdg_decoration_manager_v1_destroy(ctx->deco_manager);
	}
	if (ctx->wm_base) {
		xdg_wm_base_destroy(ctx->wm_base);
	}
	if (ctx->shm) {
		wl_shm_destroy(ctx->shm);
	}
	if (ctx->compositor) {
		wl_compositor_destroy(ctx->compositor);
	}
	if (ctx->registry) {
		wl_registry_destroy(ctx->registry);
	}
	if (ctx->display) {
		wl_display_disconnect(ctx->display);
	}
	memset(ctx, 0, sizeof(*ctx));
}

bool
wex_buffer_create(struct wex_context *ctx, struct wex_buffer *buf,
		int width, int height, uint32_t color)
{
	assert(ctx);
	assert(buf);

	int stride = width * 4;
	size_t size = (size_t)stride * height;

	int fd = memfd_create("wex-shm", MFD_CLOEXEC);
	if (fd < 0) {
		perror("memfd_create");
		return false;
	}

	if (ftruncate(fd, size) < 0) {
		perror("ftruncate");
		close(fd);
		return false;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return false;
	}

	uint32_t *pixels = (uint32_t *)data;
	for (size_t i = 0; i < (size_t)(width * height); i++) {
		pixels[i] = color;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
	struct wl_buffer *wl_buf = wl_shm_pool_create_buffer(pool, 0,
		width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	buf->wl_buffer = wl_buf;
	buf->data = data;
	buf->width = width;
	buf->height = height;
	buf->stride = stride;
	buf->size = size;

	return true;
}

void
wex_buffer_destroy(struct wex_buffer *buf)
{
	if (!buf) {
		return;
	}
	if (buf->wl_buffer) {
		wl_buffer_destroy(buf->wl_buffer);
		buf->wl_buffer = NULL;
	}
	if (buf->data && buf->size > 0) {
		munmap(buf->data, buf->size);
		buf->data = NULL;
	}
}

static void
handle_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct wex_window_obj *win = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	win->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = handle_xdg_surface_configure,
};

static void
handle_xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
		int32_t width, int32_t height, struct wl_array *states)
{
	(void)xdg_toplevel;
	(void)states;
	struct wex_window_obj *win = data;
	if (width > 0 && height > 0) {
		win->configured_width = width;
		win->configured_height = height;
	}
}

static void
handle_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	(void)xdg_toplevel;
	struct wex_window_obj *win = data;
	win->closed = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = handle_xdg_toplevel_configure,
	.close = handle_xdg_toplevel_close,
};

bool
wex_window_init(struct wex_window_obj *win, struct wex_context *ctx,
		const char *title, int width, int height, uint32_t color)
{
	assert(win);
	assert(ctx);
	memset(win, 0, sizeof(*win));

	win->ctx = ctx;
	win->width = width;
	win->height = height;
	win->color = color;

	win->surface = wl_compositor_create_surface(ctx->compositor);
	if (!win->surface) {
		return false;
	}

	win->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->wm_base, win->surface);
	if (!win->xdg_surface) {
		return false;
	}
	xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);

	win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
	if (!win->xdg_toplevel) {
		return false;
	}
	xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, win);

	if (title) {
		xdg_toplevel_set_title(win->xdg_toplevel, title);
		xdg_toplevel_set_app_id(win->xdg_toplevel, title);
	}

	if (ctx->deco_manager) {
		win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
			ctx->deco_manager, win->xdg_toplevel);
		if (win->deco) {
			zxdg_toplevel_decoration_v1_set_mode(win->deco,
				ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
		}
	}

	if (!wex_buffer_create(ctx, &win->buffer, width, height, color)) {
		return false;
	}

	return true;
}

void
wex_window_commit_buffer(struct wex_window_obj *win)
{
	assert(win);
	wl_surface_attach(win->surface, win->buffer.wl_buffer, 0, 0);
	wl_surface_damage(win->surface, 0, 0, win->width, win->height);
	wl_surface_commit(win->surface);
}

void
wex_window_finish(struct wex_window_obj *win)
{
	if (!win) {
		return;
	}
	wex_buffer_destroy(&win->buffer);
	if (win->deco) {
		zxdg_toplevel_decoration_v1_destroy(win->deco);
		win->deco = NULL;
	}
	if (win->wex_win) {
		wex_window_v1_destroy(win->wex_win);
		win->wex_win = NULL;
	}
	if (win->zone_item) {
		xx_zone_item_v1_destroy(win->zone_item);
		win->zone_item = NULL;
	}
	if (win->xdg_toplevel) {
		xdg_toplevel_destroy(win->xdg_toplevel);
		win->xdg_toplevel = NULL;
	}
	if (win->xdg_surface) {
		xdg_surface_destroy(win->xdg_surface);
		win->xdg_surface = NULL;
	}
	if (win->surface) {
		wl_surface_destroy(win->surface);
		win->surface = NULL;
	}
	memset(win, 0, sizeof(*win));
}
