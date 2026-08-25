extends Node
## Adaptive atmospheric refinement planner, coarse-parent calibrated version.
##
## L0 now runs at ~21.5 km with 30 vertical levels. This controller still scores
## precursor environments; explicit storm dynamics belong to the child nests. L0 may request L1/L2 only. L3/L4 remain reserved for
## metrics from future child solvers.

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

const LEVEL_NAMES: Array[String] = [
	"L0 planet", "L1 disturbance", "L2 convection", "L3 storm", "L4 tornado"
]
const TARGET_CELL_KM: Array[float] = [20.0, 5.0, 1.0, 0.25, 0.075]
const TARGET_VERTICAL_LEVELS: Array[int] = [30, 48, 64, 84, 112]
const DOMAIN_KM: Array[float] = [0.0, 1100.0, 420.0, 120.0, 30.0]

# Coarse-grid precursor thresholds. The previous 0.43 L1 threshold was calibrated
# like a storm-scale index and was unreachable by many legitimate ~86 km setups.
const ENTER_SCORE: Array[float] = [0.0, 0.22, 0.40, 0.72, 0.88]
const EXIT_SCORE: Array[float] = [0.0, 0.13, 0.27, 0.53, 0.68]
const PROMOTION_HOLD_S: Array[float] = [0.0, 0.0, 6.0 * 60.0, 8.0 * 60.0, 4.0 * 60.0]
const DEMOTION_HOLD_S: Array[float] = [0.0, 45.0 * 60.0, 25.0 * 60.0, 18.0 * 60.0, 8.0 * 60.0]
const MIN_LEVEL_LIFETIME_S: Array[float] = [0.0, 90.0 * 60.0, 45.0 * 60.0, 28.0 * 60.0, 12.0 * 60.0]

const PLANET_RADIUS_KM := 3500.0
const CANDIDATE_TILE_W := 32
const CANDIDATE_TILE_H := 24
const SCAN_STRIDE := 4
const MAX_RAW_CANDIDATES := 64
const MAX_ACTIVE_REGIONS := 16
const L1_NMS_DISTANCE_KM := 300.0
const TRACK_MATCH_DISTANCE_KM := 620.0
const MOTION_LEAD := 0.24
const OVERLAY_RADIUS := 1.012
const OVERLAY_SEGMENTS := 18
const L1_PREFILTER_SCORE := 0.16
const CURRENT_PARENT_MAX_REQUEST_LEVEL := RefinementLevel.CONVECTION

var regions: Array[Dictionary] = []
var enabled: bool = true
var show_debug_overlay: bool = true
var last_max_score: float = 0.0
var last_raw_candidate_count: int = 0
var last_max_metrics: Dictionary = {}

var _last_analysis_revision: int = -1
var _last_sim_seconds: float = 0.0
var _next_region_id: int = 1
var _previous_pressure := PackedFloat32Array()
var _convective_values := PackedFloat32Array()
var _overlay_dirty: bool = true

var _debug_root: Node3D
var _debug_label: Label
var _debug_map_instance_id: int = 0
var _overlay_shader: Shader


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	_last_sim_seconds = float(CelestialSystem.simulation_seconds)
	_overlay_shader = load("res://shaders/weather_refinement_patch.gdshader") as Shader


func _process(_delta: float) -> void:
	if not enabled:
		_hide_debug_overlay()
		return
	if WeatherSystem.analysis_revision >= 0 and WeatherSystem.analysis_revision != _last_analysis_revision:
		_last_analysis_revision = WeatherSystem.analysis_revision
		_scan_weather()
	_update_debug_overlay()


func _scan_weather() -> void:
	var weather: PackedFloat32Array = WeatherSystem.global_weather_values
	var diagnostics: PackedFloat32Array = WeatherSystem.global_diagnostics_values
	var products: PackedFloat32Array = WeatherSystem.global_products_values
	var expected: int = WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H * 4
	if weather.size() != expected or diagnostics.size() != expected or products.size() != expected:
		return

	_refresh_convective_values(expected)
	var sim_now: float = float(CelestialSystem.simulation_seconds)
	var sim_dt: float = maxf(sim_now - _last_sim_seconds, 0.0)
	_last_sim_seconds = sim_now
	sim_dt = minf(sim_dt, 2.0 * 3600.0)

	var candidates: Array[Dictionary] = _extract_candidates(
		weather, diagnostics, products, _convective_values)
	_update_regions(candidates, sim_dt)

	_previous_pressure.resize(WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H)
	for y: int in range(0, WeatherSystem.GLOBAL_H, SCAN_STRIDE):
		for x: int in range(0, WeatherSystem.GLOBAL_W, SCAN_STRIDE):
			var c := x + y * WeatherSystem.GLOBAL_W
			_previous_pressure[c] = weather[c * 4 + 3]
	_overlay_dirty = true
	regions_changed.emit()


func _refresh_convective_values(expected: int) -> void:
	_convective_values = PackedFloat32Array()
	var values: PackedFloat32Array = WeatherSystem.global_convective_values
	if values.size() == expected:
		_convective_values = values


func _extract_candidates(weather: PackedFloat32Array, diagnostics: PackedFloat32Array,
		products: PackedFloat32Array, convective: PackedFloat32Array) -> Array[Dictionary]:
	var raw: Array[Dictionary] = []
	var width: int = WeatherSystem.GLOBAL_W
	var height: int = WeatherSystem.GLOBAL_H
	last_max_score = 0.0
	last_raw_candidate_count = 0
	last_max_metrics = {}

	for ty: int in range(0, height, CANDIDATE_TILE_H):
		for tx: int in range(0, width, CANDIDATE_TILE_W):
			var best_score: float = 0.0
			var best_metrics: Dictionary = {}
			var best_x: int = -1
			var best_y: int = -1
			var y_end: int = mini(ty + CANDIDATE_TILE_H, height)
			var x_end: int = mini(tx + CANDIDATE_TILE_W, width)
			for y: int in range(ty, y_end, SCAN_STRIDE):
				for x: int in range(tx, x_end, SCAN_STRIDE):
					var c: int = x + y * width
					var metrics: Dictionary = _cell_metrics(c, weather, diagnostics, products, convective)
					var score: float = float(metrics["score"])
					if score > last_max_score:
						last_max_score = score
						last_max_metrics = metrics.duplicate(true)
					if score > best_score:
						best_score = score
						best_metrics = metrics
						best_x = x
						best_y = y
			if best_x >= 0 and best_score >= L1_PREFILTER_SCORE:
				best_metrics["x"] = best_x
				best_metrics["y"] = best_y
				best_metrics["dir"] = _cell_direction(best_x, best_y)
				best_metrics["target_level"] = _target_level_from_parent(best_metrics)
				if int(best_metrics["target_level"]) > RefinementLevel.PLANET:
					raw.append(best_metrics)

	last_raw_candidate_count = raw.size()
	raw.sort_custom(_candidate_score_desc)
	if raw.size() > MAX_RAW_CANDIDATES:
		raw.resize(MAX_RAW_CANDIDATES)

	var filtered: Array[Dictionary] = []
	for candidate: Dictionary in raw:
		var keep: bool = true
		for accepted: Dictionary in filtered:
			var distance_km: float = _surface_distance_km(
				candidate["dir"] as Vector3, accepted["dir"] as Vector3)
			if distance_km < L1_NMS_DISTANCE_KM:
				keep = false
				break
		if keep:
			filtered.append(candidate)
		if filtered.size() >= MAX_ACTIVE_REGIONS:
			break
	return filtered


func _cell_metrics(c: int, weather: PackedFloat32Array, diagnostics: PackedFloat32Array,
		products: PackedFloat32Array, convective: PackedFloat32Array) -> Dictionary:
	var o: int = c * 4
	var cape: float = clampf(products[o + 2], 0.0, 1.0)
	var shear: float = clampf(diagnostics[o + 3], 0.0, 1.0)
	var vorticity: float = clampf(absf(diagnostics[o] - 0.5) * 2.0, 0.0, 1.0)
	var convergence: float = clampf((0.5 - diagnostics[o + 1]) * 2.0, 0.0, 1.0)
	var storm: float = clampf(weather[o + 1], 0.0, 1.0)
	var precip: float = clampf(weather[o + 2], 0.0, 1.0)
	var low_pressure: float = clampf((0.5 - weather[o + 3]) * 2.0, 0.0, 1.0)
	var pressure_fall: float = 0.0
	if _previous_pressure.size() == WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H:
		pressure_fall = clampf((_previous_pressure[c] - weather[o + 3]) * 12.0, 0.0, 1.0)

	var ascent: float = 0.0
	var downdraft: float = 0.0
	var anvil: float = 0.0
	var wind: float = 0.0
	if convective.size() == weather.size():
		ascent = clampf(convective[o], 0.0, 1.0)
		downdraft = clampf(convective[o + 1], 0.0, 1.0)
		anvil = clampf(convective[o + 2], 0.0, 1.0)
		wind = clampf(convective[o + 3], 0.0, 1.0)

	# Environment dominates because the parent cannot resolve a mature convective
	# core. Cross terms reward loaded CAPE+shear and CAPE+convergence combinations.
	var environment: float = (
		0.30 * cape
		+ 0.20 * shear
		+ 0.15 * convergence
		+ 0.08 * vorticity
		+ 0.07 * low_pressure
		+ 0.06 * pressure_fall
		+ 0.08 * sqrt(maxf(cape * shear, 0.0))
		+ 0.06 * sqrt(maxf(cape * convergence, 0.0))
	)
	var active_structure: float = (
		0.32 * ascent
		+ 0.18 * storm
		+ 0.10 * precip
		+ 0.12 * downdraft
		+ 0.10 * anvil
		+ 0.18 * wind
	)
	var score: float = clampf(environment * 0.98 + active_structure * 0.30, 0.0, 1.0)
	var mesocyclone_proxy: float = clampf(
		sqrt(maxf(ascent * vorticity, 0.0))
		* (0.45 + 0.55 * shear) * (0.55 + 0.45 * convergence), 0.0, 1.0)
	var tornado_proxy: float = clampf(
		pow(maxf(ascent * vorticity * convergence * shear, 0.0), 0.25)
		* (0.60 + 0.40 * downdraft) * (0.65 + 0.35 * low_pressure), 0.0, 1.0)
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
	var score: float = float(m["score"])
	var cape: float = float(m["cape"])
	var shear: float = float(m["shear"])
	var convergence: float = float(m["convergence"])
	var pressure_fall: float = float(m["pressure_fall"])
	var low_pressure: float = float(m["low_pressure"])
	var ascent: float = float(m["ascent"])
	var storm: float = float(m["storm"])

	# Prewarm an L1 domain for a credible loaded environment even if the blended
	# score has not quite reached the nominal threshold yet.
	var precursor: bool = (
		cape >= 0.26 and shear >= 0.12
		and (convergence >= 0.07 or pressure_fall >= 0.05 or low_pressure >= 0.16)
	)
	if score < ENTER_SCORE[RefinementLevel.DISTURBANCE] and not precursor:
		return RefinementLevel.PLANET

	var target: int = RefinementLevel.DISTURBANCE
	var convective_trigger: bool = (
		cape >= 0.30
		and shear >= 0.16
		and (
			convergence >= 0.12
			or ascent >= 0.08
			or storm >= 0.10
			or pressure_fall >= 0.10
		)
	)
	if score >= ENTER_SCORE[RefinementLevel.CONVECTION] and convective_trigger:
		target = RefinementLevel.CONVECTION
	return mini(target, CURRENT_PARENT_MAX_REQUEST_LEVEL)


func _update_regions(candidates: Array[Dictionary], sim_dt: float) -> void:
	for region: Dictionary in regions:
		region["seen"] = false
		region["age_s"] = float(region["age_s"]) + sim_dt
		region["level_age_s"] = float(region["level_age_s"]) + sim_dt

	for candidate: Dictionary in candidates:
		var match: int = _find_region_match(candidate)
		if match < 0:
			_create_region(candidate)
		else:
			_update_region_from_candidate(regions[match], candidate, sim_dt)

	var survivors: Array[Dictionary] = []
	for region: Dictionary in regions:
		if not bool(region["seen"]):
			region["weak_s"] = float(region["weak_s"]) + sim_dt
			region["strong_s"] = 0.0
			region["score"] = float(region["score"]) * exp(-sim_dt / 3600.0)
		_apply_level_lifecycle(region)
		if int(region["level"]) > RefinementLevel.PLANET:
			survivors.append(region)
		else:
			region_removed.emit(int(region["id"]))
	regions = survivors
	_merge_regions()
	regions.sort_custom(_region_priority_desc)
	if regions.size() > MAX_ACTIVE_REGIONS:
		for i: int in range(MAX_ACTIVE_REGIONS, regions.size()):
			region_removed.emit(int(regions[i]["id"]))
		regions.resize(MAX_ACTIVE_REGIONS)


func _find_region_match(candidate: Dictionary) -> int:
	var best: int = -1
	var best_distance: float = INF
	for i: int in range(regions.size()):
		var region: Dictionary = regions[i]
		if bool(region["seen"]):
			continue
		var distance: float = _surface_distance_km(
			candidate["dir"] as Vector3, region["center_dir"] as Vector3)
		var level: int = int(region["level"])
		var radius_allowance: float = maxf(TRACK_MATCH_DISTANCE_KM, DOMAIN_KM[level] * 0.62)
		if distance <= radius_allowance and distance < best_distance:
			best_distance = distance
			best = i
	return best


func _create_region(candidate: Dictionary) -> void:
	var region: Dictionary = {
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
		"metrics": candidate.duplicate(true),
	}
	_next_region_id += 1
	regions.append(region)
	region_created.emit(int(region["id"]))


func _update_region_from_candidate(region: Dictionary, candidate: Dictionary, sim_dt: float) -> void:
	region["seen"] = true
	var old_dir: Vector3 = region["center_dir"] as Vector3
	var observed: Vector3 = candidate["dir"] as Vector3
	var motion: Vector3 = observed - old_dir
	var led: Vector3 = (observed + motion * MOTION_LEAD).normalized()
	region["previous_dir"] = old_dir
	region["center_dir"] = (old_dir * 0.38 + led * 0.62).normalized()
	region["score"] = lerpf(float(region["score"]), float(candidate["score"]), 0.62)
	region["target_level"] = int(candidate["target_level"])
	region["metrics"] = candidate.duplicate(true)
	if int(region["target_level"]) > int(region["level"]):
		region["strong_s"] = float(region["strong_s"]) + sim_dt
		region["weak_s"] = 0.0
	elif int(region["target_level"]) < int(region["level"]):
		region["weak_s"] = float(region["weak_s"]) + sim_dt
		region["strong_s"] = 0.0
	else:
		region["strong_s"] = 0.0
		region["weak_s"] = 0.0


func _apply_level_lifecycle(region: Dictionary) -> void:
	var level: int = int(region["level"])
	var target: int = int(region["target_level"])
	var score: float = float(region["score"])
	if target > level and level < RefinementLevel.TORNADO:
		var next_level: int = level + 1
		if float(region["strong_s"]) >= PROMOTION_HOLD_S[next_level]:
			region["level"] = next_level
			region["level_age_s"] = 0.0
			region["strong_s"] = 0.0
			region["weak_s"] = 0.0
			region_promoted.emit(int(region["id"]), next_level)
		return

	var should_weaken: bool = target < level or score < EXIT_SCORE[level]
	if not should_weaken:
		return
	if float(region["level_age_s"]) < MIN_LEVEL_LIFETIME_S[level]:
		return
	if float(region["weak_s"]) < DEMOTION_HOLD_S[level]:
		return
	var next_level: int = level - 1
	region["level"] = next_level
	region["level_age_s"] = 0.0
	region["weak_s"] = 0.0
	region["strong_s"] = 0.0
	region_demoted.emit(int(region["id"]), next_level)


func _merge_regions() -> void:
	var consumed: Dictionary = {}
	var merged: Array[Dictionary] = []
	for i: int in range(regions.size()):
		if consumed.has(i):
			continue
		var a: Dictionary = regions[i]
		for j: int in range(i + 1, regions.size()):
			if consumed.has(j):
				continue
			var b: Dictionary = regions[j]
			if abs(int(a["level"]) - int(b["level"])) > 1:
				continue
			var overlap_distance: float = 0.34 * (
				DOMAIN_KM[int(a["level"])] + DOMAIN_KM[int(b["level"])])
			if _surface_distance_km(a["center_dir"] as Vector3, b["center_dir"] as Vector3) > overlap_distance:
				continue
			var wa: float = maxf(float(a["score"]), 0.05)
			var wb: float = maxf(float(b["score"]), 0.05)
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
	for region: Dictionary in regions:
		out.append(region.duplicate(true))
	return out


func finest_requested_level() -> int:
	var level: int = RefinementLevel.PLANET
	for region: Dictionary in regions:
		level = maxi(level, int(region["level"]))
	return level


func resolution_at_direction(direction: Vector3) -> Dictionary:
	var d: Vector3 = direction.normalized()
	var best_level: int = RefinementLevel.PLANET
	var best_region: int = -1
	for region: Dictionary in regions:
		var level: int = int(region["level"])
		var radius_km: float = DOMAIN_KM[level] * 0.5
		if _surface_distance_km(d, region["center_dir"] as Vector3) <= radius_km and level > best_level:
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
	return float(a["level"]) * 2.0 + float(a["score"]) > float(b["level"]) * 2.0 + float(b["score"])


func _cell_direction(x: int, y: int) -> Vector3:
	var lon: float = ((float(x) + 0.5) / float(WeatherSystem.GLOBAL_W) - 0.5) * TAU
	var lat: float = (0.5 - (float(y) + 0.5) / float(WeatherSystem.GLOBAL_H)) * PI
	var cl: float = cos(lat)
	return Vector3(cl * cos(lon), sin(lat), cl * sin(lon)).normalized()


func _surface_distance_km(a: Vector3, b: Vector3) -> float:
	return acos(clampf(a.normalized().dot(b.normalized()), -1.0, 1.0)) * PLANET_RADIUS_KM


func _update_debug_overlay() -> void:
	var weather_map: Node = get_node_or_null("/root/WeatherMap")
	if weather_map == null or not bool(weather_map.get("visible")) or not show_debug_overlay:
		_hide_debug_overlay()
		return
	var world_value: Variant = weather_map.get("_world_root")
	if not (world_value is Node3D):
		return
	var world: Node3D = world_value as Node3D
	var map_id: int = weather_map.get_instance_id()
	if _debug_root == null or not is_instance_valid(_debug_root) or _debug_map_instance_id != map_id:
		_debug_map_instance_id = map_id
		_debug_root = Node3D.new()
		_debug_root.name = "AdaptiveRefinementOverlay"
		world.add_child(_debug_root)
		_debug_label = Label.new()
		_debug_label.name = "AdaptiveRefinementStatus"
		_debug_label.position = Vector2(16, 76)
		_debug_label.custom_minimum_size = Vector2(570, 180)
		_debug_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
		_debug_label.add_theme_font_size_override("font_size", 11)
		_debug_label.add_theme_color_override("font_color", Color(0.78, 0.88, 0.94))
		weather_map.add_child(_debug_label)
		_overlay_dirty = true
	_debug_root.visible = true
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
	for child: Node in _debug_root.get_children():
		child.queue_free()
	for region: Dictionary in regions:
		var level: int = int(region["level"])
		if level <= RefinementLevel.PLANET:
			continue
		var mesh_instance := MeshInstance3D.new()
		mesh_instance.mesh = _region_outline_mesh(region["center_dir"] as Vector3, DOMAIN_KM[level])
		var material := ShaderMaterial.new()
		material.shader = _overlay_shader
		material.set_shader_parameter("u_color", _level_color(level))
		mesh_instance.material_override = material
		mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		_debug_root.add_child(mesh_instance)


func _region_outline_mesh(center: Vector3, domain_km: float) -> ArrayMesh:
	var c: Vector3 = center.normalized()
	var reference: Vector3 = Vector3.RIGHT if absf(c.y) > 0.92 else Vector3.UP
	var east: Vector3 = reference.cross(c).normalized()
	var north: Vector3 = c.cross(east).normalized()
	var half_angle: float = (domain_km * 0.5) / PLANET_RADIUS_KM
	var vertices := PackedVector3Array()
	var indices := PackedInt32Array()
	for side: int in range(4):
		var previous: int = -1
		for s: int in range(OVERLAY_SEGMENTS + 1):
			var t: float = -1.0 + 2.0 * float(s) / float(OVERLAY_SEGMENTS)
			var ex: float = 0.0
			var ny: float = 0.0
			if side == 0:
				ex = t; ny = -1.0
			elif side == 1:
				ex = 1.0; ny = t
			elif side == 2:
				ex = -t; ny = 1.0
			else:
				ex = -1.0; ny = -t
			var d: Vector3 = (c + east * tan(ex * half_angle) + north * tan(ny * half_angle)).normalized()
			var current: int = vertices.size()
			vertices.append(d * OVERLAY_RADIUS)
			if previous >= 0:
				indices.append(previous)
				indices.append(current)
			previous = current
	var arrays: Array = []
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
	lines.append("ADAPTIVE REFINEMENT — COARSE-PARENT CALIBRATION")
	lines.append("L0 current %.1f km / 6L  → target %.0f km / %dL" % [
		current_global_cell_km(), TARGET_CELL_KM[0], TARGET_VERTICAL_LEVELS[0]])
	lines.append("Max precursor score %.3f   raw candidates %d   active requests %d" % [
		last_max_score, last_raw_candidate_count, regions.size()])
	if not last_max_metrics.is_empty():
		lines.append("max: CAPE %.2f  shear %.2f  conv %.2f  vort %.2f  pfall %.2f  ascent %.2f" % [
			float(last_max_metrics.get("cape", 0.0)), float(last_max_metrics.get("shear", 0.0)),
			float(last_max_metrics.get("convergence", 0.0)), float(last_max_metrics.get("vorticity", 0.0)),
			float(last_max_metrics.get("pressure_fall", 0.0)), float(last_max_metrics.get("ascent", 0.0))])
	lines.append("Finest requested: %s" % LEVEL_NAMES[finest_requested_level()])
	for i: int in range(mini(regions.size(), 5)):
		var r: Dictionary = regions[i]
		var level: int = int(r["level"])
		lines.append("#%d  %s  score %.2f  %.2f km / %dL / %.0f km domain" % [
			int(r["id"]), LEVEL_NAMES[level], float(r["score"]), TARGET_CELL_KM[level],
			TARGET_VERTICAL_LEVELS[level], DOMAIN_KM[level]])
	if regions.is_empty():
		lines.append("No L1 request yet — report the max precursor line if it stays here.")
	lines.append("Green=L1  yellow=L2; L3/L4 require future child-domain metrics")
	return "\n".join(lines)
