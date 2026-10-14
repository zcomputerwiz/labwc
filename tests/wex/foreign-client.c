/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shm-common.h"
#include "wex-window-v1-client-protocol.h"
#include "xx-zones-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct item {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	struct wex_foreign_window_v1 *foreign;
	char title[128], token[33];
	int x, y, width, height, min_width, min_height;
	unsigned mode, done, states;
	bool closed, foreign_closed;
};
struct observer {
	struct wex_context ctx;
	struct wl_registry *registry;
	struct wex_window_manager_v1 *manager;
	struct zwlr_foreign_toplevel_manager_v1 *inventory;
	struct item items[16];
	unsigned count;
};
static void token(void *data, struct wex_foreign_window_v1 *obj, const char *value)
{
	struct item *item = data;
	assert(strlen(value) == 32 && strspn(value, "0123456789abcdef") == 32);
	snprintf(item->token, sizeof(item->token), "%s", value);
}
static void geometry(void *data, struct wex_foreign_window_v1 *obj,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct item *item = data;
	item->x = x;
	item->y = y;
	item->width = width;
	item->height = height;
}
static void decoration(void *data, struct wex_foreign_window_v1 *obj, uint32_t mode)
{
	((struct item *)data)->mode = mode;
}
static void hints(void *data, struct wex_foreign_window_v1 *obj,
		int32_t min_width, int32_t min_height, int32_t width_inc,
		int32_t height_inc, int32_t base_width, int32_t base_height)
{
	struct item *item = data;
	item->min_width = min_width;
	item->min_height = min_height;
}
static void done(void *data, struct wex_foreign_window_v1 *obj)
{
	++((struct item *)data)->done;
}
static void closed(void *data, struct wex_foreign_window_v1 *obj)
{
	((struct item *)data)->foreign_closed = true;
}
static const struct wex_foreign_window_v1_listener supplement = {
	.token = token, .geometry = geometry, .decoration = decoration,
	.size_hints = hints, .done = done, .closed = closed,
};
static void title(void *data, struct zwlr_foreign_toplevel_handle_v1 *obj, const char *value)
{
	snprintf(((struct item *)data)->title, 128, "%s", value);
}
static void app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *obj, const char *value) {}
static void output(void *data, struct zwlr_foreign_toplevel_handle_v1 *obj, struct wl_output *value) {}
static void state(void *data, struct zwlr_foreign_toplevel_handle_v1 *obj, struct wl_array *values)
{
	struct item *item = data;
	uint32_t *value;
	item->states = 0;
	wl_array_for_each(value, values) {
		assert(*value < 32);
		item->states |= 1u << *value;
	}
}
static void standard_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *obj) {}
static void standard_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *obj)
{
	((struct item *)data)->closed = true;
}
static void parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *obj,
		struct zwlr_foreign_toplevel_handle_v1 *value) {}
static const struct zwlr_foreign_toplevel_handle_v1_listener standard = {
	.title = title, .app_id = app_id, .output_enter = output,
	.output_leave = output, .state = state, .done = standard_done,
	.closed = standard_closed, .parent = parent,
};
static void toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *obj,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct observer *observer = data;
	assert(observer->count < 16 && observer->manager);
	struct item *item = &observer->items[observer->count++];
	item->handle = handle;
	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &standard, item);
	item->foreign = wex_window_manager_v1_get_foreign(observer->manager, handle);
	wex_foreign_window_v1_add_listener(item->foreign, &supplement, item);
}
static void finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *obj) {}
static const struct zwlr_foreign_toplevel_manager_v1_listener inventory = { toplevel, finished };
static void global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	struct observer *observer = data;
	if (!strcmp(interface, "wex_window_manager_v1") && version >= 3) {
		observer->manager = wl_registry_bind(registry, name, &wex_window_manager_v1_interface, 3);
	} else if (!strcmp(interface, "zwlr_foreign_toplevel_manager_v1")) {
		observer->inventory = wl_registry_bind(registry, name,
			&zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(observer->inventory, &inventory, observer);
	}
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener globals = { global, removed };
static void pump(struct observer *observer)
{
	for (unsigned i = 0; i < 3; ++i) assert(wl_display_roundtrip(observer->ctx.display) >= 0);
}
static void observe(struct observer *observer)
{
	assert(wex_context_init(&observer->ctx, "wex-shell-0"));
	observer->registry = wl_display_get_registry(observer->ctx.display);
	wl_registry_add_listener(observer->registry, &globals, observer);
	pump(observer);
	assert(observer->manager && observer->inventory);
}
static struct item *find(struct observer *observer, const char *value)
{
	for (unsigned i = 0; i < observer->count; ++i)
		if (!strcmp(observer->items[i].title, value)) return &observer->items[i];
	abort();
}
static void map(struct wex_window_obj *win, struct wex_context *ctx, const char *name, bool ssd)
{
	assert(wex_window_init(win, ctx, name, 240, 180, 0xff338855));
	xdg_toplevel_set_min_size(win->xdg_toplevel, 120, 90);
	if (ssd && win->deco) zxdg_toplevel_decoration_v1_set_mode(win->deco,
		ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	wl_surface_commit(win->surface);
	assert(wl_display_roundtrip(ctx->display) >= 0);
	wex_window_commit_buffer(win);
	assert(wl_display_roundtrip(ctx->display) >= 0);
}
int main(void)
{
	struct wex_context native;
	struct wex_window_obj csd, ssd, late, frame;
	struct observer first = {0}, second = {0};
	assert(wex_context_init(&native, "wayland-0"));
	assert(!native.wex_window_manager);
	map(&csd, &native, "L2 CSD", false);
	map(&ssd, &native, "L2 SSD", true);
	observe(&first);
	observe(&second);
	struct item *a = find(&first, "L2 CSD"), *b = find(&first, "L2 SSD");
	assert(a->done && b->done && !a->foreign_closed && !b->foreign_closed);
	assert(a->width == 240 && a->height == 180 && a->min_width == 120 && a->min_height == 90);
	assert(a->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_CLIENT);
	assert(b->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER);
	assert(strcmp(a->token, b->token) && !strcmp(a->token, find(&second, "L2 CSD")->token));
	puts("PASS existing inventory, shared lifetime tokens, geometry, constraints and SSD/CSD");
	unsigned before = a->done;
	wex_foreign_window_v1_refresh(a->foreign);
	pump(&first);
	assert(a->done > before);
	struct xx_zone_v1 *zone = xx_zone_manager_v1_get_zone(native.zone_manager, NULL);
	csd.zone_item = xx_zone_manager_v1_get_zone_item(native.zone_manager, csd.xdg_toplevel);
	xx_zone_v1_add_item(zone, csd.zone_item);
	xx_zone_item_v1_set_position(csd.zone_item, 317, 229);
	wl_surface_commit(csd.surface);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&first);
	assert(a->x == 317 && a->y == 229);
	zxdg_toplevel_decoration_v1_set_mode(ssd.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&first);
	assert(b->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_CLIENT);
	zxdg_toplevel_decoration_v1_set_mode(ssd.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&first);
	assert(b->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER);
	puts("PASS live logical geometry and decoration changes");
	xdg_toplevel_set_title(csd.xdg_toplevel, "L2 renamed");
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&first);
	assert(!strcmp(a->title, "L2 renamed"));
	zwlr_foreign_toplevel_handle_v1_set_minimized(b->handle);
	pump(&first);
	assert(b->states & (1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED));
	zwlr_foreign_toplevel_handle_v1_unset_minimized(b->handle);
	pump(&first);
	assert(!(b->states & (1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)));
	puts("PASS standard title changes and minimize/restore controls");
	map(&late, &native, "L2 late", false);
	pump(&first);
	struct item *c = find(&first, "L2 late");
	assert(c->done && strcmp(c->token, a->token));
	wl_display_disconnect(second.ctx.display);
	wex_foreign_window_v1_refresh(b->foreign);
	pump(&first);
	assert(b->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER && !b->foreign_closed);
	puts("PASS late mapping and independent observer disconnection");
	assert(wex_window_init(&frame, &first.ctx, "L2 Wine frame", 100, 100, 0xffee8899));
	frame.wex_win = wex_window_manager_v1_get_window(first.ctx.wex_window_manager, frame.xdg_toplevel);
	wl_surface_commit(frame.surface);
	assert(wl_display_roundtrip(first.ctx.display) >= 0);
	wex_window_commit_buffer(&frame);
	pump(&first);
	struct item *own = find(&first, "L2 Wine frame");
	assert(own->foreign_closed && !own->token[0]);
	puts("PASS Wine-controlled frames excluded from native observation");
	wex_window_finish(&csd);
	wex_window_finish(&ssd);
	wex_window_finish(&late);
	xx_zone_v1_destroy(zone);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&first);
	assert(a->closed && a->foreign_closed && b->closed && b->foreign_closed && c->foreign_closed);
	before = a->done;
	wex_foreign_window_v1_refresh(a->foreign);
	pump(&first);
	assert(a->done == before);
	puts("PASS closed lifecycle and inert stale observers");
	wex_window_finish(&frame);
	wex_context_finish(&native);
	wl_display_disconnect(first.ctx.display);
	puts("FOREIGN_OBSERVATION_PASSED");
	return 0;
}
