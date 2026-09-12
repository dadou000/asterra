"""MCP bridge for Orbit's built-in dev server.

Orbit (OrbitSandbox.exe) opens a loopback-only TCP text protocol on
127.0.0.1:4319 whenever it is running (see
engine/dev_server/src/DevServer.cpp). This script exposes that
protocol as MCP tools so an MCP client (Claude Code, Claude Desktop,
etc.) can drive and inspect a live Orbit process for testing:
read streaming/render stats, grab a screenshot, teleport the camera,
or request a clean shutdown -- without a human at the keyboard.

Usage:
    pip install mcp
    python tools/mcp_server/orbit_mcp_server.py

Register it with Claude Code:
    claude mcp add orbit -- python C:\\path\\to\\asterra\\tools\\mcp_server\\orbit_mcp_server.py

OrbitSandbox must already be running (launch it separately, e.g. via
dist/Orbit-Windows-Release/OrbitLauncher.exe) -- this script only
talks to it, it does not launch it.
"""

from __future__ import annotations

import socket
import time

try:
    # mcp >= 2.0
    from mcp.server.mcpserver import MCPServer as _Server
except ModuleNotFoundError:
    # mcp 1.x
    from mcp.server.fastmcp import FastMCP as _Server

HOST = "127.0.0.1"
PORT = 4319
TIMEOUT_SECONDS = 5.0

mcp = _Server("orbit")


def _send_command(command: str) -> str:
    """Opens a fresh connection, sends one line, reads one line back.

    A new connection per call keeps this stateless and avoids ever
    holding a stale socket across tool invocations -- the dev server
    only tracks one client at a time anyway.
    """
    try:
        with socket.create_connection(
            (HOST, PORT), timeout=TIMEOUT_SECONDS
        ) as sock:
            sock.sendall((command.strip() + "\n").encode("ascii"))

            sock.settimeout(TIMEOUT_SECONDS)
            buffer = b""

            while b"\n" not in buffer:
                chunk = sock.recv(4096)

                if not chunk:
                    break

                buffer += chunk

            return buffer.split(b"\n", 1)[0].decode(
                "ascii", errors="replace"
            )
    except (ConnectionRefusedError, TimeoutError, OSError) as error:
        return (
            f"ERR could not reach Orbit dev server at {HOST}:{PORT} "
            f"({error}). Is OrbitSandbox running?"
        )


@mcp.tool()
def orbit_ping() -> str:
    """Checks whether a running Orbit process is reachable."""
    return _send_command("PING")


@mcp.tool()
def orbit_stats() -> str:
    """Returns the latest terrain/ocean/water streaming stats as
    space-separated key=value pairs (altitude, sample counts, cache
    occupancy, draw calls, and so on)."""
    return _send_command("STATS")


@mcp.tool()
def orbit_screenshot(path: str) -> str:
    """Captures the current Orbit window to an uncompressed BMP file
    at `path` (an absolute Windows path on the machine running
    OrbitSandbox).

    Capture reads real desktop pixels, so the dev server briefly
    raises the window's Z-order and waits a few presented frames
    before reading them back -- this call blocks for ~150ms to give
    that time to happen before returning, so the file is ready by
    the time this returns."""
    response = _send_command(f"SCREENSHOT {path}")
    time.sleep(0.15)
    return response


@mcp.tool()
def orbit_slew(
    direction_x: float,
    direction_y: float,
    direction_z: float,
    altitude_meters: float,
    duration_seconds: float,
) -> str:
    """Smoothly flies the observer to a new point over
    `duration_seconds`, instead of jumping there instantly like
    orbit_teleport.

    Use this (then take a few orbit_screenshot calls spaced a second
    or so apart while it's in flight) to actually watch the terrain
    clipmap stream and morph while moving, rather than only ever
    comparing two static teleport snapshots. `direction_x/y/z` is any
    non-zero vector from the planet center (normalized on receipt);
    `altitude_meters` is the target altitude above local terrain,
    clamped to Orbit's ground-collision floor and its 2,000 km
    ceiling."""
    return _send_command(
        f"SLEW {direction_x} {direction_y} {direction_z} "
        f"{altitude_meters} {duration_seconds}"
    )


@mcp.tool()
def orbit_teleport(
    direction_x: float,
    direction_y: float,
    direction_z: float,
    altitude_meters: float,
) -> str:
    """Moves the observer to a new point on the planet.

    `direction_x/y/z` is any non-zero vector from the planet center
    (it is normalized on receipt) and picks the surface point;
    `altitude_meters` is clamped to stay at least 2m above the
    actual sampled terrain there, up to a 2,000 km ceiling."""
    return _send_command(
        f"TELEPORT {direction_x} {direction_y} {direction_z} {altitude_meters}"
    )


@mcp.tool()
def orbit_set_debug_overlay(enabled: bool) -> str:
    """Shows or hides the F3 debug HUD (FPS, altitude, terrain/cache/
    ocean stats) -- the same toggle as pressing F3 in the window."""
    return _send_command(f"DEBUG_OVERLAY {'ON' if enabled else 'OFF'}")


@mcp.tool()
def orbit_quit() -> str:
    """Requests a clean shutdown of the running Orbit process."""
    return _send_command("QUIT")


if __name__ == "__main__":
    mcp.run()
