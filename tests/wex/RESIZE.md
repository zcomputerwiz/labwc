<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Hosted resize transaction probe

Run `tests/wex/run.sh OUTPUT_DIRECTORY` in an isolated headless session. The
`foreign-client --resize` case requires `WEX_SCENE_TEST_DIR` for screenshots.
The full suite runs unchanged older-version clients alongside the v10 case.

The v10 test checks that acceptance, xdg acknowledgement, native buffer commit
and matching frame publication are separate. It delays acknowledgements and
buffers, submits a client-selected size, checks left/top anchoring after maximum
constraints, supersedes an unacknowledged request, rejects stale cancellation
and publication, and checks fixed and single-axis constraints. It exercises all
eight edges/corners, both frame-first and native-first publication, explicit
cancellation, focus/capture loss, timeout rollback, ignored rollback, and native
and frame destruction during negotiation. Screenshot assertions verify retained
old root pixels and the final frame/native placement, not just protocol replies.

Snapshots hold buffer references through public scene/buffer APIs. Live surface
trees retain their listeners and popup/subsurface ownership. Snapshot nodes do
not accept input; the transaction requires Wine frame capture. No pixel copying,
native reparenting or decoration replacement occurs at intermediate commits.
Rollback times out after two seconds, requests the original geometry and waits
another two seconds before safely detaching to the ordinary native scene.

This checkpoint supports xdg native clients only. XWayland resize requests reply
unavailable; their observation and ordinary controls are unchanged. Wine driver
v10 consumption, interactive shell sizing, DPI conversion, real application
acceptance and maximize are separate work. Existing Wine binds v8 and bridge
observers bind v9, so this protocol addition enables no production affordances.
