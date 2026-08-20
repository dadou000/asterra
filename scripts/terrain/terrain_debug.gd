class_name TerrainDebug
extends Node3D
## Visual debugging for the streaming quadtree.
##
## The two glyph modes exist to answer one question: does a chunk's place in the
## cube-sphere parameterisation agree with the UVs its own mesh was given?
##
##   * "tile axes" builds a glyph from the quadtree node alone -- face, u0, v0,
##     size -- and floats it over the tile.
##   * "tile UV glyph" has the terrain shader draw the same glyph out of the
##     mesh's UV attribute.
##
## Nothing is shared between the two paths, so if the mapping is coherent they
## are the same glyph in the same orientation on every tile of every face. A tile
## that is rotated a quarter turn, or mirrored across a cube edge, disagrees with
## the glyph floating above it and says so at a glance.
##
## The arrow runs along +v and the short tick along +u, so the pair also shows
## handedness: a mirrored tile puts its tick on the wrong side.

var terrain: PlanetTerrain

var tile_axes := false: set = set_tile_axes
var uv_glyphs := false: set = set_uv_glyphs
var lod_tint := false: set = set_lod_tint
var face_seams := false: set = set_face_seams
var hide_water := false: set = set_hide_water
var wireframe := false: set = set_wireframe
var freeze_stream := false: set = set_freeze_stream
var heightmap := true: set = set_heightmap
var surface_texture := true: set = set_surface_texture
var force_lod := false: set = set_force_lod
var force_lod_depth := 5: set = set_force_lod_depth

var _wireframes_ready := false
var _axes: MultiMeshInstance3D
var _seams: MeshInstance3D
var _nodes: Array = []

func _ready() -> void:
	process_priority = 20
	_axes = MultiMeshInstance3D.new()
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.mesh = _build_glyph()
	mm.instance_count = 0
	_axes.multimesh = mm
	_axes.visible = false
	# The glyphs move with the tiles every frame; a computed AABB would be a
	# frame behind and cull them at exactly the moments they are wanted.
	_axes.custom_aabb = AABB(Vector3(-2e7, -2e7, -2e7), Vector3(4e7, 4e7, 4e7))
	# The glyphs float above their own tile and are meant to be readable from
	# anywhere, so they are never occluded and never lit.
	_axes.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_axes)

	_seams = MeshInstance3D.new()
	_seams.mesh = _build_seam_lines()
	_seams.visible = false
	_seams.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_seams)

func _process(_dt: float) -> void:
	if tile_axes:
		_rebuild_axes()
	if hide_water and terrain != null:
		# Re-applied every frame: chunks stream in while the toggle is on.
		for root in terrain.roots:
			_apply_water_visible(root, false)
	if face_seams:
		# Drawn in planet coordinates; the floating origin is the node's position.
		_seams.position = Frames.to_render(Vec3D.new(0, 0, 0))

# ------------------------------------------------------------------ toggles ---
func set_tile_axes(v: bool) -> void:
	tile_axes = v
	if _axes != null:
		_axes.visible = v
		if not v:
			_axes.multimesh.instance_count = 0

func set_uv_glyphs(v: bool) -> void:
	uv_glyphs = v
	_set_terrain_param("u_debug_uv", 1.0 if v else 0.0)

func set_lod_tint(v: bool) -> void:
	lod_tint = v
	_set_terrain_param("u_debug_lod", 1.0 if v else 0.0)

func set_face_seams(v: bool) -> void:
	face_seams = v
	if _seams != null:
		_seams.visible = v

func set_hide_water(v: bool) -> void:
	hide_water = v
	if terrain == null:
		return
	for root in terrain.roots:
		_apply_water_visible(root, not v)

func set_wireframe(v: bool) -> void:
	wireframe = v
	var vp := get_viewport()
	if vp == null:
		return
	if v and not _wireframes_ready:
		# A mesh only carries a wireframe index buffer if the server was building
		# them when the mesh was uploaded; without one the wireframe view draws
		# whatever else is in the buffer, which is not the mesh. Every chunk
		# already resident predates the switch, so they are re-meshed once. That
		# costs a rebuild the first time this is turned on, which is the price of
		# not carrying a second index buffer for every chunk all session.
		RenderingServer.set_debug_generate_wireframes(true)
		_wireframes_ready = true
		if terrain != null:
			terrain.build_roots()
	vp.debug_draw = Viewport.DEBUG_DRAW_WIREFRAME if v else Viewport.DEBUG_DRAW_DISABLED

## Stops the quadtree from restructuring, so a tile can be flown around and
## inspected instead of being rebuilt out from under the inspection.
func set_freeze_stream(v: bool) -> void:
	freeze_stream = v
	if terrain != null:
		terrain.set_process(not v)

## The height field, on or off. The terrain is meshed from it, so this costs a
## full re-mesh -- which is also the only way to see the bare cube sphere the
## rest of the pipeline is built on.
func set_heightmap(v: bool) -> void:
	heightmap = v
	if ChunkBuilder.debug_flat == (not v):
		return
	ChunkBuilder.debug_flat = not v
	if terrain != null:
		terrain.build_roots()

func set_surface_texture(v: bool) -> void:
	surface_texture = v
	_set_terrain_param("u_debug_plain", 0.0 if v else 1.0)

## Pin every tile to one quadtree depth instead of splitting by distance, so a
## level can be looked at on its own terms.
func set_force_lod(v: bool) -> void:
	force_lod = v
	_apply_force_lod()

func set_force_lod_depth(v: int) -> void:
	force_lod_depth = clampi(v, 0, 8)
	_apply_force_lod()

func _apply_force_lod() -> void:
	if terrain != null:
		terrain.forced_depth = force_lod_depth if force_lod else -1

func _set_terrain_param(shader_param: String, value: float) -> void:
	if terrain == null:
		return
	for mat in terrain.debug_materials():
		mat.set_shader_parameter(shader_param, value)

func _apply_water_visible(node, on: bool) -> void:
	if node.water_mi != null:
		node.water_mi.visible = on
	for c in node.children:
		_apply_water_visible(c, on)

# -------------------------------------------------------------------- axes ---
func _rebuild_axes() -> void:
	if terrain == null or terrain.roots.is_empty():
		return
	_nodes.clear()
	for root in terrain.roots:
		_collect(root, _nodes)
	var mm := _axes.multimesh
	mm.instance_count = _nodes.size()
	for i in _nodes.size():
		var node = _nodes[i]
		var cu: float = node.u0 + node.size * 0.5
		var cv: float = node.v0 + node.size * 0.5
		var c := CubeSphere.face_uv_to_dir(node.face, cu, cv)
		# The tile's own axes, taken from the parameterisation rather than from
		# anything the mesher produced.
		var e: float = node.size * 0.02
		var du := (CubeSphere.face_uv_to_dir(node.face, cu + e, cv)
			- CubeSphere.face_uv_to_dir(node.face, cu - e, cv)).normalized()
		var dv := (CubeSphere.face_uv_to_dir(node.face, cu, cv + e)
			- CubeSphere.face_uv_to_dir(node.face, cu, cv - e)).normalized()
		du = (du - c * du.dot(c)).normalized()
		dv = (dv - c * dv.dot(c)).normalized()
		var lift: float = maxf(node.arc * 0.05, 3.0)
		var pivot: Vec3D = node.chunk.get_meta("pivot")
		var pos := Frames.to_render(pivot.add(Vec3D.from_v3(c).mul(lift)))
		var s: float = node.arc * 0.34
		# The glyph is modelled pointing down -Z, Godot's forward, so mapping +v
		# onto -Z keeps the basis right-handed and the glyph unmirrored.
		mm.set_instance_transform(i, Transform3D(Basis(du * s, c * s, -dv * s), pos))
		mm.set_instance_color(i, depth_color(node.depth))

func _collect(node, out: Array) -> void:
	if node.chunk != null:
		out.append(node)
	for c in node.children:
		_collect(c, out)

## Same palette the shader uses for the LOD tint, so the two modes agree.
static func depth_color(depth: int) -> Color:
	var d := float(depth)
	return Color(
		0.5 + 0.5 * cos(TAU * (d * 0.13 + 0.0)),
		0.5 + 0.5 * cos(TAU * (d * 0.13 + 0.33)),
		0.5 + 0.5 * cos(TAU * (d * 0.13 + 0.67)))

# -------------------------------------------------------------------- mesh ---
## Arrow along -Z with a tick along +X, flat in the tangent plane. Proportions
## match `uv_glyph()` in terrain_ground.gdshader.
func _build_glyph() -> ArrayMesh:
	var v := PackedVector3Array()
	_quad(v, -0.045, 0.28, 0.045, -0.16)          # shaft
	v.append_array([Vector3(-0.15, 0.0, -0.16), Vector3(0.15, 0.0, -0.16), Vector3(0.0, 0.0, -0.40)])
	_quad(v, 0.16, 0.03, 0.34, -0.03)             # +u tick
	v.append_array([Vector3(0.34, 0.0, 0.06), Vector3(0.34, 0.0, -0.06), Vector3(0.44, 0.0, 0.0)])

	var arr := []
	arr.resize(Mesh.ARRAY_MAX)
	arr[Mesh.ARRAY_VERTEX] = v
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arr)
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.vertex_color_use_as_albedo = true
	mat.no_depth_test = true
	mat.render_priority = 2
	mesh.surface_set_material(0, mat)
	return mesh

func _quad(v: PackedVector3Array, x0: float, z0: float, x1: float, z1: float) -> void:
	var a := Vector3(x0, 0.0, z0)
	var b := Vector3(x1, 0.0, z0)
	var c := Vector3(x1, 0.0, z1)
	var d := Vector3(x0, 0.0, z1)
	v.append_array([a, b, c, a, c, d])

## The twelve cube edges, drawn where they actually land on the sphere.
func _build_seam_lines() -> ArrayMesh:
	var r: float = 1000000.0
	if Planet.cfg != null:
		r = Planet.cfg.planet_radius
	r += 900.0
	var v := PackedVector3Array()
	var col := PackedColorArray()
	for face in 6:
		for edge in 4:
			var prev := Vector3.ZERO
			for i in 65:
				var s := (float(i) / 64.0) * 2.0 - 1.0
				var d: Vector3
				match edge:
					0: d = CubeSphere.face_uv_to_dir(face, 1.0, s)
					1: d = CubeSphere.face_uv_to_dir(face, -1.0, s)
					2: d = CubeSphere.face_uv_to_dir(face, s, 1.0)
					_: d = CubeSphere.face_uv_to_dir(face, s, -1.0)
				var p := d * r
				if i > 0:
					v.append(prev)
					v.append(p)
					col.append(Color(1.0, 0.25, 0.1))
					col.append(Color(1.0, 0.25, 0.1))
				prev = p
	var arr := []
	arr.resize(Mesh.ARRAY_MAX)
	arr[Mesh.ARRAY_VERTEX] = v
	arr[Mesh.ARRAY_COLOR] = col
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arr)
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	mat.no_depth_test = true
	mat.render_priority = 3
	mesh.surface_set_material(0, mat)
	return mesh
