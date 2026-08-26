class_name TerrainOcclusionCompositorEffect
extends CompositorEffect
## Conservative GPU terrain occlusion against the resolved scene depth.
##
## Terrain supplies absolute-world conservative bounding spheres in a fixed logical
## (sector, LOD) index space. The render callback transforms them into the current
## camera frame, reduces reverse-Z depth into coarse fail-open tiles, tests each
## sphere, and asynchronously reads back only a few hundred bytes. No synchronous
## GPU readback is used in the gameplay path.

const SHADER_PATH := "res://shaders/terrain_occlusion.glsl"
const TILE_WIDTH: int = 128
const TILE_HEIGHT: int = 72
const MAX_CANDIDATES: int = 180
const TILE_COUNT: int = TILE_WIDTH * TILE_HEIGHT
const TILE_BUFFER_BYTES: int = TILE_COUNT * 4
const CANDIDATE_BUFFER_BYTES: int = MAX_CANDIDATES * 16
const RESULT_HEADER_UINTS: int = 4
const RESULT_BUFFER_BYTES: int = (RESULT_HEADER_UINTS + MAX_CANDIDATES) * 4
const REVERSE_Z_RELATIVE_MARGIN: float = 0.02

var _rd: RenderingDevice
var _shader: RID = RID()
var _pipeline: RID = RID()
var _depth_sampler: RID = RID()
var _tile_buffer: RID = RID()
var _candidate_buffer: RID = RID()
var _result_buffer: RID = RID()

var _state_mutex := Mutex.new()
var _candidate_world_spheres := PackedFloat32Array()
var _candidate_generation: int = 0
var _candidate_count: int = 0
var _floating_origin := Vector3.ZERO

var _result_mutex := Mutex.new()
var _latest_snapshot: Dictionary = {}
var _readback_pending := false
var _dispatch_frames: int = 0
var _readback_requests: int = 0
var _readback_failures: int = 0


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	access_resolved_depth = true
	enabled = true
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		return

	var shader_file: RDShaderFile = load(SHADER_PATH) as RDShaderFile
	if shader_file == null:
		push_error("TerrainOcclusionCompositorEffect: failed to load %s" % SHADER_PATH)
		return
	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	if spirv == null:
		push_error("TerrainOcclusionCompositorEffect: shader has no SPIR-V")
		return
	_shader = _rd.shader_create_from_spirv(spirv)
	if not _shader.is_valid():
		push_error("TerrainOcclusionCompositorEffect: failed to create compute shader")
		return
	_pipeline = _rd.compute_pipeline_create(_shader)
	if not _pipeline.is_valid():
		push_error("TerrainOcclusionCompositorEffect: failed to create compute pipeline")
		return

	var depth_state := RDSamplerState.new()
	depth_state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.mip_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	depth_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_depth_sampler = _rd.sampler_create(depth_state)

	_tile_buffer = _rd.storage_buffer_create(TILE_BUFFER_BYTES, PackedByteArray())
	_candidate_buffer = _rd.storage_buffer_create(CANDIDATE_BUFFER_BYTES, PackedByteArray())
	_result_buffer = _rd.storage_buffer_create(RESULT_BUFFER_BYTES, PackedByteArray())


func is_ready() -> bool:
	return _rd != null and _shader.is_valid() and _pipeline.is_valid() \
		and _depth_sampler.is_valid() and _tile_buffer.is_valid() \
		and _candidate_buffer.is_valid() and _result_buffer.is_valid()


func set_candidates(world_spheres: PackedFloat32Array, generation: int,
		floating_origin: Vector3) -> void:
	_state_mutex.lock()
	_candidate_world_spheres = world_spheres.duplicate()
	_candidate_count = mini(world_spheres.size() / 4, MAX_CANDIDATES)
	_candidate_generation = maxi(generation, 0)
	_floating_origin = floating_origin
	_state_mutex.unlock()


func set_floating_origin(value: Vector3) -> void:
	_state_mutex.lock()
	_floating_origin = value
	_state_mutex.unlock()


func get_snapshot() -> Dictionary:
	_result_mutex.lock()
	var out: Dictionary = _latest_snapshot.duplicate(true)
	_result_mutex.unlock()
	return out


func stats() -> Dictionary:
	_result_mutex.lock()
	var pending: bool = _readback_pending
	_result_mutex.unlock()
	return {
		"ready": is_ready(),
		"dispatch_frames": _dispatch_frames,
		"readback_requests": _readback_requests,
		"readback_failures": _readback_failures,
		"readback_pending": pending,
		"tile_width": TILE_WIDTH,
		"tile_height": TILE_HEIGHT,
	}


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or _rd == null:
		return
	for rid: RID in [_result_buffer, _candidate_buffer, _tile_buffer,
			_depth_sampler, _pipeline, _shader]:
		if rid.is_valid():
			_rd.free_rid(rid)


func _render_callback(callback_type: int, render_data: RenderData) -> void:
	if callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT or not enabled or not is_ready():
		return

	# One result buffer is deliberately kept in flight at a time. Dispatching into
	# it again before buffer_get_data_async() completes can make the callback's
	# camera/generation metadata describe a newer GPU result. Skipping a few
	# occlusion frames is cheap and keeps the temporal test deterministic.
	_result_mutex.lock()
	var readback_busy: bool = _readback_pending
	_result_mutex.unlock()
	if readback_busy:
		return

	var buffers: RenderSceneBuffersRD = render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	var scene_data := render_data.get_render_scene_data()
	if buffers == null or scene_data == null or buffers.get_view_count() <= 0:
		return
	var size: Vector2i = buffers.get_internal_size()
	if size.x <= 0 or size.y <= 0:
		return

	_state_mutex.lock()
	var world_spheres: PackedFloat32Array = _candidate_world_spheres.duplicate()
	var generation: int = _candidate_generation
	var candidate_count: int = _candidate_count
	var floating_origin: Vector3 = _floating_origin
	_state_mutex.unlock()
	if candidate_count <= 0 or world_spheres.size() < candidate_count * 4:
		return

	# Occlusion visibility is shared between stereo views. Test view zero only; this
	# path is currently intended for the ordinary desktop camera.
	var depth_rid: RID = buffers.get_depth_layer(0)
	if not depth_rid.is_valid():
		return

	var cam_transform: Transform3D = scene_data.get_cam_transform()
	var view_transform: Transform3D = cam_transform.affine_inverse()
	var camera_world: Vector3 = cam_transform.origin + floating_origin
	var camera_forward: Vector3 = -cam_transform.basis.z.normalized()
	var view_spheres := PackedFloat32Array()
	view_spheres.resize(candidate_count * 4)
	for i: int in candidate_count:
		var base: int = i * 4
		var radius: float = world_spheres[base + 3]
		if radius <= 0.0:
			view_spheres[base + 3] = -1.0
			continue
		var world_center := Vector3(
			world_spheres[base], world_spheres[base + 1], world_spheres[base + 2])
		var render_center: Vector3 = world_center - floating_origin
		var view_center: Vector3 = view_transform * render_center
		view_spheres[base] = view_center.x
		view_spheres[base + 1] = view_center.y
		view_spheres[base + 2] = view_center.z
		view_spheres[base + 3] = radius

	var candidate_bytes: PackedByteArray = view_spheres.to_byte_array()
	if _rd.buffer_update(_candidate_buffer, 0, candidate_bytes.size(), candidate_bytes) != OK:
		return

	var depth_uniform := RDUniform.new()
	depth_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	depth_uniform.binding = 0
	depth_uniform.add_id(_depth_sampler)
	depth_uniform.add_id(depth_rid)

	var tile_uniform := RDUniform.new()
	tile_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	tile_uniform.binding = 1
	tile_uniform.add_id(_tile_buffer)

	var candidate_uniform := RDUniform.new()
	candidate_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	candidate_uniform.binding = 2
	candidate_uniform.add_id(_candidate_buffer)

	var result_uniform := RDUniform.new()
	result_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	result_uniform.binding = 3
	result_uniform.add_id(_result_buffer)

	var uniform_set: RID = UniformSetCacheRD.get_cache(_shader, 0,
		[depth_uniform, tile_uniform, candidate_uniform, result_uniform])
	if not uniform_set.is_valid():
		return

	# Reserve the buffer before submitting work so no subsequent render callback can
	# overwrite it until its asynchronous readback has completed.
	_result_mutex.lock()
	_readback_pending = true
	_result_mutex.unlock()

	var projection: Projection = scene_data.get_view_projection(0)
	var compute_list: int = _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
	_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)

	var init_count: int = maxi(TILE_COUNT, candidate_count)
	_set_push(compute_list, 0, size, candidate_count, generation, projection)
	_rd.compute_list_dispatch(compute_list, (init_count + 63) / 64, 1, 1)
	_rd.compute_list_add_barrier(compute_list)

	_set_push(compute_list, 1, size, candidate_count, generation, projection)
	_rd.compute_list_dispatch(compute_list, (size.x + 63) / 64, size.y, 1)
	_rd.compute_list_add_barrier(compute_list)

	_set_push(compute_list, 2, size, candidate_count, generation, projection)
	_rd.compute_list_dispatch(compute_list, (candidate_count + 63) / 64, 1, 1)
	_rd.compute_list_end()
	_dispatch_frames += 1

	var callback: Callable = Callable(self, "_consume_results").bind(
		generation, candidate_count, camera_world, camera_forward)
	var err: int = _rd.buffer_get_data_async(
		_result_buffer, callback, 0, (RESULT_HEADER_UINTS + candidate_count) * 4)
	if err != OK:
		_result_mutex.lock()
		_readback_pending = false
		_readback_failures += 1
		_result_mutex.unlock()
	else:
		_readback_requests += 1


func _set_push(compute_list: int, pass_id: int, size: Vector2i,
		candidate_count: int, generation: int, projection: Projection) -> void:
	var push := PackedFloat32Array()
	_append_vec4(push, Vector4(float(pass_id), float(size.x), float(size.y), float(candidate_count)))
	_append_vec4(push, Vector4(float(TILE_WIDTH), float(TILE_HEIGHT),
		float(generation), REVERSE_Z_RELATIVE_MARGIN))
	_append_vec4(push, projection.x)
	_append_vec4(push, projection.y)
	_append_vec4(push, projection.z)
	_append_vec4(push, projection.w)
	var bytes: PackedByteArray = push.to_byte_array()
	_rd.compute_list_set_push_constant(compute_list, bytes, bytes.size())


func _consume_results(data: PackedByteArray, requested_generation: int,
		requested_count: int, camera_world: Vector3, camera_forward: Vector3) -> void:
	var snapshot: Dictionary = {}
	if data.size() >= RESULT_HEADER_UINTS * 4:
		var gpu_generation: int = int(data.decode_u32(0))
		var gpu_count: int = mini(int(data.decode_u32(4)), requested_count)
		var valid: bool = data.decode_u32(8) != 0
		if valid and gpu_generation == requested_generation and \
				data.size() >= (RESULT_HEADER_UINTS + gpu_count) * 4:
			var mask := PackedByteArray()
			mask.resize(gpu_count)
			for i: int in gpu_count:
				mask[i] = 1 if data.decode_u32((RESULT_HEADER_UINTS + i) * 4) != 0 else 0
			snapshot = {
				"generation": gpu_generation,
				"candidate_count": gpu_count,
				"occluded": mask,
				"camera_world": camera_world,
				"camera_forward": camera_forward,
				"received_msec": Time.get_ticks_msec(),
			}

	_result_mutex.lock()
	if not snapshot.is_empty():
		_latest_snapshot = snapshot
	_readback_pending = false
	_result_mutex.unlock()


static func _append_vec4(array: PackedFloat32Array, value: Vector4) -> void:
	array.append(value.x)
	array.append(value.y)
	array.append(value.z)
	array.append(value.w)
