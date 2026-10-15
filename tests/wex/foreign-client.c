/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include "shm-common.h"
#include "wex-window-v1-client-protocol.h"
#include "xx-zones-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct item {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	struct zwlr_foreign_toplevel_handle_v1 *parent;
	struct wex_foreign_window_v1 *foreign;
	char title[128], token[33], controller[33];
	int x, y, width, height, min_width, min_height;
	int max_width, max_height;
	unsigned maximum_events;
	unsigned mode, done, states, decoration_downgrades;
	bool closed, foreign_closed;
};
struct observer {
	struct wex_context ctx;
	struct wl_registry *registry;
	struct wex_window_manager_v1 *manager;
	struct zwlr_foreign_toplevel_manager_v1 *inventory;
	/* Keep closed records stable for lifetime assertions. Shell stress tests
	 * can withdraw/recreate up to 200 frames in the same native connection. */
	struct item items[512];
	unsigned count;
	unsigned protocol_version;
	char native_socket[108], native_xdisplay[128];
};
static void native_environment(void *data, struct wex_window_manager_v1 *obj,
		const char *wayland_display, const char *xwayland_display)
{
	struct observer *observer = data;
	snprintf(observer->native_socket, sizeof(observer->native_socket), "%s", wayland_display);
	snprintf(observer->native_xdisplay, sizeof(observer->native_xdisplay), "%s", xwayland_display);
}
static const struct wex_window_manager_v1_listener environment = { native_environment };
static void token(void *data, struct wex_foreign_window_v1 *obj, const char *value)
{
	struct item *item = data;
	assert(strlen(value) == 32 && strspn(value, "0123456789abcdef") == 32);
	snprintf(item->token, sizeof(item->token), "%s", value);
}
static void controller(void *data, struct wex_foreign_window_v1 *obj, const char *value)
{
	struct item *item = data;
	assert(strlen(value) == 32 && strspn(value, "0123456789abcdef") == 32);
	snprintf(item->controller, sizeof(item->controller), "%s", value);
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
	struct item *item = data;
	if (item->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER && mode != item->mode)
		item->decoration_downgrades++;
	item->mode = mode;
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
static void maximum_size(void *data, struct wex_foreign_window_v1 *obj, int32_t width, int32_t height)
{
	struct item *item = data;
	item->max_width = width;
	item->max_height = height;
	item->maximum_events++;
}
static void closed(void *data, struct wex_foreign_window_v1 *obj)
{
	((struct item *)data)->foreign_closed = true;
}
static const struct wex_foreign_window_v1_listener supplement = {
	.token = token, .geometry = geometry, .decoration = decoration,
	.size_hints = hints, .done = done, .closed = closed,
	.controller_handle = controller,
	.maximum_size = maximum_size,
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
		struct zwlr_foreign_toplevel_handle_v1 *value)
{
	((struct item *)data)->parent = value;
}
static const struct zwlr_foreign_toplevel_handle_v1_listener standard = {
	.title = title, .app_id = app_id, .output_enter = output,
	.output_leave = output, .state = state, .done = standard_done,
	.closed = standard_closed, .parent = parent,
};
static void toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *obj,
		struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct observer *observer = data;
	assert(observer->count < sizeof(observer->items) / sizeof(observer->items[0]) && observer->manager);
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
	unsigned requested = observer->protocol_version ? observer->protocol_version : 3;
	if (!strcmp(interface, "wex_window_manager_v1") && version >= requested) {
		observer->manager = wl_registry_bind(registry, name, &wex_window_manager_v1_interface, requested);
		wex_window_manager_v1_add_listener(observer->manager, &environment, observer);
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
static void map_parent(struct wex_window_obj *win, struct wex_context *ctx,
		const char *name, bool ssd, struct wex_window_obj *parent)
{
	assert(wex_window_init(win, ctx, name, 240, 180, 0xff338855));
	if (parent) xdg_toplevel_set_parent(win->xdg_toplevel,parent->xdg_toplevel);
	xdg_toplevel_set_min_size(win->xdg_toplevel, 120, 90);
	if (ssd && win->deco) zxdg_toplevel_decoration_v1_set_mode(win->deco,
		ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	wl_surface_commit(win->surface);
	assert(wl_display_roundtrip(ctx->display) >= 0);
	wex_window_commit_buffer(win);
	assert(wl_display_roundtrip(ctx->display) >= 0);
}
static void map(struct wex_window_obj *win, struct wex_context *ctx, const char *name, bool ssd)
{
	map_parent(win,ctx,name,ssd,NULL);
}
static int association_test(void);
static int publish_association(bool hosting);
static int scene_test(void);
static int resize_test(void);
static int constraints_test(void);
static int xwayland_constraints_test(void);
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 2 && !strcmp(argv[1], "--association")) return association_test();
	if (argc == 2 && !strcmp(argv[1], "--publish-association")) return publish_association(false);
	if (argc == 2 && !strcmp(argv[1], "--publish-hosting")) return publish_association(true);
	if (argc == 2 && !strcmp(argv[1], "--scene")) return scene_test();
	if (argc == 2 && !strcmp(argv[1], "--resize")) return resize_test();
	if (argc == 2 && !strcmp(argv[1], "--constraints")) return constraints_test();
	if (argc == 2 && !strcmp(argv[1], "--xwayland-constraints")) return xwayland_constraints_test();
	struct wex_context native;
	struct wex_window_obj csd, ssd, late, frame;
	struct observer first = {0}, second = {0};
	assert(wex_context_init(&native, "wayland-0"));
	assert(!native.wex_window_manager);
	map(&csd, &native, "L2 CSD", false);
	map(&ssd, &native, "L2 SSD", true);
	observe(&first);
	observe(&second);
	assert(!strcmp(first.native_socket, "wayland-0"));
	assert(!strcmp(first.native_socket, second.native_socket));
	assert(!strcmp(first.native_xdisplay, second.native_xdisplay));
	if (getenv("WEX_EXPECT_NO_XWAYLAND")) assert(!first.native_xdisplay[0]);
	puts("PASS compositor native launch endpoints across privileged observers");
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

static struct zwp_virtual_keyboard_v1 *scene_keyboard(struct observer *bridge);
static void checked_frame_handle(void *data, struct wex_window_v1 *obj, const char *value)
{
	*(bool *)data = strlen(value) == 32;
}

static struct item *wait_hints(struct observer *observer, const char *name,
		int min_width, int min_height, int max_width, int max_height)
{
	for (unsigned attempt = 0; attempt < 200; attempt++) {
		pump(observer);
		for (unsigned i = 0; i < observer->count; i++) {
			struct item *item = &observer->items[i];
			if (!strcmp(item->title, name) && item->done &&
					item->min_width == min_width && item->min_height == min_height &&
					item->max_width == max_width && item->max_height == max_height) return item;
		}
		usleep(10000);
	}
	fprintf(stderr, "Expected %s min=%d,%d max=%d,%d; observed %u views\n",
		name,min_width,min_height,max_width,max_height,observer->count);
	for (unsigned i = 0; i < observer->count; i++) {
		struct item *item = &observer->items[i];
		fprintf(stderr, "%s min=%d,%d max=%d,%d done=%u closed=%d\n",item->title,
			item->min_width,item->min_height,item->max_width,item->max_height,item->done,item->foreign_closed);
	}
	assert(!"native hints observation timed out");
	return NULL;
}

static int xwayland_constraints_test(void)
{
	struct observer observer = {.protocol_version = 9};
	observe(&observer);
	if (!observer.native_xdisplay[0]) {
		wl_display_disconnect(observer.ctx.display);
		puts("XWAYLAND_CONSTRAINTS_SKIPPED no XWayland");
		return 0;
	}
	int screen_number;
	xcb_connection_t *connection = xcb_connect(observer.native_xdisplay, &screen_number);
	assert(!xcb_connection_has_error(connection));
	xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
	while (screen_number--) xcb_screen_next(&screens);
	xcb_screen_t *screen = screens.data;
	xcb_window_t window = xcb_generate_id(connection);
	uint32_t values[] = {0x445566, XCB_EVENT_MASK_EXPOSURE};
	xcb_void_cookie_t created = xcb_create_window_checked(connection, XCB_COPY_FROM_PARENT, window, screen->root,
		50, 50, 240, 180, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
		XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
	assert(!xcb_request_check(connection, created));
	const char *name = "L2 XWayland constraints";
	xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, strlen(name), name);
	xcb_size_hints_t hints = {.flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE,
		.min_width = 120, .min_height = 90, .max_width = 320, .max_height = 240};
	xcb_icccm_set_wm_normal_hints(connection, window, &hints);
	assert(!xcb_request_check(connection, xcb_map_window_checked(connection, window)));
	xcb_flush(connection);
	struct item *item = wait_hints(&observer, name, 120, 90, 320, 240);
	hints.min_width = hints.max_width = 240;
	hints.min_height = hints.max_height = 180;
	xcb_icccm_set_wm_normal_hints(connection, window, &hints); xcb_flush(connection);
	wait_hints(&observer, name, 240, 180, 240, 180);
	hints.min_width = 120; hints.min_height = 90;
	hints.max_width = 0; hints.max_height = 200;
	xcb_icccm_set_wm_normal_hints(connection, window, &hints); xcb_flush(connection);
	wait_hints(&observer, name, 120, 90, 0, 200);
	/* Stale values without PMaxSize must not become live upper bounds. */
	hints.flags &= ~XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
	hints.max_width = 320; hints.max_height = 240;
	xcb_icccm_set_wm_normal_hints(connection, window, &hints); xcb_flush(connection);
	wait_hints(&observer, name, 120, 90, 0, 0);
	xcb_destroy_window(connection, window); xcb_flush(connection);
	for (unsigned attempt = 0; attempt < 200 && !item->foreign_closed; attempt++) {
		pump(&observer); usleep(10000);
	}
	assert(item->foreign_closed);
	xcb_disconnect(connection); wl_display_disconnect(observer.ctx.display);
	puts("XWAYLAND_CONSTRAINTS_PASSED initial/live/fixed/partial/flags/lifetime");
	return 0;
}

static int constraints_test(void)
{
	struct wex_context native;
	struct wex_window_obj win;
	struct wex_window_obj frame;
	struct observer modern = {.protocol_version = 9}, old = {.protocol_version = 8};
	assert(wex_context_init(&native, "wayland-0"));
	map(&win, &native, "L2 constraint reporting", true);
	xdg_toplevel_set_max_size(win.xdg_toplevel, 320, 240);
	wl_surface_commit(win.surface);
	assert(wl_display_roundtrip(native.display) >= 0);
	observe(&modern); observe(&old);
	bool got_frame_handle = false;
	const struct wex_window_v1_listener frame_events = {.handle = checked_frame_handle};
	assert(wex_window_init(&frame, &modern.ctx, "L2 version 9 frame", 300, 220, 0xff885544));
	frame.wex_win = wex_window_manager_v1_get_window(modern.manager, frame.xdg_toplevel);
	wex_window_v1_add_listener(frame.wex_win, &frame_events, &got_frame_handle);
	pump(&modern);
	assert(got_frame_handle);
	wex_window_finish(&frame);
	struct item *item = find(&modern, "L2 constraint reporting");
	struct item *legacy = find(&old, "L2 constraint reporting");
	assert(item->min_width == 120 && item->min_height == 90);
	assert(item->max_width == 320 && item->max_height == 240 && item->maximum_events);
	assert(!legacy->maximum_events && !legacy->max_width && !legacy->max_height);
	unsigned before = item->done;
	xdg_toplevel_set_min_size(win.xdg_toplevel, 240, 180);
	xdg_toplevel_set_max_size(win.xdg_toplevel, 240, 180);
	wl_surface_commit(win.surface);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&modern); pump(&old);
	assert(item->done > before && item->width == 240 && item->height == 180);
	assert(item->min_width == 240 && item->min_height == 180);
	assert(item->max_width == 240 && item->max_height == 180);
	assert(legacy->min_width == 240 && legacy->min_height == 180 && !legacy->maximum_events);
	xdg_toplevel_set_min_size(win.xdg_toplevel, 0, 90);
	xdg_toplevel_set_max_size(win.xdg_toplevel, 0, 200);
	wl_surface_commit(win.surface);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&modern);
	assert(item->min_width == 0 && item->min_height == 90);
	assert(item->max_width == 0 && item->max_height == 200);
	xdg_toplevel_set_min_size(win.xdg_toplevel, 0, 0);
	xdg_toplevel_set_max_size(win.xdg_toplevel, 0, 0);
	wl_surface_commit(win.surface);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&modern);
	assert(!item->min_width && !item->min_height && !item->max_width && !item->max_height);
	before = item->done;
	wl_surface_commit(win.surface);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&modern);
	assert(item->done == before);
	wex_window_finish(&win);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&modern); pump(&old);
	assert(item->foreign_closed && legacy->foreign_closed);
	wl_display_disconnect(modern.ctx.display); wl_display_disconnect(old.ctx.display);
	wex_context_finish(&native);
	puts("CONSTRAINTS_PASSED initial/live/fixed/partial/unlimited/unchanged/legacy/lifetime");
	return 0;
}

static int publish_association(bool hosting)
{
	/* poll() must see every command, including consecutive lines. */
	setvbuf(stdin,NULL,_IONBF,0);
	struct wex_context native;
	struct wex_window_obj win, child;
	bool main_alive = true, child_alive = false;
	bool follow_configures = false;
	uint32_t painted_configure = 0;
	struct observer owner = {.protocol_version = 4};
	struct zwlr_virtual_pointer_v1 *pointer = NULL;
	struct zwp_virtual_keyboard_v1 *keyboard = NULL;
	assert(wex_context_init(&native, "wayland-0"));
	map(&win, &native, "L2 Wine association peer", hosting);
	observe(&owner);
	struct item *item = find(&owner, "L2 Wine association peer");
	printf("LEASE %s %s\n", item->token, item->controller);
	fflush(stdout);
	for (;;) {
		if (child_alive && child.closed) {
			wex_window_finish(&child);
			child_alive=false;
		}
		if ((main_alive && win.closed) || (!main_alive && !child_alive)) break;
		struct pollfd fds[] = {
			{wl_display_get_fd(native.display), POLLIN, 0},
			{wl_display_get_fd(owner.ctx.display), POLLIN, 0},
			{0, POLLIN, 0},
		};
		assert(wl_display_dispatch_pending(native.display) >= 0);
		assert(wl_display_dispatch_pending(owner.ctx.display) >= 0);
		if (follow_configures && main_alive && win.configure_serial != painted_configure) {
			int width = win.configured_width > 0 ? win.configured_width : win.width;
			int height = win.configured_height > 0 ? win.configured_height : win.height;
			struct wex_buffer old = win.buffer;
			assert(wex_buffer_create(&native, &win.buffer, width, height, 0xff338855));
			win.width = width; win.height = height;
			painted_configure = win.configure_serial;
			xdg_surface_set_window_geometry(win.xdg_surface, 0, 0, width, height);
			wex_window_commit_buffer(&win);
			wex_buffer_destroy(&old);
		}
		wl_display_flush(native.display);
		wl_display_flush(owner.ctx.display);
		if (poll(fds, 3, -1) < 0) return 1;
		if (fds[0].revents & POLLIN) assert(wl_display_dispatch(native.display) >= 0);
		if (fds[1].revents & POLLIN) assert(wl_display_dispatch(owner.ctx.display) >= 0);
		if (fds[2].revents & (POLLIN | POLLHUP)) {
			char command[32];
			if (!fgets(command, sizeof(command), stdin) || !strcmp(command,"quit\n")) break;
			if (!strcmp(command,"follow-configures\n")) {
				follow_configures = true;
				painted_configure = win.configure_serial;
			}
			if (!strcmp(command,"child\n") && main_alive && !child_alive) {
				map_parent(&child,&native,"L2 Shell child",true,&win);
				child_alive=true;
			}
			if (child_alive && (!strcmp(command,"orphan\n") || !strcmp(command,"parent\n"))) {
				xdg_toplevel_set_parent(child.xdg_toplevel,!strcmp(command,"orphan\n") ? NULL : win.xdg_toplevel);
				wl_surface_commit(child.surface);
				assert(wl_display_roundtrip(native.display)>=0);
			}
			if (!strcmp(command,"parent-close\n") && main_alive) {
				wex_window_finish(&win);
				main_alive=false;
				assert(wl_display_roundtrip(native.display)>=0);
			}
			if (!strcmp(command,"parent-unmap\n") && main_alive) {
				wl_surface_attach(win.surface,NULL,0,0);
				wl_surface_commit(win.surface);
				assert(wl_display_roundtrip(native.display)>=0);
			}
			if (!strcmp(command,"parent-remap\n") && main_alive) {
				win.configured=false;
				xdg_toplevel_set_title(win.xdg_toplevel,"L2 Wine association peer");
				zxdg_toplevel_decoration_v1_set_mode(win.deco,ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
				wl_surface_commit(win.surface);
				assert(wl_display_roundtrip(native.display)>=0);
				wex_window_commit_buffer(&win);
				assert(wl_display_roundtrip(native.display)>=0);
			}
			if (!strcmp(command,"parent-state\n") && child_alive) {
				pump(&owner);
				printf("PARENT %u\n",find(&owner,"L2 Shell child")->parent != NULL);
				fflush(stdout);
			}
			if (!strcmp(command,"release\n") && item->foreign) {
				wex_foreign_window_v1_destroy(item->foreign);
				item->foreign = NULL;
				pump(&owner);
				puts("RELEASED");
				fflush(stdout);
			}
			if (!strcmp(command,"title\n")) {
				xdg_toplevel_set_title(win.xdg_toplevel,"L2 Shell renamed");
				wl_display_roundtrip(native.display);
			}
			if (!strcmp(command,"resize\n")) {
				struct wex_buffer old = win.buffer;
				assert(wex_buffer_create(&native,&win.buffer,260,200,0xff338855));
				win.width=260; win.height=200;
				xdg_surface_set_window_geometry(win.xdg_surface,0,0,260,200);
				wex_window_commit_buffer(&win);
				assert(wl_display_roundtrip(native.display)>=0);
				wex_buffer_destroy(&old);
			}
			if (!strcmp(command,"csd\n") || !strcmp(command,"ssd\n")) {
				zxdg_toplevel_decoration_v1_set_mode(win.deco,!strcmp(command,"csd\n") ?
					ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
				wl_surface_commit(win.surface);
				assert(wl_display_roundtrip(native.display)>=0);
			}
			if (!strcmp(command,"minimize\n")) zwlr_foreign_toplevel_handle_v1_set_minimized(item->handle);
			if (!strcmp(command,"limited\n") || !strcmp(command,"fixed\n") ||
					!strcmp(command,"height-limit\n") || !strcmp(command,"unlimited\n")) {
				bool fixed = !strcmp(command,"fixed\n");
				bool limited = !strcmp(command,"limited\n");
				bool height_only = !strcmp(command,"height-limit\n");
				xdg_toplevel_set_min_size(win.xdg_toplevel, fixed ? 240 : 120, fixed ? 180 : 90);
				xdg_toplevel_set_max_size(win.xdg_toplevel, fixed ? 240 : limited ? 320 : 0,
					fixed ? 180 : limited ? 240 : height_only ? 200 : 0);
				wl_surface_commit(win.surface);
				assert(wl_display_roundtrip(native.display)>=0);
			}
			if (!strcmp(command,"restore\n")) zwlr_foreign_toplevel_handle_v1_unset_minimized(item->handle);
			if (!strcmp(command,"state\n")) {
				pump(&owner);
				printf("STATE %u\n",!!(item->states & (1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)));
				fflush(stdout);
			}
			if (!strcmp(command,"geometry\n")) {
				pump(&owner);
				printf("GEOMETRY %d %d %d %d\n",item->x,item->y,item->width,item->height);
				fflush(stdout);
			}
			unsigned key;
			if (sscanf(command,"key %u",&key) == 1) {
				if (!keyboard) keyboard=scene_keyboard(&owner);
				zwp_virtual_keyboard_v1_key(keyboard,1,key,WL_KEYBOARD_KEY_STATE_PRESSED);
				zwp_virtual_keyboard_v1_key(keyboard,2,key,WL_KEYBOARD_KEY_STATE_RELEASED);
				pump(&owner);
				puts("KEY"); fflush(stdout);
			}
			int click_x, click_y;
			bool motion_only = sscanf(command,"motion %d %d",&click_x,&click_y) == 2;
			bool press_only = sscanf(command,"press %d %d",&click_x,&click_y) == 2;
			bool release_only = sscanf(command,"release-at %d %d",&click_x,&click_y) == 2;
			bool right_click = sscanf(command,"right %d %d",&click_x,&click_y) == 2;
			if (motion_only || press_only || release_only || right_click || sscanf(command,"click %d %d",&click_x,&click_y) == 2) {
				uint32_t button = right_click ? BTN_RIGHT : BTN_LEFT;
				if (!pointer) pointer=zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
					owner.ctx.virtual_pointer_manager,owner.ctx.seat);
				zwlr_virtual_pointer_v1_motion_absolute(pointer,0,click_x,click_y,1280,720);
				zwlr_virtual_pointer_v1_frame(pointer);
				if (!motion_only) {
					if (!release_only) {
						zwlr_virtual_pointer_v1_button(pointer,0,button,WL_POINTER_BUTTON_STATE_PRESSED);
						zwlr_virtual_pointer_v1_frame(pointer);
					}
					if (!press_only) {
						zwlr_virtual_pointer_v1_button(pointer,0,button,WL_POINTER_BUTTON_STATE_RELEASED);
						zwlr_virtual_pointer_v1_frame(pointer);
					}
				}
				pump(&owner);
				assert(wl_display_roundtrip(native.display)>=0);
				printf("CLICK %u %u %d\n",native.keyboard_focus_surface==win.surface,
					native.pointer_focus_surface==win.surface,native.button_event_count);
				fflush(stdout);
			}
		}
	}
	if (child_alive) wex_window_finish(&child);
	if (main_alive) wex_window_finish(&win);
	wex_context_finish(&native);
	wl_display_disconnect(owner.ctx.display);
	return 0;
}

struct resize_reply { uint32_t serial, stage, configure_serial; int x, y, width, height; };
struct frame_status {
	unsigned status, replies, group, group_replies, focus, epoch, focus_replies;
	unsigned configured_serial, configured_status, captured, capture_epoch;
	struct resize_reply resize[128];
	unsigned resize_count;
};
static void frame_handle(void *data, struct wex_window_v1 *obj, const char *handle) {}
static void association(void *data, struct wex_window_v1 *obj, uint32_t status)
{
	struct frame_status *frame = data;
	frame->status = status;
	frame->replies++;
}
static void group(void *data, struct wex_window_v1 *obj, uint32_t status)
{
	struct frame_status *frame = data;
	frame->group = status;
	frame->group_replies++;
}
static void foreign_focus(void *data, struct wex_window_v1 *obj, uint32_t epoch, uint32_t target)
{
	struct frame_status *frame = data;
	assert(target <= WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	frame->epoch = epoch;
	frame->focus = target;
	frame->focus_replies++;
}
static void configured_foreign(void *data, struct wex_window_v1 *obj, uint32_t serial, uint32_t status)
{
	struct frame_status *frame = data;
	frame->configured_serial = serial;
	frame->configured_status = status;
}
static void foreign_pointer_capture(void *data, struct wex_window_v1 *obj, uint32_t epoch, uint32_t captured)
{
	struct frame_status *frame = data;
	frame->captured = captured;
	frame->capture_epoch = epoch;
}
static void foreign_resize(void *data, struct wex_window_v1 *obj, uint32_t serial,
		uint32_t stage, uint32_t configure_serial, int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct frame_status *frame = data;
	assert(frame->resize_count < 128);
	frame->resize[frame->resize_count++] = (struct resize_reply){serial, stage, configure_serial, x, y, width, height};
}
static const struct wex_window_v1_listener frame_listener = {
	.handle = frame_handle, .foreign_association = association,
	.foreign_group = group,
	.foreign_focus = foreign_focus,
	.configured_foreign = configured_foreign,
	.foreign_pointer_capture = foreign_pointer_capture,
	.foreign_resize = foreign_resize,
};
static void prepare_frame(struct observer *bridge, struct wex_window_obj *frame,
		struct frame_status *status)
{
	assert(wex_window_init(frame, &bridge->ctx, "L2 prepared frame", 300, 220, 0xff885544));
	frame->wex_win = wex_window_manager_v1_get_window(bridge->manager, frame->xdg_toplevel);
	wex_window_v1_add_listener(frame->wex_win, &frame_listener, status);
	pump(bridge);
}
static void request_association(struct observer *bridge, struct wex_window_obj *frame,
		struct frame_status *status, const char *token_value, const char *controller_value,
		unsigned expected)
{
	unsigned before = status->replies;
	wex_window_v1_associate_foreign(frame->wex_win, token_value, controller_value);
	pump(bridge);
	assert(status->replies > before && status->status == expected);
}
static int association_test(void)
{
	struct wex_context native;
	struct wex_window_obj one, two, frame, competitor;
	struct observer owner = {.protocol_version = 4}, peer = {.protocol_version = 4};
	struct observer bridge = {.protocol_version = 4};
	struct frame_status status = {0}, competing = {0};
	assert(wex_context_init(&native, "wayland-0"));
	map(&one, &native, "L2 association native", false);
	map(&two, &native, "L2 association SSD", true);
	observe(&owner);
	observe(&peer);
	observe(&bridge);
	struct item *a = find(&owner, "L2 association native");
	struct item *b = find(&owner, "L2 association SSD");
	struct item *other = find(&peer, "L2 association native");
	assert(a->controller[0] && other->controller[0] && strcmp(a->controller, other->controller));
	assert(!strcmp(a->token, other->token));
	prepare_frame(&bridge, &frame, &status);
	prepare_frame(&bridge, &competitor, &competing);
	struct wl_surface *focus = native.keyboard_focus_surface;
	int x = a->x, y = a->y;
	request_association(&bridge, &frame, &status, a->token, a->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	request_association(&bridge, &frame, &status, a->token, a->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	request_association(&bridge, &frame, &status, b->token, a->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_UNAVAILABLE);
	request_association(&bridge, &competitor, &competing, a->token, other->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_UNAVAILABLE);
	wex_foreign_window_v1_refresh(a->foreign);
	wex_foreign_window_v1_refresh(b->foreign);
	pump(&owner);
	assert(wl_display_roundtrip(native.display) >= 0);
	assert(a->x == x && a->y == y && b->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER);
	assert(native.keyboard_focus_surface == focus && !bridge.ctx.keyboard_focus_surface);
	puts("PASS cross-connection reservation, idempotence, mismatched/busy handles and no native scene/focus change");
	char stale_controller[33];
	snprintf(stale_controller, sizeof(stale_controller), "%s", a->controller);
	wex_foreign_window_v1_destroy(a->foreign);
	a->foreign = NULL;
	pump(&owner);
	pump(&bridge);
	assert(status.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	request_association(&bridge, &frame, &status, a->token, stale_controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_UNAVAILABLE);
	request_association(&bridge, &competitor, &competing, other->token, other->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wl_display_disconnect(peer.ctx.display);
	pump(&bridge);
	assert(competing.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	puts("PASS controller destruction/disconnection and stale controller rejection");
	request_association(&bridge, &frame, &status, b->token, b->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_v1_set_layer(frame.wex_win, WEX_WINDOW_V1_LAYER_SHELL);
	pump(&bridge);
	assert(status.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	request_association(&bridge, &frame, &status, b->token, b->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_UNAVAILABLE);
	wex_window_v1_set_layer(frame.wex_win, WEX_WINDOW_V1_LAYER_NORMAL);
	request_association(&bridge, &frame, &status, b->token, b->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_finish(&frame);
	pump(&bridge);
	request_association(&bridge, &competitor, &competing, b->token, b->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	request_association(&bridge, &competitor, &competing, "", "",
		WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	request_association(&bridge, &competitor, &competing, b->token, b->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_finish(&two);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&bridge);
	assert(competing.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	request_association(&bridge, &competitor, &competing, b->token, b->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_UNAVAILABLE);
	puts("PASS shell role rejection, frame destruction, explicit release and native lifetime invalidation");
	wex_window_finish(&competitor);
	wex_window_finish(&one);
	wex_context_finish(&native);
	wl_display_disconnect(owner.ctx.display);
	wl_display_disconnect(bridge.ctx.display);
	puts("FOREIGN_ASSOCIATION_PASSED");
	return 0;
}

static void capture_scene(const char *name)
{
	char path[1024];
	const char *directory = getenv("WEX_SCENE_TEST_DIR");
	assert(directory);
	snprintf(path, sizeof(path), "%s/%s.png", directory, name);
	usleep(100000);
	pid_t child = fork();
	assert(child >= 0);
	if (!child) { execlp("grim", "grim", path, NULL); _exit(127); }
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}
static void position(struct wex_window_obj *win, struct xx_zone_v1 *zone, int x, int y)
{
	if (!win->zone_item) {
		win->zone_item = xx_zone_manager_v1_get_zone_item(win->ctx->zone_manager, win->xdg_toplevel);
		xx_zone_v1_add_item(zone, win->zone_item);
	}
	xx_zone_item_v1_set_position(win->zone_item, x, y);
	wl_surface_commit(win->surface);
	assert(wl_display_roundtrip(win->ctx->display) >= 0);
}
static void scene_pump(struct observer *bridge, struct wex_context *native)
{
	pump(bridge);
	assert(wl_display_roundtrip(native->display) >= 0);
	/* Native requests can publish events to the other connection. */
	pump(bridge);
}
static void scene_click(struct observer *bridge, struct wex_context *native,
		struct zwlr_virtual_pointer_v1 *pointer, int x, int y)
{
	zwlr_virtual_pointer_v1_motion_absolute(pointer, 0, x, y, 1280, 800);
	zwlr_virtual_pointer_v1_frame(pointer);
	scene_pump(bridge, native);
	zwlr_virtual_pointer_v1_button(pointer, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(pointer);
	scene_pump(bridge, native);
	zwlr_virtual_pointer_v1_button(pointer, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
	scene_pump(bridge, native);
}
static struct zwp_virtual_keyboard_v1 *scene_keyboard(struct observer *bridge)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	assert(ctx);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	assert(keymap);
	char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	assert(text);
	size_t size = strlen(text) + 1;
	int fd = memfd_create("wex-focus-keymap", MFD_CLOEXEC);
	assert(fd >= 0 && write(fd, text, size) == (ssize_t)size);
	struct zwp_virtual_keyboard_v1 *keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		bridge->ctx.virtual_keyboard_manager, bridge->ctx.seat);
	assert(keyboard);
	zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	free(text);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	pump(bridge);
	return keyboard;
}

static void scene_key(struct observer *bridge, struct wex_context *native,
		struct zwp_virtual_keyboard_v1 *keyboard, bool to_native)
{
	unsigned native_count = native->key_event_count, frame_count = bridge->ctx.key_event_count;
	zwp_virtual_keyboard_v1_key(keyboard, 1, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(keyboard, 2, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
	scene_pump(bridge, native);
	assert(native->key_event_count == native_count + (to_native ? 2 : 0));
	assert(bridge->ctx.key_event_count == frame_count + (to_native ? 0 : 2));
	struct wex_context *recipient = to_native ? native : &bridge->ctx;
	assert(recipient->last_key == KEY_A && recipient->last_key_state == WL_KEYBOARD_KEY_STATE_RELEASED);
}

static int scene_test(void)
{
	struct wex_context native;
	struct wex_window_obj background, target, frame;
	struct observer owner = {.protocol_version = 4}, bridge = {.protocol_version = 8};
	struct frame_status status = {0};
	assert(wex_context_init(&native, "wayland-0"));
	assert(wex_window_init(&background, &native, "L2 scene background", 500, 400, 0xff00ff00));
	wl_surface_commit(background.surface);
	assert(wl_display_roundtrip(native.display) >= 0);
	wex_window_commit_buffer(&background);
	assert(wl_display_roundtrip(native.display) >= 0);
	struct xx_zone_v1 *native_zone = xx_zone_manager_v1_get_zone(native.zone_manager, NULL);
	position(&background, native_zone, 70, 70);
	map(&target, &native, "L2 scene target", true);
	uint32_t *pixels = target.buffer.data;
	for (int y = 0; y < 180; ++y) {
		for (int x = 0; x < 240; ++x) pixels[y * 240 + x] =
			(x >= 104 && x < 136 && y >= 74 && y < 106) ? 0 : 0x80000080;
	}
	struct wl_region *input = wl_compositor_create_region(native.compositor);
	wl_region_add(input, 0, 0, 240, 180);
	wl_region_subtract(input, 104, 74, 32, 32);
	wl_surface_set_input_region(target.surface, input);
	wl_region_destroy(input);
	wex_window_commit_buffer(&target);
	assert(wl_display_roundtrip(native.display) >= 0);
	observe(&owner);
	observe(&bridge);
	struct item *item = find(&owner, "L2 scene target");
	assert(wex_window_init(&frame, &bridge.ctx, "L2 scene frame", 300, 240, 0xffff0000));
	frame.wex_win = wex_window_manager_v1_get_window(bridge.manager, frame.xdg_toplevel);
	wex_window_v1_add_listener(frame.wex_win, &frame_listener, &status);
	request_association(&bridge, &frame, &status, item->token, item->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 240, 180);
	pump(&bridge);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_NOT_READY);
	wl_surface_commit(frame.surface);
	pump(&bridge);
	wex_window_commit_buffer(&frame);
	pump(&bridge);
	struct xx_zone_v1 *frame_zone = xx_zone_manager_v1_get_zone(bridge.ctx.zone_manager, NULL);
	position(&frame, frame_zone, 100, 100);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	wex_window_v1_configure_foreign(frame.wex_win, 10, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.configured_serial == 10 && status.configured_status == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	pump(&owner);
	assert(item->decoration_downgrades == 0);
	wex_window_v1_uncommit_foreign(frame.wex_win, 11);
	scene_pump(&bridge, &native);
	assert(status.configured_serial == 11 && status.configured_status == WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
	assert(status.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_v1_uncommit_foreign(frame.wex_win, 12);
	pump(&bridge);
	assert(status.configured_serial == 12 && status.configured_status == WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
	wex_window_v1_configure_foreign(frame.wex_win, 13, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.configured_serial == 13 && status.configured_status == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	puts("PASS numbered configure/uncommit replies, idempotent detach and retained reservation");
	wex_window_commit_buffer(&target);
	scene_pump(&bridge, &native);
	capture_scene("scene-initial");
	wex_window_v1_commit_foreign(frame.wex_win, -1, 40, 240, 180);
	pump(&bridge);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_NOT_READY);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 240, 180);
	pump(&bridge);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	struct zwlr_virtual_pointer_v1 *pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
		bridge.ctx.virtual_pointer_manager, bridge.ctx.seat);
	scene_click(&bridge, &native, pointer, 105, 105);
	assert(bridge.ctx.pointer_focus_surface == frame.surface);
	scene_click(&bridge, &native, pointer, 390, 150);
	assert(bridge.ctx.pointer_focus_surface == frame.surface);
	assert(bridge.ctx.pointer_x == 290 && bridge.ctx.pointer_y == 50);
	scene_click(&bridge, &native, pointer, 150, 180);
	assert(native.pointer_focus_surface == target.surface);
	scene_click(&bridge, &native, pointer, 240, 230);
	assert(native.pointer_focus_surface == background.surface);
	wex_window_v1_activate(frame.wex_win);
	scene_pump(&bridge, &native);
	assert(native.keyboard_focus_surface == target.surface && !bridge.ctx.keyboard_focus_surface);
	puts("PASS frame-ring/native/input-hole routing and physical native activation");
	struct zwp_virtual_keyboard_v1 *keyboard = scene_keyboard(&bridge);
	wex_window_v1_set_foreign_focus(frame.wex_win, 42, WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	scene_pump(&bridge, &native);
	assert(status.epoch == 42 && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	scene_key(&bridge, &native, keyboard, true);
	wex_window_v1_set_foreign_focus(frame.wex_win, 43, WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	scene_pump(&bridge, &native);
	assert(status.epoch == 43 && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	assert(!native.keyboard_focus_surface && bridge.ctx.keyboard_focus_surface == frame.surface);
	scene_key(&bridge, &native, keyboard, false);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 42, 1);
	scene_pump(&bridge, &native);
	assert(!status.captured && status.capture_epoch == 42);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 43, 1);
	scene_pump(&bridge, &native);
	assert(status.captured && status.capture_epoch == 43);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 42, 0);
	scene_pump(&bridge, &native);
	unsigned buttons_before_capture = native.button_event_count;
	scene_click(&bridge, &native, pointer, 150, 180);
	assert(bridge.ctx.pointer_focus_surface == frame.surface);
	assert(bridge.ctx.pointer_x == 50 && bridge.ctx.pointer_y == 80);
	assert(native.button_event_count == buttons_before_capture);
	wex_window_v1_set_foreign_focus(frame.wex_win, 44, WEX_WINDOW_V1_FOCUS_TARGET_RESTORE_NATIVE);
	scene_pump(&bridge, &native);
	assert(!status.captured && status.capture_epoch == 43);
	assert(status.epoch == 44 && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	assert(native.keyboard_focus_surface == target.surface && !bridge.ctx.keyboard_focus_surface);
	scene_key(&bridge, &native, keyboard, true);
	unsigned focus_replies = status.focus_replies;
	scene_click(&bridge, &native, pointer, 240, 230);
	assert(native.keyboard_focus_surface == background.surface);
	assert(status.focus_replies > focus_replies && status.epoch == 44);
	assert(status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	wex_window_v1_set_foreign_focus(frame.wex_win, 45, WEX_WINDOW_V1_FOCUS_TARGET_RESTORE_NATIVE);
	scene_pump(&bridge, &native);
	assert(status.epoch == 45 && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	assert(native.keyboard_focus_surface == background.surface);
	wex_window_v1_set_foreign_focus(frame.wex_win, 46, WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	scene_pump(&bridge, &native);
	assert(status.epoch == 46 && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	assert(!bridge.ctx.keyboard_focus_surface);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 46, 1);
	scene_pump(&bridge, &native);
	assert(!status.captured && native.keyboard_focus_surface == background.surface);
	wex_window_v1_set_foreign_focus(frame.wex_win, 47, WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	scene_pump(&bridge, &native);
	assert(status.epoch == 47 && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	assert(native.keyboard_focus_surface == target.surface && !bridge.ctx.keyboard_focus_surface);
	puts("PASS focus epochs, temporary frame focus, native restoration and stale-menu focus-steal guards");
	position(&frame, frame_zone, 200, 150);
	scene_pump(&bridge, &native);
	wex_window_commit_buffer(&target);
	scene_pump(&bridge, &native);
	capture_scene("scene-moved");
	zwlr_foreign_toplevel_handle_v1_set_minimized(item->handle);
	pump(&owner);
	assert(item->states & (1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED));
	scene_pump(&bridge, &native);
	assert(status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	capture_scene("scene-minimized");
	/* A focus completion racing a native minimize must not restore the group.
	 * Both an old and a new client epoch require an explicit state transition. */
	wex_window_v1_set_foreign_focus(frame.wex_win, 47, WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	scene_pump(&bridge, &native);
	assert(status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	wex_window_v1_set_foreign_focus(frame.wex_win, 48, WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	scene_pump(&bridge, &native);
	pump(&owner);
	assert(item->states & (1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED));
	assert(status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	puts("PASS delayed focus requests cannot restore a minimized native group");
	wex_window_v1_activate(frame.wex_win);
	scene_pump(&bridge, &native);
	assert(native.keyboard_focus_surface == target.surface);
	assert(status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	capture_scene("scene-restored");
	/* The original Wine subsurface tree must render above native content. */
	struct wl_surface *menu = wl_compositor_create_surface(bridge.ctx.compositor);
	struct wl_subsurface *sub = wl_subcompositor_get_subsurface(bridge.ctx.subcompositor, menu, frame.surface);
	struct wex_buffer menu_buffer;
	assert(wex_buffer_create(&bridge.ctx, &menu_buffer, 40, 30, 0xffffff00));
	wl_subsurface_set_position(sub, 60, 60);
	wl_surface_attach(menu, menu_buffer.wl_buffer, 0, 0);
	wl_surface_damage(menu, 0, 0, 40, 30);
	wl_surface_commit(menu);
	wl_surface_commit(frame.surface);
	scene_pump(&bridge, &native);
	capture_scene("scene-menu");
	wl_subsurface_destroy(sub);
	wl_surface_destroy(menu);
	wex_buffer_destroy(&menu_buffer);
	wl_surface_commit(frame.surface);
	scene_pump(&bridge, &native);
	/* Native geometry/policy changes cannot keep an obsolete frame cutout. */
	wex_window_v1_set_foreign_focus(frame.wex_win, 48, WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 48, 1);
	scene_pump(&bridge, &native);
	assert(status.captured);
	struct wex_buffer original_buffer = target.buffer;
	assert(wex_buffer_create(&native, &target.buffer, 239, 180, 0x80000080));
	xdg_surface_set_window_geometry(target.xdg_surface, 0, 0, 239, 180);
	wex_window_commit_buffer(&target);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
	assert(!status.captured && status.capture_epoch == 48);
	assert(status.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	struct wex_buffer resized_buffer = target.buffer;
	target.buffer = original_buffer;
	xdg_surface_set_window_geometry(target.xdg_surface, 0, 0, 240, 180);
	wex_window_commit_buffer(&target);
	scene_pump(&bridge, &native);
	wex_buffer_destroy(&resized_buffer);
	wex_window_v1_configure_foreign(frame.wex_win, 20, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.configured_serial == 20 && status.configured_status == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	zxdg_toplevel_decoration_v1_set_mode(target.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
	wex_window_v1_configure_foreign(frame.wex_win, 21, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.configured_serial == 21 && status.configured_status == WEX_WINDOW_V1_GROUP_STATUS_NOT_READY);
	zxdg_toplevel_decoration_v1_set_mode(target.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	wex_window_v1_configure_foreign(frame.wex_win, 22, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.configured_serial == 22 && status.configured_status == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	puts("PASS native geometry/policy changes detach safely and allow explicit matching reattachment");
	/* Lose only the controller; the native app and its source buffers survive. */
	wl_display_disconnect(owner.ctx.display);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
	assert(status.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	wex_window_v1_set_foreign_focus(frame.wex_win, 48, WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	scene_pump(&bridge, &native);
	assert(status.epoch == 48 && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NONE);
	struct item *remaining = find(&bridge, "L2 scene target");
	wex_foreign_window_v1_refresh(remaining->foreign);
	pump(&bridge);
	assert(remaining->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER);
	capture_scene("scene-detached");
	puts("PASS grouped movement/minimize/restore, Wine menu preservation and controller-loss native decoration recovery");
	/* Abrupt proxy connection loss must leave the native application usable. */
	request_association(&bridge, &frame, &status, remaining->token, remaining->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	wl_display_disconnect(bridge.ctx.display);
	assert(wl_display_roundtrip(native.display) >= 0);
	memset(&owner, 0, sizeof(owner));
	owner.protocol_version = 4;
	observe(&owner);
	item = find(&owner, "L2 scene target");
	assert(item && !item->closed && item->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER);
	/* Recreate an independent frame, then crash only the native connection. */
	memset(&bridge, 0, sizeof(bridge));
	bridge.protocol_version = 7;
	observe(&bridge);
	assert(wex_window_init(&frame, &bridge.ctx, "L2 recovery frame", 300, 240, 0xffff0000));
	frame.wex_win = wex_window_manager_v1_get_window(bridge.manager, frame.xdg_toplevel);
	wex_window_v1_add_listener(frame.wex_win, &frame_listener, &status);
	request_association(&bridge, &frame, &status, item->token, item->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wl_surface_commit(frame.surface);
	pump(&bridge);
	wex_window_commit_buffer(&frame);
	pump(&bridge);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	wl_display_disconnect(native.display);
	pump(&owner);
	pump(&bridge);
	assert(item->closed && item->foreign_closed);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
	assert(status.status == WEX_WINDOW_V1_ASSOCIATION_STATUS_UNBOUND);
	puts("PASS abrupt frame and native connection loss without compositor failure");
	wex_window_finish(&frame);
	pump(&bridge);
	wl_display_disconnect(owner.ctx.display);
	wl_display_disconnect(bridge.ctx.display);
	puts("FOREIGN_SCENE_PASSED");
	return 0;
}

static struct resize_reply *
resize_find(struct frame_status *status, uint32_t serial, uint32_t stage)
{
	for (unsigned i = 0; i < status->resize_count; ++i)
		if (status->resize[i].serial == serial && status->resize[i].stage == stage)
			return &status->resize[i];
	return NULL;
}

static void
resize_buffer(struct wex_window_obj *window, int width, int height, uint32_t color)
{
	struct wex_buffer old = window->buffer;
	assert(wex_buffer_create(window->ctx, &window->buffer, width, height, color));
	window->width = width;
	window->height = height;
	xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, width, height);
	wex_window_commit_buffer(window);
	assert(wl_display_roundtrip(window->ctx->display) >= 0);
	wex_buffer_destroy(&old);
}

static void
resize_ack_buffer(struct wex_window_obj *target)
{
	xdg_surface_ack_configure(target->xdg_surface, target->configure_serial);
	wex_window_commit_buffer(target);
	assert(wl_display_roundtrip(target->ctx->display) >= 0);
}

static void
resize_wait(struct observer *bridge, struct wex_context *native,
		struct frame_status *status, uint32_t serial, uint32_t stage)
{
	/* Poll a bounded condition rather than assuming timers ran by one exact
	 * sleep deadline while another VM build may be consuming the CPU. */
	for (unsigned i = 0; i < 120 && !resize_find(status, serial, stage); ++i) {
		usleep(50000);
		scene_pump(bridge, native);
	}
	assert(resize_find(status, serial, stage));
}

static int
resize_test(void)
{
	struct wex_context native;
	struct wex_window_obj target, frame;
	struct observer owner = {.protocol_version = 9}, bridge = {.protocol_version = 10};
	struct frame_status status = {0};
	assert(wex_context_init(&native, "wayland-0"));
	map(&target, &native, "L2 resize target", true);
	observe(&owner);
	observe(&bridge);
	struct item *item = find(&owner, "L2 resize target");
	assert(wex_window_init(&frame, &bridge.ctx, "L2 resize frame", 300, 240, 0xffff0000));
	frame.wex_win = wex_window_manager_v1_get_window(bridge.manager, frame.xdg_toplevel);
	wex_window_v1_add_listener(frame.wex_win, &frame_listener, &status);
	wl_surface_commit(frame.surface);
	pump(&bridge);
	wex_window_commit_buffer(&frame);
	pump(&bridge);
	struct xx_zone_v1 *zone = xx_zone_manager_v1_get_zone(bridge.ctx.zone_manager, NULL);
	position(&frame, zone, 100, 100);
	request_association(&bridge, &frame, &status, item->token, item->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	wex_window_v1_resize_foreign(frame.wex_win, 1, 5, 80, 90, 400, 300);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 1, WEX_WINDOW_V1_RESIZE_STAGE_UNAVAILABLE));
	wex_window_v1_activate(frame.wex_win);
	scene_pump(&bridge, &native);
	wex_window_v1_set_foreign_focus(frame.wex_win, 100, WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 100, 1);
	scene_pump(&bridge, &native);
	assert(status.captured && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	xdg_toplevel_set_max_size(target.xdg_toplevel, 320, 240);
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	target.defer_configure = true;
	wex_window_v1_resize_foreign(frame.wex_win, 2, 5, 80, 90, 400, 300);
	scene_pump(&bridge, &native);
	struct resize_reply *reply = resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_ACCEPTED);
	assert(reply && reply->x == 160 && reply->y == 150 && reply->width == 320 && reply->height == 240);
	assert(reply->configure_serial == target.configure_serial);
	assert(!resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_ACKNOWLEDGED));
	wex_window_v1_commit_foreign_resize(frame.wex_win, 2, 20, 40, 240, 180);
	pump(&bridge);
	assert(resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_UNAVAILABLE));
	xdg_surface_ack_configure(target.xdg_surface, target.configure_serial);
	assert(wl_display_roundtrip(native.display) >= 0);
	pump(&bridge);
	assert(resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_ACKNOWLEDGED));
	assert(!resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_NATIVE_READY));
	/* Committing only the acknowledged xdg state is still not a content
	 * response. Neither a new buffer nor damage/viewport state was supplied. */
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	assert(!resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_NATIVE_READY));
	/* The native client chooses a different size after acknowledging. */
	resize_buffer(&target, 310, 230, 0xff0000ff);
	scene_pump(&bridge, &native);
	reply = resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_NATIVE_READY);
	assert(reply && reply->x == 170 && reply->y == 160 && reply->width == 310 && reply->height == 230);
	resize_buffer(&frame, 370, 290, 0xffffff00);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	capture_scene("resize-held");
	wex_window_v1_commit_foreign_resize(frame.wex_win, 2, 21, 40, 310, 230);
	pump(&bridge);
	assert(!resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_COMPLETE));
	wex_window_v1_commit_foreign_resize(frame.wex_win, 2, 20, 40, 310, 230);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 2, WEX_WINDOW_V1_RESIZE_STAGE_COMPLETE));
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE && status.captured);
	capture_scene("resize-complete");
	puts("PASS constrained left/top anchors, delayed ack/buffer, client-chosen size, retained roots and atomic matching frame");

	/* A new intent must not be completed by the superseded configure's ack. */
	xdg_toplevel_set_max_size(target.xdg_toplevel, 0, 0);
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	wex_window_v1_resize_foreign(frame.wex_win, 3, 10, 170, 160, 330, 250);
	scene_pump(&bridge, &native);
	uint32_t old_configure = target.configure_serial;
	wex_window_v1_resize_foreign(frame.wex_win, 4, 10, 170, 160, 350, 270);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 3, WEX_WINDOW_V1_RESIZE_STAGE_SUPERSEDED));
	xdg_surface_ack_configure(target.xdg_surface, old_configure);
	wex_window_commit_buffer(&target);
	scene_pump(&bridge, &native);
	assert(!resize_find(&status, 4, WEX_WINDOW_V1_RESIZE_STAGE_ACKNOWLEDGED));
	assert(!resize_find(&status, 4, WEX_WINDOW_V1_RESIZE_STAGE_NATIVE_READY));
	wex_window_v1_cancel_foreign_resize(frame.wex_win, 3);
	wex_window_v1_commit_foreign_resize(frame.wex_win, 3, 20, 40, 310, 230);
	pump(&bridge);
	assert(resize_find(&status, 3, WEX_WINDOW_V1_RESIZE_STAGE_STALE));
	wex_window_v1_cancel_foreign_resize(frame.wex_win, 4);
	scene_pump(&bridge, &native);
	reply = resize_find(&status, 4, WEX_WINDOW_V1_RESIZE_STAGE_ROLLING_BACK);
	assert(reply && reply->x == 170 && reply->y == 160 && reply->width == 310 && reply->height == 230);
	resize_ack_buffer(&target);
	scene_pump(&bridge, &native);
	wex_window_v1_commit_foreign_resize(frame.wex_win, 4, 20, 40, 310, 230);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 4, WEX_WINDOW_V1_RESIZE_STAGE_CANCELLED));
	puts("PASS superseded configure and stale cancel/commit cannot override current rollback");

	/* Fixed clients and independent per-axis limits are read fresh. */
	xdg_toplevel_set_min_size(target.xdg_toplevel, 310, 230);
	xdg_toplevel_set_max_size(target.xdg_toplevel, 310, 230);
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	wex_window_v1_resize_foreign(frame.wex_win, 5, 10, 170, 160, 500, 400);
	scene_pump(&bridge, &native);
	reply = resize_find(&status, 5, WEX_WINDOW_V1_RESIZE_STAGE_ACCEPTED);
	assert(reply && reply->width == 310 && reply->height == 230);
	resize_ack_buffer(&target);
	scene_pump(&bridge, &native);
	wex_window_v1_commit_foreign_resize(frame.wex_win, 5, 20, 40, 310, 230);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 5, WEX_WINDOW_V1_RESIZE_STAGE_COMPLETE));
	xdg_toplevel_set_min_size(target.xdg_toplevel, 120, 90);
	xdg_toplevel_set_max_size(target.xdg_toplevel, 0, 240);
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	wex_window_v1_resize_foreign(frame.wex_win, 6, 10, 170, 160, 350, 400);
	scene_pump(&bridge, &native);
	reply = resize_find(&status, 6, WEX_WINDOW_V1_RESIZE_STAGE_ACCEPTED);
	assert(reply && reply->width == 350 && reply->height == 240);
	/* Ignore the first configure: timeout must request coherent rollback. */
	resize_wait(&bridge, &native, &status, 6, WEX_WINDOW_V1_RESIZE_STAGE_ROLLING_BACK);
	assert(resize_find(&status, 6, WEX_WINDOW_V1_RESIZE_STAGE_ROLLING_BACK));
	resize_ack_buffer(&target);
	scene_pump(&bridge, &native);
	wex_window_v1_commit_foreign_resize(frame.wex_win, 6, 20, 40, 310, 230);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 6, WEX_WINDOW_V1_RESIZE_STAGE_CANCELLED));
	puts("PASS fixed/one-axis limits and ignored configure bounded rollback");

	wex_window_v1_resize_foreign(frame.wex_win, 7, 10, 170, 160, 350, 240);
	scene_pump(&bridge, &native);
	wex_window_v1_set_foreign_focus(frame.wex_win, 101, WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 7, WEX_WINDOW_V1_RESIZE_STAGE_ROLLING_BACK));
	assert(!status.captured);
	resize_ack_buffer(&target);
	scene_pump(&bridge, &native);
	wex_window_v1_commit_foreign_resize(frame.wex_win, 7, 20, 40, 310, 230);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 7, WEX_WINDOW_V1_RESIZE_STAGE_CANCELLED));
	assert(status.focus == WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
	wex_window_v1_set_foreign_focus(frame.wex_win, 102, WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 102, 1);
	scene_pump(&bridge, &native);
	wex_window_v1_resize_foreign(frame.wex_win, 8, 10, 170, 160, 350, 240);
	scene_pump(&bridge, &native);
	resize_wait(&bridge, &native, &status, 8, WEX_WINDOW_V1_RESIZE_STAGE_FAILED);
	assert(resize_find(&status, 8, WEX_WINDOW_V1_RESIZE_STAGE_FAILED));
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_DETACHED && !status.captured);
	puts("PASS focus loss cancels without stealing focus; ignored rollback safely detaches and releases capture");

	/* Recover after the explicit safety detach, then exercise each edge and
	 * corner. The controller serial survives detach/re-adoption. */
	resize_ack_buffer(&target);
	xdg_toplevel_set_max_size(target.xdg_toplevel, 0, 0);
	wl_surface_commit(target.surface);
	scene_pump(&bridge, &native);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 310, 230);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	wex_window_v1_activate(frame.wex_win);
	scene_pump(&bridge, &native);
	wex_window_v1_set_foreign_focus(frame.wex_win, 103, WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 103, 1);
	scene_pump(&bridge, &native);
	assert(status.captured);
	unsigned edges[] = {1, 2, 4, 8, 5, 9, 6, 10};
	int x = 170, y = 160, width = 310, height = 230;
	for (unsigned i = 0; i < 8; ++i) {
		int w = width + ((edges[i] & 12) ? 2 : 0);
		int h = height + ((edges[i] & 3) ? 2 : 0);
		if (edges[i] & 4) x -= 2;
		if (edges[i] & 1) y -= 2;
		wex_window_v1_resize_foreign(frame.wex_win, 20 + i, edges[i], x, y, w, h);
		scene_pump(&bridge, &native);
		reply = resize_find(&status, 20 + i, WEX_WINDOW_V1_RESIZE_STAGE_ACCEPTED);
		assert(reply && reply->x == x && reply->y == y && reply->width == w && reply->height == h);
		/* Frame-first as well as native-first commits must keep old pixels. */
		resize_buffer(&frame, w + 60, h + 60, 0xff00ffff);
		scene_pump(&bridge, &native);
		if (!i) capture_scene("resize-frame-first");
		xdg_surface_ack_configure(target.xdg_surface, target.configure_serial);
		resize_buffer(&target, w, h, 0xff0000ff);
		scene_pump(&bridge, &native);
		assert(resize_find(&status, 20 + i, WEX_WINDOW_V1_RESIZE_STAGE_NATIVE_READY));
		wex_window_v1_commit_foreign_resize(frame.wex_win, 20 + i, 20, 40, w, h);
		scene_pump(&bridge, &native);
		assert(resize_find(&status, 20 + i, WEX_WINDOW_V1_RESIZE_STAGE_COMPLETE));
		struct item *actual = find(&bridge, "L2 resize target");
		assert(actual->x == x && actual->y == y && actual->width == w && actual->height == h);
		assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
		width = w;
		height = h;
	}
	puts("PASS all eight edges/corners, frame-first commits and one-pixel-accurate native placement");
	wex_window_v1_resize_foreign(frame.wex_win, 30, 10, x, y, width + 10, height + 10);
	scene_pump(&bridge, &native);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 103, 0);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 30, WEX_WINDOW_V1_RESIZE_STAGE_ROLLING_BACK));
	resize_ack_buffer(&target);
	scene_pump(&bridge, &native);
	wex_window_v1_commit_foreign_resize(frame.wex_win, 30, 20, 40, width, height);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 30, WEX_WINDOW_V1_RESIZE_STAGE_CANCELLED));
	assert(!status.captured && status.focus == WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 103, 1);
	wex_window_v1_resize_foreign(frame.wex_win, 31, 10, x, y, width + 10, height + 10);
	scene_pump(&bridge, &native);
	wex_window_finish(&target);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 31, WEX_WINDOW_V1_RESIZE_STAGE_FAILED));
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_DETACHED && !status.captured);
	puts("PASS explicit capture release rolls back; native teardown releases retained buffers and listeners");
	map(&target, &native, "L2 resize replacement", true);
	pump(&owner);
	item = find(&owner, "L2 resize replacement");
	resize_buffer(&frame, 300, 240, 0xffff0000);
	request_association(&bridge, &frame, &status, item->token, item->controller,
		WEX_WINDOW_V1_ASSOCIATION_STATUS_PREPARED);
	wex_window_v1_commit_foreign(frame.wex_win, 20, 40, 240, 180);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE);
	wex_window_v1_activate(frame.wex_win);
	scene_pump(&bridge, &native);
	wex_window_v1_set_foreign_focus(frame.wex_win, 104, WEX_WINDOW_V1_FOCUS_TARGET_FRAME);
	wex_window_v1_capture_foreign_pointer(frame.wex_win, 104, 1);
	target.defer_configure = true;
	wex_window_v1_resize_foreign(frame.wex_win, 32, 10, 170, 160, 260, 200);
	scene_pump(&bridge, &native);
	assert(resize_find(&status, 32, WEX_WINDOW_V1_RESIZE_STAGE_ACCEPTED));
	wex_window_finish(&frame);
	scene_pump(&bridge, &native);
	pump(&owner);
	assert(!item->closed && item->mode == WEX_FOREIGN_WINDOW_V1_DECORATION_SERVER);
	wex_window_finish(&target);
	scene_pump(&bridge, &native);
	puts("PASS frame destruction during negotiation restores ordinary native decoration and lifetime");
	xx_zone_v1_destroy(zone);
	wex_context_finish(&native);
	wl_display_disconnect(owner.ctx.display);
	wl_display_disconnect(bridge.ctx.display);
	puts("FOREIGN_RESIZE_PASSED");
	return 0;
}
