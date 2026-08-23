extends Node
## Compatibility shim for the fully procedural GPU visual terrain.
##
## The inherited terrain/debug scripts still know the GroundHeightPageAtlas
## autoload name, but visual height pages no longer exist. Keeping this tiny API
## avoids loading the old ~4096-page atlas/table or reacting to CPU collision pages.


func atlas_texture() -> Texture2D:
	return null


func page_table_texture() -> Texture2D:
	return null


func page_birth_texture() -> Texture2D:
	return null


func ready_for_shader() -> bool:
	return false


func atlas_size() -> Vector2:
	return Vector2.ZERO


func table_capacity() -> int:
	return 0


func table_max_probes() -> int:
	return 0


func atlas_columns() -> int:
	return 0


func slot_count() -> int:
	return 0


func touch_sample(_d: Vector3, _level: int) -> bool:
	return false


func touch_samples(_directions: Array[Vector3], _level: int) -> bool:
	return false


func has_sample(_d: Vector3, _level: int) -> bool:
	return false


func stats() -> Dictionary:
	return {
		"resident_pages": 0,
		"slot_capacity": 0,
		"pages_uploaded": 0,
		"page_reuploads": 0,
		"evictions": 0,
		"table_updates": 0,
		"table_rebuilds": 0,
		"table_tombstones": 0,
		"table_insert_failures": 0,
		"uploaded_texels": 0,
		"disabled": true,
	}
