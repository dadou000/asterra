extends Node
## CPU-only distributed-precipitation authority gate.
##
## Fine tiles claim rainfall only after scheduler activation (the same event that
## publishes atlas occupancy). Their nominal SWE area accumulates in the containing
## PlanetGrid macro cell and is returned exactly on release. No RenderingDevice is
## required.

const GRID_RES := 4
const PLANET_RADIUS_M := 10000.0
const TILE_AREA_M2 := 400.0
const ABS_TOL := 1.0e-10

var _failures: Array[String] = []


func _ready() -> void:
	var store := _make_store()
	if store == null:
		_finish()
		return
	var scheduler := SparseHydroScheduler.new(8)
	var ownership := HydroCoarseFineOwnershipMap.new()
	var err := ownership.initialize(store, scheduler, TILE_AREA_M2)
	_expect(err == OK, "ownership map initialize failed (%d)" % int(err))
	if err != OK:
		_finish()
		return

	var key_a := HydroTileKey.new(CubeSphere.FACE_PX, 10, 512, 512)
	var key_b := HydroTileKey.new(CubeSphere.FACE_PX, 10, 513, 512)
	var macro_a := store.grid.dir_to_index(_tile_center(key_a))
	var macro_b := store.grid.dir_to_index(_tile_center(key_b))
	_expect(macro_a == macro_b,
		"fixture fine tiles did not land in the same coarse macro cell")
	var macro_area := store.area_m2[macro_a]
	var one_fraction := 1.0 - TILE_AREA_M2 / macro_area
	var two_fraction := 1.0 - 2.0 * TILE_AREA_M2 / macro_area

	# Reservation owns a transient slot but occupancy/fine forcing is still absent.
	var slot_a := scheduler.reserve(key_a, 0, "ownership_test")
	_expect(slot_a >= 0, "first fine tile reservation failed")
	_expect_close(ownership.coarse_fraction(macro_a), 1.0,
		"ALLOCATING tile stole precipitation authority")
	_expect(ownership.mapped_tile_count() == 0,
		"ALLOCATING tile appeared in ownership map")

	_expect(scheduler.activate_reserved(key_a, "ownership_test") == slot_a,
		"first fine tile activation failed")
	_expect_close(ownership.fine_owned_area_m2(macro_a), TILE_AREA_M2,
		"first fine tile area mismatch")
	_expect_close(ownership.coarse_fraction(macro_a), one_fraction,
		"first fine tile coarse fraction mismatch")

	var slot_b := scheduler.reserve(key_b, 0, "ownership_test")
	_expect(slot_b >= 0, "second fine tile reservation failed")
	_expect(scheduler.activate_reserved(key_b, "ownership_test") == slot_b,
		"second fine tile activation failed")
	_expect_close(ownership.fine_owned_area_m2(macro_a), 2.0 * TILE_AREA_M2,
		"fine areas did not accumulate")
	_expect_close(ownership.coarse_fraction(macro_a), two_fraction,
		"two-tile coarse fraction mismatch")

	# Rebuild from pool is deterministic and must not double-count existing map data.
	ownership.rebuild()
	_expect(ownership.mapped_tile_count() == 2,
		"ownership rebuild lost resident tiles")
	_expect_close(ownership.fine_owned_area_m2(macro_a), 2.0 * TILE_AREA_M2,
		"ownership rebuild changed accumulated area")
	_expect_close(ownership.coarse_fraction(macro_a), two_fraction,
		"ownership rebuild changed coarse fraction")

	_expect(scheduler.force_release(key_a, "ownership_test"),
		"first fine tile release failed")
	_expect_close(ownership.fine_owned_area_m2(macro_a), TILE_AREA_M2,
		"release did not return one fine footprint")
	_expect_close(ownership.coarse_fraction(macro_a), one_fraction,
		"release did not restore coarse fraction")

	_expect(scheduler.force_release(key_b, "ownership_test"),
		"second fine tile release failed")
	_expect_close(ownership.fine_owned_area_m2(macro_a), 0.0,
		"all fine area was not returned")
	_expect_close(ownership.coarse_fraction(macro_a), 1.0,
		"coarse precipitation authority was not fully restored")
	_expect(ownership.mapped_tile_count() == 0,
		"released tiles remained mapped")

	# The produced vector must be directly acceptable by the coarse system API.
	var fractions := ownership.coarse_precipitation_fractions()
	_expect(fractions.size() == store.cell_count(),
		"authority vector size differs from coarse store")
	for value in fractions:
		_expect_close(value, 1.0, "released authority vector is not all coarse")

	ownership.release()
	_finish()


func _make_store() -> PlanetHydrologyOwnershipStore:
	var cfg := GenConfig.new()
	cfg.face_res = GRID_RES
	cfg.planet_radius = PLANET_RADIUS_M
	var grid := PlanetGrid.new(GRID_RES, PLANET_RADIUS_M)
	var fields := PlanetFields.new(cfg, grid)
	fields.elev.fill(100.0)
	fields.base_elev.fill(100.0)
	fields.flow_dir.fill(255)
	fields.lake_level.fill(-1.0e9)
	fields.soil_depth.fill(0.40)
	fields.soil_sand.fill(0.45)
	fields.soil_silt.fill(0.35)
	fields.soil_clay.fill(0.20)
	fields.soil_organic.fill(0.05)
	fields.soil_moisture.fill(0.0)
	fields.aquifer.fill(0.35)
	fields.floodplain.fill(0.15)
	fields.relief.fill(20.0)
	fields.discharge.fill(0.0)
	fields.stream_order.fill(1)
	var store := PlanetHydrologyOwnershipStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_failures.append("coarse store initialization failed (%d)" % int(err))
		return null
	store.set_climatology_fallback_enabled(false)
	return store


static func _tile_center(key: HydroTileKey) -> Vector3:
	var side := float(1 << key.level)
	var u := ((float(key.x) + 0.5) / side) * 2.0 - 1.0
	var v := ((float(key.y) + 0.5) / side) * 2.0 - 1.0
	return CubeSphere.face_uv_to_dir(key.face, u, v)


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _expect_close(value: float, reference: float, message: String) -> void:
	if absf(value - reference) > ABS_TOL:
		_failures.append("%s: got %.12g expected %.12g" % [message, value, reference])


func _finish() -> void:
	if _failures.is_empty():
		print("HYDRO_PRECIPITATION_OWNERSHIP: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_PRECIPITATION_OWNERSHIP: " + failure)
		get_tree().quit(1)
