/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PROTOCOLS_XX_ZONES_H
#define LABWC_PROTOCOLS_XX_ZONES_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct server;
struct output;
struct view;
struct wlr_xdg_toplevel;

struct lab_zone_manager {
	struct wl_global *global;
	struct server *server;
	struct wl_list zones;      /* struct lab_zone.link */
	struct wl_list items;      /* struct lab_zone_item.link */
	struct wl_listener display_destroy;
};

struct lab_zone {
	struct wl_list link;       /* lab_zone_manager.zones */
	struct lab_zone_manager *manager;
	struct output *output;
	char *handle;
	struct wl_list resources;  /* struct lab_zone_resource.link */
	struct wl_listener output_destroy;
};

struct lab_zone_resource {
	struct wl_list link;
	struct wl_resource *resource;
	struct lab_zone *zone;
};

struct lab_zone_item {
	struct wl_list link;       /* lab_zone_manager.items */
	struct lab_zone_manager *manager;
	struct view *view;
	struct lab_zone *zone;
	struct wl_resource *resource;
	bool has_pending_pos;
	int pending_x;
	int pending_y;
	bool has_pos;
	int x;
	int y;
};

void lab_zone_manager_init(struct server *server);
void lab_zone_manager_finish(struct server *server);
struct lab_zone *lab_zone_get_or_create(struct lab_zone_manager *manager, struct output *output);
void lab_zone_update_output(struct lab_zone *zone);
void lab_zone_item_notify_moved(struct lab_zone_item *item);
void lab_zone_item_on_view_destroy(struct lab_zone_item *item);
struct view *lookup_view_by_xdg_toplevel_public(struct server *server, struct wlr_xdg_toplevel *xdg_toplevel);

#endif /* LABWC_PROTOCOLS_XX_ZONES_H */
