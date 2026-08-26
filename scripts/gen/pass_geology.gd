class_name PassGeology
extends RefCounted
## Geological substrate generated after macro geography and exceptional landforms.
## Volcanic landmarks are physical provinces here, not visual labels: their
## edifice receives volcanic bedrock and their tectonic setting affects resources.

const R := PlanetFields.Rock
const LM := PlanetFields.Landmark
const TB := PlanetFields.TectonicBoundary

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	var s := cfg.stream_seed("geology")
	var province := NoiseKit.cellular(s + 1, 3.4, FastNoiseLite.RETURN_CELL_VALUE)
	var province_d := NoiseKit.cellular(s + 2, 3.4, FastNoiseLite.RETURN_DISTANCE2_SUB)
	var fault_n := NoiseKit.ridged(s + 3, 9.0, 4)
	var fold_n := NoiseKit.new(s + 4, 6.0, 3)
	var strat_n := NoiseKit.new(s + 5, 4.0, 4)
	var pluton := NoiseKit.cellular(s + 6, 6.5, FastNoiseLite.RETURN_DISTANCE)
	var vein_fe := NoiseKit.ridged(s + 7, 11.0, 3)
	var vein_cu := NoiseKit.ridged(s + 8, 13.0, 3)
	var vein_qz := NoiseKit.ridged(s + 9, 15.0, 3)
	var organic := NoiseKit.new(s + 10, 3.0, 4)
	var seal := NoiseKit.new(s + 11, 5.0, 3)

	var n := grid.cell_count
	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Geology", float(c) / float(n))
		var d := grid.cell_dir(c)
		var h: float = fields.elev[c]
		var bnd: float = fields.plate_boundary[c]
		var conv: float = fields.uplift[c]
		var bas: float = fields.basin[c]
		var landmark: int = fields.landmark[c]
		var landmark_strength: float = fields.landmark_strength[c]
		var oceanic := h < cfg.shelf_depth - 10.0
		var lat := absf(d.y)

		# --- faults and folded regions -------------------------------------
		var f := NoiseKit.smoothstepf(0.55, 0.95, fault_n.u(d))
		f = f * (0.35 + 1.6 * bnd) * cfg.fault_density + maxf(0.0, conv) * 0.45
		if fields.plate_boundary_type[c] == TB.TRANSFORM:
			f += bnd * 0.72 * cfg.fault_density
		fields.fault[c] = clampf(f, 0.0, 1.0)
		fields.strata_dip[c] = clampf(
			fields.fault[c] * 0.9 + absf(fold_n.s(d)) * 0.35 * (0.2 + bnd), 0.0, 1.2)
		fields.strata_phase[c] = strat_n.s(d) * cfg.strata_thickness * 6.0 + h * 0.35

		# --- bedrock family --------------------------------------------------
		var pv := province.u(d)
		var craton := NoiseKit.smoothstepf(0.35, 0.0, province_d.u(d))
		var plutonic := 1.0 - clampf(pluton.u(d) * 2.0, 0.0, 1.0)
		var rock: int

		# Landmark geology has priority over the generic host province. This is
		# deliberately strength-gated so the outer apron transitions back into
		# whatever rock the volcano grew through.
		if landmark_strength > 0.18 and landmark in [
			LM.SHIELD_VOLCANO, LM.VOLCANIC_ISLAND, LM.RIFT_VOLCANIC_FIELD]:
			rock = R.BASALT if landmark_strength > 0.34 else R.GABBRO
		elif landmark_strength > 0.20 and landmark == LM.STRATOVOLCANO:
			rock = R.RHYOLITE_TUFF if landmark_strength > 0.48 else R.BASALT
		elif oceanic:
			if bnd > 0.6 and conv < 0.0:
				rock = R.BASALT
			elif pv > 0.72:
				rock = R.SERPENTINITE
			else:
				rock = R.GABBRO if pv < 0.30 else R.BASALT
		elif conv > 0.25 and h > 900.0:
			if plutonic > 0.55:
				rock = R.GRANITE
			elif pv > 0.62:
				rock = R.QUARTZITE
			elif pv > 0.32:
				rock = R.GNEISS
			else:
				rock = R.SCHIST
		elif conv > 0.05 and not oceanic:
			rock = R.RHYOLITE_TUFF if pv > 0.60 else R.GRANITE
		elif bas > 0.35 or h < 260.0:
			if lat < 0.45 and pv > 0.55:
				rock = R.LIMESTONE
			elif pv > 0.78:
				rock = R.DOLOMITE
			elif pv > 0.44:
				rock = R.SHALE
			elif pv > 0.16:
				rock = R.SANDSTONE
			else:
				rock = R.CONGLOMERATE
		else:
			if craton > 0.5 and plutonic > 0.4:
				rock = R.GRANITE
			elif pv > 0.66:
				rock = R.GNEISS
			elif pv > 0.34:
				rock = R.SANDSTONE
			else:
				rock = R.SCHIST
		fields.rock[c] = rock
		fields.erodibility[c] = PlanetFields.ROCK_ERODIBILITY[rock]

		# --- ore veins and mineral gradients ---------------------------------
		var vfe := NoiseKit.smoothstepf(0.62, 0.94, vein_fe.u(d))
		var vcu := NoiseKit.smoothstepf(0.66, 0.96, vein_cu.u(d))
		var vqz := NoiseKit.smoothstepf(0.70, 0.97, vein_qz.u(d))
		var fe_host := 1.0 if rock in [R.GNEISS, R.SCHIST, R.QUARTZITE] else 0.35
		fields.ore_iron[c] = clampf(vfe * fe_host * (0.4 + craton) * cfg.ore_richness, 0.0, 1.0)

		var arc := maxf(0.0, conv) * bnd * (1.0 if not oceanic else 0.4)
		if landmark == LM.STRATOVOLCANO:
			arc += landmark_strength * 0.85
		var cu_host := 1.0 if rock in [R.GRANITE, R.RHYOLITE_TUFF, R.GABBRO, R.BASALT] else 0.3
		fields.ore_copper[c] = clampf(
			vcu * cu_host * (0.25 + 1.8 * arc + 0.5 * fields.fault[c]) * cfg.ore_richness,
			0.0, 1.0)

		var qz_host := 1.0 if rock == R.QUARTZITE else (0.75 if rock == R.GRANITE else 0.12)
		fields.quartz[c] = clampf(
			vqz * qz_host * (0.3 + 1.2 * plutonic + 0.6 * fields.fault[c]) * cfg.ore_richness,
			0.0, 1.0)

		# --- coal, petroleum and natural gas ---------------------------------
		var org := NoiseKit.smoothstepf(0.1, 0.85, organic.u(d))
		var burial := clampf(bas * 1.2 - maxf(0.0, h) / 2600.0, 0.0, 1.0)
		var sealing := NoiseKit.smoothstepf(0.25, 0.8, seal.u(d))
		var coal_host := 1.0 if rock in [R.SHALE, R.SANDSTONE, R.CONGLOMERATE] else 0.15
		fields.coal[c] = clampf(burial * org * coal_host * 1.3, 0.0, 1.0)
		var petro_host := 1.0 if rock in [R.SHALE, R.LIMESTONE, R.SANDSTONE, R.DOLOMITE] else 0.08
		var petro := burial * org * petro_host * sealing * 1.5
		fields.petroleum[c] = clampf(petro, 0.0, 1.0)
		fields.gas_fraction[c] = clampf(
			0.22 + burial * 0.55 + (1.0 - sealing) * 0.15, 0.0, 1.0)

		# --- groundwater-relevant layers -------------------------------------
		var porous := 0.0
		match rock:
			R.SANDSTONE, R.CONGLOMERATE: porous = 1.0
			R.LIMESTONE, R.DOLOMITE: porous = 0.85
			R.SHALE: porous = 0.08
			R.BASALT, R.RHYOLITE_TUFF: porous = 0.45
			_: porous = 0.18
		fields.aquifer[c] = clampf(
			porous * (0.35 + 0.9 * bas) *
			(1.0 - clampf(maxf(0.0, h) / 3200.0, 0.0, 0.8)), 0.0, 1.0)

func erodibility(c: int) -> float:
	return PlanetFields.ROCK_ERODIBILITY[fields.rock[c]]
