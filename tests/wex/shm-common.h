/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef WEX_SHM_COMMON_H
#define WEX_SHM_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

struct wex_context {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_output *output;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct zxdg_decoration_manager_v1 *deco_manager;

	struct xx_zone_manager_v1 *zone_manager;
	struct wex_window_manager_v1 *wex_window_manager;
	struct wp_alpha_modifier_v1 *alpha_modifier;
	struct wp_pointer_warp_v1 *pointer_warp;
	struct zwlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
	struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;

	uint32_t last_enter_serial;
	int pointer_x;
	int pointer_y;
	bool pointer_entered;
	bool pointer_motion_received;
	struct wl_surface *pointer_focus_surface;
	struct wl_surface *keyboard_focus_surface;
	uint32_t last_key, last_key_state;
	unsigned key_event_count;
	uint32_t last_button;
	uint32_t last_button_state;
	int button_event_count;
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
	unsigned output_count;
	bool defer_configure;
	uint32_t configure_serial;
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
