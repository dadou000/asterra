extends Node
## Playable-world Phase 2 gate for dynamic-water/clipmap coordinate stability.
##
## Runs the normal Main world, finds a generated coastline, anchors a physical SWE
## patch there, then moves the real player camera through ordinary lattice snaps
## and two >8 km toroidal reanchors. Automated checks verify that the hydrology
## anchor remains planet-fixed while OceanSystem's temporary anchor changes.

const COAST_SEARCH_SAMPLES := 768
const COAST_PROBE_M := 1800.0
const DOMAIN_RES := 128
const DOMAIN_DX := 16.0
const WATER_MACRO_DT := 0.10
const SNAP_STEPS := 18
const SNAP_STEP_M := 85.0
const FAR_OFFSET_M := 9200.0
const SETTLE_FRAMES := 18
const TIMEOUT_FRAMES := 7200

var _main: Node
var _player: AsterraPlayer
var _solver: FixedHydroGPU
var _reconstruct: HydroSurfaceReconstructionGPU
var _material: ShaderMaterial
var _coast_dir := Vector3.ZERO
var _coast_right := Vector3.ZERO
var _coast_up := Vector3.ZERO
var _phase := 0
var _phase_frames := 0
var _snap_index := 0
var _frames := 0
var _water_cycle_busy := false
var _last_input_anchor := Vector3.ZERO
var _last_lattice_center := Vector2(INF, INF)
var _lattice_snap_count := 0
var _reanchor_count := 0
var _finished := false
var _hold_on_pass := false
var _status: Label


func _ready() -> void:
	process_priority = 20 # after OceanSystem(10) and WaterSystem(11)
	_main = get_parent()
	_hold_on_pass = OS.get_cmdline_user_args().has("--hydro-coast-hold")
	_build_status_overlay()
	_set_status("Waiting for generated world / player / dynamic GPU surface…")


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")
		return

	if _phase == 0:
		_try_begin()
		return

	_drive_water()
	_phase_frames += 1

	match _phase:
		1: # initial coast observation
			if _phase_frames >= SETTLE_FRAMES:
				_check_invariants("initial coast")
				_phase = 2
				_phase_frames = 0
		2: # ordinary toroidal/lattice snaps while remaining inside the 2 km field
			if _phase_frames % 3 == 0 and _snap_index < SNAP_STEPS:
				_snap_index += 1
				_move_player(float(_snap_index) * SNAP_STEP_M)
			if _snap_index >= SNAP_STEPS and _phase_frames >= SNAP_STEPS * 3 + SETTLE_FRAMES:
				_check_invariants("near-field lattice traversal")
				_phase = 3
				_phase_frames = 0
				_move_player(FAR_OFFSET_M)
		3: # force first full OceanSystem reanchor
			if _phase_frames >= SETTLE_FRAMES:
				_check_invariants("far reanchor")
				_phase = 4
				_phase_frames = 0
				_move_player(0.0)
		4: # return to the fixed physical patch, forcing another reanchor
			if _phase_frames >= SETTLE_FRAMES:
				_check_invariants("return reanchor")
				if _lattice_snap_count < 4:
					_fail("too few observed lattice snaps: %d" % _lattice_snap_count)
					return
				if _reanchor_count < 2:
					_fail("expected >=2 full clipmap reanchors, observed %d" % _reanchor_count)
					return
				_pass()


func _try_begin() -> void:
	if not Planet.ready_state or Planet.cfg == null:
		return
	var candidate: Variant = _main.get("player") if _main != null else null
	if not (candidate is AsterraPlayer):
		return
	if not WaterSystem.dynamic_surface_available():
		return
	var ocean := get_node_or_null("/root/OceanSystem")
	if ocean == null or not ocean.has_method(&"material"):
		return
	var material_value: Variant = ocean.call(&"material")
	if not (material_value is ShaderMaterial):
		return

	_player = candidate as AsterraPlayer
	_material = material_value as ShaderMaterial
	_coast_dir = _find_coast_direction()
	if _coast_dir.length_squared() < 0.5:
		_fail("could not locate a generated coastline")
		return
	var tangent: Array = CubeSphere.tangent_basis(_coast_dir)
	_coast_right = tangent[0]
	_coast_up = tangent[1]

	_player.input_enabled = false
	_player.set_mouse_captured(false)
	_player.mode = AsterraPlayer.Mode.FLY
	_move_player(0.0)

	WaterSystem.set_dynamic_surface_anchor_direction(_coast_dir)
	WaterSystem.set_dynamic_surface_center_plane(Vector2.ZERO)
	WaterSystem.set_dynamic_surface_render_enabled(true)

	var state := _build_test_water_state()
	_solver = FixedHydroGPU.new()
	_solver.name = "CoastStabilitySWE"
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(func(error: Error):
		_fail("SWE initialization failed (%d)" % int(error)))
	_solver.advance_recorded.connect(_on_solver_advance_recorded)
	var err := _solver.initialize(DOMAIN_RES, DOMAIN_RES, DOMAIN_DX, state)
	if err != OK:
		_fail("SWE initialize rejected (%d)" % int(err))
		return

	_phase = -1 # wait for reconstruction pipeline
	_set_status("Coast found. Initializing physical dynamic-water patch…")


func _on_solver_initialized() -> void:
	_reconstruct = HydroSurfaceReconstructionGPU.new()
	_reconstruct.name = "CoastStabilityReconstruction"
	add_child(_reconstruct)
	_reconstruct.initialized.connect(_on_reconstruction_initialized)
	_reconstruct.initialization_failed.connect(func(error: Error):
		_fail("surface reconstruction initialization failed (%d)" % int(error)))
	_reconstruct.reconstruction_recorded.connect(_on_reconstruction_recorded)
	_reconstruct.reconstruction_failed.connect(func(_request_id: int, error: Error):
		_fail("surface reconstruction failed (%d)" % int(error)))
	var resources := WaterSystem.surface_resources()
	var err := _reconstruct.initialize(
		_solver.current_state_rid(), DOMAIN_RES, DOMAIN_RES, DOMAIN_DX,
		_solver.dry_eps, resources.field_rid(), resources.field_resolution(),
		resources.field_half_extent_m())
	if err != OK:
		_fail("surface reconstruction initialize rejected (%d)" % int(err))


func _on_reconstruction_initialized() -> void:
	var request := _reconstruct.reconstruct(Vector2.ZERO, Vector2.ZERO, 0.0, 1.0)
	if request < 0:
		_fail("initial surface reconstruction request rejected")


func _on_reconstruction_recorded(_request_id: int) -> void:
	_water_cycle_busy = false
	if _phase == -1:
		_phase = 1
		_phase_frames = 0
		_capture_clip_state()
		_set_status("Dynamic SWE patch live at real coast. Watching initial stability…")


func _drive_water() -> void:
	if _solver == null or _reconstruct == null or _water_cycle_busy:
		return
	if not _solver.initialized_ok() or not _reconstruct.initialized_ok():
		return
	if _solver.step_pending() or _reconstruct.pending():
		return
	_water_cycle_busy = true
	if _solver.advance(WATER_MACRO_DT, 16, false) < 0:
		_water_cycle_busy = false
		_fail("SWE advance rejected")


func _on_solver_advance_recorded(_step_id: int) -> void:
	if _reconstruct == null:
		_water_cycle_busy = false
		return
	if _reconstruct.reconstruct(Vector2.ZERO, Vector2.ZERO, 0.0, 1.0) < 0:
		_water_cycle_busy = false
		_fail("surface reconstruction rejected after SWE advance")


func _build_test_water_state() -> PackedFloat32Array:
	var state := PackedFloat32Array()
	state.resize(DOMAIN_RES * DOMAIN_RES * 4)
	var half := float(DOMAIN_RES) * DOMAIN_DX * 0.5
	for y in DOMAIN_RES:
		for x in DOMAIN_RES:
			var px := (float(x) + 0.5) * DOMAIN_DX - half
			var py := (float(y) + 0.5) * DOMAIN_DX - half
			var r := Vector2(px, py)
			var r2 := r.length_squared()
			var bed := -8.0
			var eta := 1.25 * exp(-r2 / (260.0 * 260.0)) \
				- 0.28 * exp(-r2 / (620.0 * 620.0))
			var depth := maxf(eta - bed, 0.0)
			var outward := r.normalized() if r2 > 1.0 else Vector2.ZERO
			var speed := 0.48 * exp(-r2 / (420.0 * 420.0))
			var velocity := outward * speed
			var o := (x + y * DOMAIN_RES) * 4
			state[o] = depth
			state[o + 1] = depth * velocity.x
			state[o + 2] = depth * velocity.y
			state[o + 3] = bed
	return state


func _find_coast_direction() -> Vector3:
	var best := Vector3.ZERO
	var best_score := INF
	var golden := PI * (3.0 - sqrt(5.0))
	for i in COAST_SEARCH_SAMPLES:
		var fy := 1.0 - 2.0 * (float(i) + 0.5) / float(COAST_SEARCH_SAMPLES)
		var rr := sqrt(maxf(1.0 - fy * fy, 0.0))
		var phi := golden * float(i)
		var d := Vector3(cos(phi) * rr, fy, sin(phi) * rr).normalized()
		var h := Planet.macro_height(d)
		if absf(h) >= best_score:
			continue
		var basis: Array = CubeSphere.tangent_basis(d)
		var values := PackedFloat32Array()
		for offset in [Vector2(COAST_PROBE_M, 0.0), Vector2(-COAST_PROBE_M, 0.0),
				Vector2(0.0, COAST_PROBE_M), Vector2(0.0, -COAST_PROBE_M)]:
			var pd := _direction_for_offset(d, basis[0], basis[1], offset)
			values.append(Planet.macro_height(pd))
		var has_land := false
		var has_water := false
		for v in values:
			has_land = has_land or v > 2.0
			has_water = has_water or v < -2.0
		if has_land and has_water:
			best = d
			best_score = absf(h)

	if best.length_squared() < 0.5:
		return best

	# Small local refinement minimizes actual runtime terrain height rather than
	# only the macro field used for the global search.
	var step_m := 900.0
	for _pass in 7:
		var basis: Array = CubeSphere.tangent_basis(best)
		var local_best := best
		var local_score := absf(Planet.terrain_height(best))
		for k in 8:
			var a := TAU * float(k) / 8.0
			var offset := Vector2(cos(a), sin(a)) * step_m
			var d := _direction_for_offset(best, basis[0], basis[1], offset)
			var score := absf(Planet.terrain_height(d))
			if score < local_score:
				local_score = score
				local_best = d
		best = local_best
		step_m *= 0.5
	return best.normalized()


func _move_player(offset_m: float) -> void:
	if _player == null:
		return
	var d := _direction_for_offset(_coast_dir, _coast_right, _coast_up,
		Vector2(offset_m, 0.0))
	_player.spawn_at(d, 45.0)


func _direction_for_offset(center: Vector3, right: Vector3, up: Vector3,
		offset_m: Vector2) -> Vector3:
	var arc := offset_m.length()
	if arc <= 1.0e-7:
		return center.normalized()
	var theta := arc / maxf(Planet.cfg.planet_radius, 1.0)
	var tangent := (right * offset_m.x + up * offset_m.y).normalized()
	return (center * cos(theta) + tangent * sin(theta)).normalized()


func _capture_clip_state() -> void:
	if _material == null:
		return
	var anchor: Variant = _material.get_shader_parameter("u_anchor_dir")
	var lattice: Variant = _material.get_shader_parameter("u_lattice_center_plane")
	if anchor is Vector3:
		_last_input_anchor = anchor
	if lattice is Vector2:
		_last_lattice_center = lattice


func _check_invariants(label: String) -> void:
	if _material == null:
		_fail(label + ": ocean material missing")
		return
	var enabled := float(_material.get_shader_parameter("u_dynamic_surface_enabled"))
	if enabled < 0.5:
		_fail(label + ": dynamic surface unexpectedly disabled")
		return

	var field_anchor: Variant = _material.get_shader_parameter("u_dynamic_surface_anchor_dir")
	var input_anchor: Variant = _material.get_shader_parameter("u_dynamic_surface_input_anchor_dir")
	var input_right: Variant = _material.get_shader_parameter("u_dynamic_surface_input_anchor_right")
	var input_up: Variant = _material.get_shader_parameter("u_dynamic_surface_input_anchor_up")
	var ocean_anchor: Variant = _material.get_shader_parameter("u_anchor_dir")
	var lattice: Variant = _material.get_shader_parameter("u_lattice_center_plane")
	if not (field_anchor is Vector3 and input_anchor is Vector3 and input_right is Vector3 \
			and input_up is Vector3 and ocean_anchor is Vector3 and lattice is Vector2):
		_fail(label + ": required coordinate uniforms unavailable")
		return

	var fa := (field_anchor as Vector3).normalized()
	var ia := (input_anchor as Vector3).normalized()
	var oa := (ocean_anchor as Vector3).normalized()
	if fa.dot(_coast_dir) < 0.999999:
		_fail(label + ": persistent hydrology anchor drifted")
		return
	if ia.dot(oa) < 0.9999999:
		_fail(label + ": WaterSystem input frame is not synchronized with OceanSystem")
		return

	# CPU mirror of the shader's input-plane -> planet -> field-plane transform.
	# The fixed coast anchor must always map back to field (0,0), regardless of the
	# current toroidal ocean anchor.
	var coast_in_input := _planet_to_plane(_coast_dir, ia,
		(input_right as Vector3).normalized(), (input_up as Vector3).normalized())
	var roundtrip_dir := _direction_for_offset(ia,
		(input_right as Vector3).normalized(), (input_up as Vector3).normalized(), coast_in_input)
	var field_plane := _planet_to_plane(roundtrip_dir, fa, _coast_right, _coast_up)
	if field_plane.length() > 0.15:
		_fail(label + ": reanchor transform moved field origin by %.6f m" % field_plane.length())
		return

	var current_lattice := lattice as Vector2
	if _last_lattice_center.x != INF and current_lattice.distance_squared_to(_last_lattice_center) > 1.0e-6:
		_lattice_snap_count += 1
	_last_lattice_center = current_lattice

	if _last_input_anchor.length_squared() > 0.5:
		var angle := acos(clampf(_last_input_anchor.normalized().dot(ia), -1.0, 1.0))
		var moved_m := angle * Planet.cfg.planet_radius
		if moved_m > 4000.0:
			_reanchor_count += 1
	_last_input_anchor = ia

	_set_status("%s OK | lattice snaps=%d | full reanchors=%d | field roundtrip=%.4f m" % [
		label, _lattice_snap_count, _reanchor_count, field_plane.length()])


func _planet_to_plane(d: Vector3, anchor: Vector3, right: Vector3, up: Vector3) -> Vector2:
	var a := anchor.normalized()
	var p := d.normalized()
	var mu := clampf(a.dot(p), -1.0, 1.0)
	var theta := acos(mu)
	if theta <= 1.0e-8:
		return Vector2.ZERO
	var tangent := p - a * mu
	if tangent.length_squared() <= 1.0e-12:
		return Vector2.ZERO
	tangent = tangent.normalized()
	var arc := theta * Planet.cfg.planet_radius
	return Vector2(tangent.dot(right.normalized()), tangent.dot(up.normalized())) * arc


func _pass() -> void:
	if _finished:
		return
	_set_status("PASS — hydrology remained planet-fixed through %d lattice snaps and %d full reanchors." % [
		_lattice_snap_count, _reanchor_count])
	print("HYDRO_COAST_CLIPMAP: PASS lattice_snaps=", _lattice_snap_count,
		" reanchors=", _reanchor_count)
	if _hold_on_pass:
		_finished = true
		print("HYDRO_COAST_CLIPMAP: holding scene for visual inspection (--hydro-coast-hold)")
		return
	_finished = true
	_cleanup()
	get_tree().quit(0)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_COAST_CLIPMAP: " + message)
	_set_status("FAIL — " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	WaterSystem.set_dynamic_surface_render_enabled(false)
	if _reconstruct != null and is_instance_valid(_reconstruct):
		_reconstruct.release()
		_reconstruct.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()


func _build_status_overlay() -> void:
	var layer := CanvasLayer.new()
	layer.layer = 100
	add_child(layer)
	_status = Label.new()
	_status.position = Vector2(24, 24)
	_status.size = Vector2(1100, 100)
	_status.add_theme_font_size_override("font_size", 20)
	layer.add_child(_status)


func _set_status(text: String) -> void:
	if _status != null:
		_status.text = "Hydro coast/clipmap stability\n" + text
