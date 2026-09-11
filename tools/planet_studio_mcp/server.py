#!/usr/bin/env python3
"""Dependency-free MCP stdio server for a running Asterra Planet Studio."""
from __future__ import annotations

import argparse
import json
import math
import os
import socket
import sys
from typing import Any

PROTOCOL_VERSION = "2025-06-18"
MAX_MESSAGE = 8 * 1024 * 1024
MAX_RESPONSE = 32 * 1024 * 1024


def obj(properties=None, required=()):
    return {"type": "object", "properties": properties or {},
            "required": list(required), "additionalProperties": False}


def enum(*values):
    return {"type": "string", "enum": list(values)}


STRING = {"type": "string"}
BOOLEAN = {"type": "boolean"}
PAIR = {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}
VECTOR = {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}
EVENT = obj({
    "type": enum("key", "button", "motion"), "key": STRING,
    "pressed": BOOLEAN, "position": PAIR, "relative": PAIR,
    "button": {"type": "integer", "minimum": 1, "maximum": 5},
    "button_mask": {"type": "integer", "minimum": 0, "maximum": 31},
    "double_click": BOOLEAN, "shift": BOOLEAN, "ctrl": BOOLEAN, "alt": BOOLEAN,
}, ("type",))


def tool(name, description, schema, read_only=False):
    return {"name": name, "description": description, "inputSchema": schema,
            "annotations": {"readOnlyHint": read_only, "openWorldHint": False}}


TOOLS = [
    tool("studio_status", "Read editor status, dirty/apply scope, undo/redo, active body, live runtime availability and viewport size.", obj(), True),
    tool("studio_ui", "Discover current editor controls with ephemeral ids, paths, labels, tooltips, values, bounds and option indices. Refresh after edits or navigation. Labels are separate rows alongside their fields. Hidden controls are inspectable but cannot be edited.",
         obj({"include_hidden": BOOLEAN, "scope": enum("editor", "debug")}), True),
    tool("studio_category", "Open a Planet Studio category. Use studio_ui next to discover its controls and nested tabs.",
         obj({"category": enum("PLANET", "TERRAIN", "WATER", "ATMOSPHERIC", "CELESTIALS")}, ("category",))),
    tool("studio_control", "Operate an id returned by studio_ui using the actual UI callback. press: buttons; set: numeric/text/toggle; select: option/list/menu index; color: [r,g,b,a]; curve: flattened [x,y] points with x endpoints 0 and 1; file: path for an open file dialog; scroll: [horizontal,vertical]. Covers all editor panels, terrain/texture layers and presets, water, atmosphere, celestial and import/export controls. Re-discover ids after changes.",
         obj({"id": STRING, "action": enum("press", "set", "select", "color", "curve", "file", "scroll"),
              "value": {}}, ("id", "action"))),
    tool("studio_session", "Use existing session transactions: undo, redo, apply, revert, select_body, save_preset or load_preset. Presets use user://world_authoring/presets/<name>.tres. Apply can rebuild the live world. Sculpt operations follow the editor's immediate runtime behavior and are not session-undoable.",
         obj({"action": enum("undo", "redo", "apply", "revert", "select_body", "save_preset", "load_preset"),
              "body_id": STRING, "path": STRING}, ("action",))),
    tool("studio_inspect", "Read stored staged system resources. path is an array of property names and array indices, e.g. ['bodies',0,'planet_profile','terrain']. Deeper collections return truncation markers; drill down by path. Embedded image bytes are summarized.",
         obj({"path": {"type": "array", "items": {"type": ["string", "integer"]}, "maxItems": 32},
              "depth": {"type": "integer", "minimum": 0, "maximum": 6}}), True),
    tool("studio_input", "Send 1–120 keyboard/mouse events, one per frame, through Godot input. Use screenshot/control rects for positions in viewport pixels. Enables camera navigation, sculpt/paint strokes, water and terrain handle placement, and custom viewport widgets. Always pair key/button presses with releases. Mouse buttons: 1 left, 2 right, 3 middle, 4/5 wheel. Drag motion uses button_mask=1 for left. Live editing requires the game's Planet Studio, not the isolated UI scene.",
         obj({"events": {"type": "array", "items": EVENT, "minItems": 1, "maxItems": 120}}, ("events",))),
    tool("studio_camera", "Direct pole-safe editor camera control. move offset_m=[right,up,forward] in camera space or [x,y,z] in world space. rotate degrees=[yaw,pitch,roll] relative to transported surface frame. zoom sets FOV. teleport/slew take canonical world position_m; slew animates for duration_s (0–30). pose restores the exact state returned by this tool, on the same body. Units: metres/degrees; snapshot angles are radians. Requires live camera.",
         obj({"action": enum("state", "move", "rotate", "zoom", "teleport", "slew", "pose"),
              "offset_m": VECTOR, "space": enum("camera", "world"), "degrees": VECTOR,
              "fov_deg": {"type": "number", "minimum": 1, "maximum": 150}, "position_m": VECTOR,
              "duration_s": {"type": "number", "minimum": 0, "maximum": 30}, "pose": {"type": "object"}}, ("action",))),
    tool("studio_time", "Read/control the celestial clock. advance seconds accepts negative rewind; seek sets absolute seconds since system epoch. rate accepts signed seconds/real-second (-1e7..1e7). date uses the active body's calendar: integer year, zero-based day and hour in [0,24). Scrubbing pauses unless playing=true. This does not rewind water, weather, physics or undo history.",
         obj({"action": enum("state", "pause", "play", "rate", "advance", "seek", "date"),
              "seconds": {"type": "number", "minimum": -1e14, "maximum": 1e14},
              "rate": {"type": "number", "minimum": -1e7, "maximum": 1e7}, "playing": BOOLEAN,
              "year": {"type": "integer", "minimum": -100000, "maximum": 100000},
              "day": {"type": "integer", "minimum": 0}, "hour": {"type": "number", "minimum": 0, "maximum": 23.999999}}, ("action",))),
    tool("studio_view", "Discover all engine debug view names, geomorph modes, render toggles and terrain-debug flags. mode selects a discovered name (switching away from 'wireframe' also clears the terrain wireframe flag/HUD label, not just the raw Viewport.DebugDraw). full_bright enabled=true enables unshaded rendering, false restores prior view. geomorph selects index; render_toggle uses key and enabled. terrain_debug directly flips the runtime 'ASTERRA DEBUG' terrain panel's own flags (wireframe, freeze_terrain, side_cut, stable_displacement, stable_ocean_displacement, microrelief, pbr_detail, gpu_scatter, agl_height_cursor, gpu_height_cursor, physics_height_cursor take key+enabled; sink_scale, aerial_strength take key+value; key 'reset' clears all inspection flags/holds back to defaults) without the studio_ui/studio_control discovery dance. debug_menu opens/closes the game diagnostics panel; use studio_ui scope=debug and studio_control for controls terrain_debug does not cover (ring-count manual override, diagnostic-hold labels). Buffer support depends on renderer/features.",
         obj({"action": enum("state", "mode", "full_bright", "geomorph", "render_toggle", "debug_menu", "terrain_debug"),
              "mode": STRING, "enabled": BOOLEAN, "index": {"type": "integer", "minimum": 0}, "key": STRING,
              "value": {"type": "number"}}, ("action",))),
    tool("studio_screenshot", "Capture PNG. Optional name + required intent persist a unique full-resolution PNG and JSON metadata (pose, clock, views, tags) under user://world_authoring/captures. Inline preview max width 1600. hide_editor hides only Planet Studio UI during capture. Optional baseline capture id computes visual differences. Not available headless; input coordinates use actual viewport_size.",
         obj({"name": STRING, "intent": STRING, "tags": {"type": "array", "items": STRING, "maxItems": 32},
              "hide_editor": BOOLEAN, "baseline": STRING})),
    tool("studio_captures", "List last 100 named captures (optionally filtered by name), read metadata by id, or compare id against baseline. Comparison saves an absolute RGB difference image and reports mean error, changed pixels and mismatched camera/time/view metadata. Differences are not automatically regressions. Restore metadata.camera with studio_camera pose, then restore time/view before recapturing.",
         obj({"action": enum("list", "read", "compare"), "name": STRING, "id": STRING, "baseline": STRING}, ("action",))),
    tool("studio_texture_stack", "Read or replace one biome's BIOME TEXTURE band stack as structured JSON, instead of reconstructing it field-by-field from studio_inspect's raw graph nodes/links or driving each field through a separate studio_control call. get returns {layers, imported_textures, texture_choice_labels} where each layer is the same dict shape the editor itself uses (texture_choice 0-5 per texture_choice_labels, color/color_b/emission_color as [r,g,b] or [r,g,b,a], gradient_curve as a flattened [x,y,...] point list, height/slope/cavity ranges, softness, opacity, noise, tint_strength, roughness/metallic/anisotropy value+enabled, emission color/strength/enabled, custom_texture_index, height_relative). set replaces the ENTIRE stack (up to 8 bands) in one staged action -- omitted fields on a band fall back to the same defaults '+ Add texture band' uses. Round-trip get, edit the JSON, then set for the cheapest way to author bands via MCP.",
         obj({"action": enum("get", "set"), "biome_id": {"type": "integer", "minimum": 0},
              "layers": {"type": "array", "items": {"type": "object"}, "maxItems": 8}}, ("action", "biome_id"))),
]
TOOL_MAP = {item["name"]: item for item in TOOLS}


def validate(value: Any, schema: dict, path="arguments") -> None:
    """Validate the deliberately small JSON Schema subset used in this catalog."""
    kinds = schema.get("type", [])
    kinds = [kinds] if isinstance(kinds, str) else kinds
    checks = {"object": isinstance(value, dict), "array": isinstance(value, list),
              "string": isinstance(value, str), "boolean": isinstance(value, bool),
              "integer": type(value) is int,
              "number": type(value) in (int, float) and math.isfinite(value)}
    if kinds and not any(checks.get(kind, False) for kind in kinds):
        raise ValueError(f"{path}: expected {' or '.join(kinds)}")
    if "enum" in schema and value not in schema["enum"]:
        raise ValueError(f"{path}: unsupported value {value!r}")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                raise ValueError(f"{path}.{key}: required")
        props = schema.get("properties", {})
        for key, item in value.items():
            if schema.get("additionalProperties") is False and key not in props:
                raise ValueError(f"{path}.{key}: unknown argument")
            validate(item, props.get(key, {}), f"{path}.{key}")
    elif isinstance(value, list):
        if not schema.get("minItems", 0) <= len(value) <= schema.get("maxItems", MAX_MESSAGE):
            raise ValueError(f"{path}: invalid array length")
        for index, item in enumerate(value):
            validate(item, schema.get("items", {}), f"{path}[{index}]")
    elif type(value) in (int, float):
        if not math.isfinite(value) or value < schema.get("minimum", -math.inf) or value > schema.get("maximum", math.inf):
            raise ValueError(f"{path}: number outside allowed range")


class Bridge:
    def __init__(self, port: int, token: str, timeout: float):
        self.port, self.token, self.timeout = port, token, timeout

    def call(self, name: str, arguments: dict) -> dict:
        if len(self.token) < 16:
            raise ValueError("Set ASTERRA_MCP_TOKEN to the same secret (at least 16 characters) in the game and MCP server.")
        request = json.dumps({"token": self.token, "tool": name, "arguments": arguments},
                             ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"
        if len(request) > MAX_MESSAGE:
            raise ValueError("Bridge request exceeds 8 MiB.")
        # A timeout is deliberately not retried: the game may have applied the edit.
        with socket.create_connection(("127.0.0.1", self.port), self.timeout) as connection:
            connection.settimeout(self.timeout)
            connection.sendall(request)
            with connection.makefile("rb") as stream:
                response = stream.readline(MAX_RESPONSE + 1)
        if not response.endswith(b"\n") or len(response) > MAX_RESPONSE:
            raise ValueError("Incomplete or oversized game response. Check editor status before retrying mutations.")
        result = json.loads(response)
        if not isinstance(result, dict):
            raise ValueError("Invalid game response.")
        return result


class Server:
    def __init__(self, bridge):
        self.bridge = bridge
        self.initialized = False
        self.ready = False

    @staticmethod
    def error(identifier, code, message):
        return {"jsonrpc": "2.0", "id": identifier, "error": {"code": code, "message": message}}

    def dispatch(self, request):
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            return self.error(None, -32600, "Invalid JSON-RPC request")
        identifier = request.get("id")
        if "id" in request and (type(identifier) not in (int, str)):
            return self.error(None, -32600, "Request id must be a string or integer")
        method = request["method"]
        if "id" not in request:
            if method == "notifications/initialized" and self.initialized:
                self.ready = True
            return None
        params = request.get("params", {})
        if not isinstance(params, dict):
            return self.error(identifier, -32602, "params must be an object")
        if method == "initialize":
            if self.initialized:
                return self.error(identifier, -32600, "Already initialized")
            if not isinstance(params.get("protocolVersion"), str) or not isinstance(params.get("capabilities"), dict) or not isinstance(params.get("clientInfo"), dict):
                return self.error(identifier, -32602, "initialize requires protocolVersion, capabilities and clientInfo")
            self.initialized = True
            result = {"protocolVersion": PROTOCOL_VERSION,
                      "capabilities": {"tools": {"listChanged": False}},
                      "serverInfo": {"name": "asterra-planet-studio", "version": "1.0.0"},
                      "instructions": "Start with studio_status and studio_ui. Use current control ids and re-query after edits. Changes use existing editor semantics; inspect status before applying. The game must be running with Planet Studio open and MCP enabled."}
        elif method == "ping":
            result = {}
        elif not self.ready:
            return self.error(identifier, -32002, "Initialize and send notifications/initialized first")
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            name = params.get("name")
            if not isinstance(name, str) or name not in TOOL_MAP:
                return self.error(identifier, -32602, "Unknown tool")
            arguments = params.get("arguments", {})
            try:
                validate(arguments, TOOL_MAP[name]["inputSchema"])
                reply = self.bridge.call(name, arguments)
                if "image" in reply:
                    result = {"content": [{"type": "image", "data": reply["image"], "mimeType": reply["mimeType"]}]}
                    metadata = {key: value for key, value in reply.items() if key not in ("image", "mimeType")}
                    if metadata:
                        result["content"].append({"type": "text", "text": json.dumps(metadata, ensure_ascii=False)})
                else:
                    result = {"content": [{"type": "text", "text": json.dumps(reply, ensure_ascii=False)}],
                              "isError": "error" in reply}
            except (OSError, ValueError) as exc:
                result = {"content": [{"type": "text", "text": f"{type(exc).__name__}: {exc}. If a mutation timed out, inspect status before retrying."}], "isError": True}
        else:
            return self.error(identifier, -32601, "Method not found")
        return {"jsonrpc": "2.0", "id": identifier, "result": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=int(os.environ.get("ASTERRA_MCP_PORT", "9876")))
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("port must be 1024–65535 and timeout must be positive and finite")
    server = Server(Bridge(args.port, os.environ.get("ASTERRA_MCP_TOKEN", ""), args.timeout))
    while True:
        line = sys.stdin.buffer.readline(MAX_MESSAGE + 1)
        if not line:
            break
        if len(line) > MAX_MESSAGE:
            while not line.endswith(b"\n"):
                line = sys.stdin.buffer.readline(MAX_MESSAGE + 1)
                if not line:
                    break
            reply = server.error(None, -32600, "Request exceeds 8 MiB")
        else:
            try:
                request = json.loads(line)
            except (ValueError, UnicodeError):
                reply = server.error(None, -32700, "Parse error")
            else:
                reply = server.dispatch(request)
        if reply is not None:
            sys.stdout.buffer.write(json.dumps(reply, ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n")
            sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
