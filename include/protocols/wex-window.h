/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PROTOCOLS_WEX_WINDOW_H
#define LABWC_PROTOCOLS_WEX_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "wex-window-v1-protocol.h"

struct server;
struct view;
struct wex_foreign_window;
struct wex_scene;
struct wlr_box;

struct wex_window_manager {
	struct wl_global *global;
	struct server *server;
	struct wl_list windows;    /* struct wex_window.link */
	struct wl_list foreign_windows; /* supplemental observers */
	struct wl_listener display_destroy;
	struct wex_window *pointer_capture;
};

struct wex_window {
	struct wl_list link;       /* wex_window_manager.windows */
	struct wex_window_manager *manager;
	struct view *view;
	struct wl_resource *resource;
	struct wex_foreign_window *foreign;
	struct wex_scene *scene;
	char handle[33];
	uint32_t layer;
	uint32_t exclusive_edge;
	uint32_t exclusive_size;
	uint32_t focus_epoch;
	uint32_t capture_epoch;
	uint32_t resize_serial;
};

void wex_window_manager_init(struct server *server);
void wex_window_manager_finish(struct server *server);
void wex_window_on_view_destroy(struct wex_window *win);
void wex_foreign_window_update(struct view *view);
bool wex_client_is_privileged(const struct wl_client *client, struct server *server);
struct wex_scene *wex_scene_create(struct wex_window *frame, struct view *native,
	const struct wlr_box *client);
void wex_scene_destroy(struct wex_scene *scene);
void wex_scene_view_moved(struct view *view);
struct view *wex_scene_group_root(struct view *view);
struct view *wex_scene_native(struct wex_scene *scene);
bool wex_scene_matches(struct wex_scene *scene, const struct wlr_box *client);
void wex_window_drop_scene(struct wex_window *frame);
void wex_scene_view_minimized(struct view *view, bool minimized);
bool wex_scene_server_decorated(struct wex_scene *scene);
uint32_t wex_scene_focus_target(struct wex_scene *scene);
void wex_window_send_foreign_focus(struct wex_window *frame);
void wex_window_release_pointer_capture(struct wex_window *frame);
void wex_scene_resize(struct wex_window *frame, uint32_t serial, uint32_t edges,
	const struct wlr_box *desired);
void wex_scene_commit_resize(struct wex_window *frame, uint32_t serial,
	const struct wlr_box *client);
void wex_scene_cancel_resize(struct wex_window *frame, uint32_t serial);
void wex_scene_capture_lost(struct wex_scene *scene);

#endif /* LABWC_PROTOCOLS_WEX_WINDOW_H */
