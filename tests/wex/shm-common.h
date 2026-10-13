/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef WEX_SHM_COMMON_H
#define WEX_SHM_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

struct wex_context {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct zxdg_decoration_manager_v1 *deco_manager;

	struct xx_zone_manager_v1 *zone_manager;
	struct wex_window_manager_v1 *wex_window_manager;
	struct wp_alpha_modifier_v1 *alpha_modifier;
	struct wp_pointer_warp_v1 *pointer_warp;

	uint32_t last_enter_serial;
	int pointer_x;
	int pointer_y;
	bool pointer_entered;
	bool pointer_motion_received;
};

struct wex_buffer {
	struct wl_buffer *wl_buffer;
	void *data;
	int width;
	int height;
	int stride;
	size_t size;
};

struct wex_window_obj {
	struct wex_context *ctx;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zxdg_toplevel_decoration_v1 *deco;

	struct xx_zone_item_v1 *zone_item;
	struct wex_window_v1 *wex_win;

	struct wex_buffer buffer;
	int width;
	int height;
	uint32_t color;

	bool configured;
	bool closed;
	int configured_width;
	int configured_height;
};

bool wex_context_init(struct wex_context *ctx, const char *socket_name);
void wex_context_finish(struct wex_context *ctx);

bool wex_buffer_create(struct wex_context *ctx, struct wex_buffer *buf,
	int width, int height, uint32_t color);
void wex_buffer_destroy(struct wex_buffer *buf);

bool wex_window_init(struct wex_window_obj *win, struct wex_context *ctx,
	const char *title, int width, int height, uint32_t color);
void wex_window_finish(struct wex_window_obj *win);
void wex_window_commit_buffer(struct wex_window_obj *win);

#endif /* WEX_SHM_COMMON_H */
