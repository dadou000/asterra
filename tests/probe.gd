extends Node
func _ready() -> void:
	var cfg := GenConfig.new()
	var f := PlanetBake.new(cfg).bake(Callable(), true)
	Planet.configure(cfg)
	Planet.adopt(f)
	var g := f.grid
	var id := PlanetFields.Biome.TEMPERATE_FOREST
	var best := -1.0
	var bc := -1
	for c in g.cell_count:
		if f.biome[c] != id or f.elev[c] < 40.0:
			continue
		var pure := 0
		for k in 8:
			if f.biome[g.nbr[c * 8 + k]] == id:
				pure += 1
		if pure < 6:
			continue
		if Planet.terrain_height(g.cell_dir(c)) < 12.0:
			continue
		var s := float(pure) * 100.0 + f.vegetation[c] * 240.0 			+ minf(f.relief[c], 600.0) * 0.20 + f.soil_depth[c] * 30.0
		if s > best:
			best = s
			bc = c
	var d := g.cell_dir(bc)
	print("cell %d  biome=%s" % [bc, PlanetFields.BIOME_NAMES[f.biome[bc]]])
	print("  BIOME_ALBEDO   = %s" % PlanetFields.BIOME_ALBEDO[f.biome[bc]])
	print("  _cell_color    = %s" % Planet.surface_color(d))
	print("  surface (v,w,sand,frost) = %s" % Planet.surface_composition(d))
	print("  geomorph       = %s" % Planet.geomorph_composition(d))
	print("  soil sand=%.2f silt=%.2f clay=%.2f org=%.3f moist=%.2f depth=%.2f"
		% [f.soil_sand[bc], f.soil_silt[bc], f.soil_clay[bc], f.soil_organic[bc],
		f.soil_moisture[bc], f.soil_depth[bc]])
	print("  veg=%.2f wetland=%.2f floodplain=%.2f precip=%.0f temp=%.1f"
		% [f.vegetation[bc], f.wetland[bc], f.floodplain[bc], f.precip[bc], f.temp_mean[bc]])
	print("  elev=%.0f relief=%.0f rock=%s" % [f.elev[bc], f.relief[bc],
		PlanetFields.ROCK_NAMES[f.rock[bc]]])
	# What the renderer actually samples, over the ground the camera sees. The
	# lookup is domain-warped by several kilometres, so the colour at a point can
	# come from a different cell -- and a different biome -- than the one the
	# classifier assigned there.
	var basis := CubeSphere.tangent_basis(d)
	var east: Vector3 = basis[0]
	var north: Vector3 = basis[1]
	var rsum := 0.0
	var gsum := 0.0
	var bsum := 0.0
	var n := 0
	var biomes := {}
	for iy in 21:
		for ix in 21:
			var off := (east * (float(ix) - 10.0) + north * (float(iy) - 10.0)) * 200.0
			var p2 := (d * Planet.cfg.planet_radius + off).normalized()
			var col: Color = Planet.surface_color(p2)
			rsum += col.r; gsum += col.g; bsum += col.b; n += 1
			var bi: int = f.biome[g.dir_to_index(p2)]
			biomes[bi] = biomes.get(bi, 0) + 1
	print("  sampled colour over +/-2 km: (%.4f, %.4f, %.4f)  R/G=%.2f B/G=%.2f"
		% [rsum / n, gsum / n, bsum / n, rsum / gsum, bsum / gsum])
	var parts := []
	for k in biomes:
		parts.append("%s:%d" % [PlanetFields.BIOME_NAMES[k], biomes[k]])
	print("  biomes under the camera: %s" % ", ".join(parts))
	get_tree().quit()
