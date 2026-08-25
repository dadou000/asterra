extends Node
## Adaptive atmospheric refinement planner.
##
## This controller deliberately separates *where resolution is needed* from the
## numerical solver that will eventually execute each child domain. The current
## six-layer WeatherNative globe remains L0; this file already performs real
## disturbance detection, spherical clustering, moving-domain tracking,
## hysteresis, promotion/demotion and overlap merging. Requested regions are
## visualised on WeatherMap so the refinement policy can be tuned before the
## expensive dynamic-layer native solver is introduced.

signal regions_changed
signal region_promoted(region_id: int, level: int)
signal region_demoted(region_id: int, level: int)
signal region_created(region_id: int)
signal region_removed(region_id: int)

enum RefinementLevel {
	PLANET,
	DISTURBANCE,
	CONVECTION,
	STORM,
	TORNADO,
}

const LEVEL_NAMES := ["L0 planet", "L1 disturbance", "L2 convection", "L3 storm", "L4 tornado"]
# Final hierarchy targets. L0 is a target: the currently compiled global solver
# is still 256x128 (~86 km at Asterra's equator), which the debug panel reports.
const TARGET_CELL_KM := [20.0, 5.0, 1.0, 0.25, 0.075]
const TARGET_VERTICAL_LEVELS := [30, 48, 64, 84, 112]
const DOMAIN_KM := [0.0, 1100.0, 420.0, 120.0, 30.0]

# Enter/leave thresholds are intentionally different. A region has to become
# convincingly interesting before refinement is requested, but weak weather must
# persist for a while before expensive state is discarded.
const ENTER_SCORE := [0.0, 0.43, 0.64, 0.79, 0.90]
const EXIT_SCORE := [0.0, 0.28, 0.43, 0.57, 0.70]
const PROMOTION_HOLD_S := [0.0, 0.0, 12.0 * 60.0, 8.0 * 60.0, 4.0 * 60.0]
const DEMOTION_HOLD_S := [0.0, 60.0 * 60.0, 35.0 * 60.0, 18.0 * 60.0, 8.0 * 60.0]
const MIN_LEVEL_LIFETIME_S := [0.0, 2.0 * 3600.0, 55.0 * 60.0, 28.0 * 60.0, 12.0 * 60.0]

const PLANET_RADIUS_KM := 3500.0
const SCAN_INTERVAL_REAL_S := 0.50
const CANDIDATE_TILE_W := 8
const CANDIDATE_TILE_H := 6
const MAX_RAW_CANDIDATES := 48
const MAX_ACTIVE_REGIONS := 14
const L1_NMS_DISTANCE_KM := 330.0
const TRACK_MATCH_DISTANCE_KM := 620.0
const MOTION_LEAD := 0.24
const OVERLAY_RADIUS := 1.012
const OVERLAY_SEGMENTS := 18

# The present L0 only has enough information to safely prewarm through L2. L3/L4
# must be promoted from actual child-domain storm metrics once those solvers are
# live; the planner will not invent tornado refinement from an 86-km cell.
const CURRENT_PARENT_MAX_REQUEST_LEVEL := RefinementLevel.CONVECTION

var regions: Array[Dictionary] = []
var enabled := true
var show_debug_overlay := true

var _scan_accum := 0.0
var _last_sim_seconds := 0.0
var _next_region_id := 1
var _previous_pressure := PackedFloat32Array()
var _convective_values := PackedFloat32Array()
var _last_candidates: Array[Dictionary] = []

var _debug_root: Node3D
var _debug_label: Label
var _debug_map_instance_id := 0
var _overlay_dirty := true
var _overlay_shader: Shader


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	_last_sim_seconds = CelestialSystem.simulation_seconds
	_overlay_shader = load("res://shaders/weather_refinement_patch.gdshader")


func _process(delta: float) -> void:
	if not enabled:
		_hide_debug_overlay()
		return
	_scan_accum += delta
	if _scan_accum >= SCAN_INTERVAL_REAL_S:
		_scan_accum = fmod(_scan_accum, SCAN_INTERVAL_REAL_S)
		_scan_weather()
	_update_debug_overlay()


func _scan_weather() -> void:
	var weather: PackedFloat32Array = WeatherSystem.global_weather_values
	var diagnostics: PackedFloat32Array = WeatherSystem.global_diagnostics_values
	var products: PackedFloat32Array = WeatherSystem.global_products_values
	var expected := WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H * 4
	if weather.size() != expected or diagnostics.size() != expected or products.size() != expected:
		return

	_refresh_convective_values(expected)
	var sim_now := CelestialSystem.simulation_seconds
	var sim_dt := maxf(sim_now - _last_sim_seconds, 0.0)
	_last_sim_seconds = sim_now
	# Avoid an external clock reset or pathological one-frame jump instantly aging
	# every nest through multiple life-cycle stages. Normal 8192x warp still passes
	# through because WeatherSystem advances the atmosphere in bounded steps.
	sim_dt = minf(sim_dt, 2.0 * 3600.0)

	var candidates := _extract_candidates(weather, diagnostics, products, _convective_values)
	_update_regions(candidates, sim_dt)
	_previous_pressure.resize(WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H)
	for c in WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H:
		_previous_pressure[c] = weather[c * 4 + 3]
	_last_candidates = candidates
	_overlay_dirty = true
	regions_changed.emit()


func _refresh_convective_values(expected: int) -> void:
	_convective_values = PackedFloat32Array()
	var native_value: Variant = WeatherSystem.get("_native")
	if not (native_value is Object):
		return
	var native := native_value as Object
	if not native.has_method(&"get_global_convective_rgba"):
		return
	var result: Variant = native.call(&"get_global_convective_rgba")
	if result is PackedFloat32Array and result.size() == expected:
		_convective_values = result


func _extract_candidates(weather: PackedFloat32Array, diagnostics: PackedFloat32Array,
		products: PackedFloat32Array, convective: PackedFloat32Array) -> Array[Dictionary]:
	var raw: Array[Dictionary] = []
	var width := WeatherSystem.GLOBAL_W
	var height := WeatherSystem.GLOBAL_H
	for ty in range(0, height, CANDIDATE_TILE_H):
		for tx in range(0, width, CANDIDATE_TILE_W):
			var best_score := 0.0
			var best_metrics: Dictionary = {}
			var best_x := -1
			var best_y := -1
			var y_end := mini(ty + CANDIDATE_TILE_H, height)
			var x_end := mini(tx + CANDIDATE_TILE_W, width)
			for y in range(ty, y_end):
				for x in range(tx, x_end):
					var c := x + y * width
					var metrics := _cell_metrics(c, weather, diagnostics, products, convective)
					var score := float(metrics["score"])
					if score > best_score:
						best_score = score
						best_metrics = metrics
						best_x = x
						best_y = y
			if best_x >= 0 and best_score >= ENTER_SCORE[RefinementLevel.DISTURBANCE] * 0.86:
				best_metrics["x"] = best_x
				best_metrics["y"] = best_y
				best_metrics["dir"] = _cell_direction(best_x, best_y)
				best_metrics["target_level"] = _target_level_from_parent(best_metrics)
				raw.append(best_metrics)

	raw.sort_custom(_candidate_score_desc)
	if raw.size() > MAX_RAW_CANDIDATES:
		raw.resize(MAX_RAW_CANDIDATES)

	# Spherical non-maximum suppression. Long fronts may retain several separated
	# candidates, while neighbouring 86-km cells inside one cyclone collapse to a
	# single moving refinement request.
	var filtered: Array[Dictionary] = []
	for candidate in raw:
		if int(candidate["target_level"]) <= RefinementLevel.PLANET:
			continue
		var keep := true
		for accepted in filtered:
			if _surface_distance_km(candidate["dir"], accepted["dir"]) < L1_NMS_DISTANCE_KM:
				keep = false
				break
		if keep:
			filtered.append(candidate)
		if filtered.size() >= MAX_ACTIVE_REGIONS:
			break
	return filtered


func _cell_metrics(c: int, weather: PackedFloat32Array, diagnostics: PackedFloat32Array,
		products: PackedFloat32Array, convective: PackedFloat32Array) -> Dictionary:
	var o := c * 4
	var cape := clampf(products[o + 2], 0.0, 1.0)
	var shear := clampf(diagnostics[o + 3], 0.0, 1.0)
	var vorticity := clampf(absf(diagnostics[o] - 0.5) * 2.0, 0.0, 1.0)
	var convergence := clampf((0.5 - diagnostics[o + 1]) * 2.0, 0.0, 1.0)
	var storm := clampf(weather[o + 1], 0.0, 1.0)
	var precip := clampf(weather[o + 2], 0.0, 1.0)
	var low_pressure := clampf((0.5 - weather[o + 3]) * 2.0, 0.0, 1.0)
	var pressure_fall := 0.0
	if _previous_pressure.size() == WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H:
		pressure_fall = clampf((_previous_pressure[c] - weather[o + 3]) * 12.0, 0.0, 1.0)

	var ascent := 0.0
	var downdraft := 0.0
	var anvil := 0.0
	var wind := 0.0
	if convective.size() == weather.size():
		ascent = clampf(convective[o], 0.0, 1.0)
		downdraft = clampf(convective[o + 1], 0.0, 1.0)
		anvil = clampf(convective[o + 2], 0.0, 1.0)
		wind = clampf(convective[o + 3], 0.0, 1.0)

	# Environmental opportunity deliberately dominates existing cloud/rain. This
	# lets a hot capped blue-sky dryline refine *before* convective initiation.
	var environment := (
		0.27 * cape
		+ 0.18 * shear
		+ 0.14 * convergence
		+ 0.08 * vorticity
		+ 0.07 * low_pressure
		+ 0.06 * pressure_fall
		+ 0.10 * sqrt(maxf(cape * shear, 0.0))
		+ 0.10 * sqrt(maxf(cape * convergence, 0.0))
	)
	var active_structure := (
		0.34 * ascent
		+ 0.20 * storm
		+ 0.12 * precip
		+ 0.12 * downdraft
		+ 0.10 * anvil
		+ 0.12 * wind
	)
	var score := clampf(environment * 0.82 + active_structure * 0.34, 0.0, 1.0)
	# Collocated storm-scale proxies are retained for future child-domain promotion
	# even though L0 is currently forbidden from directly requesting L3/L4.
	var mesocyclone_proxy := clampf(
		sqrt(maxf(ascent * vorticity, 0.0)) * (0.45 + 0.55 * shear) * (0.55 + 0.45 * convergence),
		0.0, 1.0)
	var tornado_proxy := clampf(
		pow(maxf(ascent * vorticity * convergence * shear, 0.0), 0.25)
		* (0.60 + 0.40 * downdraft) * (0.65 + 0.35 * low_pressure),
		0.0, 1.0)
	return {
		"score": score,
		"cape": cape,
		"shear": shear,
		"vorticity": vorticity,
		"convergence": convergence,
		"storm": storm,
		"precip": precip,
		"low_pressure": low_pressure,
		"pressure_fall": pressure_fall,
		"ascent": ascent,
		"downdraft": downdraft,
		"anvil": anvil,
		"wind": wind,
		"mesocyclone_proxy": mesocyclone_proxy,
		"tornado_proxy": tornado_proxy,
	}


func _target_level_from_parent(m: Dictionary) -> int:
	var score := float(m["score"])
	if score < ENTER_SCORE[RefinementLevel.DISTURBANCE]:
		return RefinementLevel.PLANET
	var target := RefinementLevel.DISTURBANCE
	var convective_trigger := (
		float(m["cape"]) >= 0.46
		and float(m["shear"]) >= 0.31
		and (
			float(m["convergence"]) >= 0.28
			or float(m["ascent"]) >= 0.16
			or float(m["storm"]) >= 0.18
			or float(m["pressure_fall"]) >= 0.24
		)
	)
	if score >= ENTER_SCORE[RefinementLevel.CONVECTION] and convective_trigger:
		target = RefinementLevel.CONVECTION
	return mini(target, CURRENT_PARENT_MAX_REQUEST_LEVEL)


func _update_regions(candidates: Array[Dictionary], sim_dt: float) -> void:
	for region in regions:
		region["seen"] = false
		region["age_s"] = float(region["age_s"]) + sim_dt
		region["level_age_s"] = float(region["level_age_s"]) + sim_dt

	for candidate in candidates:
		var match := _find_region_match(candidate)
		if match < 0:
			_create_region(candidate)
			continue
		_update_region_from_candidate(regions[match], candidate, sim_dt)

	var survivors: Array[Dictionary] = []
	for region in regions:
		if not bool(region["seen"]):
			region["weak_s"] = float(region["weak_s"]) + sim_dt
			region["strong_s"] = 0.0
			region["score"] = float(region["score"]) * exp(-sim_dt / 3600.0)
		_apply_level_lifecycle(region, sim_dt)
		if int(region["level"]) > RefinementLevel.PLANET:
			survivors.append(region)
		else:
			region_removed.emit(int(region["id"]))
	regions = survivors
	_merge_regions()
	regions.sort_custom(_region_priority_desc)
	if regions.size() > MAX_ACTIVE_REGIONS:
		for i in range(MAX_ACTIVE_REGIONS, regions.size()):
			region_removed.emit(int(regions[i]["id"]))
		regions.resize(MAX_ACTIVE_REGIONS)


func _find_region_match(candidate: Dictionary) -> int:
	var best := -1
	var best_distance := INF
	for i in regions.size():
		var region := regions[i]
		if bool(region["seen"]):
			continue
		var distance := _surface_distance_km(candidate["dir"], region["center_dir"])
		var radius_allowance := maxf(TRACK_MATCH_DISTANCE_KM, float(DOMAIN_KM[int(region["level"])]) * 0.62)
		if distance <= radius_allowance and distance < best_distance:
			best_distance = distance
			best = i
	return best


func _create_region(candidate: Dictionary) -> void:
	var region := {
		"id": _next_region_id,
		"level": RefinementLevel.DISTURBANCE,
		"target_level": int(candidate["target_level"]),
		"center_dir": candidate["dir"],
		"previous_dir": candidate["dir"],
		"score": float(candidate["score"]),
		"age_s": 0.0,
		"level_age_s": 0.0,
		"strong_s": 0.0,
		"weak_s": 0.0,
		"seen": true,
		"metrics": candidate.duplicate(),
	}
	_next_region_id += 1
	regions.append(region)
	region_created.emit(int(region["id"]))


func _update_region_from_candidate(region: Dictionary, candidate: Dictionary, sim_dt: float) -> void:
	region["seen"] = true
	var old_dir: Vector3 = region["center_dir"]
	var observed: Vector3 = candidate["dir"]
	# The displacement of the disturbance centroid is a stable motion estimate at
	# synoptic resolution. Lead the domain slightly downstream so growing storms do
	# not immediately run into the child boundary.
	var motion := observed - old_dir
	var led := (observed + motion * MOTION_LEAD).normalized()
	region["previous_dir"] = old_dir
	region["center_dir"] = (old_dir * 0.38 + led * 0.62).normalized()
	region["score"] = lerpf(float(region["score"]), float(candidate["score"]), 0.62)
	region["target_level"] = int(candidate["target_level"])
	region["metrics"] = candidate.duplicate()
	if int(region["target_level"]) > int(region["level"]):
		region["strong_s"] = float(region["strong_s"]) + sim_dt
		region["weak_s"] = 0.0
	elif int(region["target_level"]) < int(region["level"]):
		region["weak_s"] = float(region["weak_s"]) + sim_dt
		region["strong_s"] = 0.0
	else:
		region["strong_s"] = 0.0
		region["weak_s"] = 0.0


func _apply_level_lifecycle(region: Dictionary, _sim_dt: float) -> void:
	var level := int(region["level"])
	var target := int(region["target_level"])
	var score := float(region["score"])
	if target > level and level < RefinementLevel.TORNADO:
		var next_level := level + 1
		if float(region["strong_s"]) >= PROMOTION_HOLD_S[next_level]:
			region["level"] = next_level
			region["level_age_s"] = 0.0
			region["strong_s"] = 0.0
			region["weak_s"] = 0.0
			region_promoted.emit(int(region["id"]), next_level)
		return

	var should_weaken := target < level or score < EXIT_SCORE[level]
	if not should_weaken:
		return
	if float(region["level_age_s"]) < MIN_LEVEL_LIFETIME_S[level]:
		return
	if float(region["weak_s"]) < DEMOTION_HOLD_S[level]:
		return
	var next_level := level - 1
	region["level"] = next_level
	region["level_age_s"] = 0.0
	region["weak_s"] = 0.0
	region["strong_s"] = 0.0
	region_demoted.emit(int(region["id"]), next_level)


func _merge_regions() -> void:
	var consumed := {}
	var merged: Array[Dictionary] = []
	for i in regions.size():
		if consumed.has(i):
			continue
		var a := regions[i]
		for j in range(i + 1, regions.size()):
			if consumed.has(j):
				continue
			var b := regions[j]
			if abs(int(a["level"]) - int(b["level"])) > 1:
				continue
			var overlap_distance := 0.34 * (DOMAIN_KM[int(a["level"])] + DOMAIN_KM[int(b["level"])])
			if _surface_distance_km(a["center_dir"], b["center_dir"]) > overlap_distance:
				continue
			var wa := maxf(float(a["score"]), 0.05)
			var wb := maxf(float(b["score"]), 0.05)
			a["center_dir"] = ((a["center_dir"] as Vector3) * wa + (b["center_dir"] as Vector3) * wb).normalized()
			a["score"] = maxf(float(a["score"]), float(b["score"]))
			a["level"] = maxi(int(a["level"]), int(b["level"]))
			a["target_level"] = maxi(int(a["target_level"]), int(b["target_level"]))
			a["age_s"] = maxf(float(a["age_s"]), float(b["age_s"]))
			a["level_age_s"] = maxf(float(a["level_age_s"]), float(b["level_age_s"]))
			consumed[j] = true
			region_removed.emit(int(b["id"]))
		merged.append(a)
	regions = merged


func regions_snapshot() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for region in regions:
		out.append(region.duplicate(true))
	return out


func finest_requested_level() -> int:
	var level := RefinementLevel.PLANET
	for region in regions:
		level = maxi(level, int(region["level"]))
	return level


func resolution_at_direction(direction: Vector3) -> Dictionary:
	var d := direction.normalized()
	var best_level := RefinementLevel.PLANET
	var best_region := -1
	for region in regions:
		var level := int(region["level"])
		var radius_km := DOMAIN_KM[level] * 0.5
		if _surface_distance_km(d, region["center_dir"]) <= radius_km and level > best_level:
			best_level = level
			best_region = int(region["id"])
	return {
		"level": best_level,
		"region_id": best_region,
		"cell_km": TARGET_CELL_KM[best_level],
		"vertical_levels": TARGET_VERTICAL_LEVELS[best_level],
	}


func current_global_cell_km() -> float:
	return TAU * PLANET_RADIUS_KM / float(WeatherSystem.GLOBAL_W)


func _candidate_score_desc(a: Dictionary, b: Dictionary) -> bool:
	return float(a["score"]) > float(b["score"])


func _region_priority_desc(a: Dictionary, b: Dictionary) -> bool:
	var pa := float(a["level"]) * 2.0 + float(a["score"])
	var pb := float(b["level"]) * 2.0 + float(b["score"])
	return pa > pb


func _cell_direction(x: int, y: int) -> Vector3:
	var lon := ((float(x) + 0.5) / float(WeatherSystem.GLOBAL_W) - 0.5) * TAU
	var lat := (0.5 - (float(y) + 0.5) / float(WeatherSystem.GLOBAL_H)) * PI
	var cl := cos(lat)
	return Vector3(cl * cos(lon), sin(lat), cl * sin(lon)).normalized()


func _surface_distance_km(a: Vector3, b: Vector3) -> float:
	return acos(clampf(a.normalized().dot(b.normalized()), -1.0, 1.0)) * PLANET_RADIUS_KM


func _update_debug_overlay() -> void:
	var weather_map := get_node_or_null("/root/WeatherMap")
	if weather_map == null or not bool(weather_map.get("visible")) or not show_debug_overlay:
		_hide_debug_overlay()
		return
	var world_value: Variant = weather_map.get("_world_root")
	if not (world_value is Node3D):
		return
	var world := world_value as Node3D
	var map_id := weather_map.get_instance_id()
	if _debug_root == null or not is_instance_valid(_debug_root) or _debug_map_instance_id != map_id:
		_debug_map_instance_id = map_id
		_debug_root = Node3D.new()
		_debug_root.name = "AdaptiveRefinementOverlay"
		world.add_child(_debug_root)
		_debug_label = Label.new()
		_debug_label.name = "AdaptiveRefinementStatus"
		_debug_label.position = Vector2(16, 76)
		_debug_label.custom_minimum_size = Vector2(520, 150)
		_debug_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
		_debug_label.add_theme_font_size_override("font_size", 11)
		_debug_label.add_theme_color_override("font_color", Color(0.78, 0.88, 0.94))
		weather_map.add_child(_debug_label)
		_overlay_dirty = true
	if _debug_root != null:
		_debug_root.visible = true
	if _debug_label != null:
		_debug_label.visible = true
		_debug_label.text = _debug_text()
	if _overlay_dirty:
		_rebuild_debug_regions()
		_overlay_dirty = false


func _hide_debug_overlay() -> void:
	if _debug_root != null and is_instance_valid(_debug_root):
		_debug_root.visible = false
	if _debug_label != null and is_instance_valid(_debug_label):
		_debug_label.visible = false


func _rebuild_debug_regions() -> void:
	if _debug_root == null or not is_instance_valid(_debug_root):
		return
	for child in _debug_root.get_children():
		child.queue_free()
	for region in regions:
		var level := int(region["level"])
		if level <= RefinementLevel.PLANET:
			continue
		var mesh_instance := MeshInstance3D.new()
		mesh_instance.mesh = _region_outline_mesh(region["center_dir"], DOMAIN_KM[level])
		var material := ShaderMaterial.new()
		material.shader = _overlay_shader
		material.set_shader_parameter("u_color", _level_color(level))
		mesh_instance.material_override = material
		mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		_debug_root.add_child(mesh_instance)


func _region_outline_mesh(center_value: Variant, domain_km: float) -> ArrayMesh:
	var center: Vector3 = center_value
	center = center.normalized()
	var reference := Vector3.RIGHT if absf(center.y) > 0.92 else Vector3.UP
	var east := reference.cross(center).normalized()
	var north := center.cross(east).normalized()
	var half_angle := (domain_km * 0.5) / PLANET_RADIUS_KM
	var vertices := PackedVector3Array()
	var indices := PackedInt32Array()
	var previous := -1
	# Four curved sides. Projecting every vertex back to the unit sphere makes the
	# debug footprint conform to the globe rather than float as a flat billboard.
	for side in 4:
		previous = -1
		for s in OVERLAY_SEGMENTS + 1:
			var t := -1.0 + 2.0 * float(s) / float(OVERLAY_SEGMENTS)
			var ex := 0.0
			var ny := 0.0
			if side == 0:
				ex = t
				ny = -1.0
			elif side == 1:
				ex = 1.0
				ny = t
			elif side == 2:
				ex = -t
				ny = 1.0
			else:
				ex = -1.0
				ny = -t
			var d := (center + east * tan(ex * half_angle) + north * tan(ny * half_angle)).normalized()
			var current := vertices.size()
			vertices.append(d * OVERLAY_RADIUS)
			if previous >= 0:
				indices.append(previous)
				indices.append(current)
			previous = current
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	return mesh


func _level_color(level: int) -> Color:
	match level:
		RefinementLevel.DISTURBANCE:
			return Color(0.18, 0.96, 0.48, 0.86)
		RefinementLevel.CONVECTION:
			return Color(0.98, 0.90, 0.16, 0.90)
		RefinementLevel.STORM:
			return Color(1.00, 0.48, 0.08, 0.92)
		RefinementLevel.TORNADO:
			return Color(1.00, 0.08, 0.08, 0.96)
	return Color(0.40, 0.55, 0.80, 0.70)


func _debug_text() -> String:
	var lines := PackedStringArray()
	lines.append("ADAPTIVE REFINEMENT PLANNER")
	lines.append("L0 current %.1f km / 6L  →  target %.0f km / %dL" % [
		current_global_cell_km(), TARGET_CELL_KM[0], TARGET_VERTICAL_LEVELS[0]])
	lines.append("Requests: %d   finest currently requested: %s" % [regions.size(), LEVEL_NAMES[finest_requested_level()]])
	var shown := mini(regions.size(), 5)
	for i in shown:
		var r := regions[i]
		var level := int(r["level"])
		lines.append("#%d  %s  %.2f  %.2f km cells / %dL / %.0f km domain" % [
			int(r["id"]), LEVEL_NAMES[level], float(r["score"]), TARGET_CELL_KM[level],
			TARGET_VERTICAL_LEVELS[level], DOMAIN_KM[level]])
	if regions.is_empty():
		lines.append("No disturbance currently exceeds the L1 refinement threshold.")
	lines.append("Green=L1  yellow=L2  orange/red reserved for child-domain promotion")
	return "\n".join(lines)
