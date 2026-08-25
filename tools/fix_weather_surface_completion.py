from pathlib import Path

root = Path(__file__).resolve().parents[1]

def replace_once(path, old, new):
    p = root / path
    s = p.read_text(encoding="utf-8")
    if s.count(old) != 1:
        raise RuntimeError(f"{path}: expected one match, got {s.count(old)}")
    p.write_text(s.replace(old, new, 1), encoding="utf-8")

# Coalesce repeated per-frame requests while the same local terrain build is in flight.
replace_once("scripts/weather/surface_energy_bridge.gd",
'''var _local_uploaded_center := Vector3.ZERO
var _terrain_ref: WeakRef
''',
'''var _local_uploaded_center := Vector3.ZERO
var _local_requested_center := Vector3.ZERO
var _terrain_ref: WeakRef
''')
replace_once("scripts/weather/surface_energy_bridge.gd",
'''\t_local_uploaded_center = Vector3.ZERO
\t_local_request += 1
\tcall_deferred("_upload_global_surface_fields")
''',
'''\t_local_uploaded_center = Vector3.ZERO
\t_local_requested_center = Vector3.ZERO
\t_local_request += 1
\tcall_deferred("_upload_global_surface_fields")
''')
replace_once("scripts/weather/surface_energy_bridge.gd",
'''\t_local_uploaded_center = Vector3.ZERO
\t_local_request += 1
\t_push_solar_forcing()
''',
'''\t_local_uploaded_center = Vector3.ZERO
\t_local_requested_center = Vector3.ZERO
\t_local_request += 1
\t_push_solar_forcing()
''')
replace_once("scripts/weather/surface_energy_bridge.gd",
'''func _maybe_queue_local_surface() -> void:
\tif not _surface_fields_uploaded or _native == null or not Planet.ready_state or not _has_local_observer():
\t\treturn
\tvar center: Vector3 = WeatherSystem.local_center.normalized()
\tif center.length_squared() < 0.5:
\t\treturn
\tif _local_uploaded_center.length_squared() < 0.5:
\t\t_queue_local_surface()
\t\treturn
\tvar moved_m := acos(clampf(_local_uploaded_center.dot(center), -1.0, 1.0)) * Planet.cfg.planet_radius
\tif moved_m > LOCAL_CELL_M * 0.25:
\t\t_queue_local_surface()


func _queue_local_surface() -> void:
\t_local_request += 1
\tif _local_building:
\t\treturn
\t_start_local_surface_build()
''',
'''func _maybe_queue_local_surface() -> void:
\tif not _surface_fields_uploaded or _native == null or not Planet.ready_state or not _has_local_observer():
\t\treturn
\tvar center: Vector3 = WeatherSystem.local_center.normalized()
\tif center.length_squared() < 0.5:
\t\treturn
\tif _local_requested_center.length_squared() >= 0.5:
\t\tvar requested_m := acos(clampf(_local_requested_center.dot(center), -1.0, 1.0)) \\
\t\t\t* Planet.cfg.planet_radius
\t\tif requested_m <= LOCAL_CELL_M * 0.25:
\t\t\treturn
\tif _local_uploaded_center.length_squared() >= 0.5:
\t\tvar uploaded_m := acos(clampf(_local_uploaded_center.dot(center), -1.0, 1.0)) \\
\t\t\t* Planet.cfg.planet_radius
\t\tif uploaded_m <= LOCAL_CELL_M * 0.25:
\t\t\treturn
\t_queue_local_surface(false)


func _queue_local_surface(force_rebuild: bool = true) -> void:
\tif not _has_local_observer() or Planet.cfg == null:
\t\treturn
\tvar center := WeatherSystem.local_center.normalized()
\tif not force_rebuild and _local_requested_center.length_squared() >= 0.5:
\t\tvar duplicate_m := acos(clampf(_local_requested_center.dot(center), -1.0, 1.0)) \\
\t\t\t* Planet.cfg.planet_radius
\t\tif duplicate_m <= LOCAL_CELL_M * 0.25:
\t\t\treturn
\t_local_request += 1
\t_local_requested_center = center
\tif _local_building:
\t\treturn
\t_start_local_surface_build()
''')
replace_once("scripts/weather/surface_energy_bridge.gd",
'''\tif request == _local_request and _native != null and is_instance_valid(_native):
''',
'''\tif request == _local_request and _native != null and is_instance_valid(_native):
''')
# Terrain edits intentionally force a new build even when the center did not move.
replace_once("scripts/weather/surface_energy_bridge.gd",
'''\tif distance <= WeatherSystem.local_span_m * 0.72 + radius_m:
\t\t_queue_local_surface()
''',
'''\tif distance <= WeatherSystem.local_span_m * 0.72 + radius_m:
\t\t_queue_local_surface(true)
''')

# elevation_m already represents the physical exposed surface: ocean is 0 m and lakes
# retain their actual free-water elevation. Do not blend lakes back toward sea level.
p = root / "native/weather/src/weather_native.cpp"
s = p.read_text(encoding="utf-8")
s2 = s.replace('float h = std::lerp(local_surface.elevation_m[sc], 0.0f, water);',
               'float h = local_surface.elevation_m[sc];')
s2 = s2.replace('float h0 = std::lerp(local_surface.elevation_m[c], 0.0f, water0);',
                'float h0 = local_surface.elevation_m[c];')
s2 = s2.replace('float h1 = std::lerp(local_surface.elevation_m[sc], 0.0f, water1);',
                'float h1 = local_surface.elevation_m[sc];')
if s2 == s:
    raise RuntimeError("native lake-elevation replacements did not match")
p.write_text(s2, encoding="utf-8")
print("completion follow-up fixes applied")
