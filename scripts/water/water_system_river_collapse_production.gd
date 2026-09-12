extends "res://scripts/water/water_system_river_collapse.gd"
## Final generation-boundary hardening.
##
## The base WaterSystem persistent-store handler is the sole coarse-store recycle
## path. Disable the older river-promotion-specific rebuild handler so a newly
## created coarse store can never bind to the retiring sparse generation while an
## asynchronous river collapse is finishing.


func _ready() -> void:
	super._ready()
	if PersistentHydrologySystem.store_rebuilt.is_connected(_on_river_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.disconnect(_on_river_store_rebuilt)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	var policy := get_node_or_null("/root/HydroChannelRefinement")
	out["automatic_channel_refinement"] = {} if policy == null \
		else policy.stats()
	return out
