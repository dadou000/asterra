class_name WaterSurfaceResources
extends Node
## Main-RenderingDevice resource owner for dynamic surface-water fields.
##
## The production hydrology solver will write this texture directly on the global
## RenderingDevice. Texture2DRD exposes the same RID to spatial materials, so the
## visible water coat can sample solver output without GPU -> CPU -> GPU copies.
##
## Phase 1 stores one local tangent-plane field:
##   R = dynamic surface-height residual [m]
##   G = tangent velocity X [m/s]
##   B = tangent velocity Y [m/s]
##   A = activity/foam hint [0..1]

signal resources_ready
signal resources_released
signal field_updated(revision: int)

const FIELD_RESOLUTION := 256
const FIELD_HALF_EXTENT_M := 2048.0
const CHANNELS := 4

var _rd: RenderingDevice
var _field_rid := RID()
var _field_texture: Texture2DRD
var _available := false
var _revision := 0
var _field_center_plane := Vector2.ZERO


func _ready() -> void:
	_create_resources()


func available() -> bool:
	return _available and _rd != null and _field_rid.is_valid()


func rendering_device() -> RenderingDevice:
	return _rd


func field_texture() -> Texture2D:
	return _field_texture


func field_rid() -> RID:
	return _field_rid


func field_resolution() -> int:
	return FIELD_RESOLUTION


func field_half_extent_m() -> float:
	return FIELD_HALF_EXTENT_M


func field_center_plane() -> Vector2:
	return _field_center_plane


func revision() -> int:
	return _revision


func set_field_center_plane(center_plane: Vector2) -> void:
	_field_center_plane = center_plane


func clear() -> Error:
	if not available():
		return ERR_UNAVAILABLE
	var err := _rd.texture_clear(_field_rid, Color(0.0, 0.0, 0.0, 0.0), 0, 1, 0, 1)
	if err == OK:
		_revision += 1
		field_updated.emit(_revision)
	return err


## Debug-only writer used to prove that a main-RD hydrology texture can drive the
## visible water path without any GPU readback. It intentionally performs one CPU
## upload; the production solver will replace this writer with compute dispatches.
func debug_write_gaussian(amplitude_m: float = 2.0, radius_m: float = 260.0,
		velocity_plane: Vector2 = Vector2.ZERO, activity: float = 1.0) -> Error:
	if not available():
		return ERR_UNAVAILABLE

	var radius := maxf(radius_m, 1.0)
	var values := PackedFloat32Array()
	values.resize(FIELD_RESOLUTION * FIELD_RESOLUTION * CHANNELS)
	for y in FIELD_RESOLUTION:
		var py := ((float(y) + 0.5) / float(FIELD_RESOLUTION) * 2.0 - 1.0) \
			* FIELD_HALF_EXTENT_M
		for x in FIELD_RESOLUTION:
			var px := ((float(x) + 0.5) / float(FIELD_RESOLUTION) * 2.0 - 1.0) \
				* FIELD_HALF_EXTENT_M
			var r2 := px * px + py * py
			var envelope := exp(-0.5 * r2 / (radius * radius))
			var offset := (x + y * FIELD_RESOLUTION) * CHANNELS
			values[offset + 0] = amplitude_m * envelope
			values[offset + 1] = velocity_plane.x * envelope
			values[offset + 2] = velocity_plane.y * envelope
			values[offset + 3] = clampf(activity * envelope, 0.0, 1.0)

	var err := _rd.texture_update(_field_rid, 0, values.to_byte_array())
	if err == OK:
		_revision += 1
		field_updated.emit(_revision)
	return err


func gpu_bytes_estimate() -> int:
	return FIELD_RESOLUTION * FIELD_RESOLUTION * CHANNELS * 4


func stats() -> Dictionary:
	return {
		"available": available(),
		"resolution": FIELD_RESOLUTION,
		"half_extent_m": FIELD_HALF_EXTENT_M,
		"bytes": gpu_bytes_estimate(),
		"revision": _revision,
		"center_plane": _field_center_plane,
	}


func _create_resources() -> void:
	_release_resources()
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		push_warning("WaterSurfaceResources: global RenderingDevice unavailable; "
			+ "dynamic GPU water is disabled (Forward+ or Mobile renderer required).")
		return

	var format := RDTextureFormat.new()
	format.texture_type = RenderingDevice.TEXTURE_TYPE_2D
	format.format = RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT
	format.width = FIELD_RESOLUTION
	format.height = FIELD_RESOLUTION
	format.depth = 1
	format.array_layers = 1
	format.mipmaps = 1
	format.samples = RenderingDevice.TEXTURE_SAMPLES_1
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT \
		| RenderingDevice.TEXTURE_USAGE_STORAGE_BIT \
		| RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT \
		| RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT \
		| RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT

	if not _rd.texture_is_format_supported_for_usage(format.format, format.usage_bits):
		push_error("WaterSurfaceResources: RGBA32F is unsupported for required water usages.")
		_rd = null
		return

	var zero_values := PackedFloat32Array()
	zero_values.resize(FIELD_RESOLUTION * FIELD_RESOLUTION * CHANNELS)
	var initial_data: Array[PackedByteArray] = [zero_values.to_byte_array()]
	_field_rid = _rd.texture_create(format, RDTextureView.new(), initial_data)
	if not _field_rid.is_valid():
		push_error("WaterSurfaceResources: failed to allocate dynamic water field.")
		_rd = null
		return

	_field_texture = Texture2DRD.new()
	_field_texture.texture_rd_rid = _field_rid
	_available = true
	_revision = 1
	resources_ready.emit()


func _release_resources() -> void:
	_available = false
	if _field_texture != null:
		_field_texture.texture_rd_rid = RID()
		_field_texture = null
	if _rd != null and _field_rid.is_valid():
		_rd.free_rid(_field_rid)
	_field_rid = RID()
	if _rd != null:
		resources_released.emit()
	_rd = null


func _exit_tree() -> void:
	_release_resources()
