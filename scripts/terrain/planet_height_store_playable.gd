extends "res://scripts/terrain/planet_height_store.gd"
## Deprecated playable-runtime compatibility alias.
##
## The former streamed CPU height-store implementation has been retired in favor
## of GroundHeightStore + GPU terrain queries. Keeping this script as a thin alias
## preserves old scene/script references without reintroducing the legacy cache
## internals or the Planet dependency cycle.


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["playable_adapter"] = true
	return out
