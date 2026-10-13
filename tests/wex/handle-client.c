/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include "shm-common.h"
#include "xx-zones-v1-client-protocol.h"
#include "wex-window-v1-client-protocol.h"

static char token[33];
static void got_handle(void *data, struct wex_window_v1 *window, const char *handle)
{
    snprintf(token, sizeof(token), "%s", handle);
}
static const struct wex_window_v1_listener listener = { got_handle };
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    struct wex_context *ctx = data;
    if (!strcmp(interface, "wex_window_manager_v1") && version >= 2)
    {
        wex_window_manager_v1_destroy(ctx->wex_window_manager);
        ctx->wex_window_manager = wl_registry_bind(registry, name,
                &wex_window_manager_v1_interface, 2);
    }
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { global, removed };

int main(int argc, char **argv)
{
    struct wex_context ctx;
    struct wex_window_obj win;
    if (argc != 2 || !wex_context_init(&ctx, "wex-shell-0")) return 1;
    struct wl_registry *registry = wl_display_get_registry(ctx.display);
    wl_registry_add_listener(registry, &registry_listener, &ctx);
    if (wl_display_roundtrip(ctx.display) < 0 ||
        wex_window_manager_v1_get_version(ctx.wex_window_manager) < 2) return 2;
    struct xx_zone_v1 *zone = xx_zone_manager_v1_get_zone(ctx.zone_manager, NULL);
    wl_display_roundtrip(ctx.display);
    wex_window_init(&win, &ctx, "Cross-process handle probe", 200, 200,
                    strtoul(argv[1], NULL, 16));
    win.wex_win = wex_window_manager_v1_get_window(ctx.wex_window_manager, win.xdg_toplevel);
    wex_window_v1_add_listener(win.wex_win, &listener, NULL);
    win.zone_item = xx_zone_manager_v1_get_zone_item(ctx.zone_manager, win.xdg_toplevel);
    xx_zone_v1_add_item(zone, win.zone_item);
    xx_zone_item_v1_set_position(win.zone_item, 100, 100);
    wl_surface_commit(win.surface);
    wl_display_roundtrip(ctx.display);
    wex_window_commit_buffer(&win);
    wl_display_roundtrip(ctx.display);
    if (strlen(token) != 32) return 3;
    printf("HANDLE %s\n", token);
    fflush(stdout);
    char *line = NULL;
    size_t size = 0;
    for (;;)
    {
        struct pollfd fds[] = { {wl_display_get_fd(ctx.display), POLLIN, 0}, {0, POLLIN, 0} };
        wl_display_flush(ctx.display);
        if (poll(fds, 2, -1) < 0) break;
        if ((fds[0].revents & POLLIN) && wl_display_dispatch(ctx.display) < 0) return 4;
        if (!(fds[1].revents & POLLIN)) continue;
        if (getline(&line, &size, stdin) < 0 || !strncmp(line, "quit", 4)) break;
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "above ", 6)) wex_window_v1_place_above_handle(win.wex_win, line + 6);
        else if (!strncmp(line, "below ", 6)) wex_window_v1_place_below_handle(win.wex_win, line + 6);
        else if (!strncmp(line, "layer ", 6)) wex_window_v1_set_layer(win.wex_win, atoi(line + 6));
        else return 5;
        if (wl_display_roundtrip(ctx.display) < 0) return 6;
        puts("ACK");
        fflush(stdout);
    }
    free(line);
    wex_window_finish(&win);
    xx_zone_v1_destroy(zone);
    wl_registry_destroy(registry);
    wex_context_finish(&ctx);
    return 0;
}
