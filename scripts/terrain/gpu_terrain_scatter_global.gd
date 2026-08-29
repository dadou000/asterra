extends "res://scripts/terrain/gpu_terrain_scatter_compact.gd"
## Latest-0.0.5 GPU scatter binding plus the first real-asset ecology layer.
##
## The indirect RenderingDevice compaction path is still disabled because the real
## runtime previously reported invalid/stale indirect buffers. Until that backend
## is repaired, scanned assets are a sparse accent layer over the cheap procedural
## grass/stones. Every scatter material is now fed the active rendered-terrain cache
## so placement uses the same L0 height surface as GroundGeometryClipmap.

const STABLE_FALLBACK_ONLY := true
const ECOLOGY_SHADER_PATH := "res://shaders/terrain_scatter_ecology.gdshader"
const ECOLOGY_RUNTIME_ROOT := "res://assets/scatter/runtime"

# Vertex-fallback classification is evaluated once per mesh vertex, not once per
# instance. A permissive 200k-triangle source-mesh guard therefore turns a tiny
# candidate lattice into millions of expensive terrain/context evaluations. Keep
# only genuinely light source/LOD meshes in this path; heavier library assets stay
# available for the future transform-compute/impostor renderer.
const ECOLOGY_MAX_SELECTED_LOD_TRIANGLES := 12000
const FALLBACK_SCATTER_MAX_ALTITUDE_M := 1200.0

# Preserve roughly the same physical coverage as the first integration while
# reducing the number of scanned-mesh candidates. Procedural grass/stones still
# provide dense ground coverage beneath these real-asset accents.
const ECOLOGY_ASSETS := [
	{"id":"grass_bermuda_01", "lod":0, "grid":12, "spacing":5.3, "density":0.72, "kind":0, "salt":1201, "scale_min":0.82, "scale_max":1.18, "wind":0.045, "shadows":false},
	{"id":"fern_02", "lod":0, "grid":5, "spacing":17.5, "density":0.54, "kind":1, "salt":1213, "scale_min":0.76, "scale_max":1.24, "wind":0.025, "shadows":false},
	{"id":"shrub_03", "lod":0, "grid":4, "spacing":31.0, "density":0.36, "kind":2, "salt":1229, "scale_min":0.72, "scale_max":1.34, "wind":0.018, "shadows":false},
	{"id":"anthurium_botany_01", "lod":0, "grid":2, "spacing":42.0, "density":0.36, "kind":6, "salt":1249, "scale_min":0.78, "scale_max":1.22, "wind":0.014, "shadows":false},
	{"id":"cheiridopsis_succulent", "lod":0, "grid":2, "spacing":52.0, "density":0.28, "kind":3, "salt":1277, "scale_min":0.54, "scale_max":1.10, "wind":0.002, "shadows":false},
	{"id":"boulder_01", "lod":2, "grid":2, "spacing":82.0, "density":0.34, "kind":4, "salt":1301, "scale_min":0.72, "scale_max":1.85, "wind":0.0, "shadows":false},
	{"id":"dead_tree_trunk", "lod":2, "grid":2, "spacing":72.0, "density":0.20, "kind":5, "salt":1327, "scale_min":0.78, "scale_max":1.30, "wind":0.0, "shadows":false},
	{"id":"dead_quiver_branch_01", "lod":2, "grid":4, "spacing":17.5, "density":0.28, "kind":3, "salt":1361, "scale_min":0.72, "scale_max":1.34, "wind":0.0, "shadows":false},
	{"id":"dry_quiver_leaf", "lod":2, "grid":3, "spacing":24.0, "density":0.20, "kind":3, "salt":1381, "scale_min":0.72, "scale_max":1.25, "wind":0.0, "shadows":false},
	{"id":"quiver_tree_01", "lod":0, "grid":2, "spacing":120.0, "density":0.18, "kind":7, "salt":1409, "scale_min":0.82, "scale_max":1.22, "wind":0.010, "shadows":false},
]

var _static_scatter_bound := false
var _bound_edit_generation := -1
var _bound_edit_ready := false
var _bound_active_generation := -1
var _bound_active_window_generation := -1
var _bound_active_ready := false

var _bound_terrain_cache_texture: Variant = null
var _bound_terrain_cache_generation := -1
var _bound_terrain_cache_res := -1
var _bound_terrain_cache_ready := false
var _bound_terrain_cache_anchor_dir := Vector3.ZERO
var _bound_terrain_cache_anchor_right := Vector3.ZERO
var _bound_terrain_cache_anchor_up := Vector3.ZERO
var _bound_terrain_cache_base_spacing := -1.0

var _ecology_shader: Shader
var _ecology_batches: Array[Dictionary] = []
var _ecology_loaded_asset_ids: PackedStringArray = PackedStringArray()
var _ecology_selected_triangles: int = 0
var _ecology_candidate_instances: int = 0


func _ready() -> void:
	super._ready()
	if STABLE_FALLBACK_ONLY:
		_compact_method_supported = false
		_compact_init_failed = false
		_compact_init_ready = false
		_hide_compact_batches()
	_build_ecology_batches()
	# Materials created after super._ready() missed the initial static/context bind.
	# Force one complete bind after the runtime meshes have been attached.
	_static_scatter_bound = false
	if Planet.ready_state and Planet.cfg != null:
		_bind_gpu_resources(true)


func _process(dt: float) -> void:
	super._process(dt)
	if not _debug_enabled or not Planet.ready_state or Planet.cfg == null:
		return
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null or not _have_anchor:
		return
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		return
	var observer_radius: float = planet_pos.length()
	var radial_altitude: float = observer_radius - Planet.cfg.planet_radius
	if radial_altitude > FALLBACK_SCATTER_MAX_ALTITUDE_M:
		# Grass-sized geometry is sub-pixel here. Suppress both inherited procedural
		# candidates and ecology rather than paying vertex cost out to the old 12 km
		# diagnostic ceiling.
		_set_visible(false)
		return
	if radial_altitude < MIN_RADIAL_ALTITUDE_M:
		return

	# The terrain cache can swap/advance independently of planet-context generation,
	# so check its cheap signature every frame. Shader uniforms are touched only when
	# the active cache, generation or tangent anchor actually changes.
	_bind_authoritative_terrain_cache(false)

	if _ecology_batches.is_empty():
		return
	var observer_dir: Vector3 = planet_pos / observer_radius
	var observer_surface: Vector3 = observer_dir * Planet.cfg.planet_radius
	var anchor_surface: Vector3 = _anchor_dir * Planet.cfg.planet_radius
	var rel: Vector3 = observer_surface - anchor_surface
	var px: float = rel.dot(_anchor_right)
	var py: float = rel.dot(_anchor_up)

	for batch_data: Dictionary in _ecology_batches:
		var grid: int = int(batch_data["grid"])
		var spacing: float = float(batch_data["spacing"])
		var density: float = float(batch_data["density"])
		var center := Vector2(round(px / spacing), round(py / spacing))
		var last_center: Vector2 = batch_data.get("last_center", Vector2(1.0e30, 1.0e30))
		var last_origin: Vector3 = batch_data.get("last_origin", Vector3(1.0e30, 1.0e30, 1.0e30))
		var last_anchor: Vector3 = batch_data.get("last_anchor", Vector3.ZERO)
		if center == last_center and origin == last_origin \
				and last_anchor.distance_squared_to(_anchor_dir) <= 1.0e-12:
			continue
		var batch_materials: Array = batch_data["materials"]
		for value: Variant in batch_materials:
			var material: ShaderMaterial = value as ShaderMaterial
			if material != null:
				_sync_material_window(material, grid, spacing, density, center, origin)
		batch_data["last_center"] = center
		batch_data["last_origin"] = origin
		batch_data["last_anchor"] = _anchor_dir


func _bind_gpu_resources(force: bool) -> void:
	if Planet.cfg == null:
		return

	# GroundGeometryClipmap also uses this resident global macro map. It is only the
	# cold-cache fallback for scatter now; warmed near-field placement comes directly
	# from GroundGeometryClipmap's active L0 cache below.
	var macro: Texture2DArray = Planet.global_height_texture if Planet.ready_state else null
	var macro_res: int = Planet.global_height_face_res if Planet.ready_state else 0
	if force or macro != _bound_macro or macro_res != _bound_macro_res:
		_bound_macro = macro
		_bound_macro_res = macro_res
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_scatter_macro_elevation", macro)
			material.set_shader_parameter("u_scatter_macro_face_res", float(macro_res))
			material.set_shader_parameter("u_scatter_macro_ready", 1.0 if macro != null else 0.0)

	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_ctx_ready", 0.0)
		return

	var generation: int = int(context.get("generation"))
	if force or generation != _bound_context_generation:
		_bound_context_generation = generation
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_ctx_soil", context.get("soil_texture"))
			material.set_shader_parameter("u_ctx_surface", context.get("surface_texture"))
			material.set_shader_parameter("u_ctx_geology", context.get("geology_texture"))
			material.set_shader_parameter("u_ctx_structure", context.get("structure_texture"))
			material.set_shader_parameter("u_ctx_climate", context.get("climate_texture"))
			material.set_shader_parameter("u_ctx_hydrology", context.get("hydrology_texture"))
			material.set_shader_parameter("u_ctx_rock", context.get("rock_texture"))
			material.set_shader_parameter("u_ctx_biome", context.get("biome_texture"))
			material.set_shader_parameter("u_ctx_face_res", float(context.get("face_res")))
			material.set_shader_parameter("u_ctx_ready", 1.0)

	if force or not _static_scatter_bound:
		var scatter_seed: int = Planet.cfg.stream_seed("gpu_scatter") & 0x00ffffff
		var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_scatter_seed", maxi(scatter_seed, 1))
			material.set_shader_parameter("u_scatter_detail_seed", maxi(detail_seed, 1))
			material.set_shader_parameter("u_scatter_geomorph_spacing", 0.75)
		_static_scatter_bound = true

	_bind_authoritative_terrain_cache(force)

	var edits: Node = get_node_or_null("/root/TerrainEditDeltaGPU")
	if edits != null and edits.has_method("sample_params"):
		var ep: Dictionary = edits.call("sample_params")
		var edit_generation: int = int(ep.get("generation", 0))
		var edit_ready: bool = bool(ep.get("ready", false))
		if force or edit_generation != _bound_edit_generation or edit_ready != _bound_edit_ready:
			for material: ShaderMaterial in _materials:
				material.set_shader_parameter("u_edit_delta", ep.get("texture"))
				material.set_shader_parameter("u_edit_ready", 1.0 if edit_ready else 0.0)
				material.set_shader_parameter("u_edit_center_dir", ep.get("center_dir", Vector3.RIGHT))
				material.set_shader_parameter("u_edit_center_right", ep.get("center_right", Vector3.BACK))
				material.set_shader_parameter("u_edit_center_up", ep.get("center_up", Vector3.UP))
				material.set_shader_parameter("u_edit_half_extent_m", float(ep.get("half_extent_m", 256.0)))
			_bound_edit_generation = edit_generation
			_bound_edit_ready = edit_ready

	var active: Node = get_node_or_null("/root/TerrainDeformationGPU")
	if active != null and active.has_method("sample_params"):
		var ap: Dictionary = active.call("sample_params")
		var active_generation: int = int(ap.get("generation", 0))
		var active_window_generation: int = int(ap.get("window_generation", 0))
		var active_ready: bool = bool(ap.get("ready", false))
		if force or active_generation != _bound_active_generation \
				or active_window_generation != _bound_active_window_generation \
				or active_ready != _bound_active_ready:
			for material: ShaderMaterial in _materials:
				material.set_shader_parameter("u_active_deform", ap.get("texture"))
				material.set_shader_parameter("u_active_deform_ready", 1.0 if active_ready else 0.0)
				material.set_shader_parameter("u_active_deform_center_dir", ap.get("center_dir", Vector3.RIGHT))
				material.set_shader_parameter("u_active_deform_center_right", ap.get("center_right", Vector3.BACK))
				material.set_shader_parameter("u_active_deform_center_up", ap.get("center_up", Vector3.UP))
				material.set_shader_parameter("u_active_deform_half_extent_m", float(ap.get("half_extent_m", 32.0)))
			_bound_active_generation = active_generation
			_bound_active_window_generation = active_window_generation
			_bound_active_ready = active_ready


func _bind_authoritative_terrain_cache(force: bool) -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null or not terrain.has_method("rendered_contact_sample_params"):
		_disable_authoritative_terrain_cache(force)
		return
	var params_value: Variant = terrain.call("rendered_contact_sample_params")
	if not (params_value is Dictionary):
		_disable_authoritative_terrain_cache(force)
		return
	var params: Dictionary = params_value
	if params.is_empty():
		_disable_authoritative_terrain_cache(force)
		return

	var texture: Variant = params.get("cache_texture")
	var ready: bool = bool(params.get("cache_ready", false)) and texture != null
	var cache_generation: int = int(params.get("cache_generation", 0))
	var cache_res: int = int(params.get("cache_res", 512))
	var cache_anchor_dir: Vector3 = params.get("anchor_dir", Vector3.RIGHT)
	var cache_anchor_right: Vector3 = params.get("anchor_right", Vector3.BACK)
	var cache_anchor_up: Vector3 = params.get("anchor_up", Vector3.UP)
	var base_spacing: float = float(params.get("base_spacing", 0.75))
	var changed := force \
		or texture != _bound_terrain_cache_texture \
		or ready != _bound_terrain_cache_ready \
		or cache_generation != _bound_terrain_cache_generation \
		or cache_res != _bound_terrain_cache_res \
		or cache_anchor_dir.distance_squared_to(_bound_terrain_cache_anchor_dir) > 1.0e-12 \
		or cache_anchor_right.distance_squared_to(_bound_terrain_cache_anchor_right) > 1.0e-12 \
		or cache_anchor_up.distance_squared_to(_bound_terrain_cache_anchor_up) > 1.0e-12 \
		or absf(base_spacing - _bound_terrain_cache_base_spacing) > 1.0e-6
	if not changed:
		return

	for material: ShaderMaterial in _materials:
		material.set_shader_parameter("u_scatter_terrain_cache", texture)
		material.set_shader_parameter("u_scatter_terrain_cache_ready", 1.0 if ready else 0.0)
		material.set_shader_parameter("u_scatter_terrain_cache_generation", cache_generation)
		material.set_shader_parameter("u_scatter_terrain_cache_res", cache_res)
		material.set_shader_parameter("u_scatter_terrain_anchor_dir", cache_anchor_dir)
		material.set_shader_parameter("u_scatter_terrain_anchor_right", cache_anchor_right)
		material.set_shader_parameter("u_scatter_terrain_anchor_up", cache_anchor_up)
		material.set_shader_parameter("u_scatter_terrain_base_spacing", base_spacing)

	_bound_terrain_cache_texture = texture
	_bound_terrain_cache_ready = ready
	_bound_terrain_cache_generation = cache_generation
	_bound_terrain_cache_res = cache_res
	_bound_terrain_cache_anchor_dir = cache_anchor_dir
	_bound_terrain_cache_anchor_right = cache_anchor_right
	_bound_terrain_cache_anchor_up = cache_anchor_up
	_bound_terrain_cache_base_spacing = base_spacing


func _disable_authoritative_terrain_cache(force: bool) -> void:
	if not force and not _bound_terrain_cache_ready and _bound_terrain_cache_texture == null:
		return
	for material: ShaderMaterial in _materials:
		material.set_shader_parameter("u_scatter_terrain_cache_ready", 0.0)
	_bound_terrain_cache_texture = null
	_bound_terrain_cache_ready = false
	_bound_terrain_cache_generation = -1
	_bound_terrain_cache_res = -1
	_bound_terrain_cache_anchor_dir = Vector3.ZERO
	_bound_terrain_cache_anchor_right = Vector3.ZERO
	_bound_terrain_cache_anchor_up = Vector3.ZERO
	_bound_terrain_cache_base_spacing = -1.0


func _build_ecology_batches() -> void:
	_ecology_shader = load(ECOLOGY_SHADER_PATH) as Shader
	if _ecology_shader == null:
		push_error("Terrain ecology scatter shader could not be loaded")
		return

	for definition_value: Variant in ECOLOGY_ASSETS:
		if not (definition_value is Dictionary):
			continue
		var definition: Dictionary = definition_value
		_build_ecology_asset(definition)


func _build_ecology_asset(definition: Dictionary) -> void:
	var asset_id: String = str(definition.get("id", ""))
	var lod_index: int = int(definition.get("lod", 0))
	if asset_id.is_empty():
		return
	var metadata: Dictionary = _load_ecology_metadata(asset_id)
	if metadata.is_empty():
		return
	var selected_triangles: int = _metadata_lod_triangles(metadata, lod_index)
	if selected_triangles <= 0 or selected_triangles > ECOLOGY_MAX_SELECTED_LOD_TRIANGLES:
		push_warning("Skipping ecology asset %s LOD%d (%d triangles; fallback cap %d)" % [
			asset_id, lod_index, selected_triangles, ECOLOGY_MAX_SELECTED_LOD_TRIANGLES])
		return

	var glb_path := "%s/%s/lod%d.glb" % [ECOLOGY_RUNTIME_ROOT, asset_id, lod_index]
	# GitHub Actions checks out LFS pointers, not the multi-megabyte payloads. Do not
	# ask Godot to parse a pointer as glTF; local/editor builds with LFS materialized
	# naturally pass this header test.
	if not _runtime_glb_is_materialized(glb_path):
		return
	var packed: PackedScene = load(glb_path) as PackedScene
	if packed == null:
		push_warning("Skipping ecology asset that failed to import: %s" % glb_path)
		return
	var root: Node = packed.instantiate()
	if root == null:
		return

	var height_m: float = _metadata_height_m(metadata)
	var asset_materials: Array[ShaderMaterial] = []
	var batches_before: int = _ecology_batches.size()
	_collect_ecology_meshes(root, Transform3D.IDENTITY, definition, height_m, asset_materials)
	root.free()
	if _ecology_batches.size() > batches_before:
		_ecology_loaded_asset_ids.append(asset_id)
		_ecology_selected_triangles += selected_triangles
		_ecology_candidate_instances += int(definition.get("grid", 1)) * int(definition.get("grid", 1))


func _collect_ecology_meshes(node: Node, parent_transform: Transform3D,
		definition: Dictionary, height_m: float, asset_materials: Array[ShaderMaterial]) -> void:
	var accumulated := parent_transform
	if node is Node3D:
		accumulated = parent_transform * (node as Node3D).transform

	if node is MeshInstance3D:
		var mesh_instance: MeshInstance3D = node as MeshInstance3D
		var source_mesh: ArrayMesh = mesh_instance.mesh as ArrayMesh
		if source_mesh != null and source_mesh.get_surface_count() > 0:
			var runtime_mesh: ArrayMesh = source_mesh.duplicate(true) as ArrayMesh
			if runtime_mesh != null:
				var surface_materials: Array[ShaderMaterial] = []
				for surface_index: int in range(runtime_mesh.get_surface_count()):
					var source_material: Material = mesh_instance.get_active_material(surface_index)
					var material: ShaderMaterial = _make_ecology_material(
						source_material, definition, height_m, accumulated)
					runtime_mesh.surface_set_material(surface_index, material)
					surface_materials.append(material)
					asset_materials.append(material)
					_materials.append(material)

				var grid: int = int(definition.get("grid", 1))
				var batch := _make_ecology_batch(
					"Ecology_%s_%s" % [str(definition.get("id", "asset")), mesh_instance.name],
					runtime_mesh, grid * grid, bool(definition.get("shadows", false)))
				add_child(batch)
				_ecology_batches.append({
					"instance": batch,
					"materials": surface_materials,
					"grid": grid,
					"spacing": float(definition.get("spacing", 20.0)),
					"density": float(definition.get("density", 0.25)),
					"last_center": Vector2(1.0e30, 1.0e30),
					"last_origin": Vector3(1.0e30, 1.0e30, 1.0e30),
					"last_anchor": Vector3.ZERO,
				})

	for child_value: Variant in node.get_children():
		var child: Node = child_value as Node
		if child != null:
			_collect_ecology_meshes(child, accumulated, definition, height_m, asset_materials)


func _make_ecology_material(source_material: Material, definition: Dictionary,
		height_m: float, asset_transform: Transform3D) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = _ecology_shader
	material.set_shader_parameter("u_ecology_kind", int(definition.get("kind", 0)))
	material.set_shader_parameter("u_asset_salt", int(definition.get("salt", 911)))
	material.set_shader_parameter("u_asset_scale_min", float(definition.get("scale_min", 0.82)))
	material.set_shader_parameter("u_asset_scale_max", float(definition.get("scale_max", 1.18)))
	material.set_shader_parameter("u_asset_height_m", maxf(height_m, 0.05))
	material.set_shader_parameter("u_wind_strength", float(definition.get("wind", 0.0)))
	material.set_shader_parameter("u_asset_axis_x", asset_transform.basis.x)
	material.set_shader_parameter("u_asset_axis_y", asset_transform.basis.y)
	material.set_shader_parameter("u_asset_axis_z", asset_transform.basis.z)
	material.set_shader_parameter("u_asset_origin", asset_transform.origin)

	var ecology_kind: int = int(definition.get("kind", 0))
	var is_cutout: bool = ecology_kind == 0 or ecology_kind == 1 or ecology_kind == 2 \
		or ecology_kind == 3 or ecology_kind == 6 or ecology_kind == 7
	material.set_shader_parameter("u_alpha_cutoff", 0.32 if is_cutout else 0.0)
	material.set_shader_parameter("u_specular", 0.10 if is_cutout else 0.18)

	var base: BaseMaterial3D = source_material as BaseMaterial3D
	if base == null:
		return material
	material.set_shader_parameter("u_albedo_color", base.albedo_color)
	material.set_shader_parameter("u_material_roughness", base.roughness)
	material.set_shader_parameter("u_material_metallic", base.metallic)
	material.set_shader_parameter("u_normal_scale", base.normal_scale)

	if base.albedo_texture != null:
		material.set_shader_parameter("u_source_albedo", base.albedo_texture)
		material.set_shader_parameter("u_has_albedo", 1)
	if base.normal_texture != null:
		material.set_shader_parameter("u_source_normal", base.normal_texture)
		material.set_shader_parameter("u_has_normal", 1)
	if base.roughness_texture != null:
		material.set_shader_parameter("u_source_roughness", base.roughness_texture)
		material.set_shader_parameter("u_roughness_channel", int(base.roughness_texture_channel))
		material.set_shader_parameter("u_has_roughness", 1)
	if base.metallic_texture != null:
		material.set_shader_parameter("u_source_metallic", base.metallic_texture)
		material.set_shader_parameter("u_metallic_channel", int(base.metallic_texture_channel))
		material.set_shader_parameter("u_has_metallic", 1)
	if base.ao_texture != null:
		material.set_shader_parameter("u_source_ao", base.ao_texture)
		material.set_shader_parameter("u_ao_channel", int(base.ao_texture_channel))
		material.set_shader_parameter("u_has_ao", 1)
	if is_cutout:
		material.set_shader_parameter("u_alpha_cutoff", maxf(0.28, base.alpha_scissor_threshold))
	return material


func _make_ecology_batch(node_name: String, mesh: ArrayMesh, count: int,
		cast_shadows: bool) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = mesh
	mm.instance_count = count
	mm.visible_instance_count = count
	mm.custom_aabb = AABB(
		Vector3(-SCATTER_BOUNDS_M, -SCATTER_BOUNDS_M, -SCATTER_BOUNDS_M),
		Vector3(SCATTER_BOUNDS_M * 2.0, SCATTER_BOUNDS_M * 2.0, SCATTER_BOUNDS_M * 2.0))
	for i: int in range(count):
		mm.set_instance_transform(i, Transform3D.IDENTITY)
	var batch := MultiMeshInstance3D.new()
	batch.name = node_name
	batch.multimesh = mm
	batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON if cast_shadows \
		else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	batch.visible = false
	return batch


func _runtime_glb_is_materialized(path: String) -> bool:
	var file: FileAccess = FileAccess.open(path, FileAccess.READ)
	if file == null or file.get_length() < 4:
		return false
	var magic: PackedByteArray = file.get_buffer(4)
	return magic.size() == 4 and magic[0] == 0x67 and magic[1] == 0x6c \
		and magic[2] == 0x54 and magic[3] == 0x46


func _load_ecology_metadata(asset_id: String) -> Dictionary:
	var path := "%s/%s/metadata.json" % [ECOLOGY_RUNTIME_ROOT, asset_id]
	if not FileAccess.file_exists(path):
		return {}
	var text: String = FileAccess.get_file_as_string(path)
	var parsed: Variant = JSON.parse_string(text)
	if parsed is Dictionary:
		return parsed
	return {}


func _metadata_lod_triangles(metadata: Dictionary, lod_index: int) -> int:
	var lods_value: Variant = metadata.get("lods", [])
	if not (lods_value is Array):
		return 0
	for record_value: Variant in lods_value:
		if record_value is Dictionary:
			var record: Dictionary = record_value
			if int(record.get("lod", -1)) == lod_index:
				return int(record.get("triangles", 0))
	return 0


func _metadata_height_m(metadata: Dictionary) -> float:
	var bounds_value: Variant = metadata.get("bounds_size_m", [])
	if bounds_value is Array:
		var bounds: Array = bounds_value
		if bounds.size() >= 3:
			return maxf(float(bounds[2]), 0.05)
	return 1.0


func _set_visible(value: bool) -> void:
	super._set_visible(value)
	for batch_data: Dictionary in _ecology_batches:
		var instance: MultiMeshInstance3D = batch_data.get("instance") as MultiMeshInstance3D
		if instance != null:
			instance.visible = value


func gpu_scatter_stats() -> Dictionary:
	return {
		"global_heightmap": true,
		"global_height_face_res": _bound_macro_res,
		"authoritative_terrain_cache_bound": _bound_terrain_cache_ready,
		"authoritative_terrain_cache_generation": _bound_terrain_cache_generation,
		"authoritative_terrain_cache_res": _bound_terrain_cache_res,
		"cpu_scatter_classification": false,
		"compute_supported": false if STABLE_FALLBACK_ONLY else _compact_method_supported,
		"compute_ready": false if STABLE_FALLBACK_ONLY else _compact_init_ready,
		"compute_failed": false if STABLE_FALLBACK_ONLY else _compact_init_failed,
		"stable_gpu_fallback": STABLE_FALLBACK_ONLY,
		"fallback_triangle_cap": ECOLOGY_MAX_SELECTED_LOD_TRIANGLES,
		"fallback_max_altitude_m": FALLBACK_SCATTER_MAX_ALTITUDE_M,
		"edit_delta_bound": get_node_or_null("/root/TerrainEditDeltaGPU") != null,
		"active_deform_bound": get_node_or_null("/root/TerrainDeformationGPU") != null,
		"ecology_real_assets": _ecology_loaded_asset_ids.size(),
		"ecology_asset_ids": _ecology_loaded_asset_ids,
		"ecology_mesh_batches": _ecology_batches.size(),
		"ecology_selected_lod_triangles": _ecology_selected_triangles,
		"ecology_candidate_instances": _ecology_candidate_instances,
	}
