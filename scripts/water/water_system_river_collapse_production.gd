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
