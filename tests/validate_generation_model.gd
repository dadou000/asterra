extends SceneTree
## Focused invariants for the generated map data model.

const B := PlanetFields.Biome
const TB := PlanetFields.TectonicBoundary
const LM := PlanetFields.Landmark

func _init() -> void:
	if PlanetFields.Biome.has("RIVER") or PlanetFields.Biome.has("LAKE"):
		_fail("River/lake must not exist in the biome enum")
		return

	if not _validate_hydrology_keeps_biome():
		return
	if not _validate_tectonic_landmarks():
		return

	print("GENERATION_MODEL_OK")
	quit(0)

func _validate_hydrology_keeps_biome() -> bool:
	var cfg := GenConfig.new()
	cfg.face_res = 8
	var grid := PlanetGrid.new(cfg.face_res, cfg.planet_radius)
	var fields := PlanetFields.new(cfg, grid)
	fields.lake_level.fill(-1e9)

	var c := 0
	fields.elev[c] = 120.0
	fields.temp_mean[c] = 12.0
	fields.temp_range[c] = 8.0
	fields.precip[c] = 1000.0
	fields.soil_depth[c] = 1.0
	fields.lake_level[c] = 105.0
	fields.river_width[c] = 80.0

	PassBiome.new(fields).run()
	if fields.biome[c] != B.TEMPERATE_FOREST:
		_fail("Hydrology replaced underlying biome: expected temperate forest, got %s" %
			PlanetFields.BIOME_NAMES[fields.biome[c]])
		return false
	if not fields.is_lake(c) or not fields.has_river(c):
		_fail("Hydrology attributes were not independently preserved")
		return false
	if (fields.hydrology_flags(c) & PlanetFields.HydrologyAttribute.LAKE) == 0:
		_fail("Lake hydrology flag missing")
		return false
	if (fields.hydrology_flags(c) & PlanetFields.HydrologyAttribute.RIVER) == 0:
		_fail("River hydrology flag missing")
		return false
	print("GENERATION_MODEL_BIOME_HYDROLOGY_OK")
	return true

func _validate_tectonic_landmarks() -> bool:
	var cfg := GenConfig.new()
	cfg.face_res = 16
	cfg.landmark_density = 4.0
	cfg.volcanism_strength = 1.0
	cfg.max_volcanic_landmarks = 8
	var grid := PlanetGrid.new(cfg.face_res, cfg.planet_radius)
	var fields := PlanetFields.new(cfg, grid)
	fields.lake_level.fill(-1e9)
	fields.plate_boundary.fill(1.0)

	# Transform faults can be geologically strong without being generic magma
	# factories. They should not enter the fault-driven volcanic candidate set.
	fields.plate_boundary_type.fill(TB.TRANSFORM)
	fields.uplift.fill(0.0)
	var pass := PassLandmarks.new(fields)
	var transform_candidates: Array[Dictionary] = pass._collect_volcanic_candidates()
	if not transform_candidates.is_empty():
		_fail("Transform margin generated generic volcano candidates")
		return false

	# Convergent margins produce arc stratovolcano candidates.
	fields.plate_boundary_type.fill(TB.CONVERGENT)
	fields.uplift.fill(0.60)
	fields.elev.fill(700.0)
	var convergent: Array[Dictionary] = pass._collect_volcanic_candidates()
	if convergent.is_empty():
		_fail("Convergent test margin generated no volcanic candidates")
		return false
	for candidate in convergent:
		if int(candidate["kind"]) != LM.STRATOVOLCANO:
			_fail("Convergent margin produced wrong volcano type")
			return false

	# Divergent oceanic margins produce broad shields. Applying one must change
	# the physical elevation and may build an emergent volcanic island.
	fields.plate_boundary_type.fill(TB.DIVERGENT)
	fields.uplift.fill(-0.60)
	fields.elev.fill(-500.0)
	fields.base_elev.fill(-500.0)
	fields.landmark.fill(LM.NONE)
	fields.landmark_id.fill(0)
	fields.landmark_strength.fill(0.0)
	var divergent: Array[Dictionary] = pass._collect_volcanic_candidates()
	if divergent.is_empty():
		_fail("Divergent oceanic test margin generated no volcanic candidates")
		return false
	for candidate in divergent:
		if int(candidate["kind"]) != LM.SHIELD_VOLCANO:
			_fail("Divergent oceanic margin produced wrong volcano type")
			return false

	var feature: Dictionary = divergent[0]
	var center: int = int(feature["cell"])
	pass._apply_volcanic_landform(feature, 1)
	if fields.elev[center] <= 0.0:
		_fail("Oceanic shield did not alter terrain enough to build the test island")
		return false
	if fields.landmark[center] != LM.VOLCANIC_ISLAND or fields.landmark_id[center] != 1:
		_fail("Volcanic island metadata was not attached to generated terrain")
		return false

	print("GENERATION_MODEL_TECTONICS_OK")
	return true

func _fail(message: String) -> void:
	push_error("GENERATION_MODEL_FAILED: %s" % message)
	quit(1)
