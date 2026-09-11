# Implementation and validation

## Components

- `tools/planet_studio_mcp/server.py`: standard-library Python MCP stdio server.
  Handles initialization, ping, tool listing/calls, schemas and error mapping.
- `scripts/world_authoring/mcp/planet_studio_bridge.gd`: opt-in Godot child node.
  Executes on the main thread and only resolves controls beneath its own editor.
- `scripts/world_authoring/world_authoring_editor.gd`: attaches the bridge after
  initial UI creation when `ASTERRA_MCP_ENABLED=1`. The live editor inherits this
  base, so both standalone and in-game Planet Studio use the same bridge.
- `tests/planet_studio_mcp.gd`: actual-scene headless integration test.
- `scripts/world_authoring/mcp/planet_studio_vision.gd`: camera, time, debug and capture operations.
- `scripts/world_authoring/editor_camera_frame.gd`: continuous tangent-frame transport shared by manual editor navigation and MCP.
- `tests/planet_studio_vision.gd`: pole continuity, camera, clock, debug and image-comparison tests; optional rendered capture fixture.
- `tools/planet_studio_mcp/test_server.py`: protocol/validation/transport tests.

## Transport

The public interface implements MCP protocol version `2025-06-18`, negotiating
that version in initialization. A client launches the server as a subprocess;
messages are UTF-8 newline-delimited JSON-RPC on stdin/stdout. Only protocol
responses are written to stdout. Send `notifications/initialized` after the
initialize response. No resources/prompts/subscriptions are advertised.

References: [MCP stdio transport](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports)
and [MCP tools](https://modelcontextprotocol.io/specification/2025-06-18/server/tools).

The private game socket uses one request per connection:

```json
{"token":"shared secret","tool":"studio_status","arguments":{}}
```

A newline terminates the request. Godot returns one newline-terminated JSON
object and closes the connection. This private transport is not itself MCP or
HTTP. Requests are limited to 8 MiB and Python responses to 32 MiB. Godot permits
at most eight connections, expires connections after 120 seconds and serializes
execution so callback/frame settling cannot overlap another MCP edit. Normal
user interaction is still possible; refresh discovery when a control goes stale.

UI callbacks can schedule control replacement. Each ordinary request waits two
process frames before responding. Input sequences validate completely before
injecting any events, one per frame. These frame waits do not constitute a
promise that an asynchronous terrain rebuild has completed.

Tokens are required and not logged. No generic method call, script evaluation or
arbitrary property-write endpoint is exposed. Preset shortcuts are confined to
the preset directory; file dialogs intentionally retain their normal filesystem
access for user-selected texture/shader assets. A client with the token can
operate all the editor controls, including destructive clear/delete actions.

## Run tests

From the repository root:

```powershell
python -B -m unittest discover -s tools/planet_studio_mcp -v
godot --headless --path . --script tests/planet_studio_mcp.gd
godot --headless --path . --script tests/planet_studio_vision.gd
```

Use your actual Godot executable if its command alias is unavailable. The
integration test uses port `19876`, an isolated recovery file under
`user://world_authoring/tests/`, and a test-only secret. It deletes its recovery
file on completion. It does not apply changes to a running production world.
Godot's headless GPU-water warning is expected in this test.

Python tests cover lifecycle/version negotiation, malformed requests, no replies
to notifications, tool discovery, schema validation before transport, Unicode,
tool errors/images, real subprocess stdio framing, fragmented socket responses
and token forwarding. The Godot test exercises the real editor as described in
[feature coverage](features.md).

For manual rendered validation, start the game using [setup](setup.md), open
Planet Studio and connect your client. Capture a screenshot, create a texture
band, change a numeric value, undo/redo it, import a local texture, place a water
point, and inspect the resulting preview/status. Use an expendable preset for
clear/delete actions. Releasing every input press is the caller's responsibility.
