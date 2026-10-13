/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PROTOCOLS_WEX_WINDOW_H
#define LABWC_PROTOCOLS_WEX_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "wex-window-v1-protocol.h"

struct server;
struct view;

struct wex_window_manager {
	struct wl_global *global;
	struct server *server;
	struct wl_list windows;    /* struct wex_window.link */
	struct wl_listener display_destroy;
};

struct wex_window {
	struct wl_list link;       /* wex_window_manager.windows */
	struct wex_window_manager *manager;
	struct view *view;
	struct wl_resource *resource;
	uint32_t layer;
	uint32_t exclusive_edge;
	uint32_t exclusive_size;
};

void wex_window_manager_init(struct server *server);
void wex_window_manager_finish(struct server *server);
void wex_window_on_view_destroy(struct wex_window *win);
bool wex_client_is_privileged(const struct wl_client *client, struct server *server);

#endif /* LABWC_PROTOCOLS_WEX_WINDOW_H */
