extends Node
## Quantifies final runtime displacement by biome at landscape and local scales.

func _ready() -> void:
	var cfg: GenConfig = load("res://world.tres")
	var fields := PlanetBake.new(cfg).bake(Callable(), true)
	Planet.adopt(fields)
	var selected := _select_cells(fields)
	print("\n=== BIOME TERRAIN DISPLACEMENT ===")
	for biome in selected:
		var cell: int = selected[biome]
		var landscape := _range_around(fields.grid.cell_dir(cell), 12000.0, 9)
		var local := _range_around(fields.grid.cell_dir(cell), 900.0, 9)
		print("%-27s cell %6d macro %7.0f m  relief-field %7.0f m  final 24km %7.0f m  final 1.8km %6.1f m" % [
			PlanetFields.BIOME_NAMES[biome], cell, Planet.macro_height(fields.grid.cell_dir(cell)),
			fields.relief[cell], landscape.y - landscape.x, local.y - local.x])
	var reported_shelf := CubeSphere.latlon_to_dir(deg_to_rad(36.0), deg_to_rad(-34.75))
	var reported_range := _range_around(reported_shelf, 900.0, 9)
	print("reported shelf lat 36.00 lon -34.75: macro %.2f m  pristine %.2f m  local relief %.2f m  water %.2f m  coverage %.3f" % [
		Planet.macro_height(reported_shelf), Planet.pristine_height(reported_shelf),
		reported_range.y - reported_range.x, Planet.water_height(reported_shelf),
		Planet.water_coverage(reported_shelf)])
	print("=== END BIOME TERRAIN DISPLACEMENT ===\n")
	get_tree().quit()

func _select_cells(fields: PlanetFields) -> Dictionary:
	var selected := {}
	var scores := {}
	for c in fields.grid.cell_count:
		if Planet.macro_height(fields.grid.cell_dir(c)) <= 50.0:
			continue
		var biome: int = fields.biome[c]
		var d := fields.grid.cell_dir(c)
		var score := _runtime_neighbour_relief(c, fields)
		if not scores.has(biome) or score > float(scores[biome]):
			scores[biome] = score
			selected[biome] = c
	return selected

func _runtime_neighbour_relief(cell: int, fields: PlanetFields) -> float:
	var lo := Planet.macro_height(fields.grid.cell_dir(cell))
	var hi := lo
	for k in 8:
		var h := Planet.macro_height(fields.grid.cell_dir(fields.grid.nbr[cell * 8 + k]))
		lo = minf(lo, h)
		hi = maxf(hi, h)
	return hi - lo

func _range_around(center: Vector3, radius_m: float, samples: int) -> Vector2:
	var basis := CubeSphere.tangent_basis(center)
	var east: Vector3 = basis[0]
	var north: Vector3 = basis[1]
	var lo := INF
	var hi := -INF
	for j in samples:
		var y := lerpf(-radius_m, radius_m, float(j) / float(samples - 1))
		for i in samples:
			var x := lerpf(-radius_m, radius_m, float(i) / float(samples - 1))
			var d := (center + (east * x + north * y) / Planet.cfg.planet_radius).normalized()
			var h := Planet.terrain_height(d)
			lo = minf(lo, h)
			hi = maxf(hi, h)
	return Vector2(lo, hi)
