# Setup

## 1. Launch the game

From the repository directory in PowerShell:

```powershell
$env:ASTERRA_MCP_ENABLED = '1'
$env:ASTERRA_MCP_PORT = '9876'
$env:ASTERRA_MCP_TOKEN = [guid]::NewGuid().ToString('N')
# Keep this value locally; use the same value in your MCP client configuration.
$env:ASTERRA_MCP_TOKEN
godot --path .
```

The bridge is listening from the moment the process boots, at the start menu
included — `studio_status`, `studio_input` and `studio_screenshot` work there
directly, so an MCP client can drive the start menu itself (e.g. click
**Planet Studio**) instead of a human doing it. For normal live editing,
launch the game this way and open Planet Studio (manually or via
`studio_input`): the isolated `scenes/world_authoring/PlanetStudio.tscn` scene
has the authoring UI but no live planet/camera binding.

If launching through the Godot editor, the editor process must inherit these
environment variables before you start the game. Setting them in a different
terminal after Godot is already running does not update that process.

| Variable | Default | Purpose |
|---|---|---|
| `ASTERRA_MCP_ENABLED` | disabled | Exactly `1` starts the bridge listening from game boot, at every scene. |
| `ASTERRA_MCP_TOKEN` | none | Shared secret, at least 16 characters. Required by both processes. |
| `ASTERRA_MCP_PORT` | `9876` | Loopback port, 1024–65535. Use a different port for each game instance. |

The token should be private and randomly generated. Do not commit it. The bridge
binds only to `127.0.0.1`, accepts at most eight clients, and stays up for the
whole game process (it is a boot autoload, not tied to Planet Studio's own
lifetime) — closes only when the game itself exits. Tools that need Planet
Studio open (`studio_category`, `studio_control`, `studio_session`,
`studio_inspect`) return a clear error rather than working while it is closed;
`studio_status`, `studio_input` and `studio_screenshot` work throughout.

## 2. Configure an MCP client

Adapt these absolute paths to your checkout and Python installation. This is a
generic MCP client JSON configuration; your client's settings wrapper may differ.

```json
{
  "mcpServers": {
    "asterra-planet-studio": {
      "command": "python",
      "args": [
        "-B",
        "C:/Users/david/Documents/GitHub/asterra/tools/planet_studio_mcp/server.py"
      ],
      "env": {
        "ASTERRA_MCP_PORT": "9876",
        "ASTERRA_MCP_TOKEN": "REPLACE_WITH_THE_GAME_TOKEN"
      }
    }
  }
}
```

Use an absolute Python executable path if your client cannot resolve `python`.
The server also accepts `--port 9876` and `--timeout 60`. It always connects to
loopback. It does not launch the game or install itself in any client settings.

This is an MCP **stdio** server, not an HTTP endpoint. Do not configure a URL
such as `http://localhost:9876`; that port is the private game transport.

## Troubleshooting

| Symptom | Check |
|---|---|
| Tools appear, calls fail with connection refused | Game running, enabled variable inherited, matching port. Planet Studio does NOT need to be open for the connection itself -- only for editor-specific tools. |
| Invalid authentication token | Identical token in both processes, at least 16 characters. Restart the game after changing its environment. |
| Godot says it cannot listen | Another instance occupies the port; configure a different one in both processes. |
| Stale control ID | The UI was rebuilt by an edit, undo or navigation. Call `studio_ui` again. |
| Control hidden/disabled | Open the relevant tab or select an applicable body first. Discovery with `include_hidden` does not make hidden controls editable. |
| Screenshot fails headless | Run a rendered game window. Model/UI tests work headless; GPU rendering does not. |
| Camera/placement unavailable | Use Planet Studio from the game's start menu, not the isolated UI scene. |
| Timeout during a rebuild | Inspect `studio_status` and the game before retrying. A timed-out mutation may already have executed. Increase `--timeout` if needed. |
| Tool returns success but preview has not finished | Success means the callback completed. Existing background rebuilds/bakes can continue; inspect the status and preview. |
| Editor tools error after pressing Back | Expected: `studio_category`/`studio_control`/`studio_session`/`studio_inspect` need Planet Studio open. The connection itself stays up (it is a boot autoload); check `studio_status.editor_open` and reopen Planet Studio (manually or via `studio_input`). |

To disable MCP, close the game, remove `ASTERRA_MCP_ENABLED` (or set it to `0`),
and restart. An ordinary game launch without the variable opens no MCP socket.
