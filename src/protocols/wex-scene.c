/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/transform.h>
#include "common/mem.h"
#include "labwc.h"
#include "node.h"
#include "view.h"
#include "workspaces.h"
#include "protocols/wex-window.h"

struct buffer_watch {
	struct wlr_scene_buffer *buffer;
	struct wl_listener destroy;
	struct wex_scene *owner;
	struct wl_listener *extra;
};
struct wex_scene {
	struct wex_window *frame;
	struct view *native;
	struct wlr_box client;
	struct wlr_scene_tree *restore_parent;
	struct wlr_scene_tree *ring_tree;
	struct wl_listener parent_destroy;
	struct buffer_watch root, native_root, ring[4];
	struct wlr_box pieces[4];
	struct wl_listener commit, native_frame_done;
	struct wl_listener focus_change;
	struct wl_listener native_commit;
	enum ssd_mode restore_decor;
	bool state_sync;
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

static void
buffer_destroyed(struct wl_listener *listener, void *data)
{
	struct buffer_watch *watch = wl_container_of(listener, watch, destroy);
	wl_list_remove(&watch->destroy.link);
	watch->buffer = NULL;
	if (watch->extra && watch->extra->notify) {
		wl_list_remove(&watch->extra->link);
		watch->extra->notify = NULL;
	}
	if (watch->owner && watch->owner->frame->scene == watch->owner) {
		wex_window_drop_scene(watch->owner->frame);
	}
}
static void
watch_buffer(struct buffer_watch *watch, struct wlr_scene_buffer *buffer)
{
	watch->buffer = buffer;
	watch->destroy.notify = buffer_destroyed;
	wl_signal_add(&buffer->node.events.destroy, &watch->destroy);
}
static void
parent_destroyed(struct wl_listener *listener, void *data)
{
	struct wex_scene *scene = wl_container_of(listener, scene, parent_destroy);
	wl_list_remove(&scene->parent_destroy.link);
	scene->restore_parent = NULL;
}

static bool
ring_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	struct node_descriptor *desc = buffer->node.data;
	struct view *view = desc->data;
	struct wex_scene *scene = view->wex_window->scene;
	if (!scene) {
		return false;
	}
	for (unsigned i = 0; i < 4; ++i) {
		if (scene->ring[i].buffer == buffer) {
			*sx += scene->pieces[i].x;
			*sy += scene->pieces[i].y;
			return wlr_surface_point_accepts_input(view->surface, *sx, *sy);
		}
	}
	return false;
}

static void
crop_piece(struct wex_scene *scene, unsigned i, struct wlr_box box)
{
	struct wlr_scene_buffer *buffer = scene->ring[i].buffer;
	struct wlr_surface *surface = scene->frame->view->surface;
	scene->pieces[i] = box;
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
	/* The auto scene-surface commit already updates transform, alpha and buffer.
	 * Clip the surface-coordinate opaque region to this particular piece. */
	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	pixman_region32_copy(&opaque, &surface->opaque_region);
	pixman_region32_translate(&opaque, -box.x, -box.y);
	pixman_region32_intersect_rect(&opaque, &opaque, 0, 0, box.width, box.height);
	wlr_scene_buffer_set_opaque_region(buffer, &opaque);
	pixman_region32_fini(&opaque);
}

static void
apply_ring(struct wex_scene *scene)
{
	struct wlr_surface *surface = scene->frame->view->surface;
	if (!scene->root.buffer || !surface || !surface->current.width || !surface->current.height) {
		return;
	}
	struct wlr_box client = scene->client;
	int width = surface->current.width, height = surface->current.height;
	wlr_scene_node_set_enabled(&scene->root.buffer->node, false);
	crop_piece(scene, 0, (struct wlr_box){0, 0, width, client.y});
	crop_piece(scene, 1, (struct wlr_box){0, client.y, client.x, client.height});
	crop_piece(scene, 2, (struct wlr_box){client.x + client.width, client.y,
		width - client.x - client.width, client.height});
	crop_piece(scene, 3, (struct wlr_box){0, client.y + client.height,
		width, height - client.y - client.height});
	struct wlr_box geometry = {0};
	wlr_xdg_surface_get_geometry(xdg_surface_from_view(scene->frame->view), &geometry);
	wlr_scene_node_set_position(&scene->ring_tree->node, -geometry.x, -geometry.y);
	wlr_scene_node_place_below(&scene->ring_tree->node, &scene->native->scene_tree->node);
	wlr_scene_node_place_below(&scene->native->scene_tree->node, &scene->frame->view->content_tree->node);
}

static void
frame_committed(struct wl_listener *listener, void *data)
{
	struct wex_scene *scene = wl_container_of(listener, scene, commit);
	struct wlr_surface *surface = scene->frame->view->surface;
	if (!surface->buffer || scene->client.width > surface->current.width - scene->client.x
			|| scene->client.height > surface->current.height - scene->client.y) {
		wex_window_drop_scene(scene->frame);
		return;
	}
	apply_ring(scene);
	if (scene->frame->view->output) {
		/* Borderless proxies still need frame callbacks when their root buffer
		 * is entirely cut out. Native presentation supplies those callbacks. */
		wlr_output_schedule_frame(scene->frame->view->output->wlr_output);
	}
}
static void
native_frame_done(struct wl_listener *listener, void *data)
{
	struct wex_scene *scene = wl_container_of(listener, scene, native_frame_done);
	if (scene->frame->view->surface) {
		wlr_surface_send_frame_done(scene->frame->view->surface, data);
	}
}

static struct wlr_surface *
focus_toplevel(struct wlr_surface *surface)
{
	while (surface) {
		surface = wlr_surface_get_root_surface(surface);
		struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
		if (!xdg || xdg->role != WLR_XDG_SURFACE_ROLE_POPUP) return surface;
		surface = xdg->popup->parent;
	}
	return NULL;
}

uint32_t
wex_scene_focus_target(struct wex_scene *scene)
{
	struct wlr_surface *surface = focus_toplevel(scene->native->server->seat.seat->keyboard_state.focused_surface);
	if (!surface) return WEX_WINDOW_V1_FOCUS_TARGET_NONE;
	if (surface == scene->native->surface) return WEX_WINDOW_V1_FOCUS_TARGET_NATIVE;
	if (surface == scene->frame->view->surface) return WEX_WINDOW_V1_FOCUS_TARGET_FRAME;
	return WEX_WINDOW_V1_FOCUS_TARGET_NONE;
}

static void
focus_changed(struct wl_listener *listener, void *data)
{
	struct wex_scene *scene = wl_container_of(listener, scene, focus_change);
	wex_window_send_foreign_focus(scene->frame);
}

static void
native_committed(struct wl_listener *listener, void *data)
{
	struct wex_scene *scene = wl_container_of(listener, scene, native_commit);
	struct view *native = scene->native;
	if (!native->surface->buffer || native->current.width != scene->client.width ||
			native->current.height != scene->client.height ||
			view_get_ssd_mode(native) != LAB_SSD_MODE_NONE ||
			(native->ssd_preference == LAB_SSD_PREF_CLIENT &&
			 scene->restore_decor != LAB_SSD_MODE_NONE)) {
		wex_window_drop_scene(scene->frame);
	}
}

struct wex_scene *
wex_scene_create(struct wex_window *frame, struct view *native, const struct wlr_box *client)
{
	struct view *view = frame->view;
	if (!view || !view->mapped || !native->mapped || !view->surface || !native->surface
			|| !view->surface->buffer || !native->surface->buffer
			|| client->x < 0 || client->y < 0 || client->width <= 0 || client->height <= 0
			|| client->width > view->surface->current.width - client->x
			|| client->height > view->surface->current.height - client->y
			|| native->current.width != client->width || native->current.height != client->height) {
		return NULL;
	}
	if ((native->ssd_preference == LAB_SSD_PREF_CLIENT || view_get_ssd_mode(native) == LAB_SSD_MODE_NONE)
			&& (client->x || client->y || client->width != view->surface->current.width
				|| client->height != view->surface->current.height)) {
		return NULL;
	}
	struct wlr_scene_buffer *root = find_surface_buffer(&view->content_tree->node, view->surface);
	struct wlr_scene_buffer *native_root = find_surface_buffer(&native->content_tree->node, native->surface);
	if (!root || !native_root) {
		return NULL;
	}
	struct wex_scene *scene = znew(*scene);
	scene->frame = frame;
	scene->native = native;
	scene->client = *client;
	scene->restore_decor = view_get_ssd_mode(native);
	watch_buffer(&scene->root, root);
	watch_buffer(&scene->native_root, native_root);
	scene->root.owner = scene;
	scene->native_root.owner = scene;
	scene->ring_tree = wlr_scene_tree_create(view->scene_tree);
	if (!scene->ring_tree) {
		wl_list_remove(&scene->root.destroy.link);
		wl_list_remove(&scene->native_root.destroy.link);
		free(scene);
		return NULL;
	}
	for (unsigned i = 0; i < 4; ++i) {
		struct wlr_scene_surface *piece = wlr_scene_surface_create(scene->ring_tree, view->surface);
		if (!piece) {
			wlr_scene_node_destroy(&scene->ring_tree->node);
			wl_list_remove(&scene->root.destroy.link);
			wl_list_remove(&scene->native_root.destroy.link);
			free(scene);
			return NULL;
		}
		watch_buffer(&scene->ring[i], piece->buffer);
		node_descriptor_create(&piece->buffer->node, LAB_NODE_DESC_VIEW, view);
		piece->buffer->point_accepts_input = ring_accepts_input;
	}
	scene->restore_parent = native->scene_tree->node.parent;
	scene->parent_destroy.notify = parent_destroyed;
	wl_signal_add(&scene->restore_parent->node.events.destroy, &scene->parent_destroy);
	scene->commit.notify = frame_committed;
	wl_signal_add(&view->surface->events.commit, &scene->commit);
	scene->native_frame_done.notify = native_frame_done;
	wl_signal_add(&native_root->events.frame_done, &scene->native_frame_done);
	scene->native_root.extra = &scene->native_frame_done;
	scene->focus_change.notify = focus_changed;
	wl_signal_add(&native->server->seat.seat->keyboard_state.events.focus_change, &scene->focus_change);
	scene->native_commit.notify = native_committed;
	wl_signal_add(&native->surface->events.commit, &scene->native_commit);
	view_set_ssd_mode(native, LAB_SSD_MODE_NONE);
	wlr_scene_node_reparent(&native->scene_tree->node, view->scene_tree);
	struct wlr_box geometry = {0};
	wlr_xdg_surface_get_geometry(xdg_surface_from_view(view), &geometry);
	wlr_scene_node_set_position(&native->scene_tree->node, client->x - geometry.x, client->y - geometry.y);
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	apply_ring(scene);
	return scene;
}

void
wex_scene_destroy(struct wex_scene *scene)
{
	wl_list_remove(&scene->focus_change.link);
	wl_list_remove(&scene->native_commit.link);
	struct view *native = scene->native;
	struct wlr_scene_tree *parent = scene->restore_parent;
	if (!parent) parent = native->server->workspaces.current->tree;
	wlr_scene_node_reparent(&native->scene_tree->node, parent);
	wlr_scene_node_set_position(&native->scene_tree->node, native->current.x, native->current.y);
	if (scene->restore_parent) wl_list_remove(&scene->parent_destroy.link);
	wl_list_remove(&scene->commit.link);
	if (scene->native_frame_done.notify) wl_list_remove(&scene->native_frame_done.link);
	if (scene->root.buffer) {
		wl_list_remove(&scene->root.destroy.link);
		wlr_scene_node_set_enabled(&scene->root.buffer->node, true);
	}
	if (scene->native_root.buffer) wl_list_remove(&scene->native_root.destroy.link);
	wlr_scene_node_destroy(&scene->ring_tree->node);
	view_set_ssd_mode(native, native->ssd_preference == LAB_SSD_PREF_CLIENT ? LAB_SSD_MODE_NONE : scene->restore_decor);
	/* The proxy is obsolete until a new prepared group is committed. */
	if (scene->frame->view) wlr_scene_node_set_enabled(&scene->frame->view->scene_tree->node, false);
	free(scene);
}

struct view *
wex_scene_native(struct wex_scene *scene)
{
	return scene->native;
}
bool
wex_scene_matches(struct wex_scene *scene, const struct wlr_box *client)
{
	return wlr_box_equal(&scene->client, client);
}
bool
wex_scene_server_decorated(struct wex_scene *scene)
{
	return scene->restore_decor != LAB_SSD_MODE_NONE;
}
struct view *
wex_scene_group_root(struct view *view)
{
	if (view->wex_foreign_frame && view->wex_foreign_frame->scene)
		return view->wex_foreign_frame->view;
	return view;
}

void
wex_scene_view_moved(struct view *view)
{
	struct wex_scene *scene = view->wex_window ? view->wex_window->scene :
		view->wex_foreign_frame ? view->wex_foreign_frame->scene : NULL;
	if (!scene) return;
	/* Parent scene movement is immediate; configure supplies matching native
	 * logical geometry and keeps foreign-toplevel observations consistent. */
	struct view *frame = scene->frame->view;
	struct wlr_box geometry = {0};
	wlr_xdg_surface_get_geometry(xdg_surface_from_view(frame), &geometry);
	int x = frame->current.x + scene->client.x - geometry.x;
	int y = frame->current.y + scene->client.y - geometry.y;
	wlr_scene_node_set_position(&scene->native->scene_tree->node, scene->client.x - geometry.x, scene->client.y - geometry.y);
	if (view == frame && (scene->native->pending.x != x || scene->native->pending.y != y)) {
		view_move(scene->native, x, y);
	}
}

void
wex_scene_view_minimized(struct view *view, bool minimized)
{
	struct wex_scene *scene = view->wex_window ? view->wex_window->scene :
		view->wex_foreign_frame ? view->wex_foreign_frame->scene : NULL;
	if (!scene || scene->state_sync) return;
	scene->state_sync = true;
	view_minimize(view == scene->native ? scene->frame->view : scene->native, minimized);
	scene->state_sync = false;
}
