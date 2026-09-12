class_name PlanetHydrologyRiverPromotionStore
extends PlanetHydrologyReachOwnershipStore
## Reach-aware ownership store with an explicit channel-only coarse -> fine path.
##
## Surface flood promotion and river/reach promotion are physically different:
## surface promotion may reserve sheet/flood storage, while a 1D river corridor
## must reserve only channel_storage_m3. Both use the inherited transaction ledger
## and commit_promotion()/rollback_promotion(), so there is still exactly one coarse
## debit point and snapshots remain blocked while ownership is unresolved.


func available_channel_promotion_volume_m3(cell: int) -> float:
	if not initialized or river_reaches == null or not river_reaches.is_reach_cell(cell):
		return 0.0
	return maxf(channel_storage_m3[cell] - _reserved_channel_m3[cell], 0.0)


## Reserve a channel-only parcel from one generated 1D reach.
func prepare_channel_promotion(cell: int, requested_volume_m3: float) -> Dictionary:
	if not initialized:
		return _ownership_error(ERR_UNCONFIGURED, "store_unconfigured")
	if not _pending_demotions.is_empty():
		return _ownership_error(ERR_BUSY, "demotion_transaction_pending")
	if river_reaches == null or not river_reaches.is_reach_cell(cell):
		return _ownership_error(ERR_INVALID_PARAMETER, "not_a_river_reach")
	if not is_finite(requested_volume_m3) or requested_volume_m3 <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_volume")

	var available := available_channel_promotion_volume_m3(cell)
	var epsilon := maxf(1.0e-9, absf(available) * 1.0e-12)
	if requested_volume_m3 > available + epsilon:
		var rejected := _ownership_error(
			ERR_CANT_ACQUIRE_RESOURCE, "insufficient_free_channel_water")
		rejected["available_volume_m3"] = available
		return rejected

	_reserved_channel_m3[cell] += requested_volume_m3
	var transaction_id := _next_promotion_transaction_id
	_next_promotion_transaction_id += 1
	var direction := grid.cell_dir(cell)
	var reach := river_reaches.reach_state(cell, channel_storage_m3[cell])
	var transaction := {
		"error": OK,
		"transaction_id": transaction_id,
		"transfer_direction": "coarse_1d_channel_to_fine_2d",
		"representation": "river_reach",
		"direction": direction,
		"cell": cell,
		"cell_direction": direction,
		"cell_area_m2": maxf(area_m2[cell], 1.0),
		"reserved_volume_m3": requested_volume_m3,
		"reserved_surface_volume_m3": 0.0,
		"reserved_channel_volume_m3": requested_volume_m3,
		"equivalent_uniform_depth_m": 0.0,
		"river_width_m": maxf(float(fields.river_width[cell]), 0.0),
		"river_depth_m": float(reach.get("depth_m", 0.0)),
		"river_stage_m": float(reach.get("stage_m", fields.elev[cell])),
		"river_bankfull_ratio": float(reach.get("bankfull_depth_ratio", 0.0)),
		"prepared_at_step": step_count,
		"prepared_at_simulated_seconds": simulated_seconds,
	}
	_pending_promotions[transaction_id] = transaction
	return transaction.duplicate(true)


## Conservative default parcel for one local sparse river tile. It transfers the
## current 1D cross-section over at most one fine-tile span, never the entire macro
## reach. This keeps the remaining coarse reach authoritative outside the promoted
## local segment.
func suggested_channel_tile_volume_m3(cell: int, fine_tile_span_m: float) -> float:
	if not initialized or river_reaches == null or not river_reaches.is_reach_cell(cell) \
			or not is_finite(fine_tile_span_m) or fine_tile_span_m <= 0.0:
		return 0.0
	var storage := maxf(channel_storage_m3[cell], 0.0)
	var depth := river_reaches.depth_from_storage(cell,
		minf(storage, river_reaches.bankfull_storage_for_cell(cell)))
	var cross_area := river_reaches.cross_section_area(cell, depth)
	var represented_length := minf(maxf(fine_tile_span_m, 0.0),
		maxf(river_reaches.reach_length_m[cell], 0.0))
	var parcel := cross_area * represented_length
	return minf(maxf(parcel, 0.0), available_channel_promotion_volume_m3(cell))


func channel_promotion_state(cell: int) -> Dictionary:
	if not initialized or river_reaches == null or not river_reaches.is_reach_cell(cell):
		return {}
	var reach := river_reaches.reach_state(cell, channel_storage_m3[cell])
	reach["available_channel_promotion_volume_m3"] = \
		available_channel_promotion_volume_m3(cell)
	reach["reserved_channel_promotion_volume_m3"] = \
		maxf(_reserved_channel_m3[cell], 0.0)
	return reach
