class_name CoastlineClipmapRuntime
extends Node
## Camera-centred, planet-space coastline clipmap.
##
## The streamed ocean mesh is deliberately low resolution, so its vertices must
## never decide where the shoreline is. This node samples the authoritative
## Planet.terrain_height() function into three nested tangent-plane textures and
## feeds the same signed height field to terrain, local ocean and orbit ocean
## shaders. The coastline therefore stays fixed while terrain/water geometry
## changes LOD.
##
## The expensive synthesis runs on a low-priority worker. The published texture
## remains valid while a recenter is being built, and only a ~3 MB texture-array
## upload happens on the main thread.

const CLIPMAP_RES := 512
const LEVEL_TEXEL_M := Vector3(35.0, 120.0, 480.0)
const COAST_DETAIL_BAND_M := 700.0
const RECENTER_FRACTION := 0.22
const TARGET_SCAN_INTERVAL := 0.35
const UPDATE_TILE := 32

var _texture: Texture2DArray
var _published_images: Array[Image] = []
var _published_center := Vector3(1.0, 0.0, 0.0)
var _published_right := Vector3(0.0, 0.0, -1.0)
var _published_up := Vector3(0.0, 1.0, 0.0)
var _requested_center := Vector3(1.0, 0.0, 0.0)
var _latest_center := Vector3(1.0, 0.0, 0.0)
var _have_observer := false

var _generation: int = 0
var _building := false
var _task_id: int = -1
var _cancel: TerrainBuildCancel
var _edit_dirty := false
var _dirty_regions: Array[Dictionary] = []
var _published_version: int = 0
var _last_applied_version: int = -1
var _last_orbit_texture: Texture2DArray
var _last_orbit_face_res: int = -1
var _scan_left: float = 0.0
var _terrain_ref: WeakRef
var _orbit_ocean_ref: WeakRef


func _ready() -> void:
	process_priority = 9
	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	Deltas.region_changed.connect(_on_region_changed)
	set_process(true)


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		return

	var camera := get_viewport().get_camera_3d()
	if camera != null:
		var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
		var planet_pos: Vector3 = camera.global_position + origin
		if planet_pos.length_squared() > 1.0:
			_set_observer(planet_pos.normalized())

	if _edit_dirty and not _building and _have_observer:
		if not _published_images.is_empty():
			_start_patch_build(_generation)
		else:
			_edit_dirty = false
			_queue_build(_latest_center)

	_scan_left -= dt
	var orbit_changed := Planet.orbit_elevation_texture != _last_orbit_texture \
		or Planet.orbit_texture_face_res != _last_orbit_face_res
	if _scan_left <= 0.0 or orbit_changed or _last_applied_version != _published_version:
		_scan_left = TARGET_SCAN_INTERVAL
		_sync_materials()


func _set_observer(dir: Vector3) -> void:
	_latest_center = dir.normalized()
	if not _have_observer:
		_have_observer = true
		_queue_build(_latest_center)
		return

	var reference := _requested_center if _building else _published_center
	if _texture == null:
		if not _building:
			_queue_build(_latest_center)
		return

	var trigger := float(CLIPMAP_RES) * LEVEL_TEXEL_M.x * RECENTER_FRACTION
	var moved := _surface_distance(reference, _latest_center)
	if moved >= trigger:
		_queue_build(_latest_center)


func _queue_build(center: Vector3) -> void:
	var c := center.normalized()
	# Do not manufacture a new generation every frame while an existing request
	# already targets essentially the same place.
	if _building and _surface_distance(_requested_center, c) < 100.0:
		return
	_requested_center = c
	_generation += 1
	if _building and _cancel != null:
		_cancel.cancel()
	if not _building:
		_start_build(_generation, _requested_center)


func _start_build(request_id: int, center: Vector3) -> void:
	_building = true
	var basis: Array = CubeSphere.tangent_basis(center)
	var right: Vector3 = basis[0]
	var up: Vector3 = basis[1]
	_cancel = TerrainBuildCancel.new()
	var cancel := _cancel

	# Snapshot player edits on the main thread. The worker then reads an immutable
	# copy while reproducing exactly the same terrain function used by chunks.
	var snap: Dictionary = {}
	if not Deltas.is_empty():
		var outer_half := 0.5 * float(CLIPMAP_RES) * LEVEL_TEXEL_M.z
		var ang := outer_half * 1.45 / maxf(Planet.cfg.planet_radius, 1.0)
		snap = Deltas.snapshot_for_bounds(center, ang)

	var task := func() -> void:
		var built := _build_images(center, right, up, snap, cancel)
		call_deferred("_on_images_ready", request_id, center, right, up, built)
	# Cosmetic cache work must never compete with missing terrain coverage.
	_task_id = WorkerThreadPool.add_task(task, true, "asterra_coast_clipmap")


## Rebuild only 32x32 cache tiles touched by terrain edits. The published images
## are duplicated for the worker, so rendering continues from the previous
## complete cache and the main thread never observes a partially updated layer.
func _start_patch_build(request_id: int) -> void:
	if _published_images.is_empty():
		_edit_dirty = false
		_queue_build(_latest_center)
		return
	_building = true
	_edit_dirty = false
	var regions := _dirty_regions.duplicate(true)
	_dirty_regions.clear()
	var source: Array[Image] = []
	for image in _published_images:
		source.append(image.duplicate())
	var center := _published_center
	var right := _published_right
	var up := _published_up
	var outer_half := 0.5 * float(CLIPMAP_RES) * LEVEL_TEXEL_M.z
	var ang := outer_half * 1.45 / maxf(Planet.cfg.planet_radius, 1.0)
	var snap := Deltas.snapshot_for_bounds(center, ang)
	_cancel = TerrainBuildCancel.new()
	var cancel := _cancel
	var task := func() -> void:
		var built := _patch_images(source, center, right, up, regions, snap, cancel)
		call_deferred("_on_images_ready", request_id, center, right, up, built)
	_task_id = WorkerThreadPool.add_task(task, true, "asterra_coast_tiles")


static func _build_images(center: Vector3, right: Vector3, up: Vector3,
		snap: Dictionary, cancel: TerrainBuildCancel = null) -> Dictionary:
	if not Planet.ready_state or Planet.cfg == null:
		return {}

	var radius: float = Planet.cfg.planet_radius
	var detail: TerrainDetail = Planet.make_detail()
	var images: Array[Image] = []
	var texels := [LEVEL_TEXEL_M.x, LEVEL_TEXEL_M.y, LEVEL_TEXEL_M.z]
	var half_res := float(CLIPMAP_RES) * 0.5

	for level in 3:
		if cancel != null and cancel.is_cancelled():
			return {}
		var texel: float = float(texels[level])
		var values := PackedFloat32Array()
		values.resize(CLIPMAP_RES * CLIPMAP_RES)
		for y in CLIPMAP_RES:
			if cancel != null and cancel.is_cancelled():
				return {}
			var metres_y := (float(y) + 0.5 - half_res) * texel
			var row_dir := center + up * (metres_y / radius)
			var row := y * CLIPMAP_RES
			for x in CLIPMAP_RES:
				var metres_x := (float(x) + 0.5 - half_res) * texel
				var d := (row_dir + right * (metres_x / radius)).normalized()
				var macro_h: float = Planet.macro_height(d)
				var h := macro_h + Planet.coast_profile_offset(d, macro_h)
				# Runtime detail only has enough amplitude to change the shoreline in
				# this band. Avoid four FastNoiseLite trees for unquestionably high
				# land/deep ocean pixels.
				if absf(macro_h) <= COAST_DETAIL_BAND_M:
					h = Planet.terrain_height(d, detail, snap)
				values[row + x] = h
		var img := Image.create_from_data(CLIPMAP_RES, CLIPMAP_RES, false,
			Image.FORMAT_RF, values.to_byte_array())
		images.append(img)

	return {"images": images}


static func _patch_images(images: Array[Image], center: Vector3, right: Vector3,
		up: Vector3, regions: Array[Dictionary], snap: Dictionary,
		cancel: TerrainBuildCancel = null) -> Dictionary:
	if images.size() != 3 or regions.is_empty() or not Planet.ready_state:
		return {"images": images, "patch": true}
	var radius := Planet.cfg.planet_radius
	var texels := [LEVEL_TEXEL_M.x, LEVEL_TEXEL_M.y, LEVEL_TEXEL_M.z]
	var half_res := float(CLIPMAP_RES) * 0.5
	for level in 3:
		if cancel != null and cancel.is_cancelled():
			return {}
		var texel := float(texels[level])
		var touched := {}
		for region in regions:
			var rd: Vector3 = region["center"]
			var denom := rd.dot(center)
			if denom <= 0.08:
				continue
			var metres := Vector2(rd.dot(right), rd.dot(up)) / denom * radius
			var rp := float(region["radius"]) / texel + 2.0
			var px := metres.x / texel + half_res
			var py := metres.y / texel + half_res
			var tx0 := clampi(int(floor((px - rp) / UPDATE_TILE)), 0,
				(CLIPMAP_RES - 1) / UPDATE_TILE)
			var tx1 := clampi(int(floor((px + rp) / UPDATE_TILE)), 0,
				(CLIPMAP_RES - 1) / UPDATE_TILE)
			var ty0 := clampi(int(floor((py - rp) / UPDATE_TILE)), 0,
				(CLIPMAP_RES - 1) / UPDATE_TILE)
			var ty1 := clampi(int(floor((py + rp) / UPDATE_TILE)), 0,
				(CLIPMAP_RES - 1) / UPDATE_TILE)
			for ty in range(ty0, ty1 + 1):
				for tx in range(tx0, tx1 + 1):
					touched[ty * 1024 + tx] = Vector2i(tx, ty)
		var image := images[level]
		var detail := Planet.make_detail()
		for tile_value in touched.values():
			if cancel != null and cancel.is_cancelled():
				return {}
			var tile: Vector2i = tile_value
			var x0 := tile.x * UPDATE_TILE
			var y0 := tile.y * UPDATE_TILE
			for y in range(y0, mini(y0 + UPDATE_TILE, CLIPMAP_RES)):
				var my := (float(y) + 0.5 - half_res) * texel
				var row_dir := center + up * (my / radius)
				for x in range(x0, mini(x0 + UPDATE_TILE, CLIPMAP_RES)):
					var mx := (float(x) + 0.5 - half_res) * texel
					var d := (row_dir + right * (mx / radius)).normalized()
					var macro_h := Planet.macro_height(d)
					var h := macro_h + Planet.coast_profile_offset(d, macro_h)
					if absf(macro_h) <= COAST_DETAIL_BAND_M:
						h = Planet.terrain_height(d, detail, snap)
					image.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
	return {"images": images, "patch": true}


func _on_images_ready(request_id: int, center: Vector3, right: Vector3, up: Vector3,
		built: Dictionary) -> void:
	# The callback is deferred by the finished worker, so this wait is cleanup,
	# not a main-thread stall.
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
	_building = false
	_cancel = null

	# A stale tile patch is still a strict improvement over its source image and
	# is safe to publish before applying the next dirty tile set. A stale recenter
	# has a different projection basis and remains discard-only.
	if (request_id == _generation or bool(built.get("patch", false))) \
			and not built.is_empty():
		var images: Array[Image] = []
		for value in built["images"]:
			var image: Image = value
			images.append(image)
		var tex := Texture2DArray.new()
		var err := tex.create_from_images(images)
		if err == OK:
			_texture = tex
			_published_images = images
			_published_center = center
			_published_right = right
			_published_up = up
			_published_version += 1
			_sync_materials()
		else:
			push_error("Failed to upload coastline clipmap (%d)" % err)

	# Movement/rebake may have superseded the request while the worker ran.
	if request_id != _generation:
		if _edit_dirty and not _published_images.is_empty():
			_start_patch_build(_generation)
		else:
			_start_build(_generation, _requested_center)


func _on_world_ready(_fields: PlanetFields) -> void:
	# A new bake invalidates every sampled height, including a build already in
	# flight. Keep the old task isolated and discard its result by generation id.
	_generation += 1
	_texture = null
	_published_images.clear()
	_published_version += 1
	_last_applied_version = -1
	if _building and _cancel != null:
		_cancel.cancel()
	if _have_observer:
		_requested_center = _latest_center
		if not _building:
			_start_build(_generation, _requested_center)
	_sync_materials()

func _on_coast_profile_changed() -> void:
	# A profile edit affects every seaward sample, not a bounded delta region.
	_generation += 1
	_texture = null
	_published_images.clear()
	_published_version += 1
	_last_applied_version = -1
	if _building and _cancel != null:
		_cancel.cancel()
	if _have_observer:
		_requested_center = _latest_center
		if not _building:
			_start_build(_generation, _requested_center)
	_sync_materials()


func _on_region_changed(center: Vector3, radius_m: float) -> void:
	if not _have_observer or Planet.cfg == null:
		return
	var outer_half := 0.5 * float(CLIPMAP_RES) * LEVEL_TEXEL_M.z
	var dist := _surface_distance(_latest_center, center.normalized())
	if dist <= outer_half * 1.45 + radius_m:
		# Coalesce a stream of digging edits into at most one follow-up build.
		_dirty_regions.append({"center": center.normalized(), "radius": radius_m})
		_edit_dirty = true
		_generation += 1


func _sync_materials() -> void:
	if not Planet.ready_state:
		return
	_last_orbit_texture = Planet.orbit_elevation_texture
	_last_orbit_face_res = Planet.orbit_texture_face_res
	_last_applied_version = _published_version

	var terrain: PlanetTerrain = _terrain_ref.get_ref() if _terrain_ref != null else null
	var orbit_ocean: OrbitOcean = _orbit_ocean_ref.get_ref() if _orbit_ocean_ref != null else null
	if terrain == null or orbit_ocean == null:
		_find_targets(get_tree().root)
		terrain = _terrain_ref.get_ref() if _terrain_ref != null else null
		orbit_ocean = _orbit_ocean_ref.get_ref() if _orbit_ocean_ref != null else null

	if terrain != null:
		for value in terrain.debug_materials():
			var mat: ShaderMaterial = value
			_apply_to_material(mat)
	if orbit_ocean != null:
		var maybe_mat: Variant = orbit_ocean.get("_material")
		if maybe_mat is ShaderMaterial:
			_apply_to_material(maybe_mat)


func _find_targets(node: Node) -> void:
	if _terrain_ref == null or _terrain_ref.get_ref() == null:
		if node is PlanetTerrain:
			_terrain_ref = weakref(node)
	if _orbit_ocean_ref == null or _orbit_ocean_ref.get_ref() == null:
		if node is OrbitOcean:
			_orbit_ocean_ref = weakref(node)

	if _terrain_ref != null and _terrain_ref.get_ref() != null \
			and _orbit_ocean_ref != null and _orbit_ocean_ref.get_ref() != null:
		return
	for child in node.get_children():
		_find_targets(child)
		if _terrain_ref != null and _terrain_ref.get_ref() != null \
				and _orbit_ocean_ref != null and _orbit_ocean_ref.get_ref() != null:
			return


func _apply_to_material(mat: ShaderMaterial) -> void:
	if Planet.orbit_elevation_texture != null:
		mat.set_shader_parameter("u_orbit_elevation", Planet.orbit_elevation_texture)
		mat.set_shader_parameter("u_orbit_face_res", float(Planet.orbit_texture_face_res))
		mat.set_shader_parameter("u_relief_ready", 1.0)

	mat.set_shader_parameter("u_coast_clipmap_ready", 1.0 if _texture != null else 0.0)
	mat.set_shader_parameter("u_coast_center", _published_center)
	mat.set_shader_parameter("u_coast_right", _published_right)
	mat.set_shader_parameter("u_coast_up", _published_up)
	mat.set_shader_parameter("u_coast_res", float(CLIPMAP_RES))
	mat.set_shader_parameter("u_coast_texel_m", LEVEL_TEXEL_M)
	if _texture != null:
		mat.set_shader_parameter("u_coast_clipmap", _texture)


func _surface_distance(a: Vector3, b: Vector3) -> float:
	if Planet.cfg == null:
		return INF
	var cosine := clampf(a.normalized().dot(b.normalized()), -1.0, 1.0)
	return acos(cosine) * Planet.cfg.planet_radius


func _exit_tree() -> void:
	if _cancel != null:
		_cancel.cancel()
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
