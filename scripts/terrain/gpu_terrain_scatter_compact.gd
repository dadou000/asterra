extends "res://scripts/terrain/gpu_terrain_scatter.gd"
## RenderingDevice-compacted terrain scatter.
##
## The inherited lattice renderer remains a complete fallback. On Forward+/Mobile
## this subclass allocates indirect MultiMeshes whose transform/custom-data buffers
## are written directly by terrain_scatter_compact.glsl. Only accepted candidates
## are drawn; rejected candidates never reach the vertex stage.
##
## There is intentionally no GPU -> CPU readback. The CPU only notices snapped
## window changes and records a compute dispatch on the render thread.

const COMPUTE_SHADER_PATH := "res://shaders/terrain_scatter_compact.glsl"
const PARAM_BYTES: int = 128
const COMPUTE_LOCAL_SIZE: int = 64
const FAMILY_GRASS: int = 0
const FAMILY_GEO_STONE: int = 1
const FAMILY_RIVER_STONE: int = 2
const FAMILY_COUNT: int = 3
const INVALID_CELL := Vector2i(0x3fffffff, 0x3fffffff)

var _compact_method_supported := false
var _compact_init_requested := false
var _compact_init_ready := false
var _compact_init_failed := false
var _compact_bindings_building := false
var _compact_bindings_ready := false
var _compact_binding_token := 0
var _compact_binding_generation := -1
var _compact_macro_server_rid := RID()

var _compact_batches: Array[MultiMeshInstance3D] = []
var _compact_multimesh_rids: Array[RID] = []
var _compact_materials: Array[ShaderMaterial] = []

# RenderingDevice-owned objects. Accessed by the main thread only as opaque RIDs;
# all operations using them are queued through RenderingServer.call_on_render_thread.
var _rd_shader := RID()
var _rd_pipeline := RID()
var _rd_sampler := RID()
var _rd_param_buffers: Array[RID] = []
var _rd_instance_buffers: Array[RID] = []
var _rd_command_buffers: Array[RID] = []
var _rd_uniform_sets: Array[RID] = []

var _family_cells: Array[Vector2i] = [INVALID_CELL, INVALID_CELL, INVALID_CELL]
var _family_epoch: Array[int] = [-1, -1, -1]
var _family_ready: Array[bool] = [false, false, false]
var _family_tokens: Array[int] = [0, 0, 0]
var _placement_epoch := 0
var _last_anchor_dir := Vector3.ZERO
var _last_origin := Vector3.ZERO
var _have_placement_signature := false


func _ready() -> void:
	super._ready()
	var method := RenderingServer.get_current_rendering_method()
	_compact_method_supported = method == "forward_plus" or method == "mobile"
	if not _compact_method_supported:
		return
	_build_compact_batches()
	call_deferred("_try_initialize_compute")


func _process(dt: float) -> void:
	# Reuse the proven visibility, anchor, world/context binding and fallback path.
	super._process(dt)
	if not _compact_method_supported or _compact_init_failed:
		_hide_compact_batches()
		return
	if not _compact_init_ready:
		_hide_compact_batches()
		return

	var base_visible := _grass_batch != null and _grass_batch.visible
	if not base_visible or not _debug_enabled:
		_hide_compact_batches()
		return

	if not _ensure_compute_bindings():
		_hide_compact_batches()
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null or Planet.cfg == null:
		_hide_compact_batches()
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_update_placement_epoch(origin)

	var planet_pos: Vector3 = camera.global_position + origin
	var observer_dir := planet_pos.normalized()
	var observer_surface := observer_dir * Planet.cfg.planet_radius
	var anchor_surface := _anchor_dir * Planet.cfg.planet_radius
	var rel := observer_surface - anchor_surface
	var px := rel.dot(_anchor_right)
	var py := rel.dot(_anchor_up)

	var centers: Array[Vector2i] = [
		Vector2i(roundi(px / GRASS_SPACING_M), roundi(py / GRASS_SPACING_M)),
		Vector2i(roundi(px / GEO_STONE_SPACING_M), roundi(py / GEO_STONE_SPACING_M)),
		Vector2i(roundi(px / RIVER_STONE_SPACING_M), roundi(py / RIVER_STONE_SPACING_M)),
	]

	for family: int in FAMILY_COUNT:
		if centers[family] != _family_cells[family] or _family_epoch[family] != _placement_epoch:
			_dispatch_family(family, centers[family], origin)

	_apply_compact_or_fallback_visibility(base_visible)


func _build_compact_batches() -> void:
	var grass_material := ShaderMaterial.new()
	grass_material.shader = load("res://shaders/terrain_scatter_compact_grass.gdshader")
	var geo_material := ShaderMaterial.new()
	geo_material.shader = load("res://shaders/terrain_scatter_compact_stone.gdshader")
	geo_material.set_shader_parameter("u_stone_kind", 0)
	var river_material := ShaderMaterial.new()
	river_material.shader = load("res://shaders/terrain_scatter_compact_stone.gdshader")
	river_material.set_shader_parameter("u_stone_kind", 1)
	_compact_materials = [grass_material, geo_material, river_material]

	var grass_mesh: ArrayMesh = _build_grass_clump_mesh()
	var stone_mesh: ArrayMesh = _build_stone_mesh()
	var meshes: Array[ArrayMesh] = [grass_mesh, stone_mesh, stone_mesh]
	var counts: Array[int] = [
		GRASS_GRID * GRASS_GRID,
		GEO_STONE_GRID * GEO_STONE_GRID,
		RIVER_STONE_GRID * RIVER_STONE_GRID,
	]
	var names := ["TerrainScatterGrassCompact", "TerrainScatterGeologicStoneCompact", "TerrainScatterRiverStoneCompact"]

	for family: int in FAMILY_COUNT:
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.use_custom_data = true
		# Set the Resource-side count as well as the RenderingServer-side indirect
		# allocation so editor/debug APIs keep a coherent capacity value.
		mm.instance_count = counts[family]
		mm.visible_instance_count = -1
		mm.mesh = meshes[family]
		var mm_rid := mm.get_rid()
		RenderingServer.multimesh_allocate_data(
			mm_rid, counts[family], RenderingServer.MULTIMESH_TRANSFORM_3D,
			false, true, true)
		# use_indirect allocation resets the command buffer. Re-setting the mesh is
		# required for Godot to populate indexCount/firstIndex/firstInstance.
		RenderingServer.multimesh_set_mesh(mm_rid, meshes[family].get_rid())
		RenderingServer.multimesh_set_custom_aabb(mm_rid, AABB(
			Vector3(-SCATTER_BOUNDS_M, -SCATTER_BOUNDS_M, -SCATTER_BOUNDS_M),
			Vector3(SCATTER_BOUNDS_M * 2.0, SCATTER_BOUNDS_M * 2.0, SCATTER_BOUNDS_M * 2.0)))

		var batch := MultiMeshInstance3D.new()
		batch.name = names[family]
		batch.multimesh = mm
		batch.material_override = _compact_materials[family]
		batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		batch.visible = false
		add_child(batch)
		_compact_batches.append(batch)
		_compact_multimesh_rids.append(mm_rid)


func _try_initialize_compute() -> void:
	if _compact_init_requested or _compact_init_ready or _compact_init_failed:
		return
	if not _compact_method_supported:
		return
	var resource: Resource = load(COMPUTE_SHADER_PATH)
	if resource == null or not (resource is RDShaderFile):
		# Keep the lattice fallback alive. This can happen during a first editor scan
		# before the GLSL importer has produced the RDShaderFile.
		return
	var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty() or spirv.bytecode_compute.is_empty():
		_compact_init_failed = true
		push_error("Terrain scatter compaction compute shader is not valid.")
		return
	_compact_init_requested = true
	RenderingServer.call_on_render_thread(
		_render_initialize_compute.bind(spirv, _compact_multimesh_rids.duplicate()))


func _render_initialize_compute(spirv: RDShaderSPIRV, multimesh_rids: Array) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_compute_initialized", false, RID(), RID(), RID(), [], [], [])
		return

	var shader := rd.shader_create_from_spirv(spirv, "Asterra terrain scatter compaction")
	if not shader.is_valid():
		call_deferred("_on_compute_initialized", false, RID(), RID(), RID(), [], [], [])
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_compute_initialized", false, RID(), RID(), RID(), [], [], [])
		return

	var sampler_state := RDSamplerState.new()
	var sampler := rd.sampler_create(sampler_state)
	var param_buffers: Array[RID] = []
	var instance_buffers: Array[RID] = []
	var command_buffers: Array[RID] = []
	var zeros := PackedByteArray()
	zeros.resize(PARAM_BYTES)

	for family: int in FAMILY_COUNT:
		var mm_rid: RID = multimesh_rids[family]
		var instance_buffer := RenderingServer.multimesh_get_buffer_rd_rid(mm_rid)
		var command_buffer := RenderingServer.multimesh_get_command_buffer_rd_rid(mm_rid)
		if not instance_buffer.is_valid() or not command_buffer.is_valid():
			for buffer: RID in param_buffers:
				if buffer.is_valid(): rd.free_rid(buffer)
			if sampler.is_valid(): rd.free_rid(sampler)
			if pipeline.is_valid(): rd.free_rid(pipeline)
			if shader.is_valid(): rd.free_rid(shader)
			call_deferred("_on_compute_initialized", false, RID(), RID(), RID(), [], [], [])
			return
		param_buffers.append(rd.uniform_buffer_create(PARAM_BYTES, zeros))
		instance_buffers.append(instance_buffer)
		command_buffers.append(command_buffer)

	call_deferred("_on_compute_initialized", true, shader, pipeline, sampler,
		param_buffers, instance_buffers, command_buffers)


func _on_compute_initialized(success: bool, shader: RID, pipeline: RID, sampler: RID,
		param_buffers: Array, instance_buffers: Array, command_buffers: Array) -> void:
	if not success:
		_compact_init_failed = true
		_compact_init_ready = false
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_sampler = sampler
	_rd_param_buffers.assign(param_buffers)
	_rd_instance_buffers.assign(instance_buffers)
	_rd_command_buffers.assign(command_buffers)
	_compact_init_ready = true
	_compact_init_failed = false
	_compact_bindings_ready = false
	_placement_epoch += 1


func _ensure_compute_bindings() -> bool:
	if not _compact_init_ready:
		return false
	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")) or _bound_macro == null:
		return false

	var macro_rid := _bound_macro.get_rid()
	var generation := int(context.get("generation"))
	if _compact_bindings_ready \
			and generation == _compact_binding_generation \
			and macro_rid == _compact_macro_server_rid:
		return true
	if _compact_bindings_building:
		return false

	var texture_rids := _collect_texture_server_rids(context)
	if texture_rids.size() != 9:
		return false
	_compact_bindings_building = true
	_compact_bindings_ready = false
	_compact_binding_token += 1
	var token := _compact_binding_token
	RenderingServer.call_on_render_thread(_render_build_uniform_sets.bind(
		token, generation, macro_rid, texture_rids,
		_rd_shader, _rd_sampler, _rd_instance_buffers.duplicate(),
		_rd_command_buffers.duplicate(), _rd_param_buffers.duplicate()))
	return false


func _collect_texture_server_rids(context: Node) -> Array[RID]:
	var texture_values: Array = [
		_bound_macro,
		context.get("soil_texture"),
		context.get("surface_texture"),
		context.get("geology_texture"),
		context.get("structure_texture"),
		context.get("climate_texture"),
		context.get("hydrology_texture"),
		context.get("rock_texture"),
		context.get("biome_texture"),
	]
	var result: Array[RID] = []
	for value: Variant in texture_values:
		if not (value is Texture2DArray):
			return []
		var rid := (value as Texture2DArray).get_rid()
		if not rid.is_valid():
			return []
		result.append(rid)
	return result


func _render_build_uniform_sets(token: int, generation: int, macro_server_rid: RID,
		server_texture_rids: Array, shader: RID, sampler: RID,
		instance_buffers: Array, command_buffers: Array, param_buffers: Array) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not shader.is_valid() or not sampler.is_valid():
		call_deferred("_on_uniform_sets_built", token, false, generation, macro_server_rid, [])
		return

	var rd_textures: Array[RID] = []
	for server_rid_value: Variant in server_texture_rids:
		var server_rid: RID = server_rid_value
		var rd_texture := RenderingServer.texture_get_rd_texture(server_rid, false)
		if not rd_texture.is_valid():
			call_deferred("_on_uniform_sets_built", token, false, generation, macro_server_rid, [])
			return
		rd_textures.append(rd_texture)

	var sets: Array[RID] = []
	for family: int in FAMILY_COUNT:
		var uniforms: Array[RDUniform] = []
		for binding: int in 9:
			var tex_uniform := RDUniform.new()
			tex_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
			tex_uniform.binding = binding
			tex_uniform.add_id(sampler)
			tex_uniform.add_id(rd_textures[binding])
			uniforms.append(tex_uniform)

		var instance_uniform := RDUniform.new()
		instance_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		instance_uniform.binding = 9
		instance_uniform.add_id(instance_buffers[family])
		uniforms.append(instance_uniform)

		var command_uniform := RDUniform.new()
		command_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		command_uniform.binding = 10
		command_uniform.add_id(command_buffers[family])
		uniforms.append(command_uniform)

		var params_uniform := RDUniform.new()
		params_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
		params_uniform.binding = 11
		params_uniform.add_id(param_buffers[family])
		uniforms.append(params_uniform)

		var uniform_set := rd.uniform_set_create(uniforms, shader, 0)
		if not uniform_set.is_valid() or not rd.uniform_set_is_valid(uniform_set):
			for created_set: RID in sets:
				if created_set.is_valid(): rd.free_rid(created_set)
			call_deferred("_on_uniform_sets_built", token, false, generation, macro_server_rid, [])
			return
		sets.append(uniform_set)

	call_deferred("_on_uniform_sets_built", token, true, generation, macro_server_rid, sets)


func _on_uniform_sets_built(token: int, success: bool, generation: int,
		macro_server_rid: RID, sets: Array) -> void:
	if token != _compact_binding_token:
		return
	_compact_bindings_building = false
	if not success:
		_compact_bindings_ready = false
		return
	_rd_uniform_sets.assign(sets)
	_compact_binding_generation = generation
	_compact_macro_server_rid = macro_server_rid
	_compact_bindings_ready = true
	_placement_epoch += 1
	for family: int in FAMILY_COUNT:
		_family_epoch[family] = -1
		_family_ready[family] = false


func _update_placement_epoch(origin: Vector3) -> void:
	if not _have_placement_signature:
		_last_anchor_dir = _anchor_dir
		_last_origin = origin
		_have_placement_signature = true
		_placement_epoch += 1
		return
	if _last_anchor_dir.distance_squared_to(_anchor_dir) > 1e-12 \
			or _last_origin.distance_squared_to(origin) > 1e-12:
		_last_anchor_dir = _anchor_dir
		_last_origin = origin
		_placement_epoch += 1


func _dispatch_family(family: int, center_cell: Vector2i, origin: Vector3) -> void:
	if not _compact_bindings_ready or family >= _rd_uniform_sets.size():
		return
	var params := _build_family_params(family, center_cell, origin)
	_family_cells[family] = center_cell
	_family_epoch[family] = _placement_epoch
	_family_ready[family] = false
	_family_tokens[family] += 1
	var token := _family_tokens[family]
	var candidate_count := _family_candidate_count(family)
	RenderingServer.call_on_render_thread(_render_dispatch_family.bind(
		family, token, candidate_count, params,
		_rd_pipeline, _rd_uniform_sets[family], _rd_param_buffers[family],
		_rd_command_buffers[family]))


func _build_family_params(family: int, center_cell: Vector2i, origin: Vector3) -> PackedByteArray:
	var spacing := GRASS_SPACING_M
	var density := GRASS_DENSITY
	var grid := GRASS_GRID
	if family == FAMILY_GEO_STONE:
		spacing = GEO_STONE_SPACING_M
		density = GEO_STONE_DENSITY
		grid = GEO_STONE_GRID
	elif family == FAMILY_RIVER_STONE:
		spacing = RIVER_STONE_SPACING_M
		density = RIVER_STONE_DENSITY
		grid = RIVER_STONE_GRID

	var context: Node = get_node_or_null("/root/PlanetContext")
	var context_res := float(context.get("face_res")) if context != null else 0.0
	var scatter_seed := Planet.cfg.stream_seed("gpu_scatter") & 0x00ffffff
	var detail_seed := Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	var slope_step := maxf(0.75, spacing * 0.50)

	var values := PackedFloat32Array([
		_anchor_dir.x, _anchor_dir.y, _anchor_dir.z, Planet.cfg.planet_radius,
		_anchor_right.x, _anchor_right.y, _anchor_right.z, spacing,
		_anchor_up.x, _anchor_up.y, _anchor_up.z, density,
		origin.x, origin.y, origin.z, 0.75,
		float(center_cell.x), float(center_cell.y), float(grid), float(family),
		float(_bound_macro_res), context_res, float(maxi(scatter_seed, 1)), float(maxi(detail_seed, 1)),
		float(grid * grid), 0.045, slope_step, 1.0 if _debug_enabled else 0.0,
		0.22, 0.82, 0.055, 0.0,
	])
	return values.to_byte_array()


func _render_dispatch_family(family: int, token: int, candidate_count: int,
		params: PackedByteArray, pipeline: RID, uniform_set: RID,
		param_buffer: RID, command_buffer: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not pipeline.is_valid() or not uniform_set.is_valid():
		call_deferred("_on_family_dispatch_recorded", family, token, false)
		return
	var update_error := rd.buffer_update(param_buffer, 0, PARAM_BYTES, params)
	if update_error != OK:
		call_deferred("_on_family_dispatch_recorded", family, token, false)
		return
	# Preserve the mesh-derived indexCount/firstIndex/etc. and reset only the GPU
	# generated instanceCount field (uint #1) before atomic compaction.
	var zero_count := PackedInt32Array([0]).to_byte_array()
	var clear_error := rd.buffer_update(command_buffer, 4, 4, zero_count)
	if clear_error != OK:
		call_deferred("_on_family_dispatch_recorded", family, token, false)
		return

	var compute_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	var groups := ceili(float(candidate_count) / float(COMPUTE_LOCAL_SIZE))
	rd.compute_list_dispatch(compute_list, groups, 1, 1)
	rd.compute_list_end()
	call_deferred("_on_family_dispatch_recorded", family, token, true)


func _on_family_dispatch_recorded(family: int, token: int, success: bool) -> void:
	if family < 0 or family >= FAMILY_COUNT or token != _family_tokens[family]:
		return
	_family_ready[family] = success
	if not success:
		# Keep the inherited candidate lattice visible for this family.
		_family_epoch[family] = -1


func _family_candidate_count(family: int) -> int:
	if family == FAMILY_GRASS:
		return GRASS_GRID * GRASS_GRID
	if family == FAMILY_GEO_STONE:
		return GEO_STONE_GRID * GEO_STONE_GRID
	return RIVER_STONE_GRID * RIVER_STONE_GRID


func _apply_compact_or_fallback_visibility(base_visible: bool) -> void:
	var fallback: Array[MultiMeshInstance3D] = [_grass_batch, _geo_stone_batch, _river_stone_batch]
	for family: int in FAMILY_COUNT:
		var use_compact := base_visible and _compact_bindings_ready and _family_ready[family]
		if family < _compact_batches.size():
			_compact_batches[family].visible = use_compact
		if fallback[family] != null:
			fallback[family].visible = base_visible and not use_compact


func _hide_compact_batches() -> void:
	for batch: MultiMeshInstance3D in _compact_batches:
		batch.visible = false


func set_debug_enabled(value: bool) -> void:
	super.set_debug_enabled(value)
	if not value:
		_hide_compact_batches()
	else:
		_placement_epoch += 1
		for family: int in FAMILY_COUNT:
			_family_epoch[family] = -1
			_family_ready[family] = false


func scatter_stats() -> Dictionary:
	var stats := super.scatter_stats()
	stats["compute_compaction_supported"] = _compact_method_supported
	stats["compute_compaction_initialized"] = _compact_init_ready
	stats["compute_compaction_active"] = _compact_bindings_ready and _family_ready.has(true)
	stats["compute_exact_analytic_slope"] = true
	stats["compute_indirect_draw"] = true
	stats["compute_cpu_readback"] = false
	stats["compact_grass_ready"] = _family_ready[FAMILY_GRASS]
	stats["compact_geologic_stone_ready"] = _family_ready[FAMILY_GEO_STONE]
	stats["compact_river_stone_ready"] = _family_ready[FAMILY_RIVER_STONE]
	return stats
