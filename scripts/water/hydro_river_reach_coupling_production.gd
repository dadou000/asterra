class_name HydroRiverReachCouplingProduction
extends HydroRiverReachCoupling
## Production lifecycle hardening: when release was requested during an in-flight
## exchange, reconcile the ownership result but never restart the retiring sparse
## generation. The replacement coarse owner may be re-enabled immediately.


func _restore_owners_and_pump() -> void:
	PersistentHydrologySystem.enabled = _coarse_was_enabled
	if _release_requested:
		if runtime != null:
			runtime.enabled = false
		return
	super._restore_owners_and_pump()
