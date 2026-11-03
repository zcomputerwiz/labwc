/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/transform.h>
#include <wlr/util/log.h>
#include "protocols/wex-window.h"
#include "protocols/xx-zones.h"
#include "wex-window-v1-protocol.h"
#include "labwc.h"
#include "node.h"
#if HAVE_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include "view.h"
#include "workspaces.h"
#include "common/mem.h"
#include "foreign-toplevel-internal.h"

static const struct wex_window_manager_v1_interface wex_manager_impl;
static const struct wex_window_v1_interface wex_window_impl;
static void handle_manager_get_render_surface(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
static void drop_render_children(struct wex_window *);
static void handle_window_surface_commit(struct wl_listener *, void *);
static void drop_corner_pieces(struct wex_window *);
static void drop_effects(struct wex_window *);
static void handle_manager_set_effects(struct wl_client *, struct wl_resource *,
	const char *, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t);

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

void
wex_window_release_pointer_capture(struct wex_window *frame)
{
	if (frame->manager->pointer_capture != frame) return;
	frame->manager->pointer_capture = NULL;
	if (frame->scene) wex_scene_capture_lost(frame->scene);
	wex_window_v1_send_foreign_pointer_capture(frame->resource, frame->capture_epoch, 0);
	cursor_update_focus(frame->manager->server);
}

void
wex_window_send_foreign_focus(struct wex_window *frame)
{
	if (!frame->scene || wex_scene_focus_target(frame->scene) != WEX_WINDOW_V1_FOCUS_TARGET_FRAME)
		wex_window_release_pointer_capture(frame);
	if (wl_resource_get_version(frame->resource) >= 6) {
		wex_window_v1_send_foreign_focus(frame->resource, frame->focus_epoch,
			frame->scene ? wex_scene_focus_target(frame->scene) : WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	}
}

struct wex_foreign_window {
	struct wl_list link;
	struct wex_window_manager *manager;
	struct view *view;
	struct wl_resource *resource;
	struct wl_listener toplevel_destroy;
	struct wl_listener constraints_commit;
	struct view_size_hints last_hints;
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
			wex_window_send_foreign_focus(frame);
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
	wex_window_send_foreign_focus(frame);
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
	if (wl_resource_get_version(foreign->resource) >= 9) {
		wex_foreign_window_v1_send_maximum_size(foreign->resource,
			hints.max_width, hints.max_height);
	}
	wex_foreign_window_v1_send_done(foreign->resource);
	foreign->last_hints = hints;
}

static void
foreign_constraints_committed(struct wl_listener *listener, void *data)
{
	struct wex_foreign_window *foreign = wl_container_of(listener, foreign, constraints_commit);
	struct view_size_hints hints = view_get_size_hints(foreign->view);
	if (memcmp(&hints, &foreign->last_hints, sizeof(hints))) {
		foreign_send_snapshot(foreign);
	}
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
		wl_list_remove(&foreign->constraints_commit.link);
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
	wl_list_remove(&foreign->constraints_commit.link);
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
		wl_resource_get_version(resource) >= 9 ? 9 :
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
	foreign->constraints_commit.notify = foreign_constraints_committed;
	wl_signal_add(&found->surface->events.commit, &foreign->constraints_commit);
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
	wl_list_init(&win->surface_commit.link);
	if (view->surface) {
		win->surface_commit.notify = handle_window_surface_commit;
		wl_signal_add(&view->surface->events.commit, &win->surface_commit);
	}
    if (version >= 2) wex_window_v1_send_handle(win->resource, win->handle);
}

static const struct wex_window_manager_v1_interface wex_manager_impl = {
	.destroy = handle_manager_destroy,
	.get_window = handle_manager_get_window,
	.get_foreign = handle_manager_get_foreign,
	.get_render_surface = handle_manager_get_render_surface,
	.set_effects = handle_manager_set_effects,
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
    if (!handle || !*handle) return NULL;
    struct wex_window *win;
    wl_list_for_each(win, &manager->windows, link) {
        if (win->view && !strcmp(win->handle, handle)) return win;
    }
    return NULL;
}

struct wex_render_surface {
	struct wl_list link;
	struct wex_window_manager *manager;
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wex_window *parent;
	struct wlr_scene_tree *tree;
	struct wl_listener surface_destroy, tree_destroy, parent_commit;
	int x, y, width, height;
};

static void render_detach(struct wex_render_surface *render)
{
	if (render->parent) wl_list_remove(&render->parent_commit.link);
	render->parent = NULL;
	if (render->tree) {
		wl_list_remove(&render->tree_destroy.link);
		wlr_scene_node_destroy(&render->tree->node);
		render->tree = NULL;
	}
	if (render->surface) wlr_surface_unmap(render->surface);
}

static void render_tree_destroy(struct wl_listener *listener, void *data)
{
	struct wex_render_surface *render = wl_container_of(listener, render, tree_destroy);
	wl_list_remove(&render->tree_destroy.link);
	render->tree = NULL;
}

static void render_update_clip(struct wex_render_surface *render)
{
	if (!render->parent || !render->tree) return;
	struct wlr_surface *parent = render->parent->view->surface;
	int left = render->x < 0 ? -render->x : 0;
	int top = render->y < 0 ? -render->y : 0;
	int right = parent->current.width - render->x;
	int bottom = parent->current.height - render->y;
	if (right > render->width) right = render->width;
	if (bottom > render->height) bottom = render->height;
	struct wlr_box clip = {left, top, right > left ? right - left : 0, bottom > top ? bottom - top : 0};
	/* wlroots treats an empty clip as "no clip", not "draw nothing". */
	wlr_scene_node_set_enabled(&render->tree->node, clip.width > 0 && clip.height > 0);
	wlr_scene_subsurface_tree_set_clip(&render->tree->node, &clip);
}

static void render_parent_commit(struct wl_listener *listener, void *data)
{
	struct wex_render_surface *render = wl_container_of(listener, render, parent_commit);
	render_update_clip(render);
}

static void render_role_commit(struct wlr_surface *surface)
{
	struct wex_render_surface *render = surface->role_resource ? wl_resource_get_user_data(surface->role_resource) : NULL;
	/* Rendering content never owns frame input or keyboard focus. */
	pixman_region32_clear(&surface->input_region);
	if (render && render->tree && wlr_surface_has_buffer(surface)) wlr_surface_map(surface);
	else wlr_surface_unmap(surface);
}

static const struct wlr_surface_role render_role = {
	.name = "wex_render_surface_v1",
	.commit = render_role_commit,
};

static void render_destroy_resource(struct wl_resource *resource)
{
	struct wex_render_surface *render = wl_resource_get_user_data(resource);
	if (!render) return;
	render_detach(render);
	if (render->surface) wl_list_remove(&render->surface_destroy.link);
	wl_list_remove(&render->link);
	free(render);
}

static void render_surface_destroy(struct wl_listener *listener, void *data)
{
	struct wex_render_surface *render = wl_container_of(listener, render, surface_destroy);
	wl_list_remove(&render->surface_destroy.link);
	render->surface = NULL;
	wl_resource_destroy(render->resource);
}

static void render_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void render_set_parent(struct wl_client *client, struct wl_resource *resource,
		const char *handle, int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wex_render_surface *render = wl_resource_get_user_data(resource);
	struct wex_window *parent = NULL;
	if (wex_client_is_privileged(client, render->manager->server) && strlen(handle) == 32 &&
		width > 0 && width <= 16384 && height > 0 && height <= 16384 &&
		x >= -65536 && x <= 65536 && y >= -65536 && y <= 65536)
		parent = find_window_handle(render->manager, handle);
	if (!parent || !parent->view || !parent->view->surface) {
		render_detach(render);
		return;
	}
	if (parent != render->parent || !render->tree) {
		render_detach(render);
		render->tree = wlr_scene_subsurface_tree_create(parent->view->content_tree, render->surface);
		if (!render->tree) { wl_resource_post_no_memory(resource); return; }
		render->tree_destroy.notify = render_tree_destroy;
		wl_signal_add(&render->tree->node.events.destroy, &render->tree_destroy);
		render->parent = parent;
		render->parent_commit.notify = render_parent_commit;
		wl_signal_add(&parent->view->surface->events.commit, &render->parent_commit);
	}
	render->x = x; render->y = y; render->width = width; render->height = height;
	wlr_scene_node_set_position(&render->tree->node, x, y);
	render_update_clip(render);
	render_role_commit(render->surface);
}

static const struct wex_render_surface_v1_interface render_impl = {
	.destroy = render_destroy,
	.set_parent = render_set_parent,
};

static void handle_manager_get_render_surface(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource)
{
	struct wex_window_manager *manager = wl_resource_get_user_data(resource);
	if (!wex_client_is_privileged(client, manager->server)) {
		wl_resource_post_error(resource, 0, "permission denied");
		return;
	}
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	if (!wlr_surface_set_role(surface, &render_role, resource, 0) || surface->role_resource) {
		if (surface->role_resource) wl_resource_post_error(resource, 0, "render surface already has a role object");
		return;
	}
	struct wex_render_surface *render = calloc(1, sizeof(*render));
	if (!render) { wl_client_post_no_memory(client); return; }
	render->resource = wl_resource_create(client, &wex_render_surface_v1_interface, 1, id);
	if (!render->resource) { free(render); wl_client_post_no_memory(client); return; }
	render->manager = manager; render->surface = surface;
	wl_resource_set_implementation(render->resource, &render_impl, render, render_destroy_resource);
	wlr_surface_set_role_object(surface, render->resource);
	render->surface_destroy.notify = render_surface_destroy;
	wl_signal_add(&surface->events.destroy, &render->surface_destroy);
	wl_list_insert(manager->render_surfaces.prev, &render->link);
}

static void drop_render_children(struct wex_window *parent)
{
	struct wex_render_surface *render;
	wl_list_for_each(render, &parent->manager->render_surfaces, link)
		if (render->parent == parent) render_detach(render);
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
	wex_window_send_foreign_focus(frame);
}

static void
handle_wex_window_set_foreign_focus(struct wl_client *client, struct wl_resource *resource,
		uint32_t epoch, uint32_t target)
{
	struct wex_window *frame = wl_resource_get_user_data(resource);
	frame->focus_epoch = epoch;
	if (frame->scene && !frame->view->minimized && !wex_scene_native(frame->scene)->minimized) {
		uint32_t current = wex_scene_focus_target(frame->scene);
		if (target == WEX_WINDOW_V1_FOCUS_TARGET_NATIVE) {
			desktop_focus_view_forced(wex_scene_native(frame->scene), true);
		} else if (current != WEX_WINDOW_V1_FOCUS_TARGET_NONE) {
			if (target == WEX_WINDOW_V1_FOCUS_TARGET_FRAME)
				desktop_focus_view_forced(frame->view, false);
			else if (target == WEX_WINDOW_V1_FOCUS_TARGET_RESTORE_NATIVE)
				desktop_focus_view_forced(wex_scene_native(frame->scene), false);
		}
	}
	wex_window_send_foreign_focus(frame);
}

static void
handle_wex_window_uncommit_foreign(struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
	struct wex_window *frame = wl_resource_get_user_data(resource);
	if (frame->scene) wex_window_drop_scene(frame);
	else {
		wex_window_v1_send_foreign_group(resource, WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
		wex_window_send_foreign_focus(frame);
	}
	wex_window_v1_send_configured_foreign(resource, serial, WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
}

static void
handle_wex_window_configure_foreign(struct wl_client *client, struct wl_resource *resource,
		uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wex_window *frame = wl_resource_get_user_data(resource);
	struct wlr_box rect = {x, y, width, height};
	if (wex_scene_reconfigure(frame, serial, &rect)) return;
	handle_wex_window_commit_foreign(client, resource, x, y, width, height);
	wex_window_v1_send_configured_foreign(resource, serial,
		frame->scene && wex_scene_matches(frame->scene, &rect) ?
		WEX_WINDOW_V1_GROUP_STATUS_ACTIVE : WEX_WINDOW_V1_GROUP_STATUS_NOT_READY);
}

static void
handle_wex_window_capture_foreign_pointer(struct wl_client *client, struct wl_resource *resource,
		uint32_t epoch, uint32_t capture)
{
	struct wex_window *frame = wl_resource_get_user_data(resource);
	bool allowed = capture == 1 && frame->scene && frame->view && frame->view->mapped &&
		epoch == frame->focus_epoch &&
		wex_scene_focus_target(frame->scene) == WEX_WINDOW_V1_FOCUS_TARGET_FRAME;
	if (!capture && epoch == frame->capture_epoch) wex_window_release_pointer_capture(frame);
	else if (allowed) {
		if (frame->manager->pointer_capture && frame->manager->pointer_capture != frame)
			wex_window_release_pointer_capture(frame->manager->pointer_capture);
		frame->manager->pointer_capture = frame;
		frame->capture_epoch = epoch;
		cursor_update_focus(frame->manager->server);
	}
	wex_window_v1_send_foreign_pointer_capture(resource, epoch, allowed);
}

static void
handle_wex_window_resize_foreign(struct wl_client *client, struct wl_resource *resource,
		uint32_t serial, uint32_t edges, int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wlr_box rect = {x, y, width, height};
	wex_scene_resize(wl_resource_get_user_data(resource), serial, edges, &rect);
}

static void
handle_wex_window_commit_foreign_resize(struct wl_client *client, struct wl_resource *resource,
		uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct wlr_box rect = {x, y, width, height};
	wex_scene_commit_resize(wl_resource_get_user_data(resource), serial, &rect);
}

static void
handle_wex_window_cancel_foreign_resize(struct wl_client *client, struct wl_resource *resource,
		uint32_t serial)
{
	wex_scene_cancel_resize(wl_resource_get_user_data(resource), serial);
}

struct wex_corner_piece {
	struct wlr_scene_surface *scene_surface;
	struct wlr_scene_buffer *buffer;
	struct wlr_box box;
	struct wl_listener destroy;
	struct wex_window *win;
};

static struct wlr_scene_buffer *
find_surface_buffer(struct wlr_scene_node *node, struct wlr_surface *surface)
{
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
		return scene_surface && scene_surface->surface == surface ? buffer : NULL;
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_node *child;
	wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link) {
		struct wlr_scene_buffer *found = find_surface_buffer(child, surface);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static inline int
compute_corner_inset(int r, int y)
{
	int k = r - y;
	int m = 4 * (r * r - k * (k - 1)) - 1;
	int s = 0;
	while ((s + 1) * (s + 1) <= m) s++;
	int inset = r - (s + 1) / 2;
	return inset > 0 ? inset : 0;
}

static bool
corner_piece_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	struct node_descriptor *desc = buffer->node.data;
	struct view *view = desc ? desc->data : NULL;
	struct wex_window *win = view ? view->wex_window : NULL;
	if (!win || !view || !view->surface) {
		return false;
	}
	for (size_t i = 0; i < win->num_corner_pieces; ++i) {
		if (win->corner_pieces[i].buffer == buffer) {
			*sx += win->corner_pieces[i].box.x;
			*sy += win->corner_pieces[i].box.y;
			return wlr_surface_point_accepts_input(view->surface, *sx, *sy);
		}
	}
	return false;
}

static void
corner_piece_destroyed(struct wl_listener *listener, void *data)
{
	struct wex_corner_piece *piece = wl_container_of(listener, piece, destroy);
	wl_list_remove(&piece->destroy.link);
	piece->buffer = NULL;
	piece->scene_surface = NULL;
}

static void
crop_corner_piece(struct wlr_scene_buffer *buffer, struct wlr_surface *surface, struct wlr_box box)
{
	if (!buffer) {
		return;
	}
	wlr_scene_node_set_enabled(&buffer->node, box.width > 0 && box.height > 0);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	struct wlr_fbox source;
	wlr_surface_get_buffer_source_box(surface, &source);
	int width = surface->current.buffer_width;
	int height = surface->current.buffer_height;
	wlr_fbox_transform(&source, &source, surface->current.transform, width, height);
	wlr_output_transform_coords(surface->current.transform, &width, &height);
	double source_width = source.width, source_height = source.height;
	source.x += (double)box.x * source_width / surface->current.width;
	source.y += (double)box.y * source_height / surface->current.height;
	source.width = (double)box.width * source_width / surface->current.width;
	source.height = (double)box.height * source_height / surface->current.height;
	wlr_fbox_transform(&source, &source, wlr_output_transform_invert(surface->current.transform), width, height);
	wlr_scene_buffer_set_source_box(buffer, &source);
	wlr_scene_buffer_set_dest_size(buffer, box.width, box.height);
	wlr_scene_node_set_position(&buffer->node, box.x, box.y);

	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	pixman_region32_copy(&opaque, &surface->opaque_region);
	pixman_region32_translate(&opaque, -box.x, -box.y);
	pixman_region32_intersect_rect(&opaque, &opaque, 0, 0, box.width, box.height);
	wlr_scene_buffer_set_opaque_region(buffer, &opaque);
	pixman_region32_fini(&opaque);
}

static void
drop_corner_pieces(struct wex_window *win)
{
	if (win->corner_pieces) {
		for (size_t i = 0; i < win->num_corner_pieces; ++i) {
			if (win->corner_pieces[i].buffer) {
				wl_list_remove(&win->corner_pieces[i].destroy.link);
			}
		}
		free(win->corner_pieces);
		win->corner_pieces = NULL;
	}
	win->num_corner_pieces = 0;
	if (win->corner_tree) {
		wlr_scene_node_destroy(&win->corner_tree->node);
		win->corner_tree = NULL;
	}
}

static void
drop_effects(struct wex_window *win)
{
	if (win->backdrop_tree) {
		wlr_scene_node_destroy(&win->backdrop_tree->node);
		win->backdrop_tree = NULL;
	}
	if (win->shadow_tree) {
		wlr_scene_node_destroy(&win->shadow_tree->node);
		win->shadow_tree = NULL;
	}
}

static size_t
compute_corner_slices(int width, int height, int r, struct wlr_box slices[65])
{
	if (r > 32) r = 32;
	if (r * 2 > width) r = width / 2;
	if (r * 2 > height) r = height / 2;
	if (r <= 0) return 0;

	size_t count = 0;

	/* Top slices: y = 0 .. r - 1 */
	int cur_inset = -1;
	int cur_start_y = 0;
	int cur_height = 0;
	for (int y = 0; y < r; y++) {
		int inset = compute_corner_inset(r, y);
		if (cur_inset == -1) {
			cur_inset = inset;
			cur_start_y = y;
			cur_height = 1;
		} else if (inset == cur_inset) {
			cur_height++;
		} else {
			if (width > 2 * cur_inset) {
				slices[count++] = (struct wlr_box){
					.x = cur_inset,
					.y = cur_start_y,
					.width = width - 2 * cur_inset,
					.height = cur_height,
				};
			}
			cur_inset = inset;
			cur_start_y = y;
			cur_height = 1;
		}
	}
	if (cur_height > 0 && width > 2 * cur_inset) {
		slices[count++] = (struct wlr_box){
			.x = cur_inset,
			.y = cur_start_y,
			.width = width - 2 * cur_inset,
			.height = cur_height,
		};
	}

	/* Middle slice */
	if (height > 2 * r) {
		slices[count++] = (struct wlr_box){
			.x = 0,
			.y = r,
			.width = width,
			.height = height - 2 * r,
		};
	}

	/* Bottom slices: y = height - r .. height - 1 */
	cur_inset = -1;
	cur_start_y = 0;
	cur_height = 0;
	for (int y = height - r; y < height; y++) {
		int y_from_bottom = height - 1 - y;
		int inset = compute_corner_inset(r, y_from_bottom);
		if (cur_inset == -1) {
			cur_inset = inset;
			cur_start_y = y;
			cur_height = 1;
		} else if (inset == cur_inset) {
			cur_height++;
		} else {
			if (width > 2 * cur_inset) {
				slices[count++] = (struct wlr_box){
					.x = cur_inset,
					.y = cur_start_y,
					.width = width - 2 * cur_inset,
					.height = cur_height,
				};
			}
			cur_inset = inset;
			cur_start_y = y;
			cur_height = 1;
		}
	}
	if (cur_height > 0 && width > 2 * cur_inset) {
		slices[count++] = (struct wlr_box){
			.x = cur_inset,
			.y = cur_start_y,
			.width = width - 2 * cur_inset,
			.height = cur_height,
		};
	}

	return count;
}

static void
compute_backdrop_color(uint32_t backdrop, uint32_t flags, uint32_t tint, float color[4])
{
	/* wlroots 0.18 scene does not provide a shader/render blur pass.
	 * For blur, acrylic, and mica materials, we draw the material tint alone
	 * behind translucent pixels for now. This provides a clean extension point
	 * to integrate a post-process blur pass once shader/filter scene nodes
	 * are supported.
	 */
	float r, g, b, a;

	if (flags & WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_TINT) {
		/* Tint is straight 0xAARRGGBB */
		a = ((tint >> 24) & 0xFF) / 255.0f;
		r = ((tint >> 16) & 0xFF) / 255.0f;
		g = ((tint >> 8) & 0xFF) / 255.0f;
		b = (tint & 0xFF) / 255.0f;
	} else {
		bool dark = (flags & WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_DARK) != 0;
		switch (backdrop) {
		case WEX_WINDOW_MANAGER_V1_BACKDROP_MICA:
			if (dark) {
				r = g = b = 0x20 / 255.0f;
				a = 0.80f;
			} else {
				r = g = b = 0xF3 / 255.0f;
				a = 0.50f;
			}
			break;
		case WEX_WINDOW_MANAGER_V1_BACKDROP_ACRYLIC:
			if (dark) {
				r = g = b = 0x20 / 255.0f;
				a = 0.70f;
			} else {
				r = g = b = 0xF3 / 255.0f;
				a = 0.70f;
			}
			break;
		case WEX_WINDOW_MANAGER_V1_BACKDROP_BLUR:
			if (dark) {
				r = g = b = 0x20 / 255.0f;
				a = 0.25f;
			} else {
				r = g = b = 1.0f;
				a = 0.25f;
			}
			break;
		case WEX_WINDOW_MANAGER_V1_BACKDROP_GRADIENT:
		default:
			if (dark) {
				r = g = b = 0x20 / 255.0f;
				a = 1.0f;
			} else {
				r = g = b = 0xF3 / 255.0f;
				a = 1.0f;
			}
			break;
		}
	}

	/* Premultiplied float colour for wlr_scene_rect */
	color[0] = r * a;
	color[1] = g * a;
	color[2] = b * a;
	color[3] = a;
}

void
wex_window_update_effects(struct wex_window *win)
{
	if (!win || !win->view || !win->view->surface) {
		drop_corner_pieces(win);
		drop_effects(win);
		return;
	}

	struct wlr_surface *surface = win->view->surface;
	struct wlr_scene_buffer *root = find_surface_buffer(&win->view->content_tree->node, surface);

	int width = surface->current.width;
	int height = surface->current.height;
	bool has_buffer = (surface->buffer && width > 0 && height > 0);

	struct wlr_box geometry = {0};
	struct wlr_xdg_surface *xdg = xdg_surface_from_view(win->view);
	if (xdg) {
		wlr_xdg_surface_get_geometry(xdg, &geometry);
	}

	/* 1. Corner radius clipping */
	struct wlr_box slices[65];
	size_t count = 0;

	if (win->corner_radius == 0 || win->scene || !has_buffer) {
		drop_corner_pieces(win);
		if (root) {
			wlr_scene_node_set_enabled(&root->node, true);
		}
	} else if (root) {
		wlr_scene_node_set_enabled(&root->node, false);
		count = compute_corner_slices(width, height, win->corner_radius, slices);

		if (!win->corner_tree || win->num_corner_pieces != count) {
			drop_corner_pieces(win);
			win->corner_tree = wlr_scene_tree_create(win->view->scene_tree);
			if (win->corner_tree) {
				wlr_scene_node_place_below(&win->corner_tree->node, &win->view->content_tree->node);
				wlr_scene_node_set_position(&win->corner_tree->node, -geometry.x, -geometry.y);

				win->corner_pieces = calloc(count, sizeof(*win->corner_pieces));
				if (win->corner_pieces) {
					win->num_corner_pieces = count;
					for (size_t i = 0; i < count; i++) {
						struct wlr_scene_surface *scene_surface =
							wlr_scene_surface_create(win->corner_tree, surface);
						if (!scene_surface) {
							break;
						}
						win->corner_pieces[i].scene_surface = scene_surface;
						win->corner_pieces[i].buffer = scene_surface->buffer;
						win->corner_pieces[i].win = win;
						win->corner_pieces[i].destroy.notify = corner_piece_destroyed;
						wl_signal_add(&scene_surface->buffer->node.events.destroy,
							&win->corner_pieces[i].destroy);
						node_descriptor_create(&scene_surface->buffer->node,
							LAB_NODE_DESC_VIEW, win->view);
						scene_surface->buffer->point_accepts_input = corner_piece_accepts_input;
					}
					/* each scene surface resets its crop on commit: our listener must run after theirs */
					wl_list_remove(&win->surface_commit.link);
					win->surface_commit.notify = handle_window_surface_commit;
					wl_signal_add(&surface->events.commit, &win->surface_commit);
				}
			}
		}

		if (win->corner_pieces) {
			for (size_t i = 0; i < count; i++) {
				win->corner_pieces[i].box = slices[i];
				crop_corner_piece(win->corner_pieces[i].buffer, surface, slices[i]);
			}
		}
	}

	/* 2. Backdrop */
	if (win->backdrop_tree) {
		wlr_scene_node_destroy(&win->backdrop_tree->node);
		win->backdrop_tree = NULL;
	}

	if (win->effects_backdrop != WEX_WINDOW_MANAGER_V1_BACKDROP_NONE && has_buffer) {
		win->backdrop_tree = wlr_scene_tree_create(win->view->scene_tree);
		if (win->backdrop_tree) {
			wlr_scene_node_set_position(&win->backdrop_tree->node, -geometry.x, -geometry.y);
			wlr_scene_node_place_below(&win->backdrop_tree->node, &win->view->content_tree->node);

			float bcolor[4];
			compute_backdrop_color(win->effects_backdrop, win->effects_flags,
				win->effects_tint, bcolor);

			if (win->effects_flags & WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_REGION) {
				int bx = win->effects_left;
				int by = win->effects_top;
				int bw = win->effects_right - win->effects_left;
				int bh = win->effects_bottom - win->effects_top;
				if (bw > 0 && bh > 0) {
					struct wlr_scene_rect *rect = wlr_scene_rect_create(
						win->backdrop_tree, bw, bh, bcolor);
					if (rect) {
						wlr_scene_node_set_position(&rect->node, bx, by);
					}
				}
			} else if (win->corner_radius > 0 && count > 0) {
				for (size_t i = 0; i < count; i++) {
					struct wlr_scene_rect *rect = wlr_scene_rect_create(
						win->backdrop_tree, slices[i].width, slices[i].height, bcolor);
					if (rect) {
						wlr_scene_node_set_position(&rect->node, slices[i].x, slices[i].y);
					}
				}
			} else {
				struct wlr_scene_rect *rect = wlr_scene_rect_create(
					win->backdrop_tree, width, height, bcolor);
				if (rect) {
					wlr_scene_node_set_position(&rect->node, 0, 0);
				}
			}
		}
	}

	/* 3. Shadow */
	if (win->shadow_tree) {
		wlr_scene_node_destroy(&win->shadow_tree->node);
		win->shadow_tree = NULL;
	}

	if ((win->effects_flags & WEX_WINDOW_MANAGER_V1_EFFECT_FLAGS_SHADOW) && has_buffer) {
		win->shadow_tree = wlr_scene_tree_create(win->view->scene_tree);
		if (win->shadow_tree) {
			wlr_scene_node_set_position(&win->shadow_tree->node, -geometry.x, -geometry.y);

			static const int shadow_spreads[6] = { 2, 4, 6, 8, 10, 12 };
			float shadow_color[4] = { 0.f, 0.f, 0.f, 0.025f };

			for (int i = 0; i < 6; i++) {
				int s = shadow_spreads[i];
				int sw = width + 2 * s;
				int sh = height + 2 * s;
				struct wlr_scene_rect *rect = wlr_scene_rect_create(
					win->shadow_tree, sw, sh, shadow_color);
				if (rect) {
					wlr_scene_node_set_position(&rect->node, -s, -s + 2);
				}
			}
		}
	}

	/* Stacking order below content_tree:
	 * shadow_tree -> backdrop_tree -> corner_tree -> content_tree
	 */
	if (win->backdrop_tree) {
		wlr_scene_node_place_below(&win->backdrop_tree->node, &win->view->content_tree->node);
	}
	if (win->shadow_tree) {
		if (win->backdrop_tree) {
			wlr_scene_node_place_below(&win->shadow_tree->node, &win->backdrop_tree->node);
		} else if (win->corner_tree) {
			wlr_scene_node_place_below(&win->shadow_tree->node, &win->corner_tree->node);
		} else {
			wlr_scene_node_place_below(&win->shadow_tree->node, &win->view->content_tree->node);
		}
	}
	if (win->corner_tree) {
		wlr_scene_node_place_below(&win->corner_tree->node, &win->view->content_tree->node);
		if (win->backdrop_tree) {
			wlr_scene_node_place_above(&win->corner_tree->node, &win->backdrop_tree->node);
		}
	}

	if (win->view->output) {
		wlr_output_schedule_frame(win->view->output->wlr_output);
	}
}

void
wex_window_apply_corner_radius(struct wex_window *win)
{
	wex_window_update_effects(win);
}

static void
handle_window_surface_commit(struct wl_listener *listener, void *data)
{
	struct wex_window *win = wl_container_of(listener, win, surface_commit);
	wex_window_update_effects(win);
}

static void
handle_wex_window_set_corner_radius(struct wl_client *client,
		struct wl_resource *resource, uint32_t radius)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (!win) {
		return;
	}
	if (!wex_client_is_privileged(client, win->manager->server)) {
		wl_resource_post_error(resource, 0, "permission denied");
		return;
	}
	if (!win->view || !win->view->surface || wl_resource_get_client(win->view->surface->resource) != client) {
		wl_resource_post_error(resource, 0, "permission denied: not surface owner");
		return;
	}
	if (radius > 32) {
		radius = 32;
	}
	win->corner_radius = radius;
	if (win->view) {
		win->view->wex_corner_radius = radius;
	}
	wex_window_update_effects(win);
}

static void
handle_manager_set_effects(struct wl_client *client,
		struct wl_resource *resource,
		const char *handle,
		uint32_t corner_radius,
		uint32_t backdrop,
		uint32_t flags,
		uint32_t tint,
		int32_t left,
		int32_t top,
		int32_t right,
		int32_t bottom)
{
	struct wex_window_manager *manager = wl_resource_get_user_data(resource);
	if (!wex_client_is_privileged(client, manager->server)) {
		wl_resource_post_error(resource, 0, "permission denied");
		return;
	}
	struct wex_window *win = find_window_handle(manager, handle);
	if (!win) {
		return;
	}

	win->effects_backdrop = backdrop;
	win->effects_flags = flags;
	win->effects_tint = tint;
	win->effects_left = left;
	win->effects_top = top;
	win->effects_right = right;
	win->effects_bottom = bottom;

	if (corner_radius > 32) {
		corner_radius = 32;
	}
	win->corner_radius = corner_radius;
	if (win->view) {
		win->view->wex_corner_radius = corner_radius;
	}

	wex_window_update_effects(win);
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
	.set_foreign_focus = handle_wex_window_set_foreign_focus,
	.uncommit_foreign = handle_wex_window_uncommit_foreign,
	.configure_foreign = handle_wex_window_configure_foreign,
	.capture_foreign_pointer = handle_wex_window_capture_foreign_pointer,
	.resize_foreign = handle_wex_window_resize_foreign,
	.commit_foreign_resize = handle_wex_window_commit_foreign_resize,
	.cancel_foreign_resize = handle_wex_window_cancel_foreign_resize,
	.set_corner_radius = handle_wex_window_set_corner_radius,
};

static void
wex_window_resource_destroy(struct wl_resource *resource)
{
	struct wex_window *win = wl_resource_get_user_data(resource);
	if (win) {
		if (win->surface_commit.link.prev) {
			wl_list_remove(&win->surface_commit.link);
			wl_list_init(&win->surface_commit.link);
		}
		win->corner_radius = 0;
		win->effects_backdrop = 0;
		win->effects_flags = 0;
		drop_corner_pieces(win);
		drop_effects(win);
		drop_render_children(win);
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
	if (win->surface_commit.link.prev) {
		wl_list_remove(&win->surface_commit.link);
		wl_list_init(&win->surface_commit.link);
	}
	drop_corner_pieces(win);
	drop_effects(win);
	drop_render_children(win);
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
	wl_list_init(&manager->render_surfaces);

	manager->global = wl_global_create(server->wl_display,
		&wex_window_manager_v1_interface, 13, manager, handle_wex_manager_bind);
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
	struct wex_render_surface *render, *render_tmp;
	wl_list_for_each_safe(render, render_tmp, &manager->render_surfaces, link)
		wl_resource_destroy(render->resource);
	struct wex_foreign_window *foreign, *foreign_tmp;
	wl_list_for_each_safe(foreign, foreign_tmp, &manager->foreign_windows, link) {
		wl_resource_destroy(foreign->resource);
	}
	wl_list_for_each_safe(win, win_tmp, &manager->windows, link) {
		if (win->view) {
			win->view->wex_window = NULL;
		}
		drop_corner_pieces(win);
		drop_effects(win);
		wl_list_remove(&win->link);
		free(win);
	}

	wl_global_destroy(manager->global);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
	server->wex_window_manager = NULL;
}
