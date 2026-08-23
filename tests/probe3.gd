extends Node
## What actually reaches the vertex colour attribute, versus what the sampler
## returns. Any difference between the two is a plumbing bug, not a colour choice.
func _ready() -> void:
	var cfg := GenConfig.new()
	var f := PlanetBake.new(cfg).bake(Callable(), true)
	Planet.configure(cfg)
	Planet.adopt(f)
	var g := f.grid
	var id := PlanetFields.Biome.TEMPERATE_FOREST
	var bc := -1
	var best := -1.0
	for c in g.cell_count:
		if f.biome[c] != id or f.elev[c] < 40.0:
			continue
		var pure := 0
		for k in 8:
			if f.biome[g.nbr[c * 8 + k]] == id:
				pure += 1
		if pure < 6:
			continue
		var s := float(pure) * 100.0 + f.vegetation[c] * 240.0 \
			+ minf(f.relief[c], 700.0) * 0.55 + f.soil_depth[c] * 12.0 \
			- f.floodplain[c] * 260.0 - f.wetland[c] * 180.0
		if s > best:
			best = s
			bc = c
	var d := g.cell_dir(bc)
	print("site cell %d biome=%s" % [bc, PlanetFields.BIOME_NAMES[f.biome[bc]]])
	print("  sampler surface_color = %s" % Planet.surface_color(d))

	# Replay _build_cell_colors term by term, so the term actually moving the
	# colour is visible instead of inferred.
	var base: Color = PlanetFields.BIOME_ALBEDO[f.biome[bc]]
	var veg := clampf(f.vegetation[bc], 0.0, 1.0)
	var rockiness := clampf(1.0 - f.soil_depth[bc] / 0.30, 0.0, 1.0)
	rockiness *= 1.0 - clampf(veg * 1.7, 0.0, 1.0)
	var rock_col: Color = PlanetFields.ROCK_COLORS[f.rock[bc]]
	var after_rock := base.lerp(rock_col, rockiness * 0.88)
	var max_accum := 1.0
	for c2 in g.cell_count:
		max_accum = maxf(max_accum, f.flow_accum[c2])
	var drainage := clampf(log(maxf(f.flow_accum[bc], 1.0)) / log(max_accum), 0.0, 1.0)
	var wash := smoothstep(0.55, 0.95, drainage) * (1.0 - clampf(f.precip[bc] / 900.0, 0.0, 1.0))
	var after_wash := after_rock.lerp(Color(0.330, 0.288, 0.205), wash * 0.55)
	var alluvium := f.floodplain[bc] * clampf(f.soil_organic[bc] * 3.4, 0.0, 1.0)
	var after_all := after_wash.lerp(Color(0.075, 0.062, 0.042), alluvium * 0.42)
	print("    base      %s R/G=%.2f" % [base, base.r / base.g])
	print("    rockiness=%.3f soil=%.2fm veg=%.2f -> %s R/G=%.2f"
		% [rockiness, f.soil_depth[bc], veg, after_rock, after_rock.r / after_rock.g])
	print("    wash=%.3f drain=%.2f precip=%.0f -> %s R/G=%.2f"
		% [wash, drainage, f.precip[bc], after_wash, after_wash.r / after_wash.g])
	print("    alluvium=%.3f -> %s R/G=%.2f"
		% [alluvium, after_all, after_all.r / after_all.g])

	# Build the chunk that contains this direction and read its colour array back.
	var fuv := CubeSphere.dir_to_face_uv(d)
	var face: int = fuv[0]
	var detail: TerrainDetail = Planet.make_detail()
	var data: Dictionary = ChunkBuilder.build(face, fuv[1] - 0.01, fuv[2] - 0.01,
		0.02, cfg.chunk_grid, detail, {}, false)
	var cols: PackedColorArray = data["colors"]
	var r := 0.0
	var gg := 0.0
	var b := 0.0
	for c in cols:
		r += c.r; gg += c.g; b += c.b
	var n := float(cols.size())
	print("  mesh vertex colour mean = (%.4f, %.4f, %.4f)  R/G=%.2f  over %d verts"
		% [r / n, gg / n, b / n, r / gg, cols.size()])
	print("  first three = %s %s %s" % [cols[0], cols[1], cols[2]])
	get_tree().quit()
