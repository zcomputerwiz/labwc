/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include "protocols/xx-zones.h"
#include "xx-zones-v1-protocol.h"
#include "labwc.h"
#include "view.h"
#include "ssd.h"
#include "common/mem.h"

static const struct xx_zone_manager_v1_interface zone_manager_impl;
static const struct xx_zone_v1_interface zone_impl;
static const struct xx_zone_item_v1_interface zone_item_impl;

static void zone_resource_destroy(struct wl_resource *resource);
static void zone_item_resource_destroy(struct wl_resource *resource);

static char *
generate_zone_token(void)
{
	char token[37];
	uint8_t rand_bytes[16];
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		if (fread(rand_bytes, 1, 16, f) != 16) {
			for (int i = 0; i < 16; i++) {
				rand_bytes[i] = (uint8_t)rand();
			}
		}
		fclose(f);
	} else {
		for (int i = 0; i < 16; i++) {
			rand_bytes[i] = (uint8_t)rand();
		}
	}
	snprintf(token, sizeof(token),
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		rand_bytes[0], rand_bytes[1], rand_bytes[2], rand_bytes[3],
		rand_bytes[4], rand_bytes[5], rand_bytes[6], rand_bytes[7],
		rand_bytes[8], rand_bytes[9], rand_bytes[10], rand_bytes[11],
		rand_bytes[12], rand_bytes[13], rand_bytes[14], rand_bytes[15]);
	return xstrdup(token);
}

struct lab_zone *
lab_zone_get_or_create(struct lab_zone_manager *manager, struct output *output)
{
	assert(manager);
	assert(output);

	struct lab_zone *zone;
	wl_list_for_each(zone, &manager->zones, link) {
		if (zone->output == output) {
			return zone;
		}
	}

	zone = znew(*zone);
	zone->manager = manager;
	zone->output = output;
	zone->handle = generate_zone_token();
	wl_list_init(&zone->resources);
	wl_list_insert(manager->zones.prev, &zone->link);

	return zone;
}

static void
handle_zone_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_zone_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
handle_zone_item_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
send_zone_initial_events(struct lab_zone *zone, struct wl_resource *resource)
{
	struct output *output = zone->output;
	struct wlr_box box = { 0 };
	if (output && output->wlr_output) {
		wlr_output_effective_resolution(output->wlr_output, &box.width, &box.height);
	}
	xx_zone_v1_send_size(resource, box.width, box.height);
	xx_zone_v1_send_handle(resource, zone->handle ? zone->handle : "");
	xx_zone_v1_send_done(resource);
}

static void
handle_zone_manager_get_zone(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *output_resource)
{
	struct lab_zone_manager *manager = wl_resource_get_user_data(resource);
	struct output *output = NULL;

	if (output_resource) {
		struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
		if (wlr_output) {
			output = output_from_wlr_output(manager->server, wlr_output);
		}
	}
	if (!output) {
		output = output_nearest_to_cursor(manager->server);
	}
	if (!output && !wl_list_empty(&manager->server->outputs)) {
		output = wl_container_of(manager->server->outputs.next, output, link);
	}

	uint32_t version = wl_resource_get_version(resource);

	if (!output) {
		struct wl_resource *res = wl_resource_create(client,
			&xx_zone_v1_interface, version, id);
		xx_zone_v1_send_size(res, -1, -1);
		xx_zone_v1_send_handle(res, "");
		xx_zone_v1_send_done(res);
		return;
	}

	struct lab_zone *zone = lab_zone_get_or_create(manager, output);
	struct lab_zone_resource *zr = znew(*zr);
	zr->zone = zone;
	zr->resource = wl_resource_create(client,
		&xx_zone_v1_interface, version, id);
	wl_resource_set_implementation(zr->resource, &zone_impl, zr, zone_resource_destroy);
	wl_list_insert(zone->resources.prev, &zr->link);

	send_zone_initial_events(zone, zr->resource);
}

static void
handle_zone_manager_get_zone_from_handle(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		const char *handle)
{
	struct lab_zone_manager *manager = wl_resource_get_user_data(resource);
	struct lab_zone *zone = NULL;

	if (handle && *handle) {
		struct lab_zone *z;
		wl_list_for_each(z, &manager->zones, link) {
			if (z->handle && !strcmp(z->handle, handle)) {
				zone = z;
				break;
			}
		}
	}

	uint32_t version = wl_resource_get_version(resource);

	if (!zone) {
		struct wl_resource *res = wl_resource_create(client,
			&xx_zone_v1_interface, version, id);
		wl_resource_set_implementation(res, &zone_impl, NULL, zone_resource_destroy);
		xx_zone_v1_send_size(res, -1, -1);
		xx_zone_v1_send_handle(res, "");
		xx_zone_v1_send_done(res);
		return;
	}

	struct lab_zone_resource *zr = znew(*zr);
	zr->zone = zone;
	zr->resource = wl_resource_create(client,
		&xx_zone_v1_interface, version, id);
	wl_resource_set_implementation(zr->resource, &zone_impl, zr, zone_resource_destroy);
	wl_list_insert(zone->resources.prev, &zr->link);

	send_zone_initial_events(zone, zr->resource);
}

void
lab_zone_item_on_view_destroy(struct lab_zone_item *item)
{
	if (!item) {
		return;
	}
	if (item->resource) {
		xx_zone_item_v1_send_closed(item->resource);
	}
	item->view = NULL;
}

static void
handle_zone_manager_get_zone_item(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *toplevel_resource)
{
	struct lab_zone_manager *manager = wl_resource_get_user_data(resource);
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
	struct lab_zone_item *item = znew(*item);
	item->manager = manager;
	item->view = view;
	view->zone_item = item;

	item->resource = wl_resource_create(client,
		&xx_zone_item_v1_interface, version, id);
	wl_resource_set_implementation(item->resource, &zone_item_impl, item, zone_item_resource_destroy);
	wl_list_insert(manager->items.prev, &item->link);
}

static const struct xx_zone_manager_v1_interface zone_manager_impl = {
	.destroy = handle_zone_manager_destroy,
	.get_zone_item = handle_zone_manager_get_zone_item,
	.get_zone = handle_zone_manager_get_zone,
	.get_zone_from_handle = handle_zone_manager_get_zone_from_handle,
};

static void
zone_resource_destroy(struct wl_resource *resource)
{
	struct lab_zone_resource *zr = wl_resource_get_user_data(resource);
	if (zr) {
		wl_list_remove(&zr->link);
		free(zr);
	}
}

static void
handle_zone_add_item(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *item_resource)
{
	struct lab_zone_resource *zr = wl_resource_get_user_data(resource);
	struct lab_zone_item *item = wl_resource_get_user_data(item_resource);
	if (!zr || !item || !item->view) {
		return;
	}

	struct lab_zone *zone = zr->zone;
	if (item->zone && item->zone != zone) {
		/* Emit item_left on previous zone resources for this client */
		struct lab_zone_resource *prev_zr;
		wl_list_for_each(prev_zr, &item->zone->resources, link) {
			if (wl_resource_get_client(prev_zr->resource) == client) {
				xx_zone_v1_send_item_left(prev_zr->resource, item_resource);
			}
		}
	}

	item->zone = zone;
	xx_zone_v1_send_item_entered(resource, item_resource);

	struct border margin = ssd_get_margin(item->view->ssd);
	xx_zone_item_v1_send_frame_extents(item->resource,
		margin.top, margin.bottom, margin.left, margin.right);

	if (item->has_pos) {
		xx_zone_item_v1_send_position(item->resource, item->x, item->y);
	} else if (item->has_pending_pos) {
		xx_zone_item_v1_send_position(item->resource, item->pending_x, item->pending_y);
	} else if (item->view->mapped) {
		struct wlr_box out_box = { 0 };
		wlr_output_layout_get_box(item->manager->server->output_layout,
			zone->output->wlr_output, &out_box);
		int fx = item->view->current.x - margin.left - out_box.x;
		int fy = item->view->current.y - margin.top - out_box.y;
		item->x = fx;
		item->y = fy;
		item->has_pos = true;
		xx_zone_item_v1_send_position(item->resource, fx, fy);
	} else {
		item->x = 0;
		item->y = 0;
		item->has_pos = true;
		xx_zone_item_v1_send_position(item->resource, 0, 0);
	}
}

static void
handle_zone_remove_item(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *item_resource)
{
	struct lab_zone_item *item = wl_resource_get_user_data(item_resource);
	if (!item) {
		return;
	}

	xx_zone_v1_send_item_left(resource, item_resource);
	item->zone = NULL;
}

static const struct xx_zone_v1_interface zone_impl = {
	.destroy = handle_zone_destroy,
	.add_item = handle_zone_add_item,
	.remove_item = handle_zone_remove_item,
};

static void
handle_zone_item_set_position(struct wl_client *client,
		struct wl_resource *resource,
		int32_t x, int32_t y)
{
	struct lab_zone_item *item = wl_resource_get_user_data(resource);
	if (!item || !item->view || !item->zone || !item->zone->output) {
		xx_zone_item_v1_send_position_failed(resource);
		return;
	}

	struct output *output = item->zone->output;
	struct wlr_box out_box = { 0 };
	wlr_output_layout_get_box(item->manager->server->output_layout,
		output->wlr_output, &out_box);

	int w = item->view->current.width > 0 ? item->view->current.width : item->view->pending.width;
	int h = item->view->current.height > 0 ? item->view->current.height : item->view->pending.height;
	if (w <= 0) {
		w = 100;
	}
	if (h <= 0) {
		h = 100;
	}

	/* position_failed only if the window would be entirely outside the zone */
	if (x + w <= 0 || x >= out_box.width || y + h <= 0 || y >= out_box.height) {
		xx_zone_item_v1_send_position_failed(resource);
		return;
	}

	item->has_pending_pos = true;
	item->pending_x = x;
	item->pending_y = y;
	item->view->zone_positioned = true;

	if (item->view->mapped) {
		struct border margin = ssd_get_margin(item->view->ssd);
		int target_x = out_box.x + x + margin.left;
		int target_y = out_box.y + y + margin.top;

		view_set_output(item->view, output);
		view_move(item->view, target_x, target_y);

		item->x = x;
		item->y = y;
		item->has_pos = true;
		item->has_pending_pos = false;

		xx_zone_item_v1_send_position(resource, x, y);
	}
}

static const struct xx_zone_item_v1_interface zone_item_impl = {
	.destroy = handle_zone_item_destroy,
	.set_position = handle_zone_item_set_position,
};

static void
zone_item_resource_destroy(struct wl_resource *resource)
{
	struct lab_zone_item *item = wl_resource_get_user_data(resource);
	if (item) {
		if (item->view) {
			item->view->zone_item = NULL;
		}
		wl_list_remove(&item->link);
		free(item);
	}
}

void
lab_zone_item_notify_moved(struct lab_zone_item *item)
{
	if (!item || !item->view || !item->zone || !item->zone->output || !item->resource) {
		return;
	}

	struct wlr_box out_box = { 0 };
	wlr_output_layout_get_box(item->manager->server->output_layout,
		item->zone->output->wlr_output, &out_box);

	struct border margin = ssd_get_margin(item->view->ssd);
	int fx = item->view->current.x - margin.left - out_box.x;
	int fy = item->view->current.y - margin.top - out_box.y;

	item->x = fx;
	item->y = fy;
	item->has_pos = true;

	xx_zone_item_v1_send_position(item->resource, fx, fy);
}

void
lab_zone_update_output(struct lab_zone *zone)
{
	if (!zone || !zone->output || !zone->output->wlr_output) {
		return;
	}
	struct wlr_box box = { 0 };
	wlr_output_effective_resolution(zone->output->wlr_output, &box.width, &box.height);

	struct lab_zone_resource *zr;
	wl_list_for_each(zr, &zone->resources, link) {
		xx_zone_v1_send_size(zr->resource, box.width, box.height);
	}
}

static void
handle_zone_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id)
{
	struct lab_zone_manager *manager = data;
	struct wl_resource *resource = wl_resource_create(client,
		&xx_zone_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &zone_manager_impl, manager, NULL);
}

static void
handle_display_destroy(struct wl_listener *listener, void *data)
{
	struct lab_zone_manager *manager =
		wl_container_of(listener, manager, display_destroy);
	lab_zone_manager_finish(manager->server);
}

void
lab_zone_manager_init(struct server *server)
{
	struct lab_zone_manager *manager = znew(*manager);
	manager->server = server;
	wl_list_init(&manager->zones);
	wl_list_init(&manager->items);

	manager->global = wl_global_create(server->wl_display,
		&xx_zone_manager_v1_interface, 1, manager, handle_zone_manager_bind);
	if (!manager->global) {
		wlr_log(WLR_ERROR, "failed to create xx_zone_manager_v1 global");
		free(manager);
		return;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(server->wl_display, &manager->display_destroy);

	server->zone_manager = manager;
}

void
lab_zone_manager_finish(struct server *server)
{
	struct lab_zone_manager *manager = server->zone_manager;
	if (!manager) {
		return;
	}

	struct lab_zone *zone, *zone_tmp;
	wl_list_for_each_safe(zone, zone_tmp, &manager->zones, link) {
		struct lab_zone_resource *zr, *zr_tmp;
		wl_list_for_each_safe(zr, zr_tmp, &zone->resources, link) {
			wl_list_remove(&zr->link);
			free(zr);
		}
		wl_list_remove(&zone->link);
		free(zone->handle);
		free(zone);
	}

	struct lab_zone_item *item, *item_tmp;
	wl_list_for_each_safe(item, item_tmp, &manager->items, link) {
		if (item->view) {
			item->view->zone_item = NULL;
		}
		wl_list_remove(&item->link);
		free(item);
	}

	wl_global_destroy(manager->global);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
	server->zone_manager = NULL;
}
