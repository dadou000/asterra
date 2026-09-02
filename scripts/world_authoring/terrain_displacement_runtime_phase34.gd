extends "res://scripts/world_authoring/terrain_displacement_runtime_phase33.gd"
## Phase 34: canonical production geomorph schema integration.
##
## Phase 33 introduced the conservative visibility envelope. Phase 34 moves the
## production half of that calculation behind the same semantic schema that guards
## production-stage migration, preventing the renderer safety bound and graph model
## from drifting independently.

const PRODUCTION_GEOMORPH_SCHEMA := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_schema.gd")


func _production_geomorph_guard_m() -> float:
	return PRODUCTION_GEOMORPH_SCHEMA.production_guard_m(_production_controls)


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["production_geomorph_schema_version"] = PRODUCTION_GEOMORPH_SCHEMA.SCHEMA_VERSION
	out["production_geomorph_stage_ids"] = PRODUCTION_GEOMORPH_SCHEMA.ordered_stage_ids()
	return out
