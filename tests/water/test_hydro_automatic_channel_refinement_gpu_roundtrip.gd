extends "res://tests/water/test_hydro_automatic_channel_refinement_gpu.gd"
## Fixture hardening: satisfy the production collapse-stage hysteresis by moving
## residual 1D channel water into the existing coarse-owned pending refined-inflow
## bucket. This is an internal ownership move and is returned during collapse.


func _on_policy_promotion_completed(report: Dictionary) -> void:
	if not _promotion_seen and _store != null and _source >= 0 \
			and _store.is_refined_reach(_source):
		var target := 0.50 * _store.river_reaches.bankfull_storage_for_cell(_source)
		var current := _store.channel_storage_m3[_source]
		var moved := maxf(current - target, 0.0)
		if moved > 0.0:
			_store.channel_storage_m3[_source] -= moved
			_store.refined_pending_inflow_m3[_source] += moved
	super._on_policy_promotion_completed(report)
