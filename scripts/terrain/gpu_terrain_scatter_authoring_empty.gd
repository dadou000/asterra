class_name TerrainScatterEmpty
extends "res://scripts/terrain/gpu_terrain_scatter_authoring.gd"
## Intentionally empty production scatter binding.
##
## Foliage and geology assets are being rebuilt from a clean baseline. Keep the
## scatter architecture, terrain-context bindings and authoring hooks available,
## but instantiate and display no procedural or file-backed scatter until new
## assets are deliberately reintroduced.


func _build_ecology_batches() -> void:
	# Asset-backed ecology/geology is intentionally empty.
	_ecology_batches.clear()
	_ecology_loaded_asset_ids = PackedStringArray()
	_ecology_selected_triangles = 0
	_ecology_candidate_instances = 0


func _set_visible(_value: bool) -> void:
	# Suppress all inherited procedural fallback families as well as any accidental
	# ecology batches. The scatter system remains alive for future asset integration.
	if _grass_batch != null:
		_grass_batch.visible = false
	if _geo_stone_batch != null:
		_geo_stone_batch.visible = false
	if _river_stone_batch != null:
		_river_stone_batch.visible = false
	for batch_data: Dictionary in _ecology_batches:
		var instance: MultiMeshInstance3D = batch_data.get("instance") as MultiMeshInstance3D
		if instance != null:
			instance.visible = false


func scatter_stats() -> Dictionary:
	var out: Dictionary = super.scatter_stats()
	out["asset_catalog_empty"] = true
	out["foliage_enabled"] = false
	out["geology_enabled"] = false
	out["grass_candidates"] = 0
	out["geologic_stone_candidates"] = 0
	out["river_stone_candidates"] = 0
	out["grass_radius_m"] = 0.0
	out["geologic_stone_radius_m"] = 0.0
	out["river_stone_radius_m"] = 0.0
	return out


func gpu_scatter_stats() -> Dictionary:
	var out: Dictionary = super.gpu_scatter_stats()
	out["asset_catalog_empty"] = true
	out["foliage_assets_enabled"] = false
	out["geology_assets_enabled"] = false
	out["ecology_real_assets"] = 0
	out["ecology_asset_ids"] = PackedStringArray()
	out["ecology_mesh_batches"] = 0
	out["ecology_selected_lod_triangles"] = 0
	out["ecology_candidate_instances"] = 0
	return out
