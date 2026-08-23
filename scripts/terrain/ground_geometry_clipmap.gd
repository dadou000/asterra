extends Node3D
## Fixed-cost ground geometry clipmap.
##
## The global planetary quadtree stops at a coarse level. Close terrain is six
## immutable concentric grids (~0.75, 1.5, 3, 6, 12 and 24 m) displaced from a
## seven-layer height pyramid. The seventh ~48 m layer is only the handoff target
## for the outer ring/global depth-10 terrain.
##
## Runtime movement no longer rebuilds seven complete CPU images. The visible
## grid centre follows the observer on the finest lattice while every height
## level owns an independently snapped toroidal image. A level that advances by
## one cell changes only one newly exposed row/column; unchanged texels remain in
## place. The shader maps physical plane coordinates through each layer's centre
## and circular-storage origin, so all ring meshes can keep one common centre and
## remain gap-free even though the backing height levels move at different rates.

const TARGET_FINE_DEPTH := 16
const GRID_CELLS := 64
const GRID_VERTS := GRID_CELLS + 1
const RENDER_LEVELS := 6
const STORAGE_LEVELS := RENDER_LEVELS + 1

# Two cells of extra height data on every side cover independent level snapping
# plus the +/-1 neighbour samples used to reconstruct normals at mesh boundaries.
const HEIGHT_MARGIN_CELLS := 2
const HEIGHT_TEX_SIZE := GRID_VERTS + HEIGHT_MARGIN_CELLS * 2
const HEIGHT_HALF := float(HEIGHT_TEX_SIZE - 1) * 0.5

const ACTIVE_AGL_M := 3500.0
const REANCHOR_M := 12000.0
const MATERIAL_RES := 128.0
const MATERIAL_TEXEL_M := Vector3(8.0, 128.0, 2048.0)
const GLOBAL_CUT_FRACTION := 0.90
const GLOBAL_BUILD_CAP := 4
const LOCAL_GLOBAL_BUILD_CAP := 2
const CACHE_REFRESH_DELAY := 0.20

var _material: ShaderMaterial
var _rings: Array[MeshInstance3D] = []
var _images: Array[Image] = []
var _height_texture: Texture2DArray

# Per-storage-level physical centre (tangent-plane metres) and circular image
# origin. Logical texel (0,0) maps to storage `origin`; moving the logical window
# only advances this origin and fills newly exposed texels.
var _layer_centers: Array[Vector2] = []
var _layer_origins: Array[Vector2i] = []

var _frame_dir := Vector3(1.0, 0.0, 0.0)
var _frame_right := Vector3(0.0, 0.0, -1.0)
var _frame_up := Vector3(0.0, 1.0, 0.0)
var _have_frame := false
var _frame_epoch := 0
var _base_spacing := 0.75

var _published_center := Vector2.ZERO
var _requested_center := Vector2.ZERO
var _building := false
var _task_id := -1
var _force_full_pending := false
# Bit per storage level. Cache tile arrivals refresh only their own level instead
# of forcing all seven images through another complete sampling pass.
var _refresh_mask_pending := 0
var _active := false
var _last_material_control: Texture2DArray
var _terrain_ref: WeakRef
var _cache_refresh_pending := false
var _cache_refresh_left := 0.0
var _cache_refresh_mask := 0


func _ready() -> void:
	process_priority = 9
	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/ground_geometry_clipmap.gdshader")
	_build_ring_nodes()
	_set_visible(false)
	Planet.world_ready.connect(_on_world_ready)
	Deltas.region_changed.connect(_on_region_changed)
	GroundHeightStore.tile_ready.connect(_on_height_tile_ready)


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_active(false)
		return

	if _cache_refresh_pending:
		_cache_refresh_left -= dt
		if _cache_refresh_left <= 0.0 and _have_frame and not _building:
			_flush_cache_refresh()

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_active(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_set_active(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_dir: Vector3 = planet_pos.normalized()
	var agl: float = maxf(planet_pos.length() - radius - Planet.macro_height(observer_dir), 0.0)
	if agl > ACTIVE_AGL_M:
		_set_active(false)
		return

	if not _have_frame:
		_reset_frame(observer_dir)

	var observer_surface: Vector3 = observer_dir * radius
	var frame_surface: Vector3 = _frame_dir * radius
	var rel: Vector3 = observer_surface - frame_surface
	var px: float = rel.dot(_frame_right)
	var py: float = rel.dot(_frame_up)

	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_frame(observer_dir)
		px = 0.0
		py = 0.0

	# The mesh itself follows the finest lattice. Each backing level below snaps
	# this target independently to its own cell size in the streaming worker.
	var target := Vector2(
		round(px / _base_spacing) * _base_spacing,
		round(py / _base_spacing) * _base_spacing)
	if target.distance_squared_to(_requested_center) > 1e-8:
		_queue_target(target, false, 0)

	_sync_common_uniforms(origin)
	_sync_material_control()
	_set_active(_height_texture != null)


func _reset_frame(center: Vector3) -> void:
	_frame_dir = center.normalized()
	var tangent: Array = CubeSphere.tangent_basis(_frame_dir)
	_frame_right = tangent[0]
	_frame_up = tangent[1]
	_base_spacing = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	_published_center = Vector2.ZERO
	_requested_center = Vector2.ZERO
	_have_frame = true
	_frame_epoch += 1
	_cache_refresh_pending = false
	_cache_refresh_left = 0.0
	_cache_refresh_mask = 0
	_refresh_mask_pending = 0
	_force_full_pending = false
	_images.clear()
	_layer_centers.clear()
	_layer_origins.clear()
	for _level in STORAGE_LEVELS:
		_layer_centers.append(Vector2.ZERO)
		_layer_origins.append(Vector2i.ZERO)
	_height_texture = null
	_material.set_shader_parameter("u_height_ready", 0.0)
	_set_visible(false)
	_sync_global_cutout(false)
	_queue_target(Vector2.ZERO, true, 0)


func _queue_target(target: Vector2, force_full: bool, refresh_mask: int) -> void:
	_requested_center = target
	_force_full_pending = _force_full_pending or force_full
	_refresh_mask_pending |= refresh_mask
	if _building:
		return
	_start_build()


func _flush_cache_refresh() -> void:
	_cache_refresh_pending = false
	var refresh_mask: int = _cache_refresh_mask
	_cache_refresh_mask = 0
	_cache_refresh_left = 0.0
	if refresh_mask != 0:
		_queue_target(_requested_center, false, refresh_mask)


func _start_build() -> void:
	if _building or not Planet.ready_state or Planet.cfg == null:
		return

	var force_full: bool = _force_full_pending or _images.size() != STORAGE_LEVELS
	var refresh_mask: int = _refresh_mask_pending
	var target: Vector2 = _requested_center
	_force_full_pending = false
	_refresh_mask_pending = 0
	_building = true

	var epoch: int = _frame_epoch
	var frame_dir: Vector3 = _frame_dir
	var frame_right: Vector3 = _frame_right
	var frame_up: Vector3 = _frame_up
	var radius: float = Planet.cfg.planet_radius
	var base_spacing: float = _base_spacing

	var old_centers: Array[Vector2] = []
	var old_origins: Array[Vector2i] = []
	for level in STORAGE_LEVELS:
		old_centers.append(_layer_centers[level] if level < _layer_centers.size() else Vector2.ZERO)
		old_origins.append(_layer_origins[level] if level < _layer_origins.size() else Vector2i.ZERO)

	var outer_spacing: float = base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	var outer_half: float = (float(GRID_CELLS) * 0.5 + float(HEIGHT_MARGIN_CELLS)) * outer_spacing
	var center_dir: Vector3 = _direction_for_plane_static(frame_dir, frame_right, frame_up,
		target, radius)
	var snap: Dictionary = Deltas.snapshot_for_bounds(center_dir, outer_half * 1.70 / radius)

	var task := func() -> void:
		var built: Dictionary = _build_height_update(frame_dir, frame_right, frame_up,
			old_centers, old_origins, target, base_spacing, radius, snap,
			force_full, refresh_mask)
		call_deferred("_on_build_ready", epoch, built)
	_task_id = WorkerThreadPool.add_task(task, false, "asterra_ground_clipmap")


func _on_build_ready(epoch: int, built: Dictionary) -> void:
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
	_building = false

	if epoch == _frame_epoch and not built.is_empty():
		var centers_value: Variant = built.get("centers", [])
		var origins_value: Variant = built.get("origins", [])
		var replacements_value: Variant = built.get("replacements", [])
		var updates_value: Variant = built.get("updates", [])
		if centers_value is Array and origins_value is Array \
				and replacements_value is Array and updates_value is Array:
			var centers: Array = centers_value
			var origins: Array = origins_value
			var replacements: Array = replacements_value
			var updates: Array = updates_value
			if centers.size() == STORAGE_LEVELS and origins.size() == STORAGE_LEVELS \
					and replacements.size() == STORAGE_LEVELS and updates.size() == STORAGE_LEVELS:
				_publish_height_update(built, centers, origins, replacements, updates)

	# If a page finished while the worker was busy, fold the debounced cache refresh
	# into the very next update instead of starving refinement during continuous
	# high-speed movement.
	if _cache_refresh_pending and _cache_refresh_left <= 0.0:
		_cache_refresh_pending = false
		_refresh_mask_pending |= _cache_refresh_mask
		_cache_refresh_mask = 0
		_cache_refresh_left = 0.0

	if _force_full_pending or _refresh_mask_pending != 0 \
			or _requested_center.distance_squared_to(_published_center) > 1e-8:
		_start_build()


func _publish_height_update(built: Dictionary, centers: Array, origins: Array,
		replacements: Array, updates: Array) -> void:
	var changed_layers := PackedInt32Array()

	# Initial publication must provide every layer as a complete Image so the
	# Texture2DArray can be allocated once. Later updates mutate the existing CPU
	# images in place and upload only layers whose toroidal contents changed.
	if _height_texture == null:
		var initial_images: Array[Image] = []
		for level in STORAGE_LEVELS:
			var replacement: Variant = replacements[level]
			if not (replacement is Image):
				return
			initial_images.append(replacement)
		var texture := Texture2DArray.new()
		if texture.create_from_images(initial_images) != OK:
			return
		_images = initial_images
		_height_texture = texture
		_material.set_shader_parameter("u_height_pyramid", _height_texture)
		for level in STORAGE_LEVELS:
			changed_layers.append(level)
	else:
		for level in STORAGE_LEVELS:
			var replacement: Variant = replacements[level]
			if replacement is Image:
				_images[level] = replacement
				changed_layers.append(level)
				continue

			var level_updates_value: Variant = updates[level]
			if not (level_updates_value is PackedVector3Array):
				continue
			var level_updates: PackedVector3Array = level_updates_value
			if level_updates.is_empty():
				continue
			var image: Image = _images[level]
			for sample: Vector3 in level_updates:
				image.set_pixel(int(sample.x), int(sample.y), Color(sample.z, 0.0, 0.0, 1.0))
			changed_layers.append(level)

		for level in changed_layers:
			_height_texture.update_layer(_images[level], level)

	for level in STORAGE_LEVELS:
		var new_center: Vector2 = centers[level]
		var new_origin: Vector2i = origins[level]
		_layer_centers[level] = new_center
		_layer_origins[level] = new_origin

	var built_center_value: Variant = built.get("mesh_center", _published_center)
	if built_center_value is Vector2:
		_published_center = built_center_value
	_material.set_shader_parameter("u_height_ready", 1.0)
	_sync_height_layout_uniforms()
	_set_visible(_active)
	_sync_global_cutout(_active)


static func _build_height_update(frame_dir: Vector3, frame_right: Vector3,
		frame_up: Vector3, old_centers: Array[Vector2], old_origins: Array[Vector2i],
		mesh_center: Vector2, base_spacing: float, radius: float, snap: Dictionary,
		force_full: bool, refresh_mask: int) -> Dictionary:
	var centers: Array = []
	var origins: Array = []
	var replacements: Array = []
	var updates: Array = []

	for level in STORAGE_LEVELS:
		var spacing: float = base_spacing * pow(2.0, float(level))
		var new_center := Vector2(
			round(mesh_center.x / spacing) * spacing,
			round(mesh_center.y / spacing) * spacing)
		var old_center: Vector2 = old_centers[level] if level < old_centers.size() else new_center
		var old_origin: Vector2i = old_origins[level] if level < old_origins.size() else Vector2i.ZERO
		var shift := Vector2i(
			int(round((new_center.x - old_center.x) / spacing)),
			int(round((new_center.y - old_center.y) / spacing)))
		var level_full: bool = force_full or absi(shift.x) >= HEIGHT_TEX_SIZE \
			or absi(shift.y) >= HEIGHT_TEX_SIZE
		var refresh_level: bool = (refresh_mask & (1 << level)) != 0

		if level_full:
			var image := Image.create(HEIGHT_TEX_SIZE, HEIGHT_TEX_SIZE, false, Image.FORMAT_RF)
			for logical_y in HEIGHT_TEX_SIZE:
				for logical_x in HEIGHT_TEX_SIZE:
					var plane := new_center + Vector2(
						float(logical_x) - HEIGHT_HALF,
						float(logical_y) - HEIGHT_HALF) * spacing
					var d := _direction_for_plane_static(frame_dir, frame_right, frame_up,
						plane, radius)
					var h: float = GroundHeightStore.sample_height_nonblocking(d, level, snap)
					image.set_pixel(logical_x, logical_y, Color(h, 0.0, 0.0, 1.0))
			centers.append(new_center)
			origins.append(Vector2i.ZERO)
			replacements.append(image)
			updates.append(PackedVector3Array())
			continue

		var new_origin := Vector2i(
			_wrap_index(old_origin.x + shift.x, HEIGHT_TEX_SIZE),
			_wrap_index(old_origin.y + shift.y, HEIGHT_TEX_SIZE))
		var level_updates := PackedVector3Array()
		if refresh_level or shift != Vector2i.ZERO:
			for logical_y in HEIGHT_TEX_SIZE:
				for logical_x in HEIGHT_TEX_SIZE:
					if not refresh_level and not _logical_texel_is_new(
							logical_x, logical_y, shift, HEIGHT_TEX_SIZE):
						continue
					var plane := new_center + Vector2(
						float(logical_x) - HEIGHT_HALF,
						float(logical_y) - HEIGHT_HALF) * spacing
					var d := _direction_for_plane_static(frame_dir, frame_right, frame_up,
						plane, radius)
					var h: float = GroundHeightStore.sample_height_nonblocking(d, level, snap)
					var storage_x: int = _wrap_index(new_origin.x + logical_x, HEIGHT_TEX_SIZE)
					var storage_y: int = _wrap_index(new_origin.y + logical_y, HEIGHT_TEX_SIZE)
					level_updates.append(Vector3(float(storage_x), float(storage_y), h))

		centers.append(new_center)
		origins.append(new_origin)
		replacements.append(null)
		updates.append(level_updates)

	return {
		"mesh_center": mesh_center,
		"centers": centers,
		"origins": origins,
		"replacements": replacements,
		"updates": updates,
	}


static func _logical_texel_is_new(x: int, y: int, shift: Vector2i, size: int) -> bool:
	if shift.x > 0 and x >= size - shift.x:
		return true
	if shift.x < 0 and x < -shift.x:
		return true
	if shift.y > 0 and y >= size - shift.y:
		return true
	if shift.y < 0 and y < -shift.y:
		return true
	return false


static func _wrap_index(value: int, size: int) -> int:
	var wrapped: int = value % size
	return wrapped + size if wrapped < 0 else wrapped


static func _direction_for_plane_static(frame_dir: Vector3, frame_right: Vector3,
		frame_up: Vector3, plane: Vector2, radius: float) -> Vector3:
	return (frame_dir + frame_right * (plane.x / radius)
		+ frame_up * (plane.y / radius)).normalized()


func _build_ring_nodes() -> void:
	var full: ArrayMesh = _build_grid_mesh(false)
	var ring: ArrayMesh = _build_grid_mesh(true)
	for level in RENDER_LEVELS:
		var mi := MeshInstance3D.new()
		mi.name = "GroundClipmapL%d" % level
		mi.mesh = full if level == 0 else ring
		mi.material_override = _material
		mi.set_instance_shader_parameter("clip_level", float(level))
		mi.set_instance_shader_parameter("clip_outer", 1.0 if level == RENDER_LEVELS - 1 else 0.0)
		mi.custom_aabb = AABB(Vector3(-100000.0, -100000.0, -100000.0),
			Vector3(200000.0, 200000.0, 200000.0))
		add_child(mi)
		_rings.append(mi)


static func _build_grid_mesh(with_hole: bool) -> ArrayMesh:
	var vertex_count: int = GRID_VERTS * GRID_VERTS
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	vertices.resize(vertex_count)
	normals.resize(vertex_count)
	uvs.resize(vertex_count)
	var half: float = float(GRID_CELLS) * 0.5
	for y in GRID_VERTS:
		for x in GRID_VERTS:
			var i: int = y * GRID_VERTS + x
			vertices[i] = Vector3(float(x) - half, 0.0, float(y) - half)
			normals[i] = Vector3.UP
			uvs[i] = Vector2(float(x) / float(GRID_CELLS), float(y) / float(GRID_CELLS))

	var indices := PackedInt32Array()
	var inner_half: float = float(GRID_CELLS) * 0.25
	for y in GRID_CELLS:
		for x in GRID_CELLS:
			if with_hole:
				var cx: float = float(x) + 0.5 - half
				var cy: float = float(y) + 0.5 - half
				if absf(cx) < inner_half and absf(cy) < inner_half:
					continue
			var a: int = y * GRID_VERTS + x
			var b: int = a + 1
			var c: int = a + GRID_VERTS
			var d: int = c + 1
			indices.append_array(PackedInt32Array([a, c, b, b, c, d]))

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _sync_common_uniforms(origin: Vector3) -> void:
	_material.set_shader_parameter("u_origin", origin)
	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_frame_dir", _frame_dir)
	_material.set_shader_parameter("u_frame_right", _frame_right)
	_material.set_shader_parameter("u_frame_up", _frame_up)
	_material.set_shader_parameter("u_center_plane", _published_center)
	_material.set_shader_parameter("u_base_spacing", _base_spacing)
	_material.set_shader_parameter("u_grid_cells", float(GRID_CELLS))
	_material.set_shader_parameter("u_height_tex_size", float(HEIGHT_TEX_SIZE))
	_material.set_shader_parameter("u_height_half", HEIGHT_HALF)
	_material.set_shader_parameter("u_storage_levels", float(STORAGE_LEVELS))
	_sync_height_layout_uniforms()
	_sync_global_cutout(_active and _height_texture != null)


func _sync_height_layout_uniforms() -> void:
	if _layer_centers.size() != STORAGE_LEVELS or _layer_origins.size() != STORAGE_LEVELS:
		return
	for level in STORAGE_LEVELS:
		var center: Vector2 = _layer_centers[level]
		var origin: Vector2i = _layer_origins[level]
		_material.set_shader_parameter("u_height_layout%d" % level,
			Vector4(center.x, center.y, float(origin.x), float(origin.y)))


func _sync_material_control() -> void:
	var source: Node = get_node_or_null("/root/MaterialClipmap")
	if source == null:
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var value: Variant = source.get("_texture")
	if not (value is Texture2DArray):
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var texture: Texture2DArray = value
	if texture == _last_material_control:
		return
	_last_material_control = texture
	_material.set_shader_parameter("u_material_clipmap", texture)
	_material.set_shader_parameter("u_material_clipmap_ready", 1.0)
	_material.set_shader_parameter("u_material_center", source.get("_center"))
	_material.set_shader_parameter("u_material_right", source.get("_right"))
	_material.set_shader_parameter("u_material_up", source.get("_up"))
	_material.set_shader_parameter("u_material_res", MATERIAL_RES)
	_material.set_shader_parameter("u_material_texel_m", MATERIAL_TEXEL_M)


func _sync_global_cutout(enabled: bool) -> void:
	var terrain: PlanetTerrain = _terrain_ref.get_ref() if _terrain_ref != null else null
	if terrain == null:
		terrain = _find_terrain(get_tree().root)
		if terrain != null:
			_terrain_ref = weakref(terrain)
	if terrain == null:
		return
	var mats: Array = terrain.debug_materials()
	if mats.is_empty():
		return
	var ground: ShaderMaterial = mats[0]
	ground.set_shader_parameter("u_ground_clipmap_cutout", 1.0 if enabled else 0.0)
	if not enabled:
		return
	var outer_spacing: float = _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	var outer_half: float = float(GRID_CELLS) * 0.5 * outer_spacing
	ground.set_shader_parameter("u_ground_clipmap_frame_dir", _frame_dir)
	ground.set_shader_parameter("u_ground_clipmap_right", _frame_right)
	ground.set_shader_parameter("u_ground_clipmap_up", _frame_up)
	ground.set_shader_parameter("u_ground_clipmap_center_plane", _published_center)
	ground.set_shader_parameter("u_ground_clipmap_cut_half_extent",
		outer_half * GLOBAL_CUT_FRACTION)


func _sync_terrain_worker_budget(local_active: bool) -> void:
	var terrain: PlanetTerrain = _terrain_ref.get_ref() if _terrain_ref != null else null
	if terrain == null:
		terrain = _find_terrain(get_tree().root)
		if terrain != null:
			_terrain_ref = weakref(terrain)
	if terrain == null or not (terrain is FastPlanetTerrain):
		return
	var normal_cap: int = clampi(int(OS.get_processor_count() / 4), 2, GLOBAL_BUILD_CAP)
	terrain.set("_max_concurrent_builds",
		LOCAL_GLOBAL_BUILD_CAP if local_active else normal_cap)


func _find_terrain(node: Node) -> PlanetTerrain:
	if node is PlanetTerrain:
		return node
	for child: Node in node.get_children():
		var found: PlanetTerrain = _find_terrain(child)
		if found != null:
			return found
	return null


func _set_active(value: bool) -> void:
	if _active != value:
		_sync_terrain_worker_budget(value)
	_active = value
	_set_visible(value and _height_texture != null)
	_sync_global_cutout(value and _height_texture != null)


func _set_visible(value: bool) -> void:
	for ring: MeshInstance3D in _rings:
		ring.visible = value


func _on_height_tile_ready(level: int, _face: int, _tile_x: int, _tile_y: int) -> void:
	if not _have_frame:
		return
	var used_level: int = clampi(level, 0, STORAGE_LEVELS - 1)
	_cache_refresh_mask |= 1 << used_level
	_cache_refresh_pending = true
	if _cache_refresh_left <= 0.0:
		_cache_refresh_left = CACHE_REFRESH_DELAY


func _on_world_ready(_fields: PlanetFields) -> void:
	_have_frame = false
	_frame_epoch += 1
	_cache_refresh_pending = false
	_cache_refresh_left = 0.0
	_cache_refresh_mask = 0
	_refresh_mask_pending = 0
	_force_full_pending = false
	_images.clear()
	_layer_centers.clear()
	_layer_origins.clear()
	_height_texture = null
	_material.set_shader_parameter("u_height_ready", 0.0)
	_set_visible(false)
	_sync_global_cutout(false)


func _on_region_changed(_center: Vector3, _radius_m: float) -> void:
	if not _have_frame:
		return
	# Edits are sparse and correctness-critical. Re-evaluate all seven local
	# windows, but preserve the fixed meshes and the already allocated GPU array.
	_queue_target(_requested_center, true, 0)


func _exit_tree() -> void:
	_sync_global_cutout(false)
	_sync_terrain_worker_budget(false)
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
