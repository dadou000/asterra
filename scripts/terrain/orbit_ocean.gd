class_name OrbitOcean
extends Node3D
## Global sea-level shell used only for distant graphics.
##
## The streamed water mesh is intentionally tied to terrain chunks because it
## needs local lake levels, shore depth and collision-scale detail. That makes it
## the wrong representation for orbit, where a single coarse triangle can span
## tens of kilometres. This shell is geometrically just a smooth sphere; its
## coastline is cut per-fragment from Planet.orbit_elevation_texture.

const GRID := 96
const SHELL_OFFSET_M := 1.5

var _mesh_instance: MeshInstance3D
var _material: ShaderMaterial

func _ready() -> void:
	process_priority = 11
	Frames.origin_shifted.connect(_on_origin_shifted)
	Planet.world_ready.connect(_on_world_ready)
	if Planet.ready_state:
		_refresh()

func _process(_dt: float) -> void:
	if _material != null:
		_material.set_shader_parameter("u_sun_dir", Frames.helion_dir)

func _on_world_ready(_fields: PlanetFields) -> void:
	_refresh()

func _refresh() -> void:
	if not Planet.ready_state or Planet.orbit_elevation_texture == null:
		return

	if _mesh_instance == null:
		_mesh_instance = MeshInstance3D.new()
		_mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(_mesh_instance)

	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/orbit_ocean.gdshader")
	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height)
	_material.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_material.set_shader_parameter("u_orbit_elevation", Planet.orbit_elevation_texture)
	_material.set_shader_parameter("u_orbit_face_res", float(Planet.orbit_texture_face_res))

	var mesh := _build_shell(Planet.cfg.planet_radius + SHELL_OFFSET_M)
	mesh.surface_set_material(0, _material)
	_mesh_instance.mesh = mesh
	_sync_origin()

func _build_shell(radius: float) -> ArrayMesh:
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var indices := PackedInt32Array()
	var row := GRID + 1

	for face in 6:
		var base := verts.size()
		for j in GRID + 1:
			var v := float(j) / float(GRID) * 2.0 - 1.0
			for i in GRID + 1:
				var u := float(i) / float(GRID) * 2.0 - 1.0
				var d := CubeSphere.face_uv_to_dir(face, u, v)
				verts.append(d * radius)
				norms.append(d)
		for j in GRID:
			for i in GRID:
				var a := base + j * row + i
				var b := a + 1
				var c := a + row
				var d_i := c + 1
				indices.append_array([a, c, b, b, c, d_i])

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

func _on_origin_shifted(_delta: Vector3) -> void:
	_sync_origin()

func _sync_origin() -> void:
	position = Frames.to_render(Vec3D.new(0.0, 0.0, 0.0))
	if _material != null:
		_material.set_shader_parameter("u_origin",
			Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z)))
