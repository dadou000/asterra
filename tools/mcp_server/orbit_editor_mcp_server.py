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
import time
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


def _wait_for_project(project_id: str, timeout_seconds: float) -> dict[str, Any]:
    """Poll project.info until Studio has relaunched into project_id.

    Switching projects hands off to a fresh Studio process, so the RPC port
    briefly goes away. The old process can also still answer for a moment, which
    is why the project ID (not mere reachability) is what is awaited.
    """
    deadline = time.monotonic() + timeout_seconds
    last_error = "no reply yet"

    while time.monotonic() < deadline:
        try:
            info = _rpc("project.info")
            if isinstance(info, dict) and info.get("id") == project_id:
                return info
            last_error = "Studio still reports another project"
        except RuntimeError as error:
            last_error = str(error)
        time.sleep(0.5)

    raise RuntimeError(
        f"Studio did not relaunch into project {project_id} within "
        f"{timeout_seconds:.0f}s ({last_error})."
    )


@mcp.tool()
def orbit_project_create(
    root: str,
    name: str,
    wait: bool = True,
    timeout_seconds: float = 120.0,
) -> dict[str, Any]:
    """Create a new Orbit project and switch Studio to it.

    root is the new project's directory (created if missing; relative paths are
    resolved by Studio's working directory, so prefer an absolute path). Studio
    relaunches into the project; with wait=True this returns once the new
    Studio answers project.info for the new project ID.
    """
    result = _rpc("project.create", {"root": root, "name": name})
    if wait:
        _wait_for_project(result["id"], timeout_seconds)
    return result


@mcp.tool()
def orbit_project_open(
    path: str,
    wait: bool = True,
    timeout_seconds: float = 120.0,
) -> dict[str, Any]:
    """Open an existing Orbit project (directory or Project.orbit.toml).

    Studio saves the current project, then relaunches into the target.
    """
    result = _rpc("project.open", {"path": path})
    if wait:
        _wait_for_project(result["id"], timeout_seconds)
    return result


@mcp.tool()
def orbit_view_terrain_overlays_get(view_id: str = "studio.primary") -> dict[str, Any]:
    """Which terrain diagnostic overlays a viewport draws."""
    return _rpc("view.terrain_overlays_get", {"id": view_id})


@mcp.tool()
def orbit_view_terrain_overlays_set(
    view_id: str = "studio.primary",
    dirty_page_bounds: bool | None = None,
    build_states: bool | None = None,
    physical_lod: bool | None = None,
    clipmap_rings: bool | None = None,
    clipmap_levels: bool | None = None,
    clipmap_sample_health: bool | None = None,
    clipmap_hole_view: bool | None = None,
    clipmap_projection_view: bool | None = None,
    clipmap_shading_view: bool | None = None,
    clipmap_wireframe: bool | None = None,
    clipmap_freeze: bool | None = None,
    cache_status: bool | None = None,
    authored_constraints: bool | None = None,
    biome_weights: bool | None = None,
    process_masks: bool | None = None,
    drainage_vectors: bool | None = None,
) -> dict[str, Any]:
    """Turn terrain diagnostic overlays on/off for a viewport. Omitted flags keep
    their current value. clipmap_rings outlines only the ACTIVE clipmap levels (the
    dynamic planner's range, finest and coarsest brighter); clipmap_levels tints
    the terrain surface by clipmap level so each active level and its hand-off are
    visible. clipmap_sample_health colours each vertex by what is wrong with its GPU
    sample (red bad elevation, green bad morph target, blue bad slope, grey healthy) to
    find corrupted strips. clipmap_hole_view culls nothing and colours each vertex by why it
    would be culled (red beyond the horizon, green inside a finer level's hole, blue inside it
    while that level fades in, cyan culled by distance bands, grey drawn normally), to find
    gaps where a finer level is missing. clipmap_projection_view culls nothing and colours
    each vertex by where its clip position lands (red non-finite, green behind the camera,
    blue outside the near/far range, cyan off screen sideways, grey on screen) to find
    triangles the GPU clips away. clipmap_shading_view colours each terrain pixel by which
    interpolated shading input is bad (yellow biome weights sum to zero, magenta non-finite
    position, red/green/blue terrain normal / body-fixed normal / surface direction, dark
    grey fine), as emission. clipmap_wireframe draws the terrain mesh as a wireframe (water hidden);
    clipmap_freeze freezes the clipmap (plan, window, residency, content) where it
    is so the camera can fly away and look at the rings from outside; turning it
    off snaps the clipmap back to the camera. Frozen/wireframe state is reported in
    view.text_diagnostics' clipmap_plan. The active levels are listed there too."""
    params: dict[str, Any] = {"id": view_id}
    for key, value in {
        "dirty_page_bounds": dirty_page_bounds,
        "build_states": build_states,
        "physical_lod": physical_lod,
        "clipmap_rings": clipmap_rings,
        "clipmap_levels": clipmap_levels,
        "clipmap_sample_health": clipmap_sample_health,
        "clipmap_hole_view": clipmap_hole_view,
        "clipmap_projection_view": clipmap_projection_view,
        "clipmap_shading_view": clipmap_shading_view,
        "clipmap_wireframe": clipmap_wireframe,
        "clipmap_freeze": clipmap_freeze,
        "cache_status": cache_status,
        "authored_constraints": authored_constraints,
        "biome_weights": biome_weights,
        "process_masks": process_masks,
        "drainage_vectors": drainage_vectors,
    }.items():
        if value is not None:
            params[key] = value
    return _rpc("view.terrain_overlays_set", params)


@mcp.tool()
def orbit_viewport_focus_surface(
    u: float,
    v: float,
    view_id: str = "studio.primary",
) -> dict[str, Any]:
    """Move the viewport camera to a low vantage point over the terrain under the
    viewport position (u, v), each in 0..1 with (0, 0) at the top-left. Same as
    double-clicking the terrain. Returns focused=false if that position does not
    hit terrain."""
    return _rpc("viewport.focus_surface", {"id": view_id, "u": u, "v": v})


@mcp.tool()
def orbit_view_terrain_layers_get(view_id: str = "studio.primary") -> dict[str, Any]:
    """Which terrain layers a viewport draws (production_surface = near-field
    clipmap terrain, macro_globe = orbital patches, ocean, surface_effects) and its
    lod_bias_stops, and the dynamic clipmap planner (dynamic_clipmaps,
    clipmap_pixels_per_vertex)."""
    return _rpc("view.terrain_layers_get", {"id": view_id})


@mcp.tool()
def orbit_view_terrain_layers_set(
    view_id: str = "studio.primary",
    production_surface: bool | None = None,
    full_clipmap: bool | None = None,
    macro_globe: bool | None = None,
    ocean: bool | None = None,
    surface_effects: bool | None = None,
    lod_bias_stops: float | None = None,
    dynamic_clipmaps: bool | None = None,
    clipmap_pixels_per_vertex: float | None = None,
    clipmap_fade_seconds: float | None = None,
    experimental_distance_bands: bool | None = None,
    clipmap_band_edges_meters: list[float] | None = None,
    clipmap_band_scale: float | None = None,
    clipmap_partial_updates: bool | None = None,
    physical_pages: bool | None = None,
    clouds: bool | None = None,
    cloud_lab: dict[str, Any] | None = None,
    cloud_resolution_scale: float | None = None,
    cloud_godray_strength: float | None = None,
    cloud_light_volume: bool | None = None,
    bypass_cloud_shadow: bool | None = None,
    bypass_indirect_lighting: bool | None = None,
    bypass_near_field_water: bool | None = None,
    bypass_atmosphere: bool | None = None,
    bypass_hybrid_reflections: bool | None = None,
    bypass_radiance_cache: bool | None = None,
    indirect_coverage_view: bool | None = None,
    cloud_volume_debug_altitude: float | None = None,
    cloud_temporal: bool | None = None,
) -> dict[str, Any]:
    """Choose which terrain layers a viewport draws and its LOD bias. Omitted
    fields keep their value. lod_bias_stops is clamped to [-4, 4]: +1 keeps richer
    representations longer and doubles orbital patch resolution, -1 is coarser and
    cheaper (with full_clipmap it scales the clipmap planner's target spacing too).
    full_clipmap (default true) draws the production clipmap from the ground to
    orbit and never uses the orbital globe; false brings back the hand-off to the
    orbital globe. dynamic_clipmaps draws and generates only the clipmap levels the
    camera can use (off = the whole 20-level ladder at every altitude);
    clipmap_pixels_per_vertex ([0.25, 32], default 3) is the target sample spacing
    in pixels at the nearest ground, lower keeps finer levels longer.
    clipmap_fade_seconds ([0, 5], default 0.4) is how long a clipmap level takes to
    dissolve in or out when the plan adds or drops it (0 = instant, they pop).
    experimental_distance_bands (default false) is an EXPERIMENT: with full_clipmap,
    clipmap level k is drawn only where the camera's distance to the terrain lies
    between clipmap_band_edges_meters[k-1] and [k] (up to 16 increasing metres,
    default [100, 500, 2000, 10000, 40000, 160000, 640000, 2560000, 10000000,
    40000000]; the last is the farthest distance drawn), and neighbouring bands
    cross-fade per pixel so the rings resize continuously with the camera instead
    of following the fixed 2:1 ladder. clipmap_band_scale ([0.1, 10], default 1)
    multiplies every edge, a one-number way to make all the clipmap distances
    larger or smaller. Changing the edges or scale rebuilds the terrain renderer.
    clipmap_partial_updates (default true) makes banded levels refresh only the strip
    that scrolled into view; false regenerates a whole level on every scroll (for
    comparison; clipmap_plan.generated_samples counts what was generated).
    physical_pages (default true) composites the derived physical pages (the
    cache_status bounds) into the clipmap's elevation and water depth; false draws
    the plain generated terrain, to tell page-related height steps from the generator.
    clouds (default true) ray-marches the body's cloud layer in the viewport (a
    layer with no built cloud field draws nothing). cloud_resolution_scale
    (0.25-1, default 0.5) is the ray-march resolution relative to the viewport; 1
    gives crisp edges at about four times the cost. cloud_godray_strength (0-2,
    default 1) scales the crepuscular rays in cloud-shadowed air; 0 turns them
    off. cloud_light_volume (default true) caches the optical depth towards the sun around
    the camera (out to ~370 km) so cloud-on-cloud shadows and god rays reach far at a low sun;
    false marches everything per sample (for comparison). bypass_cloud_shadow,
    bypass_indirect_lighting (final gather + hybrid reflections), bypass_hybrid_reflections (only the reflections stage, to tell the two apart), bypass_radiance_cache (only the radiance-cache fallback of the final gather), bypass_near_field_water and
    bypass_atmosphere (also skips the clouds drawn after it) each skip one stage of the frame
    for this view, to bisect a rendering artefact (default false). indirect_coverage_view
    replaces the final gather's contribution with its coverage (red confidence, green
    gathered brightness on a log scale, magenta = the gather returned nothing for the
    pixel, i.e. no indirect light there). cloud_volume_debug_altitude
    (metres, default 0 = off) draws a horizontal slice of that volume at the given altitude
    over the view as a heatmap of the optical depth towards the sun (blue clear to white
    opaque, magenta = voxel not ready, nothing outside the cascades). -1 shows the scene
    depth buffer instead (view-space distance, log scale, magenta = no depth written). cloud_lab replaces the weather
    with ONE isolated cloud for judging vertical development, shape and
    self-shadowing: {enabled, type (0.05 stratus, 0.2 stratocumulus, 0.32
    nimbostratus, 0.5 cumulus, 0.72 congestus, 1.0 cumulonimbus), coverage, cirrus
    (anvil / high cloud), precipitation, radius_meters, height_scale (exaggerates
    vertical growth, 0.25-4), distance_meters (ahead of the camera along the
    ground), sun_override + sun_elevation_degrees + sun_azimuth_degrees (light it
    from a chosen sun), maturity (life cycle: 0 towering cumulus, 0.3 growing
    cumulonimbus, 0.6 mature with anvil, 0.9 dissipating), organisation (0 single
    cell, 0.5 multicell of mixed ages, 1 organised), density (0.2-6), cirrus_sheet (0-1:
    patchy thin cirrus on the anti-sun side of the cell, where its shadow falls), seed, place
    (true places the cloud ahead of the camera now)}. The chosen
    level range is reported as clipmap_plan by orbit_view_text_diagnostics.
    Transient view state."""
    params: dict[str, Any] = {"id": view_id}
    for key, value in {
        "production_surface": production_surface,
        "full_clipmap": full_clipmap,
        "macro_globe": macro_globe,
        "ocean": ocean,
        "surface_effects": surface_effects,
        "lod_bias_stops": lod_bias_stops,
        "dynamic_clipmaps": dynamic_clipmaps,
        "clipmap_pixels_per_vertex": clipmap_pixels_per_vertex,
        "clipmap_fade_seconds": clipmap_fade_seconds,
        "experimental_distance_bands": experimental_distance_bands,
        "clipmap_band_edges_meters": clipmap_band_edges_meters,
        "clipmap_band_scale": clipmap_band_scale,
        "clipmap_partial_updates": clipmap_partial_updates,
        "physical_pages": physical_pages,
        "clouds": clouds,
        "cloud_lab": cloud_lab,
        "cloud_resolution_scale": cloud_resolution_scale,
        "cloud_godray_strength": cloud_godray_strength,
        "cloud_light_volume": cloud_light_volume,
        "bypass_cloud_shadow": bypass_cloud_shadow,
        "bypass_indirect_lighting": bypass_indirect_lighting,
        "bypass_near_field_water": bypass_near_field_water,
        "bypass_hybrid_reflections": bypass_hybrid_reflections,
        "bypass_radiance_cache": bypass_radiance_cache,
        "bypass_atmosphere": bypass_atmosphere,
        "indirect_coverage_view": indirect_coverage_view,
        "cloud_volume_debug_altitude": cloud_volume_debug_altitude,
        "cloud_temporal": cloud_temporal,
    }.items():
        if value is not None:
            params[key] = value
    return _rpc("view.terrain_layers_set", params)


@mcp.tool()
def orbit_view_text_diagnostics(
    view_id: str = "studio.primary",
    cursor_u: float | None = None,
    cursor_v: float | None = None,
) -> dict[str, Any]:
    """Full numeric + text diagnostic for a viewport: camera position, heading,
    pitch, distance from the planet core, height above datum (sea level), above the
    terrain and above the water surface, and for the point below the camera (and
    under the cursor when cursor_u/cursor_v in [0,1] are given) latitude/longitude,
    terrain/coarse elevation, water depth, radius from core, slope, downhill
    bearing, climate and biome weights. `text` is what the viewport HUD draws."""
    params: dict[str, Any] = {"id": view_id}
    if cursor_u is not None and cursor_v is not None:
        params["cursor_u"] = cursor_u
        params["cursor_v"] = cursor_v
    return _rpc("view.text_diagnostics", params)


@mcp.tool()
def orbit_view_text_diagnostics_set(
    enabled: bool,
    view_id: str = "studio.primary",
) -> dict[str, Any]:
    """Show or hide the text diagnostics HUD drawn over a viewport (the 'Text
    readout' checkbox in the viewport Diagnostics properties)."""
    return _rpc("view.text_diagnostics_set", {"id": view_id, "enabled": enabled})


@mcp.tool()
def orbit_project_discover(roots: list[str] | None = None) -> list[dict[str, Any]]:
    """Scan the usual folders (Documents, Desktop, Downloads, Orbit's Projects
    folder) for Orbit projects, newest first. Pass roots to scan other folders.
    Open one with orbit_project_open."""
    return _rpc("project.discover", {"roots": roots} if roots else {})


@mcp.tool()
def orbit_project_recent() -> list[dict[str, Any]]:
    """List recently opened projects (most recent first) with availability."""
    return _rpc("project.recent")


@mcp.tool()
def orbit_world_active() -> dict[str, Any]:
    """Return the open authoring world and session generation."""
    return _rpc("world.active")


@mcp.tool()
def orbit_world_list() -> list[dict[str, Any]]:
    """Return the project's world-document catalog."""
    return _rpc("world.list")


@mcp.tool()
def orbit_world_describe(path: str) -> dict[str, Any]:
    """Describe one project world document by relative path."""
    return _rpc("world.describe", {"path": path})


@mcp.tool()
def orbit_world_create(path: str, display_name: str) -> dict[str, Any]:
    """Create a versioned world document, e.g. path='Worlds/Moon.orbitworld'."""
    return _rpc("world.create", {"path": path, "display_name": display_name})


@mcp.tool()
def orbit_world_open(path: str) -> dict[str, Any]:
    """Atomically switch the authoring session to another project world."""
    return _rpc("world.open", {"path": path})


@mcp.tool()
def orbit_world_close() -> dict[str, Any]:
    """Close the active world; project-level RPC stays available."""
    return _rpc("world.close")


@mcp.tool()
def orbit_world_set_startup(path: str) -> dict[str, Any]:
    """Persist which world opens first, without opening it."""
    return _rpc("world.set_startup", {"path": path})


@mcp.tool()
def orbit_world_set_display_name(path: str, display_name: str) -> dict[str, Any]:
    """Change a world's display name without renaming its file."""
    return _rpc(
        "world.set_display_name",
        {"path": path, "display_name": display_name},
    )


@mcp.tool()
def orbit_body_list() -> list[dict[str, Any]]:
    """List celestial body objects in the active world."""
    return _rpc("body.list")


@mcp.tool()
def orbit_body_create(parent_id: str, name: str | None = None) -> dict[str, Any]:
    """Create a celestial body under a Celestial System through the shared command.

    Returns the body with Earth-like defaults; author it with orbit_property_set
    (see docs/ORBIT_MCP.md for the body property IDs and a Moon recipe).
    """
    params: dict[str, Any] = {"parent": parent_id}
    if name is not None:
        params["name"] = name
    return _rpc("body.create", params)


@mcp.tool()
def orbit_body_capabilities(body_id: str) -> list[dict[str, Any]]:
    """Return the real capability domains registered for one body."""
    return _rpc("body.capabilities", {"body": body_id})


@mcp.tool()
def orbit_body_set_capability(
    body_id: str,
    capability: str,
    enabled: bool,
) -> dict[str, Any]:
    """Enable or disable a body capability (currently 'surface.terrain')."""
    return _rpc(
        "body.set_capability",
        {"body": body_id, "capability": capability, "enabled": enabled},
    )


@mcp.tool()
def orbit_viewport_focus_body() -> dict[str, Any]:
    """Frame the primary viewport on its current target body (the selection)."""
    return _rpc("viewport.focus_body")


@mcp.tool()
def orbit_cpu_timings() -> dict[str, Any]:
    """Return last and rolling 120-frame CPU timings for the live Studio frame loop."""
    return _rpc("studio.cpu_timings")


@mcp.tool()
def orbit_renderdoc_status() -> dict[str, Any]:
    """RenderDoc state: available (Studio was launched with ORBIT_RENDERDOC=1 and
    RenderDoc is installed), capturing, and last_capture_path (the newest .rdc)."""
    return _rpc("renderdoc.status")


@mcp.tool()
def orbit_renderdoc_capture() -> dict[str, Any]:
    """Capture the next presented frame with RenderDoc. Poll orbit_renderdoc_status
    until capturing is false and last_capture_path changes, then open the .rdc in
    the RenderDoc UI to step through passes. Errors if RenderDoc is unavailable."""
    return _rpc("renderdoc.capture")


@mcp.tool()
def orbit_profiler_status() -> dict[str, Any]:
    """CPU micro-profiler state: configuration, frame-time summary, the last 120
    frame times, and recent hitch captures (Perfetto traces with one lane per
    thread and per CPU core, plus the stack frames that dominated each stall)."""
    return _rpc("profiler.status")


@mcp.tool()
def orbit_profiler_configure(
    enabled: bool | None = None,
    hitch_threshold_ms: float | None = None,
    stall_threshold_ms: float | None = None,
    capture_window_ms: float | None = None,
    max_hitch_files: int | None = None,
) -> dict[str, Any]:
    """Change the micro-profiler: turn it on/off, the frame length that counts as
    a hitch (written to disk), the length at which stack sampling starts, how much
    history a capture keeps, and how many hitch files are retained."""
    params = {
        key: value
        for key, value in {
            "enabled": enabled,
            "hitch_threshold_ms": hitch_threshold_ms,
            "stall_threshold_ms": stall_threshold_ms,
            "capture_window_ms": capture_window_ms,
            "max_hitch_files": max_hitch_files,
        }.items()
        if value is not None
    }
    return _rpc("profiler.configure", params)


@mcp.tool()
def orbit_profiler_capture(
    window_ms: float | None = None, path: str | None = None
) -> dict[str, Any]:
    """Write the last window_ms of every thread and CPU core as a Chrome/Perfetto
    trace (open at https://ui.perfetto.dev) and return its path."""
    params: dict[str, Any] = {}
    if window_ms is not None:
        params["window_ms"] = window_ms
    if path is not None:
        params["path"] = path
    return _rpc("profiler.capture", params)


@mcp.tool()
def orbit_profiler_panel_get() -> dict[str, Any]:
    """State of the Studio Profiler panel: paused or live, snapshot source (live
    or a loaded hitch file), options, visible time range and selected slice."""
    return _rpc("profiler.panel_get")


@mcp.tool()
def orbit_profiler_panel_set(
    paused: bool | None = None,
    window_ms: float | None = None,
    grouping: str | None = None,
    freeze_on_hitch: bool | None = None,
    min_slice_ms: float | None = None,
    filter: str | None = None,
    reset_view: bool | None = None,
    view_begin_ms: float | None = None,
    view_span_ms: float | None = None,
    zoom_frame: int | None = None,
    select_thread: str | None = None,
    select_time_ms: float | None = None,
    clear_selection: bool | None = None,
    zoom_to_selection: bool | None = None,
    load_trace: str | None = None,
) -> dict[str, Any]:
    """Drive the Profiler panel like its controls. paused pauses/resumes the live
    capture; grouping is 'threads' or 'cores'; view_begin_ms/view_span_ms are
    relative to the snapshot start; zoom_frame picks a frame (-1 = longest);
    select_thread + select_time_ms selects the slice there; load_trace opens a
    hitch or capture file (paused). Returns the new panel state."""
    params: dict[str, Any] = {
        key: value
        for key, value in {
            "paused": paused,
            "window_ms": window_ms,
            "grouping": grouping,
            "freeze_on_hitch": freeze_on_hitch,
            "min_slice_ms": min_slice_ms,
            "filter": filter,
            "reset_view": reset_view,
            "view_begin_ms": view_begin_ms,
            "view_span_ms": view_span_ms,
            "zoom_frame": zoom_frame,
            "zoom_to_selection": zoom_to_selection,
            "load_trace": load_trace,
        }.items()
        if value is not None
    }
    if select_thread is not None and select_time_ms is not None:
        params["select"] = {"thread": select_thread, "time_ms": select_time_ms}
    elif clear_selection:
        params["select"] = None
    return _rpc("profiler.panel_set", params)


@mcp.tool()
def orbit_profiler_snapshot(top_scopes: int = 15, slowest: int = 15) -> dict[str, Any]:
    """Analysis of what the Profiler panel shows (live window, frozen snapshot or
    loaded hitch file) within its visible range: frame stats, per-lane busy time,
    heaviest scopes, heaviest GPU passes (GPU time per render pass), longest
    slices (thread, core, self time) and stall stack samples. Pause first with orbit_profiler_panel_set(paused=True) to analyse a
    fixed moment."""
    return _rpc("profiler.snapshot", {"top_scopes": top_scopes, "slowest": slowest})


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
def orbit_object_delete(object_id: str) -> dict[str, Any]:
    """Delete one leaf object (no children) as an undoable command."""
    return _rpc("object.delete", {"id": object_id})


@mcp.tool()
def orbit_object_duplicate(object_id: str) -> dict[str, Any]:
    """Clone one leaf object with its stored properties; returns the new id."""
    return _rpc("object.duplicate", {"id": object_id})


@mcp.tool()
def orbit_celestial_create_from_recipe(
    kind: str,
    parent_id: str,
    name: str | None = None,
    radius_m: float | None = None,
    mass_kg: float | None = None,
    density_kg_m3: float | None = None,
    temperature_k: float | None = None,
    rotation_period_s: float | None = None,
    semi_major_axis_m: float | None = None,
    eccentricity: float | None = None,
    inclination_deg: float | None = None,
    central_mu_m3_s2: float | None = None,
    atmosphere: bool | None = None,
    ocean: bool | None = None,
    synchronous_rotation: bool | None = None,
    seed: int = 1,
) -> dict[str, Any]:
    """Create a star, rocky_planet or moon from Orbit's physical recipes.

    parent_id is a celestial system/reference node for star and rocky_planet,
    and a celestial body for moon. Omitted numbers keep the recipe defaults
    (Sun / Earth / Moon). One undoable transaction."""
    params: dict[str, Any] = {"kind": kind, "parent": parent_id, "seed": seed}
    optional = {
        "name": name,
        "radius_m": radius_m,
        "mass_kg": mass_kg,
        "density_kg_m3": density_kg_m3,
        "temperature_k": temperature_k,
        "rotation_period_s": rotation_period_s,
        "semi_major_axis_m": semi_major_axis_m,
        "eccentricity": eccentricity,
        "inclination_deg": inclination_deg,
        "central_mu_m3_s2": central_mu_m3_s2,
        "atmosphere": atmosphere,
        "ocean": ocean,
        "synchronous_rotation": synchronous_rotation,
    }
    params.update({k: v for k, v in optional.items() if v is not None})
    return _rpc("celestial.create_from_recipe", params)


@mcp.tool()
def orbit_world_ensure_planet_surfaces() -> dict[str, Any]:
    """Give every spherical planet/moon without one a Terrain Surface (one undo
    step). Stars, giants, compact objects and ellipsoid bodies are skipped.
    New planets already get a surface automatically."""
    return _rpc("world.ensure_planet_surfaces", {})


@mcp.tool()
def orbit_celestial_capabilities(body_id: str) -> list[dict[str, Any]]:
    """List a celestial body's capability domains (atmosphere, clouds, rings, ...)
    with state 'absent', 'enabled' or 'disabled'."""
    return _rpc("celestial.capabilities", {"body": body_id})


@mcp.tool()
def orbit_celestial_set_capability(
    body_id: str,
    type_id: str,
    enabled: bool,
) -> dict[str, Any]:
    """Enable/disable a capability on a celestial body. Enabling a missing one
    creates it; disabling keeps its authored values. Use type_id from
    orbit_celestial_capabilities."""
    return _rpc(
        "celestial.set_capability",
        {"body": body_id, "type": type_id, "enabled": enabled},
    )


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
    """Capture the completed offscreen Studio RenderView to an image file: PNG, or
    a 32-bit BMP when the path ends in .bmp."""
    return _rpc("viewport.screenshot", {"path": path})


@mcp.tool()
def orbit_events_since(sequence: int = 0) -> dict[str, Any]:
    """Replay editor events newer than sequence.

    Events are sequenced by Orbit and retained in a bounded journal, covering
    semantic object changes, selection, content, plugins and viewport changes.
    """
    return _rpc("event.since", {"sequence": sequence})


# ---------------------------------------------------------------------------
# Studio panels
# ---------------------------------------------------------------------------


@mcp.tool()
def orbit_panel_list() -> list[dict[str, Any]]:
    """List every editor panel (tab) with whether it is open and visible."""
    return _rpc("studio.panel_list")


@mcp.tool()
def orbit_panel_focus(title: str) -> dict[str, Any]:
    """Open a panel by title (case-insensitive) and bring its tab to the front."""
    return _rpc("studio.panel_focus", {"title": title})


@mcp.tool()
def orbit_viewport_navigate(
    delta_seconds: float = 0.016,
    mouse_dx: float = 0.0,
    mouse_dy: float = 0.0,
    move_right: float = 0.0,
    move_forward: float = 0.0,
    move_up: float = 0.0,
    boost: bool = False,
) -> dict[str, Any]:
    """Apply one camera navigation step to the primary viewport, like the
    right-mouse look + WASD/QE gesture. Uses terrain navigation when the active
    body has terrain, otherwise the same navigation on a reference sphere. Call repeatedly to move continuously."""
    return _rpc(
        "viewport.navigate",
        {
            "delta_seconds": delta_seconds,
            "mouse_dx": mouse_dx,
            "mouse_dy": mouse_dy,
            "move_right": move_right,
            "move_forward": move_forward,
            "move_up": move_up,
            "boost": boost,
        },
    )


@mcp.tool()
def orbit_view_mode_set(mode: str, view_id: str = "studio.primary") -> dict[str, Any]:
    """Set a viewport's mode: perspective, body_map, debug or system."""
    return _rpc("view.mode_set", {"id": view_id, "mode": mode})


@mcp.tool()
def orbit_view_debug_field_set(
    field: str | None = None,
    view_id: str = "studio.primary",
) -> dict[str, Any]:
    """Pick the terrain data field shown in debug mode (e.g. 'Drainage',
    'Final Biome', 'Scatter Density'). Call without a field to list them."""
    params: dict[str, Any] = {"id": view_id}
    if field is not None:
        params["field"] = field
    return _rpc("view.debug_field_set", params)


@mcp.tool()
def orbit_workspace_get() -> dict[str, Any]:
    """Return the active Studio workspace mode."""
    return _rpc("studio.workspace_get", {})


@mcp.tool()
def orbit_workspace_set(mode: str) -> dict[str, Any]:
    """Switch workspace mode: Scene, Planet, Celestial, Simulation or Shading."""
    return _rpc("studio.workspace_set", {"mode": mode})


@mcp.tool()
def orbit_bubble_open(object_id: str) -> dict[str, Any]:
    """Open an object's parameter bubble in the active mode toolbar."""
    return _rpc("studio.bubble_open", {"id": object_id})


@mcp.tool()
def orbit_panel_close(title: str) -> dict[str, Any]:
    """Close a panel by title; reopen it with orbit_panel_focus."""
    return _rpc("studio.panel_close", {"title": title})


# ---------------------------------------------------------------------------
# Debug tab: which GBuffer channel each Studio viewport renders
# ---------------------------------------------------------------------------


@mcp.tool()
def orbit_view_surface_debug_get(view_id: str = "studio.primary") -> dict[str, Any]:
    """Return which GBuffer channel a Studio viewport is currently showing.

    view_id is a RenderView slot: "studio.primary" (Viewport) or
    "studio.map" (Body Map / Debug View).
    """
    return _rpc("view.surface_debug_get", {"id": view_id})


@mcp.tool()
def orbit_view_surface_debug_set(
    surface_debug_mode: str,
    view_id: str = "studio.primary",
) -> dict[str, Any]:
    """Switch which GBuffer channel a Studio viewport renders.

    surface_debug_mode: lit | base_color_roughness | normal_metallic |
    emission_metadata. view_id is a RenderView slot: "studio.primary"
    (Viewport) or "studio.map" (Body Map / Debug View). This is the same
    switch as the Debug tab's toolbar and each Viewport panel's own
    "Surface View" buttons.
    """
    return _rpc(
        "view.surface_debug_set",
        {"id": view_id, "surface_debug_mode": surface_debug_mode},
    )


# ---------------------------------------------------------------------------
# Shading tab: content tree, shaders, materials, preview
#
# Paths may be project-relative ("Content/Shading/Lunar.shade.hlsl") or
# content-relative ("Shading/Lunar.shade.hlsl"). Shader edits are compiled
# live inside the running Studio; nothing here restarts it. See
# docs/ORBIT_SHADING.md.
# ---------------------------------------------------------------------------


@mcp.tool()
def orbit_shading_options() -> dict[str, Any]:
    """List preview shapes, lighting presets, backgrounds and shader templates."""
    return _rpc("shading.options")


@mcp.tool()
def orbit_shading_tree() -> dict[str, Any]:
    """Return the Content tree (folders and assets, nested) shown in the tab."""
    return _rpc("shading.tree")


@mcp.tool()
def orbit_shading_folder_create(path: str) -> dict[str, Any]:
    """Create a folder under Content."""
    return _rpc("shading.folder_create", {"path": path})


@mcp.tool()
def orbit_shading_rename(path: str, name: str) -> dict[str, Any]:
    """Rename a file or folder in place (name is a single path component)."""
    return _rpc("shading.rename", {"path": path, "name": name})


@mcp.tool()
def orbit_shading_move(path: str, folder: str) -> dict[str, Any]:
    """Move a file or folder into an existing folder."""
    return _rpc("shading.move", {"path": path, "folder": folder})


@mcp.tool()
def orbit_shading_trash(path: str) -> dict[str, Any]:
    """Reversibly remove a file or folder (it moves to .orbit/Trash)."""
    return _rpc("shading.trash", {"path": path})


@mcp.tool()
def orbit_shading_shader_create(
    folder: str,
    name: str,
    template: str = "lit",
) -> dict[str, Any]:
    """Create <folder>/<name>.shade.hlsl from a template and open it.

    Templates: lit, unlit, lunar_regolith, debug_normals (see orbit_shading_options).
    """
    return _rpc(
        "shading.shader_create",
        {"folder": folder, "name": name, "template": template},
    )


@mcp.tool()
def orbit_shading_material_create(
    folder: str,
    name: str,
    shader: str,
) -> dict[str, Any]:
    """Create <folder>/<name>.orbitshadermaterial bound to a shader and open it."""
    return _rpc(
        "shading.material_create",
        {"folder": folder, "name": name, "shader": shader},
    )


@mcp.tool()
def orbit_shading_select(path: str | None = None) -> dict[str, Any]:
    """Open a shader or shader material in the tab (null clears the selection)."""
    return _rpc("shading.select", {"path": path})


@mcp.tool()
def orbit_shading_source_read(path: str) -> dict[str, Any]:
    """Read the text of a shader source file."""
    return _rpc("shading.source_read", {"path": path})


@mcp.tool()
def orbit_shading_source_write(path: str, text: str) -> dict[str, Any]:
    """Write a *.shade.hlsl file, open it, and compile it live.

    A compile error is a normal result: check `compiled` and `diagnostics`
    (they name the shader file and line). The preview keeps the last working
    shader while there are errors.
    """
    return _rpc("shading.source_write", {"path": path, "text": text})


@mcp.tool()
def orbit_shading_status() -> dict[str, Any]:
    """Return selection, compile status/diagnostics, parameters and preview settings."""
    return _rpc("shading.status")


@mcp.tool()
def orbit_shading_param_set(name: str, value: Any) -> dict[str, Any]:
    """Set a shader parameter: a number or list for float/color3, or a
    content-relative path string (e.g. "Content/Textures/Rock.jpg") for a
    texture2d parameter. Saved into an open shader material."""
    return _rpc("shading.param_set", {"name": name, "value": value})


@mcp.tool()
def orbit_shading_param_reset(name: str) -> dict[str, Any]:
    """Remove a parameter override so the shader's declared default applies."""
    return _rpc("shading.param_reset", {"name": name})


@mcp.tool()
def orbit_shading_preview_get() -> dict[str, Any]:
    """Return the preview settings (shape, lighting, background, sun, camera)."""
    return _rpc("shading.preview_get")


@mcp.tool()
def orbit_shading_preview_set(
    shape: str | None = None,
    lighting: str | None = None,
    background: str | None = None,
    sun_azimuth_degrees: float | None = None,
    sun_elevation_degrees: float | None = None,
    exposure: float | None = None,
    model_yaw_degrees: float | None = None,
    animate: bool | None = None,
    live_compile: bool | None = None,
    camera_yaw_degrees: float | None = None,
    camera_pitch_degrees: float | None = None,
    camera_distance: float | None = None,
    camera_fov_degrees: float | None = None,
    mesh: str | None = None,
    clear_mesh: bool = False,
) -> dict[str, Any]:
    """Change preview settings; omitted values are unchanged.

    shape: sphere | plane | cube | mesh. lighting: studio | sun | overcast |
    sunset | space. background: environment | gradient | gray | checker.

    mesh is a Wavefront .obj path under Content: it loads the mesh and switches
    the shape to 'mesh' (the model is centred and fitted to the preview). A
    mesh saved elsewhere reloads automatically. clear_mesh=True removes it.
    """
    params: dict[str, Any] = {}
    if mesh is not None:
        params["mesh"] = mesh
    elif clear_mesh:
        params["mesh"] = None
    for key, value in (
        ("shape", shape),
        ("lighting", lighting),
        ("background", background),
        ("sun_azimuth_degrees", sun_azimuth_degrees),
        ("sun_elevation_degrees", sun_elevation_degrees),
        ("exposure", exposure),
        ("model_yaw_degrees", model_yaw_degrees),
        ("animate", animate),
        ("live_compile", live_compile),
    ):
        if value is not None:
            params[key] = value

    camera: dict[str, Any] = {}
    for key, value in (
        ("yaw_degrees", camera_yaw_degrees),
        ("pitch_degrees", camera_pitch_degrees),
        ("distance", camera_distance),
        ("fov_degrees", camera_fov_degrees),
    ):
        if value is not None:
            camera[key] = value
    if camera:
        params["camera"] = camera

    return _rpc("shading.preview_set", params)


@mcp.tool()
def orbit_shading_recompile() -> dict[str, Any]:
    """Recompile the open shader from the editor buffer and return the status."""
    return _rpc("shading.recompile")


@mcp.tool()
def orbit_shading_screenshot(path: str) -> dict[str, Any]:
    """Capture the Shading preview to a BMP.

    The tab must have rendered at least once: open it with orbit_panel_focus
    or select a shader first.
    """
    return _rpc("shading.screenshot", {"path": path})


@mcp.tool()
def orbit_time_get() -> dict[str, Any]:
    """The simulation clock that drives planetary rotation, orbits, the sun and
    the atmosphere/weather: playing or paused, rate (simulation seconds per real
    second), time since the epoch (microseconds, seconds, text) and the step
    size of the Step buttons. Studio starts paused."""
    return _rpc("time.get")


@mcp.tool()
def orbit_time_set(
    playing: bool | None = None,
    rate: float | None = None,
    time_microseconds: int | None = None,
    step_seconds: float | None = None,
) -> dict[str, Any]:
    """Drive the simulation transport (the Simulate / Pause band at the bottom
    of Studio). playing=True simulates, False pauses; rate is simulation seconds
    per real second (negative runs backwards); time_microseconds jumps to an
    absolute time; step_seconds sets the size of the Step buttons."""
    params: dict[str, Any] = {}
    for key, value in {
        "playing": playing,
        "rate": rate,
        "time_microseconds": time_microseconds,
        "step_seconds": step_seconds,
    }.items():
        if value is not None:
            params[key] = value
    return _rpc("time.set", params)


@mcp.tool()
def orbit_time_step(seconds: float | None = None) -> dict[str, Any]:
    """Advance the simulation clock by `seconds` (negative rewinds), whether it
    is playing or paused: the Step buttons. Defaults to the configured step
    size."""
    return _rpc("time.step", {} if seconds is None else {"seconds": seconds})


@mcp.tool()
def orbit_viewport_pose_get(view_id: str = "studio.primary") -> dict[str, Any]:
    """Exact camera pose of a Studio perspective view: target body object id,
    planet-fixed observer position, surface frame and look angles. Pass it to
    orbit_viewport_pose_set to put the camera back."""
    return _rpc("viewport.pose_get", {"id": view_id})


@mcp.tool()
def orbit_viewport_pose_set(
    pose_json: str,
    view_id: str = "studio.primary",
) -> dict[str, Any]:
    """Restore a pose returned by orbit_viewport_pose_get (as JSON text). The
    view's target body must already be the pose's target_object (select it
    first); otherwise restored is false."""
    params = json.loads(pose_json)
    if not isinstance(params, dict):
        raise ValueError("pose_json must decode to a JSON object.")
    params["id"] = view_id
    return _rpc("viewport.pose_set", params)


@mcp.tool()
def orbit_reports_list(
    status: str | None = None,
    scope: str | list[str] | None = None,
    transient: bool | None = None,
    text: str | None = None,
) -> list[dict[str, Any]]:
    """List issue reports, newest first. status: unresolved | pending |
    resolved. scope: performance | visual_quality | bug | crash | other (one or
    several). transient filters problems that come and go. text matches the
    title, description or a tag."""
    params: dict[str, Any] = {}
    for key, value in {
        "status": status,
        "scope": scope,
        "transient": transient,
        "text": text,
    }.items():
        if value is not None:
            params[key] = value
    return _rpc("reports.list", params)


@mcp.tool()
def orbit_reports_get(id: str) -> dict[str, Any]:
    """One report (id 7 or "R-0007") with its starting condition and, for a
    transient report, its ending condition: simulation time/rate, project,
    world, camera pose, view diagnostics text and frame-time statistics."""
    return _rpc("reports.get", {"id": id})


@mcp.tool()
def orbit_reports_create(
    title: str = "",
    description: str | None = None,
    scopes: list[str] | None = None,
    tags: list[str] | None = None,
    transient: bool = False,
    status: str | None = None,
    capture_start: bool = True,
    screenshot: bool = True,
) -> dict[str, Any]:
    """Raise an issue report. scopes: any of performance, visual_quality, bug,
    crash, other. transient=True is a problem that comes and goes: it gets a
    starting condition now and an ending condition later
    (orbit_reports_capture which="end"). capture_start captures the situation
    (time, camera, location, diagnostics) right now, with a PNG screenshot of the
    viewport (screenshot=False skips it); the file is the condition's
    screenshot_path in the result."""
    params: dict[str, Any] = {
        "title": title,
        "transient": transient,
        "capture_start": capture_start,
        "screenshot": screenshot,
    }
    for key, value in {
        "description": description,
        "scopes": scopes,
        "tags": tags,
        "status": status,
    }.items():
        if value is not None:
            params[key] = value
    return _rpc("reports.create", params)


@mcp.tool()
def orbit_reports_update(
    id: str,
    title: str | None = None,
    description: str | None = None,
    resolution_note: str | None = None,
    status: str | None = None,
    scopes: list[str] | None = None,
    tags: list[str] | None = None,
    transient: bool | None = None,
) -> dict[str, Any]:
    """Edit a report: status (unresolved | pending | resolved), scopes, tags,
    text fields. Making a report non-transient drops its ending condition."""
    params: dict[str, Any] = {"id": id}
    for key, value in {
        "title": title,
        "description": description,
        "resolution_note": resolution_note,
        "status": status,
        "scopes": scopes,
        "tags": tags,
        "transient": transient,
    }.items():
        if value is not None:
            params[key] = value
    return _rpc("reports.update", params)


@mcp.tool()
def orbit_reports_capture(
    id: str,
    which: str = "start",
    screenshot: bool = True,
) -> dict[str, Any]:
    """Capture the situation Studio is in now into a report. which="start"
    replaces the starting condition; which="end" sets the ending condition of a
    transient report (a non-transient report has none). A PNG screenshot of the
    viewport is attached too (screenshot=False skips it)."""
    return _rpc(
        "reports.capture", {"id": id, "which": which, "screenshot": screenshot}
    )


@mcp.tool()
def orbit_reports_show(id: str) -> dict[str, Any]:
    """Open Studio's Reports panel on a report (its screenshot, conditions and
    notes), the same as clicking its row."""
    return _rpc("reports.show", {"id": id})


@mcp.tool()
def orbit_reports_open_screenshot(id: str, which: str = "start") -> dict[str, Any]:
    """Show a report condition's screenshot file in the file browser, like the
    "Show file" button in the Reports panel. Returns the file path."""
    return _rpc("reports.open_screenshot", {"id": id, "which": which})


@mcp.tool()
def orbit_reports_restore(id: str, which: str = "start") -> dict[str, Any]:
    """Recreate the situation a report was captured in: the simulation clock
    goes to the captured time and rate (paused), the captured body is selected
    and the camera returns to the captured pose. pose is "pending" while the
    body loads and finishes on its own."""
    return _rpc("reports.restore", {"id": id, "which": which})


@mcp.tool()
def orbit_reports_delete(id: str) -> dict[str, Any]:
    """Delete a report. Ids are never reused."""
    return _rpc("reports.delete", {"id": id})


@mcp.tool()
def orbit_reports_export(
    id: str | None = None,
    status: str | None = None,
    path: str | None = None,
) -> dict[str, Any]:
    """Markdown write-up of one report (id) or of every report matching status.
    With path, also writes it to that file."""
    params: dict[str, Any] = {}
    for key, value in {"id": id, "status": status, "path": path}.items():
        if value is not None:
            params[key] = value
    return _rpc("reports.export", params)


@mcp.tool()
def orbit_view_zoom_get(view_id: str = "studio.primary") -> dict[str, Any]:
    """Camera zoom of a viewport: a telephoto factor on the field of view
    (1 = default, 2 = half the angle). Range 0.5 to 100."""
    return _rpc("view.zoom_get", {"id": view_id})


@mcp.tool()
def orbit_view_zoom_set(
    zoom: float,
    view_id: str = "studio.primary",
) -> dict[str, Any]:
    """Set the camera zoom (the viewport Zoom control / mouse wheel). Clamped
    to 0.5..100. Returns the zoom applied."""
    return _rpc("view.zoom_set", {"id": view_id, "zoom": zoom})


@mcp.tool()
def orbit_viewport_capture_start(
    kind: str = "fullscreen",
    width: int | None = None,
    height: int | None = None,
    path: str | None = None,
    settle_frames: int | None = None,
) -> dict[str, Any]:
    """Start a high-resolution capture of the primary viewport (the Screenshot
    and Ultra 16K buttons). kind: fullscreen (window resolution), ultra (16K:
    15360 px on the long side, keeping the viewport aspect) or custom (width and
    height up to 16384). Saves a PNG (default <project>/Screenshots; a .bmp path
    gives a BMP). The view is
    resized, given frames to converge, captured and restored, so this returns at
    once: poll orbit_viewport_capture_status until state is idle."""
    params: dict[str, Any] = {"kind": kind}
    for key, value in {
        "width": width,
        "height": height,
        "path": path,
        "settle_frames": settle_frames,
    }.items():
        if value is not None:
            params[key] = value
    return _rpc("viewport.capture_start", params)


@mcp.tool()
def orbit_viewport_screenshots_open() -> dict[str, Any]:
    """Open the folder screenshots are saved to (<project>/Screenshots) in the
    file browser, like the viewport "Screenshot Files" button. Returns its path."""
    return _rpc("viewport.screenshots_open")


@mcp.tool()
def orbit_viewport_capture_status() -> dict[str, Any]:
    """State of the high-resolution capture (idle, resizing, settling,
    restoring) and the outcome of the last one: path, size, file_bytes or
    error."""
    return _rpc("viewport.capture_status")


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
        mime_type="image/png",
    )
    def orbit_viewport_screenshot_resource() -> bytes:
        """Return a fresh PNG capture of Orbit Studio's primary viewport."""
        import tempfile

        with tempfile.NamedTemporaryFile(
            suffix=".png", delete=False
        ) as temporary:
            path = Path(temporary.name)

        try:
            _rpc("viewport.screenshot", {"path": str(path)})
            return path.read_bytes()
        finally:
            path.unlink(missing_ok=True)


if __name__ == "__main__":
    mcp.run()
