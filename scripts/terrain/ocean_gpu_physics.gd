class_name OceanGPUPhysics
extends Node
## Batched GPU water-surface queries for buoyancy/hydrodynamics.
##
## This deliberately does not create one physics object per wave. A caller packs
## hull/pontoon probe points into one dispatch and receives surface height, normal,
## velocity and breaking intensity. Godot's rigid-body solver still applies the
## final force/torque on CPU, but the expensive per-probe wave evaluation lives on
## the GPU and scales to many probes with one compute submission.

const LOCAL_SIZE := 64
const QUERY_FLOATS := 8
const RESULT_FLOATS := 12

var _rd: RenderingDevice
var _shader: RID
var _pipeline: RID
var _available := false


func _ready() -> void:
	# A local RenderingDevice keeps compute resources independent of the frame
	# renderer and avoids accidental stalls in the main RenderingDevice state.
	_rd = RenderingServer.create_local_rendering_device()
	if _rd == null:
		return
	var shader_file: RDShaderFile = load("res://shaders/ocean_buoyancy.glsl")
	if shader_file == null:
		return
	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	_shader = _rd.shader_create_from_spirv(spirv)
	if not _shader.is_valid():
		return
	_pipeline = _rd.compute_pipeline_create(_shader)
	_available = _pipeline.is_valid()


func available() -> bool:
	return _available and Planet.ready_state and Planet.cfg != null


## Synchronous batched query intended for physics-tick aggregation, not individual
## probes. Keep probe counts in the tens/hundreds so one dispatch/readback serves
## an entire vehicle set. `coast_dirs` are landward tangent directions; if absent,
## the kernel falls back to a stable swell direction.
func sample_batch(points_planet: PackedVector3Array, depths: PackedFloat32Array,
		coast_dirs: PackedVector3Array = PackedVector3Array(), wave_scale: float = 1.0) -> Array[Dictionary]:
	var count := mini(points_planet.size(), depths.size())
	if count <= 0 or not available():
		return []

	var now_s := float(Time.get_ticks_usec()) / 1000000.0
	var input := PackedFloat32Array()
	input.resize(count * QUERY_FLOATS)
	for i in count:
		var p := points_planet[i]
		var d := Vector3(0.827, 0.201, 0.525)
		if i < coast_dirs.size() and coast_dirs[i].length_squared() > 1e-8:
			d = coast_dirs[i].normalized()
		var o := i * QUERY_FLOATS
		input[o] = p.x
		input[o + 1] = p.y
		input[o + 2] = p.z
		input[o + 3] = maxf(depths[i], 0.0)
		input[o + 4] = d.x
		input[o + 5] = d.y
		input[o + 6] = d.z
		input[o + 7] = now_s

	var output_bytes := PackedByteArray()
	output_bytes.resize(count * RESULT_FLOATS * 4)
	var params := PackedFloat32Array([
		Planet.cfg.planet_radius,
		maxf(wave_scale, 0.0),
		float(count),
		0.0,
	])

	var query_buffer := _rd.storage_buffer_create(input.to_byte_array().size(), input.to_byte_array())
	var result_buffer := _rd.storage_buffer_create(output_bytes.size(), output_bytes)
	var params_buffer := _rd.storage_buffer_create(params.to_byte_array().size(), params.to_byte_array())
	if not query_buffer.is_valid() or not result_buffer.is_valid() or not params_buffer.is_valid():
		_free_rid(query_buffer)
		_free_rid(result_buffer)
		_free_rid(params_buffer)
		return []

	var uniforms: Array[RDUniform] = []
	uniforms.append(_storage_uniform(0, query_buffer))
	uniforms.append(_storage_uniform(1, result_buffer))
	uniforms.append(_storage_uniform(2, params_buffer))
	var uniform_set := _rd.uniform_set_create(uniforms, _shader, 0)
	if not uniform_set.is_valid():
		_free_rid(query_buffer)
		_free_rid(result_buffer)
		_free_rid(params_buffer)
		return []

	var compute_list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
	_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	_rd.compute_list_dispatch(compute_list, int(ceil(float(count) / float(LOCAL_SIZE))), 1, 1)
	_rd.compute_list_end()
	_rd.submit()
	_rd.sync()

	var bytes := _rd.buffer_get_data(result_buffer)
	var values := bytes.to_float32_array()
	var out: Array[Dictionary] = []
	out.resize(count)
	for i in count:
		var o := i * RESULT_FLOATS
		out[i] = {
			"height": values[o],
			"normal": Vector3(values[o + 4], values[o + 5], values[o + 6]),
			"velocity": Vector3(values[o + 8], values[o + 9], values[o + 10]),
			"breaking": values[o + 11],
		}

	_free_rid(uniform_set)
	_free_rid(query_buffer)
	_free_rid(result_buffer)
	_free_rid(params_buffer)
	return out


func _storage_uniform(binding: int, buffer: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(buffer)
	return uniform


func _free_rid(rid: RID) -> void:
	if _rd != null and rid.is_valid():
		_rd.free_rid(rid)


func _exit_tree() -> void:
	if _rd == null:
		return
	_free_rid(_pipeline)
	_free_rid(_shader)
	_available = false
