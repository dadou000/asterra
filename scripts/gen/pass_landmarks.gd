class_name PassLandmarks
extends RefCounted
## Exceptional macro landforms layered on top of broad plate relief.
##
## This pass is intentionally early. A volcano changes the physical elevation
## before geology, erosion, drainage and climate run, so downstream systems react
## to the actual mountain/island rather than to a decorative map marker.
##
## Placement is tectonic rather than biome-driven:
##   convergent -> clustered volcanic arcs / stratovolcanoes
##   divergent oceanic -> broad shield volcanoes, sometimes emergent islands
##   divergent continental -> rift volcanic fields
##   transform -> suppressed (no generic volcano placement)

const TB := PlanetFields.TectonicBoundary
const LM := PlanetFields.Landmark

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	if progress.is_valid():
		progress.call("Landmarks: tectonic candidates", 0.0)
	var candidates := _collect_volcanic_candidates()
	var selected := _select_volcanic_centers(candidates)
	if selected.is_empty():
		if progress.is_valid():
			progress.call("Landmarks", 1.0)
		return

	for i in selected.size():
		_apply_volcanic_landform(selected[i], i + 1)
		if progress.is_valid():
			progress.call("Landmarks: volcanic relief",
				float(i + 1) / float(selected.size()))

func _collect_volcanic_candidates() -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	var seed := cfg.stream_seed("landmarks_volcanic_candidates")
	var density := clampf(cfg.landmark_density, 0.0, 4.0)
	if density <= 0.0:
		return result

	for c in grid.cell_count:
		var boundary_type: int = fields.plate_boundary_type[c]
		if boundary_type != TB.CONVERGENT and boundary_type != TB.DIVERGENT:
			continue
		var bnd: float = fields.plate_boundary[c]
		if bnd < 0.42:
			continue

		var motion: float
		var kind: int
		if boundary_type == TB.CONVERGENT:
			motion = clampf(fields.uplift[c] / 0.72, 0.0, 1.0)
			kind = LM.STRATOVOLCANO
		else:
			motion = clampf(-fields.uplift[c] / 0.62, 0.0, 1.0)
			kind = LM.SHIELD_VOLCANO if fields.elev[c] < 0.0 else LM.RIFT_VOLCANIC_FIELD

		var tectonic := bnd * (0.25 + 0.75 * motion)
		var chance := 0.0028 * density * (0.30 + 0.70 * tectonic)
		var random_gate := HashRNG.unit3(seed, c, 17)
		if random_gate >= chance:
			continue

		# The random term prevents every selected center from sitting at exactly
		# the strongest point of a margin while tectonic forcing remains dominant.
		var score := tectonic * 0.82 + HashRNG.unit3(seed, c, 31) * 0.18
		result.append({"cell": c, "kind": kind, "score": score})
	return result

func _select_volcanic_centers(candidates: Array[Dictionary]) -> Array[Dictionary]:
	candidates.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["score"]) > float(b["score"]))

	var selected: Array[Dictionary] = []
	var max_count := maxi(cfg.max_volcanic_landmarks, 0)
	if max_count == 0:
		return selected

	for candidate in candidates:
		if selected.size() >= max_count:
			break
		var c: int = int(candidate["cell"])
		var kind: int = int(candidate["kind"])
		var d := grid.cell_dir(c)
		var min_separation := _minimum_separation_m(kind)
		var allowed := true
		for existing in selected:
			var ec: int = int(existing["cell"])
			var ed := grid.cell_dir(ec)
			var distance := acos(clampf(d.dot(ed), -1.0, 1.0)) * cfg.planet_radius
			var pair_min := minf(min_separation,
				_minimum_separation_m(int(existing["kind"])))
			if distance < pair_min:
				allowed = false
				break
		if allowed:
			selected.append(candidate)
	return selected

func _minimum_separation_m(kind: int) -> float:
	match kind:
		LM.STRATOVOLCANO:
			# Arc volcanoes are allowed to form fairly tight chains.
			return 28000.0
		LM.SHIELD_VOLCANO:
			return 62000.0
		LM.RIFT_VOLCANIC_FIELD:
			return 52000.0
		_:
			return 40000.0

func _apply_volcanic_landform(feature: Dictionary, feature_id: int) -> void:
	var c: int = int(feature["cell"])
	var requested_kind: int = int(feature["kind"])
	var center := grid.cell_dir(c)
	var base_height: float = fields.elev[c]
	var seed := cfg.stream_seed("landmark_volcano_%d" % feature_id)
	var u0 := HashRNG.unit3(seed, c, 3)
	var u1 := HashRNG.unit3(seed, c, 7)
	var strength := maxf(cfg.volcanism_strength, 0.0)

	var radius_m: float
	var relief_m: float
	match requested_kind:
		LM.STRATOVOLCANO:
			radius_m = lerpf(26000.0, 52000.0, u0)
			relief_m = lerpf(1900.0, 4300.0, u1) * strength
		LM.RIFT_VOLCANIC_FIELD:
			radius_m = lerpf(48000.0, 105000.0, u0)
			relief_m = lerpf(550.0, 1550.0, u1) * strength
		_:
			radius_m = lerpf(62000.0, 145000.0, u0)
			# Oceanic shields need enough relief to occasionally grow from a ridge
			# or seamount into a genuine island, but not every shield must emerge.
			relief_m = lerpf(2700.0, 5900.0, u1) * strength

	var final_kind := requested_kind
	if requested_kind == LM.SHIELD_VOLCANO and base_height < 0.0:
		if base_height + relief_m * 0.82 > 15.0:
			final_kind = LM.VOLCANIC_ISLAND

	var rough := NoiseKit.new(seed + 101, 72.0, 3, 2.0, 0.52)
	var max_angle := radius_m / maxf(cfg.planet_radius, 1.0)
	var min_dot := cos(max_angle)

	for cell in grid.cell_count:
		var d := grid.cell_dir(cell)
		var dp := center.dot(d)
		if dp < min_dot:
			continue
		var distance := acos(clampf(dp, -1.0, 1.0)) * cfg.planet_radius
		var x := clampf(distance / radius_m, 0.0, 1.0)
		var influence := 1.0 - x
		if influence <= 0.0:
			continue

		var profile := _volcanic_profile(requested_kind, x)
		var local_rough := lerpf(0.90, 1.10, rough.u(d))
		var delta_h := relief_m * profile * local_rough

		# Explicit summit crater/caldera. The edifice continues underneath it,
		# which gives a raised rim instead of simply punching a hole in terrain.
		if requested_kind != LM.RIFT_VOLCANIC_FIELD:
			var crater_radius := 0.115 if requested_kind == LM.STRATOVOLCANO else 0.075
			if x < crater_radius:
				var q := 1.0 - x / crater_radius
				delta_h -= relief_m * q * q * (0.20 if requested_kind == LM.STRATOVOLCANO else 0.08)
			var rim := maxf(0.0, 1.0 - absf(x - crater_radius) / 0.045)
			delta_h += relief_m * rim * rim * 0.045

		fields.elev[cell] += delta_h
		fields.base_elev[cell] += delta_h

		# Overlapping volcanic systems add their physical relief, but metadata is
		# owned by whichever feature has the strongest local influence.
		if influence > fields.landmark_strength[cell]:
			fields.landmark_strength[cell] = influence
			fields.landmark[cell] = final_kind
			fields.landmark_id[cell] = feature_id

func _volcanic_profile(kind: int, x: float) -> float:
	var q := maxf(1.0 - x, 0.0)
	match kind:
		LM.STRATOVOLCANO:
			# Steep cone with a broad lower apron.
			return q * 0.78 + pow(q, 2.8) * 0.22
		LM.RIFT_VOLCANIC_FIELD:
			# Broad regional swell; erosion later cuts individual valleys through it.
			return pow(q, 2.3)
		_:
			# Shield volcano: much broader and shallower than a stratovolcano.
			var dome := maxf(1.0 - x * x, 0.0)
			return pow(dome, 2.15)
