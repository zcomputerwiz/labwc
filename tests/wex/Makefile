# SPDX-License-Identifier: GPL-2.0-only

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -I.
LIBS = -lwayland-client -lrt -lxkbcommon

WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols
TOP_PROTOCOLS_DIR ?= ../../protocols

PROTO_GEN_C = \
	xx-zones-v1-protocol.c \
	wex-window-v1-protocol.c \
	wlr-foreign-toplevel-management-unstable-v1-protocol.c \
	pointer-warp-v1-protocol.c \
	alpha-modifier-v1-protocol.c \
	xdg-shell-protocol.c \
	xdg-decoration-unstable-v1-protocol.c \
	wlr-virtual-pointer-unstable-v1-protocol.c \
	virtual-keyboard-unstable-v1-protocol.c

PROTO_GEN_H = \
	xx-zones-v1-client-protocol.h \
	wex-window-v1-client-protocol.h \
	wlr-foreign-toplevel-management-unstable-v1-client-protocol.h \
	pointer-warp-v1-client-protocol.h \
	alpha-modifier-v1-client-protocol.h \
	xdg-shell-client-protocol.h \
	xdg-decoration-unstable-v1-client-protocol.h \
	wlr-virtual-pointer-unstable-v1-client-protocol.h \
	virtual-keyboard-unstable-v1-client-protocol.h

PROTO_OBJS = $(PROTO_GEN_C:.c=.o)
COMMON_OBJS = shm-common.o $(PROTO_OBJS)

TARGETS = zones-client stack-client alpha-client warp-client policy-client handle-client foreign-client

all: $(TARGETS)

$(TARGETS:=.o): shm-common.h

xx-zones-v1-client-protocol.h: $(TOP_PROTOCOLS_DIR)/xx-zones-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

xx-zones-v1-protocol.c: $(TOP_PROTOCOLS_DIR)/xx-zones-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

wex-window-v1-client-protocol.h: $(TOP_PROTOCOLS_DIR)/wex-window-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

wex-window-v1-protocol.c: $(TOP_PROTOCOLS_DIR)/wex-window-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

wlr-foreign-toplevel-management-unstable-v1-client-protocol.h: $(TOP_PROTOCOLS_DIR)/wlr-foreign-toplevel-management-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

wlr-foreign-toplevel-management-unstable-v1-protocol.c: $(TOP_PROTOCOLS_DIR)/wlr-foreign-toplevel-management-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

pointer-warp-v1-client-protocol.h: $(TOP_PROTOCOLS_DIR)/pointer-warp-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

pointer-warp-v1-protocol.c: $(TOP_PROTOCOLS_DIR)/pointer-warp-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

alpha-modifier-v1-client-protocol.h: $(WAYLAND_PROTOCOLS_DIR)/staging/alpha-modifier/alpha-modifier-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

alpha-modifier-v1-protocol.c: $(WAYLAND_PROTOCOLS_DIR)/staging/alpha-modifier/alpha-modifier-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

xdg-shell-client-protocol.h: $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) client-header $< $@

xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) private-code $< $@

xdg-decoration-unstable-v1-client-protocol.h: $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

xdg-decoration-unstable-v1-protocol.c: $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

shm-common.o: shm-common.c shm-common.h $(PROTO_GEN_H)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c $(PROTO_GEN_H)
	$(CC) $(CFLAGS) -c $< -o $@

zones-client: zones-client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

stack-client: stack-client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

handle-client: handle-client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -Wno-unused-parameter -o $@ $^ $(LIBS)

foreign-client: foreign-client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) -lxcb-icccm -lxcb

alpha-client: alpha-client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

wlr-virtual-pointer-unstable-v1-client-protocol.h: $(TOP_PROTOCOLS_DIR)/wlr-virtual-pointer-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

wlr-virtual-pointer-unstable-v1-protocol.c: $(TOP_PROTOCOLS_DIR)/wlr-virtual-pointer-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

virtual-keyboard-unstable-v1-client-protocol.h: $(TOP_PROTOCOLS_DIR)/virtual-keyboard-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

virtual-keyboard-unstable-v1-protocol.c: $(TOP_PROTOCOLS_DIR)/virtual-keyboard-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

warp-client: warp-client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

policy-client: policy-client.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(TARGETS) *.o $(PROTO_GEN_C) $(PROTO_GEN_H)

.PHONY: all clean
