/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/transform.h>
#include <wlr/util/log.h>
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
/* Plain scene buffers retain immutable references. Do not replace buffers on
 * wlr_scene_surface nodes: those own wlroots' mutable-client-buffer bookkeeping.
 * The live surface trees keep their listeners, popups and frame callbacks. */
struct resize_snapshot {
	struct wlr_scene_buffer *node;
	struct wlr_buffer *buffer;
};
struct resize_transaction {
	struct wex_scene *scene;
	uint32_t serial, configure_serial, edges;
	bool acknowledged, ready, rollback;
	struct wlr_box original, requested, actual;
	int frame_width, frame_height;
	int frame_x, frame_y, frame_content_x, frame_content_y;
	int native_content_x, native_content_y;
	int native_x, native_y;
	struct resize_snapshot ring[4], native;
	struct wl_listener ack;
	struct wl_listener frame_done;
	struct wl_event_source *timeout;
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
	struct resize_transaction *resize;
};

static void resize_rollback(struct wex_scene *scene);
static void resize_native_committed(struct wex_scene *scene);
static void resize_hold(struct wex_scene *scene);
static void resize_finish(struct wex_scene *scene, uint32_t stage);

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
	if (scene->resize && surface->buffer) {
		resize_hold(scene);
		if (scene->frame->view->output)
			wlr_output_schedule_frame(scene->frame->view->output->wlr_output);
		return;
	}
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
	if (scene->resize && wex_scene_focus_target(scene) != WEX_WINDOW_V1_FOCUS_TARGET_FRAME)
		resize_rollback(scene);
	wex_window_send_foreign_focus(scene->frame);
}

static void
native_committed(struct wl_listener *listener, void *data)
{
	struct wex_scene *scene = wl_container_of(listener, scene, native_commit);
	struct view *native = scene->native;
	if (scene->resize && native->surface->buffer &&
			view_get_ssd_mode(native) == LAB_SSD_MODE_NONE &&
			native->ssd_preference != LAB_SSD_PREF_CLIENT) {
		resize_native_committed(scene);
		resize_hold(scene);
		if (native->output) wlr_output_schedule_frame(native->output->wlr_output);
		return;
	}
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
		wlr_log(WLR_DEBUG, "Hosted group not ready: frame mapped=%d buffer=%d size=%dx%d native mapped=%d buffer=%d size=%dx%d client=%d,%d %dx%d",
			view && view->mapped, view && view->surface && view->surface->buffer,
			view && view->surface ? view->surface->current.width : 0,
			view && view->surface ? view->surface->current.height : 0,
			native->mapped, native->surface && native->surface->buffer,
			native->current.width, native->current.height,
			client->x, client->y, client->width, client->height);
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
	/* Decoration observers must see the prepared replacement before SSD is
	 * hidden. Otherwise an intermediate NONE snapshot looks like a CSD change
	 * to a shell, even though the following snapshot reports SERVER again.
	 * All allocations/listeners are ready and no fallible operation remains. */
	frame->scene = scene;
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
	if (scene->resize) resize_finish(scene, WEX_WINDOW_V1_RESIZE_STAGE_FAILED);
	wex_window_release_pointer_capture(scene->frame);
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
	if (scene->resize) {
		resize_hold(scene);
		return;
	}
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
	if (minimized && scene->resize) resize_rollback(scene);
	scene->state_sync = true;
	view_minimize(view == scene->native ? scene->frame->view : scene->native, minimized);
	scene->state_sync = false;
}

static void
resize_event(struct wex_window *frame, uint32_t serial, uint32_t stage,
		uint32_t configure_serial, const struct wlr_box *rect)
{
	struct wlr_box empty = {0};
	if (!rect) rect = &empty;
	wex_window_v1_send_foreign_resize(frame->resource, serial, stage,
		configure_serial, rect->x, rect->y, rect->width, rect->height);
}

static bool
snapshot_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	/* The transaction requires frame capture. A snapshot must never deliver
	 * input to a different/current native surface behind its old pixels. */
	return false;
}

static bool
snapshot_create(struct resize_snapshot *snapshot, struct wlr_scene_buffer *live,
		struct wlr_surface *surface)
{
	if (!surface->buffer) return false;
	snapshot->buffer = wlr_buffer_lock(&surface->buffer->base);
	snapshot->node = wlr_scene_buffer_create(live->node.parent, snapshot->buffer);
	if (!snapshot->node) {
		wlr_buffer_unlock(snapshot->buffer);
		snapshot->buffer = NULL;
		return false;
	}
	wlr_scene_buffer_set_source_box(snapshot->node, &live->src_box);
	wlr_scene_buffer_set_dest_size(snapshot->node, live->dst_width, live->dst_height);
	wlr_scene_buffer_set_transform(snapshot->node, live->transform);
	wlr_scene_buffer_set_opacity(snapshot->node, live->opacity);
	wlr_scene_buffer_set_filter_mode(snapshot->node, live->filter_mode);
	wlr_scene_buffer_set_opaque_region(snapshot->node, &live->opaque_region);
	wlr_scene_node_set_position(&snapshot->node->node, live->node.x, live->node.y);
	wlr_scene_node_set_enabled(&snapshot->node->node, live->node.enabled);
	wlr_scene_node_place_above(&snapshot->node->node, &live->node);
	snapshot->node->point_accepts_input = snapshot_accepts_input;
	return true;
}

static void
snapshot_destroy(struct resize_snapshot *snapshot)
{
	if (snapshot->node) wlr_scene_node_destroy(&snapshot->node->node);
	if (snapshot->buffer) wlr_buffer_unlock(snapshot->buffer);
}

static void
resize_hold(struct wex_scene *scene)
{
	struct resize_transaction *resize = scene->resize;
	if (!resize) return;
	for (unsigned i = 0; i < 4; ++i)
		wlr_scene_node_set_enabled(&scene->ring[i].buffer->node, false);
	wlr_scene_node_set_enabled(&scene->root.buffer->node, false);
	wlr_scene_node_set_enabled(&scene->native_root.buffer->node, false);
	wlr_scene_node_set_position(&scene->native->scene_tree->node, resize->native_x, resize->native_y);
	wlr_scene_node_set_position(&scene->frame->view->scene_tree->node,
		resize->frame_x, resize->frame_y);
	wlr_scene_node_set_position(&scene->frame->view->content_tree->node,
		resize->frame_content_x, resize->frame_content_y);
	wlr_scene_node_set_position(&scene->native->content_tree->node,
		resize->native_content_x, resize->native_content_y);
}

static void
resize_frame_done(struct wl_listener *listener, void *data)
{
	struct resize_transaction *resize = wl_container_of(listener, resize, frame_done);
	wlr_surface_send_frame_done(resize->scene->native->surface, data);
	wlr_surface_send_frame_done(resize->scene->frame->view->surface, data);
}

static void
resize_finish(struct wex_scene *scene, uint32_t stage)
{
	struct resize_transaction *resize = scene->resize;
	scene->resize = NULL;
	if (resize->timeout) wl_event_source_remove(resize->timeout);
	wl_list_remove(&resize->ack.link);
	wl_list_remove(&resize->frame_done.link);
	for (unsigned i = 0; i < 4; ++i) snapshot_destroy(&resize->ring[i]);
	snapshot_destroy(&resize->native);
	if (scene->native_root.buffer)
		wlr_scene_node_set_enabled(&scene->native_root.buffer->node, true);
	struct view *views[] = {scene->frame->view, scene->native};
	for (unsigned i = 0; i < 2; ++i) {
		if (!views[i] || !views[i]->surface) continue;
		struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(views[i]->surface);
		if (!xdg) continue; /* The role may already be gone during native teardown. */
		struct wlr_box geometry;
		wlr_xdg_surface_get_geometry(xdg, &geometry);
		wlr_scene_node_set_position(&views[i]->content_tree->node, -geometry.x, -geometry.y);
	}
	resize_event(scene->frame, resize->serial, stage, resize->configure_serial,
		&resize->actual);
	free(resize);
}

static int
resize_timeout(void *data)
{
	struct wex_scene *scene = data;
	if (!scene->resize->rollback) resize_rollback(scene);
	else wex_window_drop_scene(scene->frame);
	return 0;
}

static void
resize_configure(struct wex_scene *scene, uint32_t stage)
{
	struct resize_transaction *resize = scene->resize;
	resize->acknowledged = resize->ready = false;
	/* Keep the xdg serial independent of the controller serial. Calling set_size
	 * explicitly also sends a fresh configure for same-sized rollback requests. */
	resize->configure_serial = wlr_xdg_toplevel_set_size(
		xdg_surface_from_view(scene->native)->toplevel, resize->requested.width, resize->requested.height);
	view_move_resize(scene->native, resize->requested);
	resize_event(scene->frame, resize->serial, stage, resize->configure_serial,
		&resize->requested);
	wl_event_source_timer_update(resize->timeout, 2000);
	resize_hold(scene);
}

static void
resize_rollback(struct wex_scene *scene)
{
	struct resize_transaction *resize = scene->resize;
	if (!resize || resize->rollback) return;
	resize->rollback = true;
	resize->requested = resize->original;
	resize->edges = 0;
	resize_configure(scene, WEX_WINDOW_V1_RESIZE_STAGE_ROLLING_BACK);
}

void
wex_scene_capture_lost(struct wex_scene *scene)
{
	resize_rollback(scene);
}

static void
resize_ack(struct wl_listener *listener, void *data)
{
	struct resize_transaction *resize = wl_container_of(listener, resize, ack);
	struct wlr_xdg_surface_configure *configure = data;
	/* A newer configure may carry the same size plus activation state. An
	 * acknowledgement of a superseded configure cannot satisfy the new request. */
	if (!resize->acknowledged && (int32_t)(configure->serial - resize->configure_serial) >= 0) {
		resize->acknowledged = true;
		resize_event(resize->scene->frame, resize->serial, WEX_WINDOW_V1_RESIZE_STAGE_ACKNOWLEDGED,
			resize->configure_serial, &resize->requested);
	}
}

static void
resize_native_committed(struct wex_scene *scene)
{
	struct resize_transaction *resize = scene->resize;
	struct wlr_xdg_surface *xdg = xdg_surface_from_view(scene->native);
	uint32_t content = WLR_SURFACE_STATE_BUFFER | WLR_SURFACE_STATE_SURFACE_DAMAGE |
		WLR_SURFACE_STATE_BUFFER_DAMAGE | WLR_SURFACE_STATE_VIEWPORT |
		WLR_SURFACE_STATE_SCALE | WLR_SURFACE_STATE_TRANSFORM;
	if (!resize->acknowledged ||
			(int32_t)(xdg->current.configure_serial - resize->configure_serial) < 0) return;
	/* An ack followed by an otherwise empty commit changes xdg state, not
	 * content. Keep waiting for a buffer/damage/viewport response. Retained
	 * buffers may legitimately be reused with damage or new viewport state. */
	if (!(scene->native->surface->current.committed & content)) return;
	struct wlr_box previous = resize->actual;
	resize->actual = scene->native->current;
	if (resize->edges & WLR_EDGE_LEFT)
		resize->actual.x = resize->requested.x + resize->requested.width - resize->actual.width;
	if (resize->edges & WLR_EDGE_TOP)
		resize->actual.y = resize->requested.y + resize->requested.height - resize->actual.height;
	if (resize->ready && wlr_box_equal(&previous, &resize->actual)) return;
	resize->ready = true;
	resize_event(scene->frame, resize->serial, WEX_WINDOW_V1_RESIZE_STAGE_NATIVE_READY,
		resize->configure_serial, &resize->actual);
	if (scene->native->output) wlr_output_schedule_frame(scene->native->output->wlr_output);
}

static int
constrain_axis(int value, int min, int max, int base, int increment)
{
	min = min > 0 ? min : (base > 0 ? base : 1);
	max = max > 0 && max < 32768 ? max : 32768;
	if (max < min) max = min;
	if (value < min) value = min;
	if (value > max) value = max;
	if (increment > 1) {
		if (base <= 0) base = min;
		/* Hints are untrusted 32-bit integers. Use a non-negative grid index
		 * and wide arithmetic even when an application reports a huge base. */
		int64_t first_index = min > base ? ((int64_t)min - base + increment - 1) / increment : 0;
		int64_t last_index = max >= base ? ((int64_t)max - base) / increment : -1;
		int64_t first = base + first_index * increment;
		int64_t last = base + last_index * increment;
		if (first <= last) {
			int64_t index = value > base ? ((int64_t)value - base + increment / 2) / increment : 0;
			int64_t snapped = base + index * increment;
			if (snapped < first) snapped = first;
			if (snapped > last) snapped = last;
			value = snapped;
		}
	}
	return value;
}

void
wex_scene_resize(struct wex_window *frame, uint32_t serial, uint32_t edges,
		const struct wlr_box *desired)
{
	struct wex_scene *scene = frame->scene;
	if (!serial || (frame->resize_serial && (int32_t)(serial - frame->resize_serial) <= 0)) {
		resize_event(frame, serial, WEX_WINDOW_V1_RESIZE_STAGE_STALE, 0, NULL);
		return;
	}
	struct view_size_hints hints = scene ? view_get_size_hints(scene->native) : (struct view_size_hints){0};
	if (!scene || scene->native->type != LAB_XDG_SHELL_VIEW ||
			hints.min_width > 32768 || hints.min_height > 32768 ||
			frame->manager->pointer_capture != frame || frame->capture_epoch != frame->focus_epoch ||
			wex_scene_focus_target(scene) != WEX_WINDOW_V1_FOCUS_TARGET_FRAME ||
			!edges || (edges & ~15u) || (edges & 3) == 3 || (edges & 12) == 12 ||
			desired->width <= 0 || desired->height <= 0 ||
			desired->width > 32768 || desired->height > 32768 ||
			desired->x < -1000000 || desired->x > 1000000 ||
			desired->y < -1000000 || desired->y > 1000000 ||
			(scene->resize && scene->resize->rollback)) {
		resize_event(frame, serial, WEX_WINDOW_V1_RESIZE_STAGE_UNAVAILABLE, 0, NULL);
		return;
	}
	struct resize_transaction *resize = scene->resize;
	if (!resize) {
		resize = znew(*resize);
		resize->scene = scene;
		resize->original = scene->native->current;
		struct view *view = frame->view;
		resize->frame_width = view->surface->current.width;
		resize->frame_height = view->surface->current.height;
		resize->frame_x = view->scene_tree->node.x;
		resize->frame_y = view->scene_tree->node.y;
		resize->frame_content_x = view->content_tree->node.x;
		resize->frame_content_y = view->content_tree->node.y;
		resize->native_content_x = scene->native->content_tree->node.x;
		resize->native_content_y = scene->native->content_tree->node.y;
		resize->native_x = scene->native->scene_tree->node.x;
		resize->native_y = scene->native->scene_tree->node.y;
		bool snapshots = snapshot_create(&resize->native, scene->native_root.buffer, scene->native->surface);
		for (unsigned i = 0; i < 4 && snapshots; ++i)
			snapshots = snapshot_create(&resize->ring[i], scene->ring[i].buffer, view->surface);
		resize->timeout = wl_event_loop_add_timer(wl_display_get_event_loop(
			frame->manager->server->wl_display), resize_timeout, scene);
		if (!snapshots || !resize->timeout) {
			for (unsigned i = 0; i < 4; ++i) snapshot_destroy(&resize->ring[i]);
			snapshot_destroy(&resize->native);
			if (resize->timeout) wl_event_source_remove(resize->timeout);
			free(resize);
			resize_event(frame, serial, WEX_WINDOW_V1_RESIZE_STAGE_UNAVAILABLE, 0, NULL);
			return;
		}
		resize->ack.notify = resize_ack;
		wl_signal_add(&xdg_surface_from_view(scene->native)->events.ack_configure, &resize->ack);
		resize->frame_done.notify = resize_frame_done;
		wl_signal_add(&resize->native.node->events.frame_done, &resize->frame_done);
		scene->resize = resize;
	} else {
		resize_event(frame, resize->serial, WEX_WINDOW_V1_RESIZE_STAGE_SUPERSEDED,
			resize->configure_serial, &resize->requested);
	}
	frame->resize_serial = resize->serial = serial;
	resize->edges = edges;
	resize->requested = *desired;
	resize->requested.width = constrain_axis(desired->width, hints.min_width, hints.max_width,
		hints.base_width, hints.width_inc);
	resize->requested.height = constrain_axis(desired->height, hints.min_height, hints.max_height,
		hints.base_height, hints.height_inc);
	if (edges & WLR_EDGE_LEFT) resize->requested.x += desired->width - resize->requested.width;
	if (edges & WLR_EDGE_TOP) resize->requested.y += desired->height - resize->requested.height;
	resize_configure(scene, WEX_WINDOW_V1_RESIZE_STAGE_ACCEPTED);
}

void
wex_scene_cancel_resize(struct wex_window *frame, uint32_t serial)
{
	if (!frame->scene || !frame->scene->resize || frame->scene->resize->serial != serial)
		resize_event(frame, serial, WEX_WINDOW_V1_RESIZE_STAGE_STALE, 0, NULL);
	else resize_rollback(frame->scene);
}

void
wex_scene_commit_resize(struct wex_window *frame, uint32_t serial, const struct wlr_box *client)
{
	struct wex_scene *scene = frame->scene;
	struct resize_transaction *resize = scene ? scene->resize : NULL;
	if (!resize || resize->serial != serial) {
		resize_event(frame, serial, WEX_WINDOW_V1_RESIZE_STAGE_STALE, 0, NULL);
		return;
	}
	struct wlr_surface *surface = frame->view->surface;
	if (!resize->ready || !surface->buffer || client->x != scene->client.x || client->y != scene->client.y ||
			client->width != scene->native->current.width || client->height != scene->native->current.height ||
			client->width != resize->actual.width || client->height != resize->actual.height ||
			surface->current.width != resize->frame_width - scene->client.width + client->width ||
			surface->current.height != resize->frame_height - scene->client.height + client->height ||
			(resize->rollback && (client->width != resize->original.width || client->height != resize->original.height))) {
		resize_event(frame, serial, WEX_WINDOW_V1_RESIZE_STAGE_UNAVAILABLE,
			resize->configure_serial, NULL);
		return;
	}
	struct wlr_box actual = resize->actual, geometry;
	uint32_t stage = resize->rollback ? WEX_WINDOW_V1_RESIZE_STAGE_CANCELLED : WEX_WINDOW_V1_RESIZE_STAGE_COMPLETE;
	scene->client = *client;
	/* Nothing is presented between these node updates: both surface commits
	 * were processed, then the old retained roots are released in this dispatch. */
	resize_finish(scene, stage);
	wlr_xdg_surface_get_geometry(xdg_surface_from_view(frame->view), &geometry);
	wlr_scene_node_set_position(&frame->view->content_tree->node, -geometry.x, -geometry.y);
	wlr_xdg_surface_get_geometry(xdg_surface_from_view(scene->native), &geometry);
	wlr_scene_node_set_position(&scene->native->content_tree->node, -geometry.x, -geometry.y);
	view_move(scene->native, actual.x, actual.y);
	wlr_xdg_surface_get_geometry(xdg_surface_from_view(frame->view), &geometry);
	view_move(frame->view, actual.x - client->x + geometry.x, actual.y - client->y + geometry.y);
	apply_ring(scene);
	wex_scene_view_moved(frame->view);
}
