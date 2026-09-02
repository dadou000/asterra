class_name HydroCoarseFineOwnershipMap
extends RefCounted
## Spatial authority map for distributed precipitation across coarse/fine water reps.
##
## Every solver-visible sparse tile receives atmospheric forcing over its nominal
## SWE footprint. The persistent coarse store must therefore relinquish exactly that
## footprint from the macro cell containing the fine tile centre, otherwise rainfall
## is injected twice. Macro cells are orders of magnitude larger than one fine tile,
## so centre assignment is the first conservative partition; area is accumulated and
## clamped to each macro cell's physical spherical area.
##
## This object owns policy metadata only. It never changes water state and never
## decides whether fine forcing is currently authoritative; HydroWeatherCoupling
## applies/clears the produced fractions based on actual forcing publication state.

signal fractions_changed(fractions: PackedFloat64Array)

var store: PlanetHydrologyOwnershipStore
var scheduler: SparseHydroScheduler
var fine_tile_area_m2 := 0.0

var _initialized := false
var _fine_area_by_cell := PackedFloat64Array()
var _coarse_fraction := PackedFloat64Array()
var _tile_to_cell: Dictionary = {} # packed HydroTileKey -> PlanetGrid cell


func initialize(p_store: PlanetHydrologyOwnershipStore,
		p_scheduler: SparseHydroScheduler, p_fine_tile_area_m2: float) -> Error:
	if _initialized:
		return ERR_ALREADY_IN_USE
	if p_store == null or not p_store.initialized or p_scheduler == null \
			or p_scheduler.pool == null or not is_finite(p_fine_tile_area_m2) \
			or p_fine_tile_area_m2 <= 0.0:
		return ERR_INVALID_PARAMETER
	store = p_store
	scheduler = p_scheduler
	fine_tile_area_m2 = p_fine_tile_area_m2
	_fine_area_by_cell.resize(store.cell_count())
	_coarse_fraction.resize(store.cell_count())
	_fine_area_by_cell.fill(0.0)
	_coarse_fraction.fill(1.0)
	if not scheduler.tile_woken.is_connected(_on_tile_woken):
		scheduler.tile_woken.connect(_on_tile_woken)
	if not scheduler.tile_released.is_connected(_on_tile_released):
		scheduler.tile_released.connect(_on_tile_released)
	_initialized = true
	rebuild()
	return OK


func initialized_ok() -> bool:
	return _initialized


func fine_owned_area_m2(cell: int = -1) -> float:
	if not _initialized:
		return 0.0
	if cell >= 0:
		return _fine_area_by_cell[cell] if cell < _fine_area_by_cell.size() else 0.0
	var total := 0.0
	for value in _fine_area_by_cell:
		total += value
	return total


func coarse_precipitation_fractions() -> PackedFloat64Array:
	return _coarse_fraction.duplicate()


func coarse_fraction(cell: int) -> float:
	if not _initialized or cell < 0 or cell >= _coarse_fraction.size():
		return 1.0
	return _coarse_fraction[cell]


func mapped_tile_count() -> int:
	return _tile_to_cell.size()


func stats() -> Dictionary:
	var affected := 0
	var minimum_fraction := 1.0
	for value in _coarse_fraction:
		if value < 1.0 - 1.0e-12:
			affected += 1
			minimum_fraction = minf(minimum_fraction, value)
	return {
		"initialized": _initialized,
		"fine_tile_area_m2": fine_tile_area_m2,
		"mapped_tiles": _tile_to_cell.size(),
		"affected_coarse_cells": affected,
		"fine_owned_area_m2": fine_owned_area_m2(),
		"minimum_coarse_fraction": minimum_fraction,
	}


## Reconstruct from authoritative pool state. ALLOCATING tiles are intentionally
## absent: GPU occupancy is still zero and atmospheric forcing cannot touch them.
func rebuild() -> void:
	if not _initialized or store == null or scheduler == null:
		return
	_tile_to_cell.clear()
	_fine_area_by_cell.fill(0.0)
	for record in scheduler.pool.active_records():
		var state := int(record.get("state", HydroTilePool.TileState.ALLOCATING))
		if state == HydroTilePool.TileState.ALLOCATING:
			continue
		var tile_id := int(record.get("tile_id", -1))
		if tile_id < 0:
			# HydroTilePool records use stable id under `id` on older fixtures.
			tile_id = int(record.get("id", -1))
		if tile_id < 0:
			var slot := int(record.get("slot", -1))
			tile_id = scheduler.pool.id_for_slot(slot)
		if tile_id >= 0:
			_add_tile(tile_id, false)
	_recompute_fractions()
	fractions_changed.emit(_coarse_fraction.duplicate())


func _on_tile_woken(tile_id: int, _slot: int, _reason: String) -> void:
	if not _initialized or _tile_to_cell.has(tile_id):
		return
	_add_tile(tile_id, true)


func _on_tile_released(tile_id: int, _slot: int, _reason: String) -> void:
	if not _initialized:
		return
	var cell_variant: Variant = _tile_to_cell.get(tile_id, null)
	if cell_variant == null:
		return
	var cell := int(cell_variant)
	_tile_to_cell.erase(tile_id)
	if cell >= 0 and cell < _fine_area_by_cell.size():
		_fine_area_by_cell[cell] = maxf(_fine_area_by_cell[cell] - fine_tile_area_m2, 0.0)
		_recompute_cell_fraction(cell)
	fractions_changed.emit(_coarse_fraction.duplicate())


func _add_tile(tile_id: int, emit_change: bool) -> void:
	var key := HydroTileKey.unpack(tile_id)
	if key == null or store == null or store.grid == null:
		return
	var cell := store.grid.dir_to_index(_tile_center_dir(key))
	if cell < 0 or cell >= store.cell_count():
		return
	_tile_to_cell[tile_id] = cell
	_fine_area_by_cell[cell] += fine_tile_area_m2
	_recompute_cell_fraction(cell)
	if emit_change:
		fractions_changed.emit(_coarse_fraction.duplicate())


func _recompute_fractions() -> void:
	for cell in store.cell_count():
		_recompute_cell_fraction(cell)


func _recompute_cell_fraction(cell: int) -> void:
	var coarse_area := maxf(store.area_m2[cell], 1.0)
	var fine_area := clampf(_fine_area_by_cell[cell], 0.0, coarse_area)
	_coarse_fraction[cell] = clampf(1.0 - fine_area / coarse_area, 0.0, 1.0)


static func _tile_center_dir(key: HydroTileKey) -> Vector3:
	var side := float(1 << key.level)
	var u := ((float(key.x) + 0.5) / side) * 2.0 - 1.0
	var v := ((float(key.y) + 0.5) / side) * 2.0 - 1.0
	return CubeSphere.face_uv_to_dir(key.face, u, v)


func release() -> void:
	if scheduler != null:
		if scheduler.tile_woken.is_connected(_on_tile_woken):
			scheduler.tile_woken.disconnect(_on_tile_woken)
		if scheduler.tile_released.is_connected(_on_tile_released):
			scheduler.tile_released.disconnect(_on_tile_released)
	store = null
	scheduler = null
	_tile_to_cell.clear()
	_fine_area_by_cell = PackedFloat64Array()
	_coarse_fraction = PackedFloat64Array()
	_initialized = false
