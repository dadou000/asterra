class_name CoastlineClipmapRuntime
extends Node
## Compatibility stub.
##
## The coastline is now evaluated directly on the GPU from the same procedural
## height field as GroundGeometryClipmap. Keeping this autoload node avoids
## breaking older scene/script references, but it intentionally performs no CPU
## terrain sampling, worker builds, image creation or texture uploads.

const CLIPMAP_RES := 512
const LEVEL_TEXEL_M := Vector3(35.0, 120.0, 480.0)

var _texture: Texture2DArray = null
var _published_center := Vector3(1.0, 0.0, 0.0)
var _published_right := Vector3(0.0, 0.0, -1.0)
var _published_up := Vector3(0.0, 1.0, 0.0)
var _published_version: int = 0


func _ready() -> void:
	set_process(false)


func gpu_backed() -> bool:
	return true


func legacy_cache_enabled() -> bool:
	return false
