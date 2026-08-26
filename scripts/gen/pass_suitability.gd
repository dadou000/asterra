class_name PassSuitability
extends RefCounted
## Infrastructure suitability derived from physical terrain and hydrology.

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	var n := grid.cell_count
	var slope := PackedFloat32Array()
	var relief := PackedFloat32Array()
	slope.resize(n)
	relief.resize(n)

	for c in n:
		var h: float = fields.elev[c]
		var base := c * 8
		var lo := h
		var hi := h
		for k in 8:
			var e: float = fields.elev[grid.nbr[base + k]]
			lo = minf(lo, e)
			hi = maxf(hi, e)
		relief[c] = hi - lo
		fields.relief[c] = relief[c]
		slope[c] = relief[c] / grid.cell_size[c]

	var ring2 := PackedFloat32Array()
	ring2.resize(n)
	for c in n:
		var sum := 0.0
		var cnt := 0
		var base := c * 8
		for k in 8:
			var nb := grid.nbr[base + k]
			sum += fields.elev[nb]
			cnt += 1
			var b2 := nb * 8
			for k2 in 8:
				sum += fields.elev[grid.nbr[b2 + k2]]
				cnt += 1
		ring2[c] = sum / float(cnt)

	if progress.is_valid():
		progress.call("Infrastructure suitability", 0.5)

	for c in n:
		var h: float = fields.elev[c]
		# Lakes are not biomes, but they are still physically unusable ground.
		if fields.is_water(c):
			fields.corridor[c] = 0.0
			fields.suitability[c] = 0.0
			continue

		var flat := clampf(1.0 - slope[c] * 30.0, 0.0, 1.0)
		var below := clampf((ring2[c] - h) / 260.0, 0.0, 1.0)
		var valley := below * flat

		var river := NoiseKit.smoothstepf(8.0, 120.0, fields.river_width[c])
		var rec_slot: int = fields.flow_dir[c] if fields.flow_dir[c] < 8 else 0
		var rec_h: float = fields.elev[grid.nbr[c * 8 + rec_slot]]
		var grad := absf(h - rec_h) / grid.cell_size[c]
		var gentle := clampf(1.0 - grad * 90.0, 0.0, 1.0)
		var river_corridor := river * gentle

		var pass_score := 0.0
		if ring2[c] > 700.0 and relief[c] > 60.0:
			pass_score = clampf((ring2[c] - h) / 420.0, 0.0, 1.0) * clampf(1.0 - slope[c] * 14.0, 0.0, 1.0)

		var coastal := 0.0
		var base := c * 8
		for k in 8:
			if fields.elev[grid.nbr[base + k]] < 0.0:
				coastal = 1.0
				break
		coastal *= clampf(1.0 - slope[c] * 22.0, 0.0, 1.0)

		var plain := flat * clampf(1.0 - below, 0.0, 1.0) * clampf(1.0 - h / 2600.0, 0.0, 1.0)
		var corr := maxf(
			maxf(valley, river_corridor * 1.05),
			maxf(pass_score * 0.95, maxf(coastal * 0.9, plain * 0.75)))
		fields.corridor[c] = clampf(corr, 0.0, 1.0)

		var bearing := clampf(1.0 - fields.soil_organic[c] * 1.6 - fields.wetland[c] * 0.8, 0.05, 1.0)
		var dry := clampf(1.0 - fields.floodplain[c] * 0.75 - fields.wetland[c] * 0.9, 0.0, 1.0)
		var altitude_pen := clampf(1.0 - maxf(0.0, h - 2200.0) / 2600.0, 0.1, 1.0)
		fields.suitability[c] = clampf(flat * bearing * dry * altitude_pen, 0.0, 1.0)

	var src := fields.corridor.duplicate()
	for c in n:
		if fields.is_water(c):
			fields.corridor[c] = 0.0
			continue
		var base := c * 8
		var sum := src[c] * 2.0
		var wsum := 2.0
		for k in 8:
			var w := 1.0 if (k & 1) == 0 else 0.7071
			sum += src[grid.nbr[base + k]] * w
			wsum += w
		fields.corridor[c] = sum / wsum
