extends "res://scripts/terrain/gpu_terrain_scatter_authoring.gd"
## Temporary foliage-free production binding.
##
## The scatter architecture stays intact while the old vegetation library is being
## replaced. Only geological scatter is allowed to instantiate. The inherited
## procedural grass batch is also forced off so deleting the foliage assets produces
## a genuinely foliage-free baseline in game.

const GEOLOGY_ONLY_ASSETS := [
	{"id":"boulder_01", "lod":2, "grid":2, "spacing":82.0, "density":0.34, "kind":4, "salt":1301, "scale_min":0.72, "scale_max":1.85, "wind":0.0, "shadows":false},
]


func _build_ecology_batches() -> void:
	_ecology_shader = load(ECOLOGY_SHADER_PATH) as Shader
	if _ecology_shader == null:
		push_error("Terrain ecology scatter shader could not be loaded")
		return
	for definition_value: Variant in GEOLOGY_ONLY_ASSETS:
		if definition_value is Dictionary:
			_build_ecology_asset(definition_value as Dictionary)


func _set_visible(value: bool) -> void:
	super._set_visible(value)
	# Grass is procedural rather than file-backed, but keeping it visible after an
	# asset purge would make the world still look populated by the old foliage set.
	if _grass_batch != null:
		_grass_batch.visible = false


func scatter_stats() -> Dictionary:
	var out: Dictionary = super.scatter_stats()
	out["foliage_enabled"] = false
	out["grass_candidates"] = 0
	out["grass_radius_m"] = 0.0
	return out


func gpu_scatter_stats() -> Dictionary:
	var out: Dictionary = super.gpu_scatter_stats()
	out["foliage_assets_enabled"] = false
	return out
