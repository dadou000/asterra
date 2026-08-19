class_name PassGeology
extends RefCounted
## 1.3 Geology first.
##
## The geological substrate exists *before* soil, biomes and cities because it
## decides two things nothing else can: where resources are, and how the ground
## behaves when you excavate or load it. Rock family is chosen from tectonic
## setting rather than painted, so an ore body always has a reason to be there.

const R := PlanetFields.Rock

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
		var oceanic := h < cfg.shelf_depth - 10.0
		var lat := absf(d.y)

		# --- faults and folded regions -------------------------------------
		var f := NoiseKit.smoothstepf(0.55, 0.95, fault_n.u(d))
		f = clampf(f * (0.35 + 1.6 * bnd) * cfg.fault_density + maxf(0.0, conv) * 0.45, 0.0, 1.0)
		fields.fault[c] = f
		fields.strata_dip[c] = clampf(f * 0.9 + absf(fold_n.s(d)) * 0.35 * (0.2 + bnd), 0.0, 1.2)
		fields.strata_phase[c] = strat_n.s(d) * cfg.strata_thickness * 6.0 + h * 0.35

		# --- bedrock family --------------------------------------------------
		var pv := province.u(d)
		var craton := NoiseKit.smoothstepf(0.35, 0.0, province_d.u(d))  # province interiors
		var plutonic := 1.0 - clampf(pluton.u(d) * 2.0, 0.0, 1.0)
		var rock: int
		if oceanic:
			if bnd > 0.6 and conv < 0.0:
				rock = R.BASALT
			elif pv > 0.72:
				rock = R.SERPENTINITE
			else:
				rock = R.GABBRO if pv < 0.30 else R.BASALT
		elif conv > 0.25 and h > 900.0:
			# Core of an orogen: metamorphic with granite plutons.
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
			# Sedimentary basin fill; carbonates need warm shallow water.
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
		# Continuous copy so runtime detail amplitude does not jump at macro cell
		# boundaries -- a nearest-neighbour lookup here shows up as visible terraces.
		fields.erodibility[c] = PlanetFields.ROCK_ERODIBILITY[rock]

		# --- ore veins and mineral gradients ---------------------------------
		var vfe := NoiseKit.smoothstepf(0.62, 0.94, vein_fe.u(d))
		var vcu := NoiseKit.smoothstepf(0.66, 0.96, vein_cu.u(d))
		var vqz := NoiseKit.smoothstepf(0.70, 0.97, vein_qz.u(d))
		# Banded iron sits in old cratonic and metamorphic terrain.
		var fe_host := 1.0 if rock in [R.GNEISS, R.SCHIST, R.QUARTZITE] else 0.35
		fields.ore_iron[c] = clampf(vfe * fe_host * (0.4 + craton) * cfg.ore_richness, 0.0, 1.0)
		# Porphyry copper wants a volcanic arc above a subduction zone.
		var arc := maxf(0.0, conv) * bnd * (1.0 if not oceanic else 0.4)
		var cu_host := 1.0 if rock in [R.GRANITE, R.RHYOLITE_TUFF, R.GABBRO] else 0.3
		fields.ore_copper[c] = clampf(vcu * cu_host * (0.25 + 1.8 * arc + 0.5 * f) * cfg.ore_richness, 0.0, 1.0)
		# High-purity quartz / silica for Axiom: hydrothermal veins in quartzite
		# and pegmatite margins of granite plutons.
		var qz_host := 1.0 if rock == R.QUARTZITE else (0.75 if rock == R.GRANITE else 0.12)
		fields.quartz[c] = clampf(vqz * qz_host * (0.3 + 1.2 * plutonic + 0.6 * f) * cfg.ore_richness, 0.0, 1.0)

		# --- coal, petroleum and natural gas ---------------------------------
		# All three need a basin. Coal needs swampy deposition; hydrocarbons need
		# organic-rich source rock, burial and a seal.
		var org := NoiseKit.smoothstepf(0.1, 0.85, organic.u(d))
		var burial := clampf(bas * 1.2 - maxf(0.0, h) / 2600.0, 0.0, 1.0)
		var sealing := NoiseKit.smoothstepf(0.25, 0.8, seal.u(d))
		var coal_host := 1.0 if rock in [R.SHALE, R.SANDSTONE, R.CONGLOMERATE] else 0.15
		fields.coal[c] = clampf(burial * org * coal_host * 1.3, 0.0, 1.0)
		var petro_host := 1.0 if rock in [R.SHALE, R.LIMESTONE, R.SANDSTONE, R.DOLOMITE] else 0.08
		var petro := burial * org * petro_host * sealing * 1.5
		fields.petroleum[c] = clampf(petro, 0.0, 1.0)
		# Deeper, hotter basins crack oil to gas.
		fields.gas_fraction[c] = clampf(0.22 + burial * 0.55 + (1.0 - sealing) * 0.15, 0.0, 1.0)

		# --- groundwater-relevant layers -------------------------------------
		var porous := 0.0
		match rock:
			R.SANDSTONE, R.CONGLOMERATE: porous = 1.0
			R.LIMESTONE, R.DOLOMITE: porous = 0.85   # karst
			R.SHALE: porous = 0.08                   # aquitard
			R.BASALT, R.RHYOLITE_TUFF: porous = 0.45
			_: porous = 0.18
		fields.aquifer[c] = clampf(porous * (0.35 + 0.9 * bas) * (1.0 - clampf(maxf(0.0, h) / 3200.0, 0.0, 0.8)), 0.0, 1.0)

## Erodibility of the surface bedrock, used by the erosion pass.
func erodibility(c: int) -> float:
	return PlanetFields.ROCK_ERODIBILITY[fields.rock[c]]
