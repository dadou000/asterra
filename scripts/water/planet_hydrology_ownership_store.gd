class_name PlanetHydrologyOwnershipStore
extends PlanetHydrologyStore
## Transactional ownership layer between persistent coarse hydrology and fine SWE.
##
## Promotion is deliberately two-phase:
##   prepare_promotion()  -> reserve free coarse water, physical state unchanged
##   fine tile stage/seed -> external acknowledgement
##   commit_promotion()   -> debit coarse state exactly once
##
## Demotion is the symmetric incoming transaction:
##   prepare_demotion()   -> validate/lock exact incoming fine parcel
##   fine tile unpublish  -> external acknowledgement
##   commit_demotion()    -> add the parcel to coarse storage exactly once
##
## A failed fine allocation/seed calls rollback_promotion(), leaving coarse water
## untouched. A failed fine unpublish calls rollback_demotion(), leaving the fine
## tile authoritative. While either direction is pending, coarse stepping is paused
## so representation ownership cannot race the planetary routing step.
##
## Pending ownership transactions are intentionally NOT serialized. snapshot()
## fails closed while ownership is unresolved, so save/load cannot duplicate or
## lose a parcel crossing the coarse/fine representation boundary.

var cumulative_promoted_to_fine_m3 := 0.0
var cumulative_demoted_from_fine_m3 := 0.0

var _next_promotion_transaction_id := 1
var _pending_promotions: Dictionary = {}
var _reserved_surface_m3 := PackedFloat64Array()
var _reserved_channel_m3 := PackedFloat64Array()

var _next_demotion_transaction_id := 1
var _pending_demotions: Dictionary = {}


func initialize(p_fields: PlanetFields) -> Error:
	var err := super.initialize(p_fields)
	if err != OK:
		return err
	_reserved_surface_m3.resize(cell_count())
	_reserved_channel_m3.resize(cell_count())
	_reserved_surface_m3.fill(0.0)
	_reserved_channel_m3.fill(0.0)
	_pending_promotions.clear()
	_pending_demotions.clear()
	_next_promotion_transaction_id = 1
	_next_demotion_transaction_id = 1
	cumulative_promoted_to_fine_m3 = 0.0
	cumulative_demoted_from_fine_m3 = 0.0
	return OK


func step(dt_s: float) -> Dictionary:
	if not _pending_promotions.is_empty():
		return {
			"error": ERR_BUSY,
			"reason": "promotion_transaction_pending",
			"pending_promotions": _pending_promotions.size(),
			"pending_demotions": _pending_demotions.size(),
		}
	if not _pending_demotions.is_empty():
		return {
			"error": ERR_BUSY,
			"reason": "demotion_transaction_pending",
			"pending_promotions": _pending_promotions.size(),
			"pending_demotions": _pending_demotions.size(),
		}
	return super.step(dt_s)


func pending_promotion_count() -> int:
	return _pending_promotions.size()


func pending_demotion_count() -> int:
	return _pending_demotions.size()


func pending_ownership_transaction_count() -> int:
	return _pending_promotions.size() + _pending_demotions.size()


func promotion_transaction(transaction_id: int) -> Dictionary:
	if not _pending_promotions.has(transaction_id):
		return {}
	return (_pending_promotions[transaction_id] as Dictionary).duplicate(true)


func demotion_transaction(transaction_id: int) -> Dictionary:
	if not _pending_demotions.has(transaction_id):
		return {}
	return (_pending_demotions[transaction_id] as Dictionary).duplicate(true)


func reserved_promotion_volume_m3(cell: int = -1) -> float:
	if not initialized:
		return 0.0
	if cell >= 0:
		if cell >= cell_count():
			return 0.0
		return maxf(_reserved_surface_m3[cell], 0.0) \
			+ maxf(_reserved_channel_m3[cell], 0.0)
	var total := 0.0
	for c in cell_count():
		total += maxf(_reserved_surface_m3[c], 0.0)
		total += maxf(_reserved_channel_m3[c], 0.0)
	return total


func available_promotion_volume_m3(cell: int) -> float:
	if not initialized or cell < 0 or cell >= cell_count() or fields.elev[cell] <= 0.0:
		return 0.0
	var surface_available := maxf(surface_storage_m3[cell] - _reserved_surface_m3[cell], 0.0)
	var channel_available := maxf(channel_storage_m3[cell] - _reserved_channel_m3[cell], 0.0)
	return surface_available + channel_available


## Reserve an exact amount of free (surface/channel) water for a future fine-SWE
## seed. Soil water is deliberately excluded: it remains a coarse subsurface
## reservoir until a groundwater representation explicitly owns it.
func prepare_promotion(cell: int, requested_volume_m3: float) -> Dictionary:
	if not initialized:
		return _ownership_error(ERR_UNCONFIGURED, "store_unconfigured")
	if not _pending_demotions.is_empty():
		return _ownership_error(ERR_BUSY, "demotion_transaction_pending")
	if cell < 0 or cell >= cell_count() or fields.elev[cell] <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_land_cell")
	if not is_finite(requested_volume_m3) or requested_volume_m3 <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_volume")

	var available := available_promotion_volume_m3(cell)
	var epsilon := maxf(1.0e-9, absf(available) * 1.0e-12)
	if requested_volume_m3 > available + epsilon:
		var rejected := _ownership_error(ERR_CANT_ACQUIRE_RESOURCE, "insufficient_free_water")
		rejected["available_volume_m3"] = available
		return rejected

	var surface_available := maxf(surface_storage_m3[cell] - _reserved_surface_m3[cell], 0.0)
	var surface_reserved := minf(requested_volume_m3, surface_available)
	var channel_reserved := maxf(requested_volume_m3 - surface_reserved, 0.0)
	_reserved_surface_m3[cell] += surface_reserved
	_reserved_channel_m3[cell] += channel_reserved

	var transaction_id := _next_promotion_transaction_id
	_next_promotion_transaction_id += 1
	var area := maxf(area_m2[cell], 1.0)
	var direction := grid.cell_dir(cell)
	var transaction := {
		"error": OK,
		"transaction_id": transaction_id,
		"transfer_direction": "coarse_to_fine",
		# Compatibility: existing callers expect `direction` to be the cell Vector3.
		"direction": direction,
		"cell": cell,
		"cell_direction": direction,
		"cell_area_m2": area,
		"reserved_volume_m3": requested_volume_m3,
		"reserved_surface_volume_m3": surface_reserved,
		"reserved_channel_volume_m3": channel_reserved,
		# A fine initializer may distribute this volume according to its own bed
		# raster, but it must acknowledge exactly this conserved amount.
		"equivalent_uniform_depth_m": requested_volume_m3 / area,
		"prepared_at_step": step_count,
		"prepared_at_simulated_seconds": simulated_seconds,
	}
	_pending_promotions[transaction_id] = transaction
	return transaction.duplicate(true)


## Called only after the fine representation confirms that the exact reserved
## volume has been seeded/accepted. This is the sole coarse debit point.
func commit_promotion(transaction_id: int) -> Dictionary:
	if not _pending_promotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_transaction")
	var transaction := _pending_promotions[transaction_id] as Dictionary
	var cell := int(transaction["cell"])
	var surface_volume := float(transaction["reserved_surface_volume_m3"])
	var channel_volume := float(transaction["reserved_channel_volume_m3"])
	var total_volume := float(transaction["reserved_volume_m3"])
	var epsilon := maxf(1.0e-9, absf(total_volume) * 1.0e-12)
	if surface_storage_m3[cell] + epsilon < surface_volume \
			or channel_storage_m3[cell] + epsilon < channel_volume:
		return _ownership_error(ERR_INVALID_DATA, "reserved_storage_invariant_broken")

	surface_storage_m3[cell] = maxf(surface_storage_m3[cell] - surface_volume, 0.0)
	channel_storage_m3[cell] = maxf(channel_storage_m3[cell] - channel_volume, 0.0)
	_release_promotion_reservation(transaction)
	_pending_promotions.erase(transaction_id)
	cumulative_promoted_to_fine_m3 += total_volume

	var result := transaction.duplicate(true)
	result["committed"] = true
	result["coarse_storage_m3"] = total_storage_m3()
	result["mass_error_m3"] = mass_error_m3()
	return result


## Release a reservation after a failed/cancelled fine allocation or seed. No
## physical water is modified because prepare never debits coarse storage.
func rollback_promotion(transaction_id: int) -> Dictionary:
	if not _pending_promotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_transaction")
	var transaction := _pending_promotions[transaction_id] as Dictionary
	_release_promotion_reservation(transaction)
	_pending_promotions.erase(transaction_id)
	var result := transaction.duplicate(true)
	result["rolled_back"] = true
	return result


## Reserve a validated incoming fine parcel. This does NOT add water yet. The
## reservation globally pauses coarse routing and blocks snapshots so the caller can
## safely unpublish the exact fine owner before committing the coarse addition.
##
## Surface/channel classification is explicit. Automatic flood collapse currently
## returns its full parcel to surface storage; future river/reach collapse may use
## the channel component.
func prepare_demotion(cell: int, surface_volume_m3: float,
		channel_volume_m3: float = 0.0) -> Dictionary:
	if not initialized:
		return _ownership_error(ERR_UNCONFIGURED, "store_unconfigured")
	if pending_ownership_transaction_count() > 0:
		return _ownership_error(ERR_BUSY, "ownership_transaction_pending")
	if cell < 0 or cell >= cell_count() or fields.elev[cell] <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_land_cell")
	if not is_finite(surface_volume_m3) or not is_finite(channel_volume_m3) \
			or surface_volume_m3 < 0.0 or channel_volume_m3 < 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_volume")
	var total := surface_volume_m3 + channel_volume_m3
	if not is_finite(total) or total <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_volume")

	var transaction_id := _next_demotion_transaction_id
	_next_demotion_transaction_id += 1
	var direction := grid.cell_dir(cell)
	var transaction := {
		"error": OK,
		"transaction_id": transaction_id,
		"transfer_direction": "fine_to_coarse",
		"direction": direction,
		"cell": cell,
		"cell_direction": direction,
		"cell_area_m2": maxf(area_m2[cell], 1.0),
		"incoming_volume_m3": total,
		"incoming_surface_volume_m3": surface_volume_m3,
		"incoming_channel_volume_m3": channel_volume_m3,
		"prepared_at_step": step_count,
		"prepared_at_simulated_seconds": simulated_seconds,
	}
	_pending_demotions[transaction_id] = transaction
	return transaction.duplicate(true)


## Commit a previously validated incoming parcel. No fallible external operation
## belongs between fine unpublish and this call; after prepare_demotion() succeeds,
## this is only deterministic local state addition plus transaction removal.
func commit_demotion(transaction_id: int) -> Dictionary:
	if not _pending_demotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_transaction")
	var transaction := _pending_demotions[transaction_id] as Dictionary
	var cell := int(transaction["cell"])
	var surface_volume := float(transaction["incoming_surface_volume_m3"])
	var channel_volume := float(transaction["incoming_channel_volume_m3"])
	var total := float(transaction["incoming_volume_m3"])

	surface_storage_m3[cell] += surface_volume
	channel_storage_m3[cell] += channel_volume
	_pending_demotions.erase(transaction_id)
	cumulative_demoted_from_fine_m3 += total

	var result := transaction.duplicate(true)
	result["committed"] = true
	result["accepted_volume_m3"] = total
	result["surface_volume_m3"] = surface_volume
	result["channel_volume_m3"] = channel_volume
	result["coarse_storage_m3"] = total_storage_m3()
	result["mass_error_m3"] = mass_error_m3()
	return result


## Cancel an incoming reservation when the fine side remains authoritative.
## Physical coarse storage was not modified by prepare_demotion().
func rollback_demotion(transaction_id: int) -> Dictionary:
	if not _pending_demotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_transaction")
	var transaction := _pending_demotions[transaction_id] as Dictionary
	_pending_demotions.erase(transaction_id)
	var result := transaction.duplicate(true)
	result["rolled_back"] = true
	return result


## Compatibility/direct transfer helper. Production asynchronous collapse should
## use prepare_demotion() -> fine unpublish -> commit_demotion() instead.
func accept_demotion(cell: int, surface_volume_m3: float,
		channel_volume_m3: float = 0.0) -> Dictionary:
	var prepared := prepare_demotion(cell, surface_volume_m3, channel_volume_m3)
	if int(prepared.get("error", FAILED)) != OK:
		return prepared
	return commit_demotion(int(prepared["transaction_id"]))


func mass_error_m3() -> float:
	if not initialized:
		return 0.0
	var expected := initial_storage_m3 + cumulative_precipitation_m3 \
		+ cumulative_climatology_input_m3 + cumulative_demoted_from_fine_m3 \
		- cumulative_outlet_m3 - cumulative_promoted_to_fine_m3
	return expected - total_storage_m3()


func mass_relative_error() -> float:
	var scale := maxf(initial_storage_m3 + cumulative_precipitation_m3 \
		+ cumulative_climatology_input_m3 + cumulative_demoted_from_fine_m3, 1.0)
	return absf(mass_error_m3()) / scale


func cell_state(cell: int) -> Dictionary:
	var state := super.cell_state(cell)
	if state.is_empty():
		return state
	state["promotion_reserved_m3"] = reserved_promotion_volume_m3(cell)
	state["promotion_available_m3"] = available_promotion_volume_m3(cell)
	return state


func promotion_candidates(max_count: int = 64,
		surface_depth_threshold_m: float = 0.025,
		discharge_ratio_threshold: float = 2.0) -> Array[Dictionary]:
	var candidates := super.promotion_candidates(max_count,
		surface_depth_threshold_m, discharge_ratio_threshold)
	for candidate in candidates:
		var cell := int(candidate.get("cell", -1))
		candidate["available_promotion_volume_m3"] = available_promotion_volume_m3(cell)
		candidate["reserved_promotion_volume_m3"] = reserved_promotion_volume_m3(cell)
	return candidates


func snapshot() -> Dictionary:
	# Saving unresolved ownership is unsafe in either direction. The fine side may
	# already contain a promoted seed, or may still contain a parcel reserved for
	# demotion that coarse storage has not accepted yet.
	if pending_ownership_transaction_count() > 0:
		return {}
	var data := super.snapshot()
	if data.is_empty():
		return data
	data["cumulative_promoted_to_fine_m3"] = cumulative_promoted_to_fine_m3
	data["cumulative_demoted_from_fine_m3"] = cumulative_demoted_from_fine_m3
	return data


func restore_snapshot(data: Dictionary) -> Error:
	var err := super.restore_snapshot(data)
	if err != OK:
		return err
	_clear_pending_ownership_transactions()
	cumulative_promoted_to_fine_m3 = maxf(
		float(data.get("cumulative_promoted_to_fine_m3", 0.0)), 0.0)
	cumulative_demoted_from_fine_m3 = maxf(
		float(data.get("cumulative_demoted_from_fine_m3", 0.0)), 0.0)
	return OK


func stats() -> Dictionary:
	var out := super.stats()
	out["ownership_transactions"] = {
		"pending": pending_ownership_transaction_count(),
		"pending_promotions": _pending_promotions.size(),
		"pending_demotions": _pending_demotions.size(),
		"reserved_m3": reserved_promotion_volume_m3(),
		"promoted_to_fine_m3": cumulative_promoted_to_fine_m3,
		"demoted_from_fine_m3": cumulative_demoted_from_fine_m3,
		"snapshot_blocked": pending_ownership_transaction_count() > 0,
	}
	# Override the base ledger values with the representation-aware ones.
	out["mass_error_m3"] = mass_error_m3()
	out["mass_relative_error"] = mass_relative_error()
	return out


func _release_promotion_reservation(transaction: Dictionary) -> void:
	var cell := int(transaction.get("cell", -1))
	if cell < 0 or cell >= cell_count():
		return
	_reserved_surface_m3[cell] = maxf(_reserved_surface_m3[cell]
		- float(transaction.get("reserved_surface_volume_m3", 0.0)), 0.0)
	_reserved_channel_m3[cell] = maxf(_reserved_channel_m3[cell]
		- float(transaction.get("reserved_channel_volume_m3", 0.0)), 0.0)


func _clear_pending_promotions() -> void:
	_pending_promotions.clear()
	if not _reserved_surface_m3.is_empty():
		_reserved_surface_m3.fill(0.0)
	if not _reserved_channel_m3.is_empty():
		_reserved_channel_m3.fill(0.0)


func _clear_pending_ownership_transactions() -> void:
	_clear_pending_promotions()
	_pending_demotions.clear()


func _ownership_error(error: Error, reason: String) -> Dictionary:
	return {
		"error": error,
		"reason": reason,
	}
