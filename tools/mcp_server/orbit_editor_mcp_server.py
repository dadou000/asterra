"""MCP adapter for Orbit Studio's structured JSON-RPC authoring API.

OrbitStudio exposes newline-framed JSON-RPC 2.0 on 127.0.0.1:4320.
This adapter deliberately contains very little engine policy: semantic schemas,
commands, enablement and mutations are owned by Orbit and discovered at runtime.

Usage:
    pip install -r tools/mcp_server/requirements.txt
    python tools/mcp_server/orbit_editor_mcp_server.py
"""

from __future__ import annotations

import itertools
import json
import os
import socket
from pathlib import Path
from typing import Any

try:
    # MCP Python SDK v2 public API.
    from mcp.server import MCPServer as _Server
except (ImportError, ModuleNotFoundError):
    # Keep the bridge usable with supported v1 environments.
    from mcp.server.fastmcp import FastMCP as _Server

HOST = os.environ.get("ORBIT_RPC_HOST", "127.0.0.1")
PORT = int(os.environ.get("ORBIT_RPC_PORT", "4320"))
TIMEOUT_SECONDS = float(os.environ.get("ORBIT_RPC_TIMEOUT", "5.0"))
MAX_RESPONSE_BYTES = 8 * 1024 * 1024

mcp = _Server("orbit-studio")
_request_ids = itertools.count(1)


def _rpc(method: str, params: dict[str, Any] | list[Any] | None = None) -> Any:
    request_id = next(_request_ids)
    request = {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": method,
        "params": {} if params is None else params,
    }
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8") + b"\n"

    try:
        with socket.create_connection((HOST, PORT), timeout=TIMEOUT_SECONDS) as sock:
            sock.settimeout(TIMEOUT_SECONDS)
            sock.sendall(payload)

            buffer = bytearray()

            while True:
                while b"\n" not in buffer:
                    chunk = sock.recv(65536)
                    if not chunk:
                        raise RuntimeError(
                            "Orbit Studio closed the RPC connection before replying."
                        )
                    buffer.extend(chunk)
                    if len(buffer) > MAX_RESPONSE_BYTES:
                        raise RuntimeError(
                            "Orbit Studio RPC response exceeded the adapter limit."
                        )

                raw_line, _, remainder = bytes(buffer).partition(b"\n")
                buffer = bytearray(remainder)

                if not raw_line:
                    continue

                message = json.loads(raw_line.decode("utf-8"))

                # JSON-RPC server notifications can share the same connection
                # with a response. They are intentionally ignored by this
                # one-shot helper; event.since provides lossless replay.
                if (
                    isinstance(message, dict)
                    and message.get("jsonrpc") == "2.0"
                    and "method" in message
                    and "id" not in message
                ):
                    continue

                if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
                    raise RuntimeError(
                        f"Invalid Orbit Studio JSON-RPC response: {message!r}"
                    )

                if message.get("id") != request_id:
                    continue

                response = message
                break
    except (ConnectionRefusedError, TimeoutError, OSError) as error:
        raise RuntimeError(
            f"Could not reach Orbit Studio RPC at {HOST}:{PORT}: {error}"
        ) from error

    if "error" in response:
        error = response["error"]
        code = error.get("code", "unknown") if isinstance(error, dict) else "unknown"
        message = error.get("message", str(error)) if isinstance(error, dict) else str(error)
        data = error.get("data") if isinstance(error, dict) else None
        suffix = f" | data={data!r}" if data is not None else ""
        raise RuntimeError(f"Orbit RPC error {code}: {message}{suffix}")

    return response.get("result")


@mcp.tool()
def orbit_project_info() -> dict[str, Any]:
    """Return the open Orbit Studio project ID, name, root and startup world."""
    return _rpc("project.info")


@mcp.tool()
def orbit_cpu_timings() -> dict[str, Any]:
    """Return last and rolling 120-frame CPU timings for the live Studio frame loop."""
    return _rpc("studio.cpu_timings")


@mcp.tool()
def orbit_build_profiles() -> list[dict[str, Any]]:
    """Return build profiles from the open Project.orbit.toml."""
    return _rpc("build.profiles")


@mcp.tool()
def orbit_build_validate(profile: str | None = None) -> dict[str, Any]:
    """Validate a build profile through Orbit's shared headless BuildService."""
    params: dict[str, Any] = {}
    if profile is not None:
        params["profile"] = profile
    return _rpc("build.validate", params)


@mcp.tool()
def orbit_build_cook(profile: str | None = None) -> dict[str, Any]:
    """Checkpoint Studio state and cook a build profile through BuildService."""
    params: dict[str, Any] = {}
    if profile is not None:
        params["profile"] = profile
    return _rpc("build.cook", params)


@mcp.tool()
def orbit_build_package(profile: str | None = None) -> dict[str, Any]:
    """Checkpoint Studio state and assemble a standalone OrbitPlayer package."""
    params: dict[str, Any] = {}
    if profile is not None:
        params["profile"] = profile
    return _rpc("build.package", params)


@mcp.tool()
def orbit_schema_catalog() -> list[dict[str, Any]]:
    """Return semantic object/property schemas generated by the running engine."""
    return _rpc("schema.catalog")


@mcp.tool()
def orbit_command_catalog() -> list[dict[str, Any]]:
    """Return every registered command, typed parameters and live enablement."""
    return _rpc("command.catalog")


@mcp.tool()
def orbit_invoke_command(
    command_id: str,
    arguments: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Invoke a registered Orbit command by stable ID using typed arguments."""
    return _rpc(
        "command.invoke",
        {"id": command_id, "arguments": arguments or {}},
    )


@mcp.tool()
def orbit_invoke_command_by_name(
    name: str,
    arguments: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Resolve a command by exact display name and invoke it.

    The lookup is done against the live command catalog, so plugin commands are
    included automatically. Ambiguous duplicate names are rejected.
    """
    matches = [entry for entry in _rpc("command.catalog") if entry["name"] == name]
    if not matches:
        raise RuntimeError(f"No Orbit command named {name!r}.")
    if len(matches) != 1:
        ids = [entry["id"] for entry in matches]
        raise RuntimeError(f"Orbit command name {name!r} is ambiguous: {ids}")
    return _rpc(
        "command.invoke",
        {"id": matches[0]["id"], "arguments": arguments or {}},
    )


@mcp.tool()
def orbit_object_roots() -> list[dict[str, Any]]:
    """Return root objects from the semantic editor tree."""
    return _rpc("object.roots")


@mcp.tool()
def orbit_object_children(parent_id: str) -> list[dict[str, Any]]:
    """Return direct children of one semantic editor object."""
    return _rpc("object.children", {"parent": parent_id})


@mcp.tool()
def orbit_object_get(object_id: str) -> dict[str, Any]:
    """Return one semantic object and all explicitly authored properties."""
    return _rpc("object.get", {"id": object_id})


@mcp.tool()
def orbit_object_create(
    type_id: str,
    name: str,
    parent_id: str | None = None,
) -> dict[str, Any]:
    """Create an object through Orbit's CommandService and return its stable ID."""
    return _rpc(
        "object.create",
        {"type": type_id, "name": name, "parent": parent_id},
    )


@mcp.tool()
def orbit_object_rename(object_id: str, name: str) -> dict[str, Any]:
    """Rename an object through Orbit's transactional command layer."""
    return _rpc("object.rename", {"id": object_id, "name": name})


@mcp.tool()
def orbit_object_reparent(
    object_id: str,
    parent_id: str | None = None,
) -> dict[str, Any]:
    """Move an object under another object, or to root when parent_id is null."""
    return _rpc("object.reparent", {"id": object_id, "parent": parent_id})


@mcp.tool()
def orbit_property_set(
    object_id: str,
    property_id: str,
    value: Any,
) -> dict[str, Any]:
    """Set a schema property through validation, persistence and undo history."""
    return _rpc(
        "property.set",
        {"object": object_id, "property": property_id, "value": value},
    )


@mcp.tool()
def orbit_path_create_network(
    name: str,
    parent_id: str | None = None,
    profile_asset: str | None = None,
) -> dict[str, Any]:
    """Create a semantic path network.

    profile_asset is the stable ContentService asset ID string when a
    .orbitpathprofile asset is assigned.
    """
    params: dict[str, Any] = {
        "name": name,
        "parent": parent_id,
    }
    if profile_asset is not None:
        params["profile_asset"] = profile_asset
    return _rpc("path.create_network", params)


@mcp.tool()
def orbit_path_create_node(
    network_id: str,
    name: str,
    anchor: dict[str, Any],
) -> dict[str, Any]:
    """Create a path node with a frame, surface, or entity/socket anchor.

    Frame anchor:
      {"kind":"frame","frame":"<FrameId>","position":[x,y,z]}
    Surface anchor:
      {"kind":"surface","body":"<BodyId>","coordinate":[lat,lon,offset]}
    Entity/socket anchor:
      {"kind":"entity_socket","entity":"<ObjectId>","socket":"name",
       "position":[x,y,z]}
    """
    return _rpc(
        "path.create_node",
        {
            "network": network_id,
            "name": name,
            "anchor": anchor,
        },
    )


@mcp.tool()
def orbit_path_connect(
    start_node_id: str,
    end_node_id: str,
    mode: str = "direct",
    name: str | None = None,
    start_handle: list[float] | None = None,
    end_handle: list[float] | None = None,
) -> dict[str, Any]:
    """Connect two nodes using direct, bezier, or routed semantic geometry."""
    params: dict[str, Any] = {
        "start": start_node_id,
        "end": end_node_id,
        "mode": mode,
    }
    if name is not None:
        params["name"] = name
    if start_handle is not None:
        params["start_handle"] = start_handle
    if end_handle is not None:
        params["end_handle"] = end_handle
    return _rpc("path.connect", params)


@mcp.tool()
def orbit_path_set_bezier_handles(
    edge_id: str,
    start_handle: list[float],
    end_handle: list[float],
) -> dict[str, Any]:
    """Edit both cubic Bezier handles through the shared transaction layer."""
    return _rpc(
        "path.set_bezier_handles",
        {
            "edge": edge_id,
            "start_handle": start_handle,
            "end_handle": end_handle,
        },
    )


@mcp.tool()
def orbit_path_set_profile(
    object_id: str,
    profile_asset: str | None,
) -> dict[str, Any]:
    """Assign a path-profile asset to a network or edge override.

    Pass profile_asset=None to clear the assignment.
    """
    return _rpc(
        "path.set_profile",
        {
            "object": object_id,
            "profile_asset": profile_asset,
        },
    )


@mcp.tool()
def orbit_path_route_status(edge_id: str) -> dict[str, Any]:
    """Return async derived-route state for a routed PathEdge."""
    return _rpc("path.route_status", {"edge": edge_id})


@mcp.tool()
def orbit_path_route_result(edge_id: str) -> dict[str, Any]:
    """Return the current derived routed polyline, frame and route cost."""
    return _rpc("path.route_result", {"edge": edge_id})


@mcp.tool()
def orbit_path_derived_result(edge_id: str) -> dict[str, Any]:
    """Return M20 visual/lane/reference/collision/nav derived state for a PathEdge."""
    return _rpc("path.derived_result", {"edge": edge_id})


@mcp.tool()
def orbit_path_route_invalidate(edge_id: str) -> dict[str, Any]:
    """Invalidate one derived route without changing project authority."""
    return _rpc("path.route_invalidate", {"edge": edge_id})


@mcp.tool()
def orbit_path_inspect(object_id: str) -> dict[str, Any]:
    """Inspect a semantic path network, node, or edge."""
    return _rpc("path.inspect", {"id": object_id})


@mcp.tool()
def orbit_selection_get() -> list[str]:
    """Return the ordered shared selection from Orbit Studio."""
    return _rpc("selection.get")


@mcp.tool()
def orbit_selection_set(object_ids: list[str]) -> dict[str, Any]:
    """Replace Orbit Studio's shared selection with validated object IDs."""
    return _rpc("selection.set", {"ids": object_ids})


@mcp.tool()
def orbit_selection_clear() -> dict[str, Any]:
    """Clear the shared Orbit Studio selection."""
    return _rpc("selection.clear")


@mcp.tool()
def orbit_transaction_begin(label: str) -> dict[str, Any]:
    """Begin one atomic, undoable multi-operation authoring transaction."""
    return _rpc("transaction.begin", {"label": label})


@mcp.tool()
def orbit_transaction_commit() -> dict[str, Any]:
    """Commit the active Orbit authoring transaction."""
    return _rpc("transaction.commit")


@mcp.tool()
def orbit_transaction_rollback() -> dict[str, Any]:
    """Rollback the active Orbit authoring transaction without persisting it."""
    return _rpc("transaction.rollback")


@mcp.tool()
def orbit_undo() -> dict[str, Any]:
    """Undo the latest committed authoring transaction."""
    return _rpc("history.undo")


@mcp.tool()
def orbit_redo() -> dict[str, Any]:
    """Redo the latest undone authoring transaction."""
    return _rpc("history.redo")


@mcp.tool()
def orbit_viewport_get() -> dict[str, Any]:
    """Return the primary Studio RenderView dimensions and camera state."""
    return _rpc("viewport.get")


@mcp.tool()
def orbit_viewport_set_camera(
    frame_id: str | None = None,
    clear_frame: bool = False,
    position: list[float] | None = None,
    forward: list[float] | None = None,
    up: list[float] | None = None,
    vertical_fov_radians: float | None = None,
    near_plane_meters: float | None = None,
    far_plane_meters: float | None = None,
) -> dict[str, Any]:
    """Update fields of the primary Studio RenderView camera.

    Vector arguments are three-number arrays. Omitted values retain the
    existing camera state. Set clear_frame=True to clear the frame binding.
    """
    params: dict[str, Any] = {}

    if clear_frame:
        params["frame"] = None
    elif frame_id is not None:
        params["frame"] = frame_id
    if position is not None:
        params["position"] = position
    if forward is not None:
        params["forward"] = forward
    if up is not None:
        params["up"] = up
    if vertical_fov_radians is not None:
        params["vertical_fov_radians"] = vertical_fov_radians
    if near_plane_meters is not None:
        params["near_plane_meters"] = near_plane_meters
    if far_plane_meters is not None:
        params["far_plane_meters"] = far_plane_meters

    return _rpc("viewport.set_camera", params)


@mcp.tool()
def orbit_viewport_screenshot(path: str) -> dict[str, Any]:
    """Capture the completed offscreen Studio RenderView to a 32-bit BMP."""
    return _rpc("viewport.screenshot", {"path": path})


@mcp.tool()
def orbit_events_since(sequence: int = 0) -> dict[str, Any]:
    """Replay editor events newer than sequence.

    Events are sequenced by Orbit and retained in a bounded journal, covering
    semantic object changes, selection, content, plugins and viewport changes.
    """
    return _rpc("event.since", {"sequence": sequence})


@mcp.tool()
def orbit_rpc_call(method: str, params_json: str = "{}") -> Any:
    """Call any JSON-RPC method exposed by Orbit Studio.

    This escape hatch supports newly added engine/plugin RPC methods before the
    Python adapter grows a dedicated convenience tool.
    """
    params = json.loads(params_json)
    if not isinstance(params, (dict, list)):
        raise ValueError("params_json must decode to a JSON object or array.")
    return _rpc(method, params)


if hasattr(mcp, "resource"):
    @mcp.resource(
        "orbit://viewport/screenshot",
        mime_type="image/bmp",
    )
    def orbit_viewport_screenshot_resource() -> bytes:
        """Return a fresh BMP capture of Orbit Studio's primary viewport."""
        import tempfile

        with tempfile.NamedTemporaryFile(
            suffix=".bmp", delete=False
        ) as temporary:
            path = Path(temporary.name)

        try:
            _rpc("viewport.screenshot", {"path": str(path)})
            return path.read_bytes()
        finally:
            path.unlink(missing_ok=True)


if __name__ == "__main__":
    mcp.run()
