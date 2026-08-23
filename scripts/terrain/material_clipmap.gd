extends Node
## Virtual material-control clipmap for terrain PBR composition.
##
## Geometry now carries only a fallback classification. Close and mid-distance
## material weights come from three camera-centred texture levels, allowing the
## material stream to recenter independently of visual and collision chunks.

## These texels hold material *weights*, not albedo or normals. 128 samples per
## axis preserve the same three world-space coverage bands while cutting an
## expensive recenter from 196,608 planet evaluations to 49,152. Fine PBR detail
## remains in the material textures and is therefore unaffected by this value.
const RES := 128
# 8 m ground control, 128 m regional control, and 2.048 km orbital control.
# At 128 texels the levels retain the original 1/16/262 km spans, so material
# identity does not fall back to mesh vertices in an ordinary aircraft view.
const TEXEL_M := Vector3(8.0, 128.0, 2048.0)
const RECENTER_FRACTION := 0.20

var _texture: Texture2DArray
var _center := Vector3(1.0, 0.0, 0.0)
var _right := Vector3(0.0, 0.0, -1.0)
var _up := Vector3(0.0, 1.0, 0.0)
var _requested := Vector3(1.0, 0.0, 0.0)
var _generation := 0
var _building := false
var _task_id := -1
var _terrain_ref: WeakRef


func _ready() -> void:
	process_priority = 7
	Planet.world_ready.connect(_on_world_ready)


func _process(_dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		return
	if _texture != null and (_terrain_ref == null or _terrain_ref.get_ref() == null):
		_sync_material()
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos := camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		return
	var d := planet_pos.normalized()
	var trigger := float(RES) * TEXEL_M.x * RECENTER_FRACTION
	if _texture == null:
		if not _building:
			_queue(d)
	elif _surface_distance(_requested if _building else _center, d) >= trigger:
		_queue(d)


func _queue(center: Vector3) -> void:
	var target := center.normalized()
	# Coalesce identical requests while a worker owns the current generation.
	# Without this, the missing-texture path superseded that worker every frame,
	# so no material clipmap could ever publish.
	if _building and _surface_distance(_requested, target) < 1.0:
		return
	_requested = target
	_generation += 1
	if _building:
		return
	_start(_generation, _requested)


func _start(request: int, center: Vector3) -> void:
	_building = true
	var basis := CubeSphere.tangent_basis(center)
	var right: Vector3 = basis[0]
	var up: Vector3 = basis[1]
	var task := func() -> void:
		var images := _build_images(center, right, up)
		call_deferred("_ready_images", request, center, right, up, images)
	_task_id = WorkerThreadPool.add_task(task, false, "asterra_material_clipmap")


static func _build_images(center: Vector3, right: Vector3, up: Vector3) -> Array[Image]:
	var images: Array[Image] = []
	if not Planet.ready_state:
		return images
	var radius := Planet.cfg.planet_radius
	var half := float(RES) * 0.5
	for texel_value in [TEXEL_M.x, TEXEL_M.y, TEXEL_M.z]:
		var texel := float(texel_value)
		var image := Image.create(RES, RES, false, Image.FORMAT_RGBA8)
		for y in RES:
			var my := (float(y) + 0.5 - half) * texel
			var row := center + up * (my / radius)
			for x in RES:
				var mx := (float(x) + 0.5 - half) * texel
				var d := (row + right * (mx / radius)).normalized()
				image.set_pixel(x, y, Planet.surface_composition(d))
		images.append(image)
	return images


func _ready_images(request: int, center: Vector3, right: Vector3, up: Vector3,
		images: Array[Image]) -> void:
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
		_task_id = -1
	_building = false
	if request == _generation and images.size() == 3:
		var texture := Texture2DArray.new()
		if texture.create_from_images(images) == OK:
			_texture = texture
			_center = center
			_right = right
			_up = up
			_sync_material()
	if request != _generation:
		_start(_generation, _requested)


func _sync_material() -> void:
	var terrain: PlanetTerrain = _terrain_ref.get_ref() if _terrain_ref != null else null
	if terrain == null:
		terrain = _find_terrain(get_tree().root)
		if terrain != null:
			_terrain_ref = weakref(terrain)
	if terrain == null:
		return
	for value in terrain.debug_materials():
		var material: ShaderMaterial = value
		material.set_shader_parameter("u_material_clipmap_ready", 1.0)
		material.set_shader_parameter("u_material_clipmap", _texture)
		material.set_shader_parameter("u_material_center", _center)
		material.set_shader_parameter("u_material_right", _right)
		material.set_shader_parameter("u_material_up", _up)
		material.set_shader_parameter("u_material_res", float(RES))
		material.set_shader_parameter("u_material_texel_m", TEXEL_M)


func _find_terrain(node: Node) -> PlanetTerrain:
	if node is PlanetTerrain:
		return node
	for child in node.get_children():
		var found := _find_terrain(child)
		if found != null:
			return found
	return null


func _on_world_ready(_fields: PlanetFields) -> void:
	_generation += 1
	_texture = null
	if not _building:
		_queue(_requested)


func _surface_distance(a: Vector3, b: Vector3) -> float:
	return acos(clampf(a.dot(b), -1.0, 1.0)) * Planet.cfg.planet_radius


func _exit_tree() -> void:
	if _task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_task_id)
