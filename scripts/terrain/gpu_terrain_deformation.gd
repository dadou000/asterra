extends Node
## GPU active deformation tile.
##
## Expensive per-cell plastic deformation is rasterized by one compute dispatch per
## rendered frame. The tile is deliberately local and high resolution: 256x256 at
## 0.25 m/texel (64 m square). Two RGBA32F textures ping-pong so compute never
## writes the texture it is reading. R=height delta, G=compaction, B=damage.
##
## Physics normally samples this field through TerrainDeformationQueryGPU, which
## reads only requested points. The old whole-texture asynchronous mirror remains
## as a low-rate fallback until that query service is ready.

const SHADER_PATH := "res://shaders/terrain_deformation_active.glsl"
const RESOLUTION := 256
const SAMPLE_SPACING_M := 0.25
const HALF_EXTENT_M := float(RESOLUTION) * SAMPLE_SPACING_M * 0.5
const SAFE_HALF_EXTENT_M := HALF_EXTENT_M - 3.0
const MAX_CONTACTS := 64
const CONTACT_FLOATS := 12
const CONTACT_BUFFER_BYTES := MAX_CONTACTS * CONTACT_FLOATS * 4
const PARAM_BYTES := 16
const BYTES_PER_TEXEL := 16
const FALLBACK_READBACK_HZ := 10.0

var supported: bool = false
var ready_state: bool = false
var failed: bool = false
var texture: Texture2DRD

var center_dir := Vector3.RIGHT
var center_right := Vector3.BACK
var center_up := Vector3.UP

var _center_valid := false
var _init_requested := false
var _dispatch_in_flight := false
var _active_index := 0
var _dispatch_token := 0
var _window_generation := 0
var _steps := 0
var _readbacks := 0
var _rejected_contacts := 0
var _has_active_content := false

var _rd_shader := RID()
var _rd_pipeline := RID()
var _rd_contact_buffer := RID()
var _rd_param_buffer := RID()
var _rd_textures: Array[RID] = []
var _uniform_sets: Array[RID] = []
var _textures: Array[Texture2DRD] = []

var _pending_contacts: Array[PackedFloat32Array] = []
var _readback_accum_s := 0.0
var _latest_readback := PackedByteArray()
var _latest_readback_token := -1
var _latest_readback_generation := -1
var _latest_readback_time_s := 0.0

var _bound_material: ShaderMaterial
var _bound_texture: Texture2DRD
var _bound_ready := false
var _bound_window_generation := -1


func _ready() -> void:
	process_priority = -2
	var method: String = RenderingServer.get_current_rendering_method()
	supported = method == "forward_plus" or method == "mobile"
	if supported:
		call_deferred("_try_initialize")


func _exit_tree() -> void:
	var rids: Array[RID] = []
	for rid: RID in _uniform_sets:
		rids.append(rid)
	_uniform_sets.clear()
	for wrapper: Texture2DRD in _textures:
		wrapper.texture_rd_rid = RID()
	_textures.clear()
	for rid: RID in _rd_textures:
		rids.append(rid)
	_rd_textures.clear()
	for rid: RID in [_rd_contact_buffer, _rd_param_buffer, _rd_pipeline, _rd_shader]:
		rids.append(rid)
	_rd_contact_buffer = RID()
	_rd_param_buffer = RID()
	_rd_pipeline = RID()
	_rd_shader = RID()
	texture = null
	if not rids.is_empty():
		RenderingServer.call_on_render_thread(_render_free_rids.bind(rids))


func _process(dt: float) -> void:
	if not supported or failed:
		_sync_renderer_binding()
		return
	if not ready_state:
		_try_initialize()
		return
	_readback_accum_s += dt
	if not _pending_contacts.is_empty() and not _dispatch_in_flight:
		_dispatch_pending()
	_sync_renderer_binding()


func enqueue_contact(center_direction: Vector3, radius_m: float, sink_m: float,
		compaction_gain: float, damage_gain: float, rim_fraction: float,
		max_depth_m: float, footprint_type: int = 0, shape_radius_m: float = 0.0,
		penetration_m: float = 0.0) -> bool:
	if not ready_state or failed or Planet.cfg == null:
		return false
	if center_direction.length_squared() <= 1e-12 or sink_m <= 0.0:
		return false
	var d: Vector3 = center_direction.normalized()
	if not _center_valid:
		_set_center(d)
	var local_m: Vector2 = _project_local(d)
	var required_extent_m: float = maxf(absf(local_m.x), absf(local_m.y))
	required_extent_m += maxf(radius_m, SAMPLE_SPACING_M) * 1.95
	if required_extent_m > SAFE_HALF_EXTENT_M:
		_rejected_contacts += 1
		return false
	if _pending_contacts.size() >= MAX_CONTACTS:
		_rejected_contacts += 1
		return false
	var record := PackedFloat32Array([
		local_m.x, local_m.y, maxf(radius_m, SAMPLE_SPACING_M), sink_m,
		maxf(compaction_gain, 0.0), maxf(damage_gain, 0.0),
		clampf(rim_fraction, 0.0, 0.95), maxf(max_depth_m, 0.05),
		float(footprint_type), maxf(shape_radius_m, 0.0), maxf(penetration_m, 0.0), 0.0,
	])
	_pending_contacts.append(record)
	_has_active_content = true
	return true


func active_height_offset(direction: Vector3) -> float:
	var query: Node = get_node_or_null("/root/TerrainDeformationQueryGPU")
	if query != null and bool(query.get("ready_state")) and query.has_method("active_height_offset"):
		return float(query.call("active_height_offset", direction))
	return _sample_channel(direction, 0)


func active_state(direction: Vector3) -> Vector2:
	var query: Node = get_node_or_null("/root/TerrainDeformationQueryGPU")
	if query != null and bool(query.get("ready_state")) and query.has_method("active_state"):
		var value: Variant = query.call("active_state", direction)
		if value is Vector2:
			return value as Vector2
	return Vector2(_sample_channel(direction, 1), _sample_channel(direction, 2))


func rd_texture_rids() -> Array[RID]:
	var out: Array[RID] = []
	out.assign(_rd_textures)
	return out


func active_texture_index() -> int:
	return _active_index


func window_generation() -> int:
	return _window_generation


func field_generation() -> int:
	return _steps


func project_local(direction: Vector3) -> Vector2:
	if direction.length_squared() <= 1e-12:
		return Vector2(INF, INF)
	return _project_local(direction.normalized())


func clear_active() -> void:
	_pending_contacts.clear()
	_latest_readback.clear()
	_latest_readback_token = -1
	_latest_readback_generation = -1
	_center_valid = false
	_has_active_content = false
	_window_generation += 1
	_active_index = 0
	if _textures.size() == 2:
		texture = _textures[0]
	if ready_state and _rd_textures.size() == 2:
		# These are compute-owned images. Clearing them on the render thread avoids
		# requiring the incompatible CAN_UPDATE texture capability just to upload a
		# CPU-built zero buffer during a reset.
		RenderingServer.call_on_render_thread(_render_clear.bind(_rd_textures))


func sample_params() -> Dictionary:
	return {
		"texture": texture,
		"ready": ready_state and _center_valid and _has_active_content,
		"center_dir": center_dir,
		"center_right": center_right,
		"center_up": center_up,
		"half_extent_m": HALF_EXTENT_M,
		"resolution": RESOLUTION,
		"spacing_m": SAMPLE_SPACING_M,
		"readback_ready": _latest_readback_generation == _window_generation and not _latest_readback.is_empty(),
		"generation": _steps,
		"window_generation": _window_generation,
		"active_index": _active_index,
	}


func stats() -> Dictionary:
	var readback_age_s := INF
	if _latest_readback_time_s > 0.0:
		readback_age_s = maxf(Time.get_ticks_msec() * 0.001 - _latest_readback_time_s, 0.0)
	return {
		"gpu": true,
		"supported": supported,
		"ready": ready_state,
		"failed": failed,
		"resolution": RESOLUTION,
		"spacing_m": SAMPLE_SPACING_M,
		"extent_m": HALF_EXTENT_M * 2.0,
		"pending_contacts": _pending_contacts.size(),
		"dispatch_in_flight": _dispatch_in_flight,
		"steps": _steps,
		"readbacks": _readbacks,
		"readback_age_s": readback_age_s,
		"rejected_contacts": _rejected_contacts,
		"active": _center_valid and _has_active_content,
	}


func _set_center(d: Vector3) -> void:
	center_dir = d.normalized()
	var basis: Array = CubeSphere.tangent_basis(center_dir)
	center_right = (basis[0] as Vector3).normalized()
	center_up = (basis[1] as Vector3).normalized()
	_center_valid = true
	_window_generation += 1
	_latest_readback.clear()
	_latest_readback_generation = -1


func _project_local(direction: Vector3) -> Vector2:
	var d: Vector3 = direction.normalized()
	var denom: float = d.dot(center_dir)
	if denom <= 0.01 or Planet.cfg == null:
		return Vector2(INF, INF)
	var scale: float = Planet.cfg.planet_radius / denom
	return Vector2(d.dot(center_right) * scale, d.dot(center_up) * scale)


func _sample_channel(direction: Vector3, channel: int) -> float:
	if not _center_valid or Planet.cfg == null or direction.length_squared() <= 1e-12:
		return 0.0
	if _latest_readback_generation != _window_generation:
		return 0.0
	var expected_bytes: int = RESOLUTION * RESOLUTION * BYTES_PER_TEXEL
	if _latest_readback.size() < expected_bytes:
		return 0.0
	var local_m: Vector2 = _project_local(direction)
	if not is_finite(local_m.x) or not is_finite(local_m.y):
		return 0.0
	if absf(local_m.x) >= HALF_EXTENT_M or absf(local_m.y) >= HALF_EXTENT_M:
		return 0.0
	var fx: float = local_m.x / SAMPLE_SPACING_M + float(RESOLUTION) * 0.5 - 0.5
	var fy: float = local_m.y / SAMPLE_SPACING_M + float(RESOLUTION) * 0.5 - 0.5
	fx = clampf(fx, 0.0, float(RESOLUTION - 1))
	fy = clampf(fy, 0.0, float(RESOLUTION - 1))
	var x0: int = int(floor(fx))
	var y0: int = int(floor(fy))
	var x1: int = mini(x0 + 1, RESOLUTION - 1)
	var y1: int = mini(y0 + 1, RESOLUTION - 1)
	var tx: float = fx - float(x0)
	var ty: float = fy - float(y0)
	var v00: float = _decode_texel_channel(x0, y0, channel)
	var v10: float = _decode_texel_channel(x1, y0, channel)
	var v01: float = _decode_texel_channel(x0, y1, channel)
	var v11: float = _decode_texel_channel(x1, y1, channel)
	return lerpf(lerpf(v00, v10, tx), lerpf(v01, v11, tx), ty)


func _decode_texel_channel(x: int, y: int, channel: int) -> float:
	var clamped_channel: int = clampi(channel, 0, 3)
	var offset: int = (y * RESOLUTION + x) * BYTES_PER_TEXEL + clamped_channel * 4
	return _latest_readback.decode_float(offset)


func _dispatch_pending() -> void:
	if _pending_contacts.is_empty() or _uniform_sets.size() != 2:
		return
	var count: int = mini(_pending_contacts.size(), MAX_CONTACTS)
	var packed := PackedFloat32Array()
	packed.resize(MAX_CONTACTS * CONTACT_FLOATS)
	for contact_index in count:
		var record: PackedFloat32Array = _pending_contacts[contact_index]
		for component in CONTACT_FLOATS:
			packed[contact_index * CONTACT_FLOATS + component] = record[component]
	_pending_contacts.clear()
	var params := PackedFloat32Array([
		float(count), float(RESOLUTION), SAMPLE_SPACING_M, HALF_EXTENT_M,
	])
	_dispatch_token += 1
	var token: int = _dispatch_token
	var source_index: int = _active_index
	var target_index: int = 1 - source_index
	var query: Node = get_node_or_null("/root/TerrainDeformationQueryGPU")
	var query_ready: bool = query != null and bool(query.get("ready_state"))
	var want_readback: bool = (not query_ready) and _readback_accum_s >= 1.0 / FALLBACK_READBACK_HZ
	if want_readback:
		_readback_accum_s = 0.0
	_dispatch_in_flight = true
	RenderingServer.call_on_render_thread(_render_dispatch.bind(
		token, target_index, _window_generation, packed.to_byte_array(),
		params.to_byte_array(), want_readback, _rd_pipeline,
		_uniform_sets[source_index], _rd_contact_buffer, _rd_param_buffer,
		_rd_textures[target_index]))


func _render_dispatch(token: int, target_index: int, window_generation: int,
		contact_bytes: PackedByteArray, param_bytes: PackedByteArray,
		want_readback: bool, pipeline: RID, uniform_set: RID,
		contact_buffer: RID, param_buffer: RID, target_texture: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not pipeline.is_valid() or not uniform_set.is_valid():
		call_deferred("_on_dispatch_failed", token)
		return
	if rd.buffer_update(contact_buffer, 0, CONTACT_BUFFER_BYTES, contact_bytes) != OK:
		call_deferred("_on_dispatch_failed", token)
		return
	if rd.buffer_update(param_buffer, 0, PARAM_BYTES, param_bytes) != OK:
		call_deferred("_on_dispatch_failed", token)
		return
	var compute_list: int = rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	rd.compute_list_dispatch(compute_list, RESOLUTION / 8, RESOLUTION / 8, 1)
	rd.compute_list_end()
	if want_readback:
		var readback_error: Error = rd.texture_get_data_async(target_texture, 0,
			_on_render_readback.bind(token, window_generation))
		if readback_error != OK:
			call_deferred("_on_readback_failed")
	call_deferred("_on_dispatch_submitted", token, target_index)


func _on_render_readback(data: PackedByteArray, token: int, window_generation: int) -> void:
	call_deferred("_accept_readback", data, token, window_generation)


func _on_dispatch_submitted(token: int, target_index: int) -> void:
	if token != _dispatch_token:
		return
	_active_index = target_index
	if target_index >= 0 and target_index < _textures.size():
		texture = _textures[target_index]
	_dispatch_in_flight = false
	_steps += 1


func _accept_readback(data: PackedByteArray, token: int, window_generation: int) -> void:
	if window_generation != _window_generation or token < _latest_readback_token:
		return
	var expected_bytes: int = RESOLUTION * RESOLUTION * BYTES_PER_TEXEL
	if data.size() < expected_bytes:
		return
	_latest_readback = data
	_latest_readback_token = token
	_latest_readback_generation = window_generation
	_latest_readback_time_s = Time.get_ticks_msec() * 0.001
	_readbacks += 1


func _on_dispatch_failed(token: int) -> void:
	if token == _dispatch_token:
		_dispatch_in_flight = false
	failed = true
	push_error("GPU terrain deformation dispatch failed; CPU fallback remains available.")


func _on_readback_failed() -> void:
	pass


func _try_initialize() -> void:
	if _init_requested or ready_state or failed or not supported:
		return
	# Headless/CPU-only runs have no RenderingDevice. Deformation remains available
	# through the existing CPU service, so this is an expected capability choice,
	# not an initialization error worth reporting to players or CI.
	if RenderingServer.get_rendering_device() == null:
		supported = false
		return
	var resource: Resource = load(SHADER_PATH)
	if resource == null or not (resource is RDShaderFile):
		return
	var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty() or spirv.bytecode_compute.is_empty():
		failed = true
		push_error("GPU terrain deformation shader is invalid.")
		return
	_init_requested = true
	RenderingServer.call_on_render_thread(_render_initialize.bind(spirv))


func _render_initialize(spirv: RDShaderSPIRV) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized", false, "no RenderingDevice", RID(), RID(), [], RID(), RID(), [])
		return
	var shader: RID = rd.shader_create_from_spirv(spirv, "Asterra active terrain deformation")
	if not shader.is_valid():
		call_deferred("_on_initialized", false, "compute shader creation failed", RID(), RID(), [], RID(), RID(), [])
		return
	var pipeline: RID = rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, "compute pipeline creation failed", RID(), RID(), [], RID(), RID(), [])
		return

	var usage_bits: int = (
		RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT |
		RenderingDevice.TEXTURE_USAGE_STORAGE_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT |
		RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT)
	if not rd.texture_is_format_supported_for_usage(
			RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT, usage_bits):
		rd.free_rid(pipeline)
		rd.free_rid(shader)
		call_deferred("_on_initialized", false,
			"RGBA32F storage/copy texture format is unsupported", RID(), RID(), [], RID(), RID(), [])
		return
	var format := RDTextureFormat.new()
	format.width = RESOLUTION
	format.height = RESOLUTION
	format.depth = 1
	format.array_layers = 1
	format.mipmaps = 1
	format.texture_type = RenderingDevice.TEXTURE_TYPE_2D
	format.format = RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT
	format.samples = RenderingDevice.TEXTURE_SAMPLES_1
	format.usage_bits = usage_bits
	var view := RDTextureView.new()
	var rd_textures: Array[RID] = []
	for _index in 2:
		rd_textures.append(rd.texture_create(format, view, []))
	var zero_contacts := PackedByteArray()
	zero_contacts.resize(CONTACT_BUFFER_BYTES)
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_BYTES)
	var contact_buffer: RID = rd.storage_buffer_create(CONTACT_BUFFER_BYTES, zero_contacts)
	var param_buffer: RID = rd.uniform_buffer_create(PARAM_BYTES, zero_params)
	var ok: bool = contact_buffer.is_valid() and param_buffer.is_valid()
	for rid: RID in rd_textures:
		ok = ok and rid.is_valid()
	if ok:
		for rid: RID in rd_textures:
			rd.texture_clear(rid, Color(0.0, 0.0, 0.0, 0.0), 0, 1, 0, 1)
	var sets: Array[RID] = []
	if ok:
		for source_index in 2:
			var target_index: int = 1 - source_index
			var uniforms: Array[RDUniform] = []
			var source_uniform := RDUniform.new()
			source_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
			source_uniform.binding = 0
			source_uniform.add_id(rd_textures[source_index])
			uniforms.append(source_uniform)
			var target_uniform := RDUniform.new()
			target_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
			target_uniform.binding = 1
			target_uniform.add_id(rd_textures[target_index])
			uniforms.append(target_uniform)
			var contact_uniform := RDUniform.new()
			contact_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
			contact_uniform.binding = 2
			contact_uniform.add_id(contact_buffer)
			uniforms.append(contact_uniform)
			var param_uniform := RDUniform.new()
			param_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
			param_uniform.binding = 3
			param_uniform.add_id(param_buffer)
			uniforms.append(param_uniform)
			var uniform_set: RID = rd.uniform_set_create(uniforms, shader, 0)
			if not uniform_set.is_valid():
				ok = false
				break
			sets.append(uniform_set)
	var failure_reason: String = "GPU texture, buffer or uniform-set creation failed" if not ok else ""
	call_deferred("_on_initialized", ok, failure_reason, shader, pipeline, rd_textures,
		contact_buffer, param_buffer, sets)


func _on_initialized(success: bool, failure_reason: String, shader: RID, pipeline: RID, rd_textures: Array,
		contact_buffer: RID, param_buffer: RID, sets: Array) -> void:
	if not success:
		failed = true
		push_error("GPU terrain deformation initialization failed (%s); CPU fallback remains active." % failure_reason)
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_contact_buffer = contact_buffer
	_rd_param_buffer = param_buffer
	_rd_textures.assign(rd_textures)
	_uniform_sets.assign(sets)
	_textures.clear()
	for rid: RID in _rd_textures:
		var wrapper := Texture2DRD.new()
		wrapper.texture_rd_rid = rid
		_textures.append(wrapper)
	if _textures.size() != 2:
		failed = true
		return
	texture = _textures[0]
	ready_state = true
	failed = false


func _render_clear(rd_textures: Array) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		return
	for value: Variant in rd_textures:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.texture_clear(rid, Color(0.0, 0.0, 0.0, 0.0), 0, 1, 0, 1)


func _render_free_rids(rids: Array) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		return
	for rid: RID in rids:
		if rid.is_valid():
			rd.free_rid(rid)


func _sync_renderer_binding() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null:
		return
	var value: Variant = terrain.get("_material")
	if not (value is ShaderMaterial):
		return
	var material := value as ShaderMaterial
	var ready_now: bool = ready_state and _center_valid and _has_active_content
	var material_changed: bool = material != _bound_material
	var texture_changed: bool = texture != _bound_texture
	var ready_changed: bool = ready_now != _bound_ready
	var center_changed: bool = _window_generation != _bound_window_generation
	if not material_changed and not texture_changed and not ready_changed and not center_changed:
		return
	_bound_material = material
	if material_changed or texture_changed:
		material.set_shader_parameter("u_active_deform", texture)
		_bound_texture = texture
	if material_changed or ready_changed:
		material.set_shader_parameter("u_active_deform_ready", 1.0 if ready_now else 0.0)
		_bound_ready = ready_now
	if material_changed or center_changed:
		material.set_shader_parameter("u_active_deform_center_dir", center_dir)
		material.set_shader_parameter("u_active_deform_center_right", center_right)
		material.set_shader_parameter("u_active_deform_center_up", center_up)
		material.set_shader_parameter("u_active_deform_half_extent_m", HALF_EXTENT_M)
		_bound_window_generation = _window_generation
