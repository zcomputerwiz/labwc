/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>
#include "protocols/wex-window.h"
#include "protocols/xx-zones.h"
#include "wex-window-v1-protocol.h"
#include "labwc.h"
#if HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include "view.h"
#include "workspaces.h"
#include "common/mem.h"
#include "foreign-toplevel-internal.h"

static const struct wex_window_manager_v1_interface wex_manager_impl;
static const struct wex_window_v1_interface wex_window_impl;

static bool
generate_window_handle(char handle[33])
{
    unsigned char bytes[16];
    FILE *file = fopen("/dev/urandom", "rb");
    if (!file) return false;
    bool ok = fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    fclose(file);
    if (!ok) return false;
    for (unsigned i = 0; i < sizeof(bytes); ++i)
        snprintf(handle + 2 * i, 3, "%02x", bytes[i]);
    return true;
}

static void wex_window_resource_destroy(struct wl_resource *resource);

struct wex_foreign_window {
	struct wl_list link;
	struct wex_window_manager *manager;
	struct view *view;
	struct wl_resource *resource;
	struct wl_listener toplevel_destroy;
	struct wex_window *frame;
	char controller_handle[33];
};

static void
release_foreign_association(struct wex_window *frame, bool notify)
{
	struct wex_foreign_window *foreign = frame->foreign;
	if (!foreign) {
		return;
	}
	if (frame->scene) {
		struct wex_scene *scene = frame->scene;
		frame->scene = NULL;
		wex_scene_destroy(scene);
		if (notify && frame->view) {
			wex_window_v1_send_foreign_group(frame->resource,
				WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
		}
	}
	if (foreign->view && foreign->view->wex_foreign_frame == frame) {
		foreign->view->wex_foreign_frame = NULL;
	}
	foreign->frame = NULL;
	frame->foreign = NULL;
	if (notify && frame->view) {
		wex_window_v1_send_foreign_association(frame->resource,
			WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	}
}

void
wex_window_drop_scene(struct wex_window *frame)
{
	struct wex_scene *scene = frame->scene;
	if (!scene) return;
	frame->scene = NULL;
	wex_scene_destroy(scene);
	if (frame->view) wex_window_v1_send_foreign_group(frame->resource,
		WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
}

static void
foreign_send_snapshot(struct wex_foreign_window *foreign)
{
	struct view *view = foreign->view;
	if (!view) {
		return;
	}
	uint32_t mode = WEX_FOREIGN_WINDOW_V1_DECORATION_NONE;
	if (view->ssd_preference == LAB_SSD_PREF_CLIENT) {
		mode = WEX_FOREIGN_WINDOW_V1_DECORATION_CLIENT;
	} else if (view_get_ssd_mode(view) != LAB_SSD_MODE_NONE ||
			(view->wex_foreign_frame && view->wex_foreign_frame->scene &&
			wex_scene_server_decorated(view->wex_foreign_frame->scene))) {
		mode = WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER;
	}
	struct view_size_hints hints = view_get_size_hints(view);
	wex_foreign_window_v1_send_geometry(foreign->resource,
		view->current.x, view->current.y, view->current.width,
		view->current.height);
	wex_foreign_window_v1_send_decoration(foreign->resource, mode);
	wex_foreign_window_v1_send_size_hints(foreign->resource,
		hints.min_width, hints.min_height, hints.width_inc, hints.height_inc,
		hints.base_width, hints.base_height);
	wex_foreign_window_v1_send_done(foreign->resource);
}

void
wex_foreign_window_update(struct view *view)
{
	struct wex_window_manager *manager = view->server->wex_window_manager;
	if (!manager) {
		return;
	}
	struct wex_foreign_window *foreign;
	wl_list_for_each(foreign, &manager->foreign_windows, link) {
		if (foreign->view == view) {
			foreign_send_snapshot(foreign);
		}
	}
}

static void
foreign_resource_destroy(struct wl_resource *resource)
{
	struct wex_foreign_window *foreign = wl_resource_get_user_data(resource);
	if (foreign->frame) {
		release_foreign_association(foreign->frame, true);
	}
	if (foreign->view) {
		wl_list_remove(&foreign->toplevel_destroy.link);
	}
	wl_list_remove(&foreign->link);
	free(foreign);
}

static void
handle_foreign_toplevel_destroy(struct wl_listener *listener, void *data)
{
	struct wex_foreign_window *foreign =
		wl_container_of(listener, foreign, toplevel_destroy);
	if (foreign->frame) {
		release_foreign_association(foreign->frame, true);
	}
	wl_list_remove(&foreign->toplevel_destroy.link);
	foreign->view = NULL;
	wex_foreign_window_v1_send_closed(foreign->resource);
}

static void
handle_foreign_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_foreign_refresh(struct wl_client *client, struct wl_resource *resource)
{
	foreign_send_snapshot(wl_resource_get_user_data(resource));
}

static const struct wex_foreign_window_v1_interface foreign_impl = {
	.destroy = handle_foreign_destroy,
	.refresh = handle_foreign_refresh,
};

static void
handle_manager_get_foreign(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *toplevel_resource)
{
	struct wex_window_manager *manager = wl_resource_get_user_data(resource);
	struct view *view, *found = NULL;
	/* wlroots exposes the handle resource list. Resolve by resource identity,
	 * rather than depending on its implementation-private user data layout. */
	wl_list_for_each(view, &manager->server->views, link) {
		if (view_is_wex(view) || !view->foreign_toplevel
				|| !view->foreign_toplevel->wlr_toplevel.handle) {
			continue;
		}
		struct wl_resource *candidate;
		wl_resource_for_each(candidate,
				&view->foreign_toplevel->wlr_toplevel.handle->resources) {
			if (candidate == toplevel_resource) {
				found = view;
				break;
			}
		}
		if (found) {
			break;
		}
	}
	struct wex_foreign_window *foreign = znew(*foreign);
	foreign->manager = manager;
	foreign->resource = wl_resource_create(client,
		&wex_foreign_window_v1_interface,
		wl_resource_get_version(resource) >= 4 ? 4 : 1, id);
	if (!foreign->resource) {
		free(foreign);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(foreign->resource, &foreign_impl,
		foreign, foreign_resource_destroy);
	wl_list_insert(manager->foreign_windows.prev, &foreign->link);
	if (!found) {
		wex_foreign_window_v1_send_closed(foreign->resource);
		return;
	}
	if (!found->wex_foreign_token[0]
			&& !generate_window_handle(found->wex_foreign_token)) {
		wl_client_post_implementation_error(client, "cannot generate native token");
		return;
	}
	foreign->view = found;
	if (wl_resource_get_version(foreign->resource) >= 4) {
		if (!generate_window_handle(foreign->controller_handle)) {
			foreign->view = NULL;
			wl_client_post_implementation_error(client, "cannot generate controller handle");
			return;
		}
		wex_foreign_window_v1_send_controller_handle(foreign->resource,
			foreign->controller_handle);
	}
	foreign->toplevel_destroy.notify = handle_foreign_toplevel_destroy;
	wl_signal_add(&found->foreign_toplevel->events.toplevel_destroy,
		&foreign->toplevel_destroy);
	wex_foreign_window_v1_send_token(foreign->resource,
		found->wex_foreign_token);
	foreign_send_snapshot(foreign);
}

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
	if (!generate_window_handle(win->handle)) {
        free(win);
        wl_client_post_implementation_error(client, "cannot generate window handle");
        return;
    }
    win->manager = manager;
	win->view = view;
	view->wex_window = win;
	view_set_ssd_mode(view, LAB_SSD_MODE_NONE);

	win->resource = wl_resource_create(client,
		&wex_window_v1_interface, version, id);
	wl_resource_set_implementation(win->resource, &wex_window_impl, win, wex_window_resource_destroy);
	wl_list_insert(manager->windows.prev, &win->link);
    if (version >= 2) wex_window_v1_send_handle(win->resource, win->handle);
}

static const struct wex_window_manager_v1_interface wex_manager_impl = {
	.destroy = handle_manager_destroy,
	.get_window = handle_manager_get_window,
	.get_foreign = handle_manager_get_foreign,
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

	if (!win || !sibling || win == sibling || !win->view || !sibling->view
        || win->layer != sibling->layer
        || win->view->scene_tree->node.parent != sibling->view->scene_tree->node.parent) {
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

	if (!win || !sibling || win == sibling || !win->view || !sibling->view
        || win->layer != sibling->layer
        || win->view->scene_tree->node.parent != sibling->view->scene_tree->node.parent) {
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
	if (win->foreign && layer != WEX_WINDOW_V1_LAYER_NORMAL
			&& layer != WEX_WINDOW_V1_LAYER_TOPMOST) {
		release_foreign_association(win, true);
	}
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
	if (win->scene) {
		desktop_focus_view_forced(wex_scene_native(win->scene), true);
		return;
	}

	desktop_focus_view_forced(win->view, /*raise*/ true);
	win->manager->server->active_view = win->view;
}

static struct wex_window *
find_window_handle(struct wex_window_manager *manager, const char *handle)
{
    struct wex_window *win;
    wl_list_for_each(win, &manager->windows, link) {
        if (win->view && !strcmp(win->handle, handle)) return win;
    }
    return NULL;
}

static void
handle_wex_window_place_above_handle(struct wl_client *client,
        struct wl_resource *resource, const char *handle)
{
    struct wex_window *win = wl_resource_get_user_data(resource);
    struct wex_window *sibling = find_window_handle(win->manager, handle);
    if (sibling) handle_wex_window_place_above(client, resource, sibling->resource);
}

static void
handle_wex_window_place_below_handle(struct wl_client *client,
        struct wl_resource *resource, const char *handle)
{
    struct wex_window *win = wl_resource_get_user_data(resource);
    struct wex_window *sibling = find_window_handle(win->manager, handle);
    if (sibling) handle_wex_window_place_below(client, resource, sibling->resource);
}

static void
handle_wex_window_restore(struct wl_client *client, struct wl_resource *resource)
{
    struct wex_window *win = wl_resource_get_user_data(resource);
    if (win && win->view && win->view->minimized) view_minimize(win->view, false);
}

static void
handle_wex_window_associate_foreign(struct wl_client *client,
		struct wl_resource *resource, const char *token, const char *handle)
{
	struct wex_window *frame = wl_resource_get_user_data(resource);
	struct wex_foreign_window *foreign;
	if (!*token && !*handle) {
		release_foreign_association(frame, false);
		wex_window_v1_send_foreign_association(resource,
			WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
		return;
	}
	if (!frame->view || (frame->layer != WEX_WINDOW_V1_LAYER_NORMAL
			&& frame->layer != WEX_WINDOW_V1_LAYER_TOPMOST)) {
		goto unavailable;
	}
	wl_list_for_each(foreign, &frame->manager->foreign_windows, link) {
		struct view *view = foreign->view;
		if (!view || view_is_wex(view) || view == frame->view
				|| strcmp(view->wex_foreign_token, token)
				|| !foreign->controller_handle[0]
				|| strcmp(foreign->controller_handle, handle)) {
			continue;
		}
		if (view->wex_foreign_frame && view->wex_foreign_frame != frame) {
			goto unavailable;
		}
		if (frame->foreign != foreign) {
			release_foreign_association(frame, false);
			frame->foreign = foreign;
			foreign->frame = frame;
			view->wex_foreign_frame = frame;
		}
		wex_window_v1_send_foreign_association(resource,
			WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
		return;
	}
unavailable:
	wex_window_v1_send_foreign_association(resource,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_UNAVAILABLE);
}

static void
handle_wex_window_commit_foreign(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wex_window *frame = wl_resource_get_user_data(resource);
	struct wlr_box rect = {x, y, width, height};
	if (frame->foreign && frame->foreign->view && !frame->scene) {
		frame->scene = wex_scene_create(frame, frame->foreign->view, &rect);
		if (frame->scene) {
			wex_scene_view_moved(frame->view);
			wex_foreign_window_update(frame->foreign->view);
		}
	}
	wex_window_v1_send_foreign_group(resource, frame->scene && wex_scene_matches(frame->scene, &rect) ?
		WEX_WINDOW_V1_GROUP_STATUS_ACTIVE : WEX_WINDOW_V1_GROUP_STATUS_NOT_READY);
}

static const struct wex_window_v1_interface wex_window_impl = {
	.destroy = handle_wex_window_destroy,
	.place_above = handle_wex_window_place_above,
	.place_below = handle_wex_window_place_below,
	.place_bottom = handle_wex_window_place_bottom,
	.set_layer = handle_wex_window_set_layer,
	.set_exclusive_edge = handle_wex_window_set_exclusive_edge,
	.activate = handle_wex_window_activate,
    .place_above_handle = handle_wex_window_place_above_handle,
    .place_below_handle = handle_wex_window_place_below_handle,
    .restore = handle_wex_window_restore,
	.associate_foreign = handle_wex_window_associate_foreign,
	.commit_foreign = handle_wex_window_commit_foreign,
};

static void
wex_window_resource_destroy(struct wl_resource *resource)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (win) {
		release_foreign_association(win, false);
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
	release_foreign_association(win, false);
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
	if (version >= 3) {
		const char *xdisplay = "";
#if HAVE_XWAYLAND
		if (manager->server->xwayland) {
			xdisplay = manager->server->xwayland->display_name;
		}
#endif
		wex_window_manager_v1_send_native_environment(resource,
			manager->server->wex_native_socket_name, xdisplay);
	}
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
	wl_list_init(&manager->foreign_windows);

	manager->global = wl_global_create(server->wl_display,
		&wex_window_manager_v1_interface, 5, manager, handle_wex_manager_bind);
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
	struct wex_foreign_window *foreign, *foreign_tmp;
	wl_list_for_each_safe(foreign, foreign_tmp, &manager->foreign_windows, link) {
		wl_resource_destroy(foreign->resource);
	}
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
