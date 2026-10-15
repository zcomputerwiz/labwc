/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "protocols/pointer-warp.h"
#include "pointer-warp-v1-protocol.h"
#include "labwc.h"
#include "input/cursor.h"
#include "view.h"
#include "common/mem.h"

static void
handle_warp_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_warp_pointer(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *surface_resource,
		struct wl_resource *pointer_resource,
		wl_fixed_t x,
		wl_fixed_t y,
		uint32_t serial)
{
	(void)client; (void)pointer_resource; (void)serial;
	struct lab_pointer_warp_manager *manager = wl_resource_get_user_data(resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!surface) {
		return;
	}

	struct view *view = view_from_wlr_surface(surface);
	if (!view) {
		return;
	}

	double sx = wl_fixed_to_double(x);
	double sy = wl_fixed_to_double(y);

	/* Check bounds within surface */
	if (sx < 0 || sx > surface->current.width || sy < 0 || sy > surface->current.height) {
		return;
	}

	double lx = view->current.x + sx;
	double ly = view->current.y + sy;
	if (view->type == LAB_XDG_SHELL_VIEW) {
		struct wlr_box geometry;
		wlr_xdg_surface_get_geometry(xdg_surface_from_view(view), &geometry);
		lx -= geometry.x;
		ly -= geometry.y;
	}

	struct seat *seat = &manager->server->seat;
	wlr_cursor_warp(seat->cursor, NULL, lx, ly);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t time_msec = now.tv_sec * 1000 + now.tv_nsec / 1000000;

	/* A warp can cross views (for example a task menu starting a Wine move
	 * loop). Route it like normal motion so enter/leave and surface-local
	 * coordinates belong to the surface now under the pointer. */
	if (cursor_process_motion(manager->server, time_msec, &sx, &sy)) {
		wlr_seat_pointer_notify_motion(seat->seat, time_msec, sx, sy);
	}
	wlr_seat_pointer_notify_frame(seat->seat);
}

static const struct wp_pointer_warp_v1_interface warp_impl = {
	.destroy = handle_warp_destroy,
	.warp_pointer = handle_warp_pointer,
};

static void
handle_warp_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id)
{
	struct lab_pointer_warp_manager *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&wp_pointer_warp_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &warp_impl, manager, NULL);
}

static void
handle_display_destroy(struct wl_listener *listener, void *data)
{
	struct lab_pointer_warp_manager *manager =
		wl_container_of(listener, manager, display_destroy);
	lab_pointer_warp_manager_finish(manager->server);
}

void
lab_pointer_warp_manager_init(struct server *server)
{
	struct lab_pointer_warp_manager *manager = znew(*manager);
	manager->server = server;

	manager->global = wl_global_create(server->wl_display,
		&wp_pointer_warp_v1_interface, 1, manager, handle_warp_bind);
	if (!manager->global) {
		wlr_log(WLR_ERROR, "failed to create wp_pointer_warp_v1 global");
		free(manager);
		return;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(server->wl_display, &manager->display_destroy);

	server->pointer_warp = manager;
}

void
lab_pointer_warp_manager_finish(struct server *server)
{
	struct lab_pointer_warp_manager *manager = server->pointer_warp;
	if (!manager) {
		return;
	}

	wl_global_destroy(manager->global);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
	server->pointer_warp = NULL;
}
