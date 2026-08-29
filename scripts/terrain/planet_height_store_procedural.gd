extends "res://scripts/terrain/planet_height_store_playable.gd"
## Deprecated procedural-height-store compatibility alias.
##
## The former procedural CPU tile builder depended on private cache fields from
## PlanetHeightStorePlayable. That implementation was retired when the playable
## store became a thin adapter over GroundHeightStore + GPU terrain queries. Keep
## this path loadable for old scenes/tools, but do not resurrect the obsolete
## cache namespace or duplicate terrain synthesis.


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["procedural_collision"] = false
	out["procedural_collision_adapter"] = true
	out["terrain_detail_generator"] = false
	out["retired_legacy_builder"] = true
	return out
