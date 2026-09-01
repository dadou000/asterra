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
##
## IMPORTANT: every mutation of the global RenderingDevice is dispatched through
## RenderingServer.call_on_render_thread(). Only the Texture2DRD wrapper and
## lightweight state are touched on the game thread.

signal resources_ready
signal resources_released
signal field_updated(revision: int)
signal field_update_failed(error: Error)

const FIELD_RESOLUTION := 256
const FIELD_HALF_EXTENT_M := 2048.0
const CHANNELS := 4

var _rd: RenderingDevice
var _field_rid := RID()
var _field_texture: Texture2DRD
var _available := false
var _creation_pending := false
var _revision := 0
var _field_center_plane := Vector2.ZERO


func _ready() -> void:
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		push_warning("WaterSurfaceResources: global RenderingDevice unavailable; "
			+ "dynamic GPU water is disabled (Forward+ or Mobile renderer required).")
		return
	_creation_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_create_resources_render_thread"))


func available() -> bool:
	return _available and _rd != null and _field_rid.is_valid()


func creation_pending() -> bool:
	return _creation_pending


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


## Queue a GPU clear. OK means the request was accepted; field_updated is emitted
## when the render-thread operation actually succeeds.
func clear() -> Error:
	if not available():
		return ERR_UNAVAILABLE
	var rid := _field_rid
	RenderingServer.call_on_render_thread(
		Callable(self, &"_clear_render_thread").bind(rid))
	return OK


## Debug-only writer used to prove that a main-RD hydrology texture can drive the
## visible water path without any GPU readback. It intentionally performs one CPU
## upload; the production solver will replace this writer with compute dispatches.
## OK means the upload was queued, not that the render thread has completed it.
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

	var rid := _field_rid
	var bytes := values.to_byte_array()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_update_render_thread").bind(rid, bytes))
	return OK


func gpu_bytes_estimate() -> int:
	return FIELD_RESOLUTION * FIELD_RESOLUTION * CHANNELS * 4


func stats() -> Dictionary:
	return {
		"available": available(),
		"creation_pending": _creation_pending,
		"resolution": FIELD_RESOLUTION,
		"half_extent_m": FIELD_HALF_EXTENT_M,
		"bytes": gpu_bytes_estimate(),
		"revision": _revision,
		"center_plane": _field_center_plane,
	}


## Render-thread only.
func _create_resources_render_thread() -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_resource_creation", RID(), ERR_UNAVAILABLE)
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

	if not rd.texture_is_format_supported_for_usage(format.format, format.usage_bits):
		call_deferred("_finish_resource_creation", RID(), ERR_UNAVAILABLE)
		return

	var zero_values := PackedFloat32Array()
	zero_values.resize(FIELD_RESOLUTION * FIELD_RESOLUTION * CHANNELS)
	var initial_data: Array[PackedByteArray] = [zero_values.to_byte_array()]
	var rid := rd.texture_create(format, RDTextureView.new(), initial_data)
	if not rid.is_valid():
		call_deferred("_finish_resource_creation", RID(), ERR_CANT_CREATE)
		return
	call_deferred("_finish_resource_creation", rid, OK)


func _finish_resource_creation(rid: RID, error: Error) -> void:
	_creation_pending = false
	if error != OK or not rid.is_valid():
		push_error("WaterSurfaceResources: failed to allocate dynamic water field "
			+ "on the global RenderingDevice (error %d)." % int(error))
		return

	_field_rid = rid
	_field_texture = Texture2DRD.new()
	_field_texture.texture_rd_rid = _field_rid
	_available = true
	_revision = 1
	resources_ready.emit()


## Render-thread only.
func _clear_render_thread(rid: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	var err: Error = ERR_UNAVAILABLE
	if rd != null and rid.is_valid() and rd.texture_is_valid(rid):
		err = rd.texture_clear(rid, Color(0.0, 0.0, 0.0, 0.0), 0, 1, 0, 1)
	call_deferred("_finish_field_update", err)


## Render-thread only.
func _update_render_thread(rid: RID, bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	var err: Error = ERR_UNAVAILABLE
	if rd != null and rid.is_valid() and rd.texture_is_valid(rid):
		err = rd.texture_update(rid, 0, bytes)
	call_deferred("_finish_field_update", err)


func _finish_field_update(error: Error) -> void:
	if error != OK:
		field_update_failed.emit(error)
		return
	_revision += 1
	field_updated.emit(_revision)


## Render-thread only. This method must not touch scene-tree state.
func _free_rid_render_thread(rid: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null and rid.is_valid() and rd.texture_is_valid(rid):
		rd.free_rid(rid)


func _release_resources() -> void:
	_available = false
	_creation_pending = false
	if _field_texture != null:
		_field_texture.texture_rd_rid = RID()
		_field_texture = null
	if _field_rid.is_valid():
		var rid := _field_rid
		RenderingServer.call_on_render_thread(
			Callable(self, &"_free_rid_render_thread").bind(rid))
	_field_rid = RID()
	resources_released.emit()


func _exit_tree() -> void:
	_release_resources()
	_rd = null
