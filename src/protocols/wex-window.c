/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "protocols/wex-window.h"
#include "protocols/xx-zones.h"
#include "wex-window-v1-protocol.h"
#include "labwc.h"
#include "view.h"
#include "workspaces.h"
#include "common/mem.h"

static const struct wex_window_manager_v1_interface wex_manager_impl;
static const struct wex_window_v1_interface wex_window_impl;

static void wex_window_resource_destroy(struct wl_resource *resource);

bool
wex_client_is_privileged(const struct wl_client *client, struct server *server)
{
	if (!client || !server || !server->wex_socket_name) {
		return false;
	}
	int fd = wl_client_get_fd((struct wl_client *)client);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_un addr = { 0 };
	socklen_t len = sizeof(addr);
	if (getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
		if (strstr(addr.sun_path, server->wex_socket_name)) {
			return true;
		}
	}
	return false;
}

static void
handle_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_manager_get_window(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *toplevel_resource)
{
	struct wex_window_manager *manager = wl_resource_get_user_data(resource);
	if (!wex_client_is_privileged(client, manager->server)) {
		return;
	}

	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_from_resource(toplevel_resource);
	if (!toplevel || !toplevel->base) {
		return;
	}

	struct view *view = toplevel->base->data;
	if (!view) {
		view = lookup_view_by_xdg_toplevel_public(manager->server, toplevel);
	}
	if (!view) {
		return;
	}

	uint32_t version = wl_resource_get_version(resource);
	struct wex_window *win = znew(*win);
	win->manager = manager;
	win->view = view;
	view->wex_window = win;

	win->resource = wl_resource_create(client,
		&wex_window_v1_interface, version, id);
	wl_resource_set_implementation(win->resource, &wex_window_impl, win, wex_window_resource_destroy);
	wl_list_insert(manager->windows.prev, &win->link);
}

static const struct wex_window_manager_v1_interface wex_manager_impl = {
	.destroy = handle_manager_destroy,
	.get_window = handle_manager_get_window,
};

static void
handle_wex_window_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_wex_window_place_above(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *sibling_resource)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	struct wex_window *sibling = wl_resource_get_user_data(sibling_resource);

	if (!win || !sibling || !win->view || !sibling->view) {
		return;
	}

	wlr_scene_node_place_above(&win->view->scene_tree->node,
		&sibling->view->scene_tree->node);
	wl_list_remove(&win->view->link);
	wl_list_insert(sibling->view->link.prev, &win->view->link);
}

static void
handle_wex_window_place_below(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *sibling_resource)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	struct wex_window *sibling = wl_resource_get_user_data(sibling_resource);

	if (!win || !sibling || !win->view || !sibling->view) {
		return;
	}

	wlr_scene_node_place_below(&win->view->scene_tree->node,
		&sibling->view->scene_tree->node);
	wl_list_remove(&win->view->link);
	wl_list_insert(&sibling->view->link, &win->view->link);
}

static void
handle_wex_window_place_bottom(struct wl_client *client,
		struct wl_resource *resource)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (!win || !win->view) {
		return;
	}

	wlr_scene_node_lower_to_bottom(&win->view->scene_tree->node);
	wl_list_remove(&win->view->link);
	wl_list_insert(win->manager->server->views.prev, &win->view->link);
}

static void
handle_wex_window_set_layer(struct wl_client *client,
		struct wl_resource *resource,
		uint32_t layer)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (!win || !win->view) {
		return;
	}

	win->layer = layer;
	win->view->wex_layer = layer;
	struct server *server = win->manager->server;

	switch (layer) {
	case WEX_WINDOW_V1_LAYER_TOPMOST:
		wlr_scene_node_reparent(&win->view->scene_tree->node,
			server->view_tree_always_on_top);
		break;
	case WEX_WINDOW_V1_LAYER_DESKTOP:
		wlr_scene_node_reparent(&win->view->scene_tree->node,
			server->desktop_tree);
		view_set_ssd_mode(win->view, LAB_SSD_MODE_NONE);
		break;
	case WEX_WINDOW_V1_LAYER_SHELL:
		wlr_scene_node_reparent(&win->view->scene_tree->node,
			server->shell_tree);
		view_set_ssd_mode(win->view, LAB_SSD_MODE_NONE);
		break;
	case WEX_WINDOW_V1_LAYER_NORMAL:
	default:
		if (win->view->workspace) {
			wlr_scene_node_reparent(&win->view->scene_tree->node,
				win->view->workspace->tree);
		} else if (server->workspaces.current) {
			wlr_scene_node_reparent(&win->view->scene_tree->node,
				server->workspaces.current->tree);
		}
		break;
	}
}

static void
handle_wex_window_set_exclusive_edge(struct wl_client *client,
		struct wl_resource *resource,
		uint32_t edge, uint32_t size)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (!win || !win->view) {
		return;
	}

	win->exclusive_edge = edge;
	win->exclusive_size = size;
	win->view->wex_exclusive_edge = edge;
	win->view->wex_exclusive_size = size;

	if (win->view->output) {
		output_update_usable_area(win->view->output);
	} else {
		output_update_all_usable_areas(win->manager->server, false);
	}
}

static void
handle_wex_window_activate(struct wl_client *client,
		struct wl_resource *resource)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (!win || !win->view) {
		return;
	}

	if (win->view->minimized) {
		view_minimize(win->view, false);
	}

	wlr_scene_node_raise_to_top(&win->view->scene_tree->node);

	if (win->view->surface) {
		seat_focus_surface(&win->manager->server->seat, win->view->surface);
		win->manager->server->active_view = win->view;
	}
}

static const struct wex_window_v1_interface wex_window_impl = {
	.destroy = handle_wex_window_destroy,
	.place_above = handle_wex_window_place_above,
	.place_below = handle_wex_window_place_below,
	.place_bottom = handle_wex_window_place_bottom,
	.set_layer = handle_wex_window_set_layer,
	.set_exclusive_edge = handle_wex_window_set_exclusive_edge,
	.activate = handle_wex_window_activate,
};

static void
wex_window_resource_destroy(struct wl_resource *resource)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (win) {
		if (win->view) {
			win->view->wex_window = NULL;
		}
		wl_list_remove(&win->link);
		free(win);
	}
}

void
wex_window_on_view_destroy(struct wex_window *win)
{
	if (!win) {
		return;
	}
	win->view = NULL;
}

static void
handle_wex_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id)
{
	struct wex_window_manager *manager = data;
	if (!wex_client_is_privileged(client, manager->server)) {
		wl_resource_post_error(wl_client_get_object(client, id),
			WL_DISPLAY_ERROR_INVALID_OBJECT, "permission denied");
		return;
	}

	struct wl_resource *resource = wl_resource_create(client,
		&wex_window_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wex_manager_impl, manager, NULL);
}

static void
handle_display_destroy(struct wl_listener *listener, void *data)
{
	struct wex_window_manager *manager =
		wl_container_of(listener, manager, display_destroy);
	wex_window_manager_finish(manager->server);
}

void
wex_window_manager_init(struct server *server)
{
	struct wex_window_manager *manager = znew(*manager);
	manager->server = server;
	wl_list_init(&manager->windows);

	manager->global = wl_global_create(server->wl_display,
		&wex_window_manager_v1_interface, 1, manager, handle_wex_manager_bind);
	if (!manager->global) {
		wlr_log(WLR_ERROR, "failed to create wex_window_manager_v1 global");
		free(manager);
		return;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(server->wl_display, &manager->display_destroy);

	server->wex_window_manager = manager;
}

void
wex_window_manager_finish(struct server *server)
{
	struct wex_window_manager *manager = server->wex_window_manager;
	if (!manager) {
		return;
	}

	struct wex_window *win, *win_tmp;
	wl_list_for_each_safe(win, win_tmp, &manager->windows, link) {
		if (win->view) {
			win->view->wex_window = NULL;
		}
		wl_list_remove(&win->link);
		free(win);
	}

	wl_global_destroy(manager->global);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
	server->wex_window_manager = NULL;
}
