/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PROTOCOLS_WEX_WINDOW_H
#define LABWC_PROTOCOLS_WEX_WINDOW_H

#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "wex-window-v1-protocol.h"

struct server;
struct view;
struct wex_foreign_window;
struct wex_scene;
struct wlr_box;
struct wlr_scene_tree;
struct wex_corner_piece;
struct wlr_surface;

struct wex_window_manager {
	struct wl_global *global;
	struct server *server;
	struct wl_list windows;    /* struct wex_window.link */
	struct wl_list foreign_windows; /* supplemental observers */
	struct wl_list render_surfaces; /* producer-owned cross-process render roles */
	struct wl_listener display_destroy;
	struct wex_window *pointer_capture;
	struct wl_list thumbnails;        /* struct wex_thumbnail_entry.link */
	struct wl_resource *thumbnail_resource; /* owner of the thumbnail list */
	struct wl_list manager_resources; /* struct wex_manager_bound.link */
	uint64_t frame_counter;
	bool peek_active;
	char peek_target[33];
	char peek_keep[64][33];
	size_t peek_keep_count;
	struct wl_resource *peek_resource;
	struct wl_client *peek_client;

	/* Shell keys (v15): combinations taken from native clients for the shell */
	struct wl_resource *shell_keys_resource; /* owner of the list */
	uint32_t shell_keys[32][2];              /* keysym, modifiers */
	size_t shell_keys_count;
	bool shell_keys_sending;                 /* keys go to the shell */
	bool shell_keys_held;                    /* hold_keyboard */
	bool shell_keys_start_down;
	uint32_t shell_keys_start;               /* the keycode that started the sending */
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
	uint32_t corner_radius;
	struct wlr_scene_tree *corner_tree;
	struct wl_listener surface_commit;
	struct wex_corner_piece *corner_pieces;
	size_t num_corner_pieces;

	/* DWM effects (v13) */
	uint32_t effects_backdrop;
	uint32_t effects_flags;
	uint32_t effects_tint;
	int32_t effects_left;
	int32_t effects_top;
	int32_t effects_right;
	int32_t effects_bottom;
	struct wlr_scene_tree *shadow_tree;
	struct wlr_scene_tree *backdrop_tree;

	/* Live thumbnails (v14) */
	struct wlr_scene_tree *thumbnail_tree;
};

void wex_window_apply_corner_radius(struct wex_window *win);
void wex_window_update_effects(struct wex_window *win);
void wex_window_drop_thumbnails(struct wex_window *win);
/* when: the display time of a present event, NULL for the time of the commit */
void wex_window_manager_on_output_commit(struct wex_window_manager *manager, const struct timespec *when);
void wex_window_manager_on_view_map(struct view *view);
void wex_window_manager_refresh_thumbnails(struct wex_window_manager *manager);

void wex_window_manager_init(struct server *server);
void wex_window_manager_finish(struct server *server);
void wex_window_on_view_destroy(struct wex_window *win);
void wex_foreign_window_update(struct view *view);
bool wex_client_is_privileged(const struct wl_client *client, struct server *server);
/* Shell keys (v15). keyboard.c, for each key while the seat is not locked: true when the key went to the shell, so
 * the focused client must not get it. keysym: without modifiers (level 1); modifiers: WLR_MODIFIER_* held. */
bool wex_window_manager_shell_key(struct wex_window_manager *manager, struct wlr_surface *focused,
	uint32_t keycode, uint32_t keysym, uint32_t modifiers, bool pressed);
bool wex_window_manager_has_shell_keys(struct wex_window_manager *manager);
struct wex_scene *wex_scene_create(struct wex_window *frame, struct view *native,
	const struct wlr_box *client);
void wex_scene_destroy(struct wex_scene *scene);
void wex_scene_view_moved(struct view *view);
struct view *wex_scene_group_root(struct view *view);
struct view *wex_scene_native(struct wex_scene *scene);
bool wex_scene_matches(struct wex_scene *scene, const struct wlr_box *client);
bool wex_scene_reconfigure(struct wex_window *frame, uint32_t serial, const struct wlr_box *client);
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
