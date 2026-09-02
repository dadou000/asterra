class_name PlanetRiverComponentPromotionBridgeProduction
extends PlanetRiverComponentPromotionBridge
## Production owner-pause hardening for component promotion.
##
## Both the sparse runtime and persistent coarse owner remain paused across hidden
## terrain staging, junction seeding, batch publication, coarse commit and coupling
## registration. Pre-commit failures restore both owners; post-commit ambiguity
## deliberately fails closed.

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


## The base bridge's generic failure tail intentionally fails the coarse owner closed.
## Production distinguishes pre-commit rollback (safe to resume) from post-commit
## ambiguity (must remain paused).
func _fail(error: Error, stage: String) -> void:
	var failed := _request_id
	if not _coarse_committed and _transaction_id >= 0:
		store.rollback_component_channel_promotion(_transaction_id)
		_transaction_id = -1
	if not _coarse_committed:
		_unpublish_or_cancel_members()
		_restore_runtime()
	else:
		if runtime != null:
			runtime.enabled = false
		PersistentHydrologySystem.enabled = false
	_clear_request()
	promotion_failed.emit(failed, error, stage)


func _published_report() -> Dictionary:
	var report := super._published_report()
	# Preserve the existing single-reach promotion signal convention. The component
	# outlet is the representative cell and its first member supplies a representative
	# tile/slot; complete component identity remains available in the graph fields.
	var outlet_cell := int(report.get("downstream_outlet_cell", -1))
	report["cell"] = outlet_cell
	var reach_reports_value: Variant = report.get("reach_reports", null)
	if reach_reports_value is Array:
		for value: Variant in reach_reports_value:
			if not (value is Dictionary):
				continue
			var reach := value as Dictionary
			if int(reach.get("cell", -1)) != outlet_cell:
				continue
			var members_value: Variant = reach.get("members", null)
			if members_value is Array and not (members_value as Array).is_empty():
				var member := (members_value as Array)[0] as Dictionary
				report["tile_id"] = int(member.get("tile_id", -1))
				report["slot"] = int(member.get("slot", -1))
			break
	return report
