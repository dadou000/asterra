class_name PlanetRiverReachCollapseBridgeProduction
extends PlanetRiverReachCollapseBridge
## Production teardown hardening for asynchronous 16-byte tile-state readback.
##
## If release is requested while collapse diagnostics are pending, let the current
## ownership transaction resolve first. The retiring sparse generation is never
## restarted afterward, and RIDs are freed only after the readback callback returns.

var _production_release_requested := false


func release() -> void:
	if _production_release_requested:
		return
	_production_release_requested = true
	if busy() and diagnostic != null and diagnostic.pending():
		return
	_finish_production_release()


func _on_fine_state_ready(request_id: int, slot: int, state: Dictionary) -> void:
	super._on_fine_state_ready(request_id, slot, state)
	if _production_release_requested and not busy():
		_finish_production_release()


func _on_fine_state_failed(request_id: int, slot: int, error: Error) -> void:
	super._on_fine_state_failed(request_id, slot, error)
	if _production_release_requested and not busy():
		_finish_production_release()


func _finish_request(restore_owners: bool) -> void:
	# Never restart a sparse generation that its owner is already retiring.
	super._finish_request(restore_owners and not _production_release_requested)


func _finish_production_release() -> void:
	if busy():
		return
	# Call the base implementation only after asynchronous ownership work is done.
	super.release()
	_production_release_requested = false
