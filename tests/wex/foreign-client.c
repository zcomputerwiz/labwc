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
#include "shm-common.h"
#include "wex-window-v1-client-protocol.h"
#include "xx-zones-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct item {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	struct wex_foreign_window_v1 *foreign;
	char title[128], token[33], controller[33];
	int x, y, width, height, min_width, min_height;
	unsigned mode, done, states, decoration_downgrades;
	bool closed, foreign_closed;
};
struct observer {
	struct wex_context ctx;
	struct wl_registry *registry;
	struct wex_window_manager_v1 *manager;
	struct zwlr_foreign_toplevel_manager_v1 *inventory;
	struct item items[16];
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
static void closed(void *data, struct wex_foreign_window_v1 *obj)
{
	((struct item *)data)->foreign_closed = true;
}
static const struct wex_foreign_window_v1_listener supplement = {
	.token = token, .geometry = geometry, .decoration = decoration,
	.size_hints = hints, .done = done, .closed = closed,
	.controller_handle = controller,
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
static int association_test(void);
static int publish_association(bool hosting);
static int scene_test(void);
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--association")) return association_test();
	if (argc == 2 && !strcmp(argv[1], "--publish-association")) return publish_association(false);
	if (argc == 2 && !strcmp(argv[1], "--publish-hosting")) return publish_association(true);
	if (argc == 2 && !strcmp(argv[1], "--scene")) return scene_test();
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

static int publish_association(bool hosting)
{
	struct wex_context native;
	struct wex_window_obj win;
	struct observer owner = {.protocol_version = 4};
	struct zwlr_virtual_pointer_v1 *pointer = NULL;
	assert(wex_context_init(&native, "wayland-0"));
	map(&win, &native, "L2 Wine association peer", hosting);
	observe(&owner);
	struct item *item = find(&owner, "L2 Wine association peer");
	printf("LEASE %s %s\n", item->token, item->controller);
	fflush(stdout);
	for (;;) {
		if (win.closed) break;
		struct pollfd fds[] = {
			{wl_display_get_fd(native.display), POLLIN, 0},
			{wl_display_get_fd(owner.ctx.display), POLLIN, 0},
			{0, POLLIN, 0},
		};
		assert(wl_display_dispatch_pending(native.display) >= 0);
		assert(wl_display_dispatch_pending(owner.ctx.display) >= 0);
		wl_display_flush(native.display);
		wl_display_flush(owner.ctx.display);
		if (poll(fds, 3, -1) < 0) return 1;
		if (fds[0].revents & POLLIN) assert(wl_display_dispatch(native.display) >= 0);
		if (fds[1].revents & POLLIN) assert(wl_display_dispatch(owner.ctx.display) >= 0);
		if (fds[2].revents & (POLLIN | POLLHUP)) {
			char command[32];
			if (!fgets(command, sizeof(command), stdin) || !strcmp(command,"quit\n")) break;
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
			if (!strcmp(command,"restore\n")) zwlr_foreign_toplevel_handle_v1_unset_minimized(item->handle);
			if (!strcmp(command,"state\n")) {
				pump(&owner);
				printf("STATE %u\n",!!(item->states & (1u << ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)));
				fflush(stdout);
			}
			int click_x, click_y;
			if (sscanf(command,"click %d %d",&click_x,&click_y) == 2) {
				if (!pointer) pointer=zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
					owner.ctx.virtual_pointer_manager,owner.ctx.seat);
				zwlr_virtual_pointer_v1_motion_absolute(pointer,0,click_x,click_y,1280,720);
				zwlr_virtual_pointer_v1_frame(pointer);
				zwlr_virtual_pointer_v1_button(pointer,0,BTN_LEFT,WL_POINTER_BUTTON_STATE_PRESSED);
				zwlr_virtual_pointer_v1_frame(pointer);
				zwlr_virtual_pointer_v1_button(pointer,0,BTN_LEFT,WL_POINTER_BUTTON_STATE_RELEASED);
				zwlr_virtual_pointer_v1_frame(pointer);
				pump(&owner);
				assert(wl_display_roundtrip(native.display)>=0);
				printf("CLICK %u %u %d\n",native.keyboard_focus_surface==win.surface,
					native.pointer_focus_surface==win.surface,native.button_event_count);
				fflush(stdout);
			}
		}
	}
	wex_window_finish(&win);
	wex_context_finish(&native);
	wl_display_disconnect(owner.ctx.display);
	return 0;
}

struct frame_status { unsigned status, replies, group, group_replies, focus, epoch, focus_replies, configured_serial, configured_status; };
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
static const struct wex_window_v1_listener frame_listener = {
	.handle = frame_handle, .foreign_association = association,
	.foreign_group = group,
	.foreign_focus = foreign_focus,
	.configured_foreign = configured_foreign,
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
	struct observer owner = {.protocol_version = 4}, bridge = {.protocol_version = 7};
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
	wex_window_v1_set_foreign_focus(frame.wex_win, 44, WEX_WINDOW_V1_FOCUS_TARGET_RESTORE_NATIVE);
	scene_pump(&bridge, &native);
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
	struct wex_buffer original_buffer = target.buffer;
	assert(wex_buffer_create(&native, &target.buffer, 239, 180, 0x80000080));
	xdg_surface_set_window_geometry(target.xdg_surface, 0, 0, 239, 180);
	wex_window_commit_buffer(&target);
	scene_pump(&bridge, &native);
	assert(status.group == WEX_WINDOW_V1_GROUP_STATUS_DETACHED);
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
