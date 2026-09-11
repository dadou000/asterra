class_name TerrainScatterEmpty
extends "res://scripts/terrain/gpu_terrain_scatter_authoring.gd"
## Production scatter binding with an author-managed real-asset ecology library.
##
## The class name is retained for scene/autoload compatibility. Assets are now
## discovered from assets/scatter/asset_manifest.json and can be rebuilt/reloaded
## from Planet Studio instead of being hard-coded in the renderer.


func _build_ecology_batches() -> void:
	super._build_ecology_batches()


func _set_visible(value: bool) -> void:
	super._set_visible(value)


func scatter_stats() -> Dictionary:
	var out: Dictionary = super.scatter_stats()
	out["asset_catalog_empty"] = _ecology_loaded_asset_ids.is_empty()
	out["foliage_enabled"] = not _ecology_loaded_asset_ids.is_empty()
	out["geology_enabled"] = not _ecology_loaded_asset_ids.is_empty()
	return out


func gpu_scatter_stats() -> Dictionary:
	var out: Dictionary = super.gpu_scatter_stats()
	out["asset_catalog_empty"] = _ecology_loaded_asset_ids.is_empty()
	out["foliage_assets_enabled"] = not _ecology_loaded_asset_ids.is_empty()
	out["geology_assets_enabled"] = not _ecology_loaded_asset_ids.is_empty()
	return out
