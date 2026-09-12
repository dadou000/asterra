# Planet Studio MCP

Control the running game's Planet Studio from any MCP client. The server exposes
the editor's actual controls and input path, plus its shared session operations.
New fields and layer controls appear automatically in discovery. Edits use the
same callbacks, validation, history and runtime behavior as manual editing.

- [Setup and troubleshooting](setup.md)
- [Tools, arguments and examples](tools.md)
- [Feature coverage](features.md)
- [Implementation and tests](development.md)
- [Camera, time, debug views and named visual comparisons](vision.md)

There are twelve tools: `studio_status`, `studio_ui`, `studio_category`,
`studio_control`, `studio_session`, `studio_inspect`, `studio_input` and
`studio_screenshot`, `studio_camera`, `studio_time`, `studio_view` and
`studio_captures`. Tool discovery works even while the game is closed.

The bridge is a **boot autoload** (`scripts/world_authoring/mcp/
planet_studio_bridge.gd`, registered in `project.godot`) — it listens from the
very first frame the game process is up, independent of which scene is
showing, and it survives every scene change for the rest of that process
(closing Planet Studio, returning to the start menu, loading a different body
or preset). It finds the live Planet Studio editor Control dynamically each
call, so nothing needs to reconnect once it opens.

`studio_status`, `studio_input` and `studio_screenshot` work with **no editor
open at all** — `studio_status.editor_open` is `false` and other
editor-specific fields come back `null`/omitted, but the game can still be
observed and driven. This is what makes end-to-end automation from a cold
launch possible: screenshot the start menu, click through it with
`studio_input`, poll `studio_status` for `editor_open` to flip `true` once
Planet Studio has opened. `studio_category`, `studio_control`,
`studio_session` and `studio_inspect` do require Planet Studio to be open —
they return a clear `{"error": ...}` (never crash the game) until it is.

The bridge is **off by default**. Enable it explicitly and give the game and
server the same local secret. The MCP client launches a Python stdio server;
that server connects to a private, authenticated loopback socket in Godot.
Python 3.10+ is sufficient; there are no pip packages to install.

Typical workflow:

1. Start the game with MCP enabled.
2. Connect an MCP client using the configuration in [setup](setup.md).
3. Optionally drive the start menu with `studio_input` (see above), or open
   **Planet Studio** yourself.
4. Call `studio_status`, open a category, and call `studio_ui`.
5. Use returned control IDs to edit values or press buttons. Re-query after edits.
6. Inspect the staged model/status, then apply or revert using `studio_session`.

Most model edits are staged. Some existing editor operations, including runtime
sculpting and shader previews, are immediate. MCP preserves those semantics;
`undo` and `revert` are not a universal rollback of every runtime action.
