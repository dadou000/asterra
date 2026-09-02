class_name PlanetRiverComponentPromotionBridgeProduction
extends PlanetRiverComponentPromotionBridge
## Production owner-pause hardening for component promotion.
##
## The base bridge already blocks coarse stepping through its ownership transaction.
## Production additionally pauses the global coarse owner for the complete hidden
## stage/seed/publish/register window and restores both owners on every rollback.

var _component_coarse_was_enabled := true


func promote_component(cells: PackedInt32Array) -> int:
	_component_coarse_was_enabled = bool(PersistentHydrologySystem.enabled)
	var request_id := super.promote_component(cells)
	if request_id >= 0:
		PersistentHydrologySystem.enabled = false
	return request_id


func _restore_runtime() -> void:
	super._restore_runtime()
	PersistentHydrologySystem.enabled = _component_coarse_was_enabled


func _published_report() -> Dictionary:
	var report := super._published_report()
	# Preserve the existing river promotion signal convention: component events use
	# the downstream outlet as their representative coarse cell while retaining the
	# full `cells` array and `component_id` in the report.
	report["cell"] = int(report.get("downstream_outlet_cell", -1))
	return report
