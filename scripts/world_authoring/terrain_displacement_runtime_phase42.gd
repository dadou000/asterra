extends "res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd"
## Phase 42A: publish the active transactional production geomorph snapshot to the
## renderer/cache bridge.
##
## This is read-only runtime state. Rejected graph candidates already restore
## `_production_controls` in Phase 33, so consumers can never observe settings that
## failed the displacement transaction.

const GEOMORPH_CONTRACT := preload(
	"res://scripts/world_authoring/model/terrain_geomorph_gpu_contract.gd")


func active_production_controls() -> Dictionary:
	return GEOMORPH_CONTRACT.normalized_controls(_production_controls)


func active_production_controls_fingerprint(fallback_seed: int) -> String:
	return GEOMORPH_CONTRACT.fingerprint(_production_controls, fallback_seed)


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["geomorph_gpu_contract_version"] = GEOMORPH_CONTRACT.CONTRACT_VERSION
	out["active_production_controls"] = active_production_controls()
	return out
