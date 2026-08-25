extends Node
## Compatibility shell for the old camera-centred material clipmap.
##
## Terrain material classification is now a resident six-face global texture built
## during Planet.adopt(). This autoload intentionally performs no recenter checks,
## no WorkerThreadPool jobs and no runtime Texture2DArray creation. The legacy
## fields remain only so older debug/runtime code can inspect the node safely.

var _texture: Texture2DArray
var _center := Vector3(1.0, 0.0, 0.0)
var _right := Vector3(0.0, 0.0, -1.0)
var _up := Vector3(0.0, 1.0, 0.0)


func _ready() -> void:
	process_priority = 7
	Planet.world_ready.connect(_on_world_ready)
	if Planet.ready_state:
		_refresh()


func _process(_dt: float) -> void:
	# Explicitly zero runtime work. The texture is immutable until a new generated
	# world is adopted, at which point world_ready refreshes the reference.
	pass


func _on_world_ready(_fields: PlanetFields) -> void:
	_refresh()


func _refresh() -> void:
	_texture = Planet.global_material_texture if Planet.has_method("global_material_stats") else null


func global_texture() -> Texture2DArray:
	return _texture


func stats() -> Dictionary:
	var source: Dictionary = Planet.global_material_stats() \
		if Planet.has_method("global_material_stats") else {}
	return {
		"mode": "global_resident",
		"resident": _texture != null,
		"face_res": int(source.get("face_res", 0)),
		"cache_hit": bool(source.get("cache_hit", false)),
		"compressed_bytes": int(source.get("compressed_bytes", 0)),
		"raw_bytes": int(source.get("raw_bytes", 0)),
		"load_or_build_ms": int(source.get("load_or_build_ms", 0)),
		"streaming": false,
		"recenter_jobs": 0,
		"in_flight": 0,
	}
