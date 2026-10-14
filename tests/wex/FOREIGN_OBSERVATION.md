# Native supplemental observation

`foreign-client` runs against the test harness's private headless compositor.
It creates normal-socket SSD/CSD clients and two independent privileged
observers. The standard wlr inventory remains responsible for metadata and
ordinary control; the v3 private manager supplies geometry, size hints,
decoration policy and a native-view lifetime token.

`get_foreign` does not adopt a window or change focus/decorations. Tokens are
shared across observers, distinct between views and never dereferenced as
pointers or connection-local Wayland object IDs. Wine-controlled views produce
an inert closed observer with no token. A closed observer accepts destroy and
ignores refresh. Native destruction and observer disconnection are exercised.

The vendored `protocols/wlr-foreign-toplevel-management-unstable-v1.xml` is the
unmodified upstream protocol from
https://github.com/swaywm/wlr-protocols/blob/master/unstable/wlr-foreign-toplevel-management-unstable-v1.xml
(retrieved 2026-10-14). Its original copyright/permission notice is preserved.
Generating its interface definitions is necessary because wlroots does not
export those protocol symbols from its shared library. Resource resolution uses
wlroots' public handle resource list, independent of implementation-private
resource user data.

This checkpoint provides observation only. Native/frame binding, atomic groups,
keyboard focus separation and shell integration are subsequent L2 work.
