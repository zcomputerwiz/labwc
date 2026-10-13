/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PROTOCOLS_POINTER_WARP_H
#define LABWC_PROTOCOLS_POINTER_WARP_H

#include <wayland-server-core.h>

struct server;

struct lab_pointer_warp_manager {
	struct wl_global *global;
	struct server *server;
	struct wl_listener display_destroy;
};

void lab_pointer_warp_manager_init(struct server *server);
void lab_pointer_warp_manager_finish(struct server *server);

#endif /* LABWC_PROTOCOLS_POINTER_WARP_H */
