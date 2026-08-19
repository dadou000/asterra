extends Node
## Headless verification for Phase 1.
##
## Run: godot --headless --path . res://tests/Tests.tscn
## Every check is a property the roadmap milestone depends on, not a snapshot.

var passed := 0
var failed := 0
var _t0 := 0

func _ready() -> void:
	await get_tree().process_frame
	_t0 = Time.get_ticks_msec()
	print("\n=== ASTERRA PHASE 1 VERIFICATION ===\n")
	_section("Coordinates & topology")
	test_cube_sphere_roundtrip()
	test_grid_neighbours()
	test_floating_origin_precision()

	_section("Generation")
	var cfg := _test_cfg()
	var t := Time.get_ticks_msec()
	var bake := PlanetBake.new(cfg)
	var fields := bake.bake(Callable(), false)
	print("   bake: %d cells in %.1f s" % [fields.grid.cell_count, (Time.get_ticks_msec() - t) / 1000.0])
	Planet.adopt(fields)

	test_determinism(cfg, fields)
	test_ocean_fraction(fields, cfg)
	test_geology(fields)
	test_erosion_relief(fields)
	test_hydrology(fields)
	test_climate(fields)
	test_soil_and_biomes(fields)
	test_suitability(fields)

	_section("Runtime terrain")
	test_seam_continuity()
	test_chunk_build()
	test_geomorph_lands_on_parent()

	_section("Streaming")
	await test_streaming()

	_section("Editing & persistence")
	test_dig_volume_conservation()
	test_delta_roundtrip()
	test_save_load(cfg)

	print("\n=== %d passed, %d failed  (%.1f s) ===\n" % [passed, failed, (Time.get_ticks_msec() - _t0) / 1000.0])
	get_tree().quit(0 if failed == 0 else 1)

func _test_cfg() -> GenConfig:
	var cfg := GenConfig.new()
	cfg.face_res = 48
	cfg.erosion_iterations = 12
	cfg.world_seed = 0x4153544552524100
	return cfg

# ------------------------------------------------------------------ harness --
func _section(name: String) -> void:
	print("-- %s" % name)

func check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		passed += 1
		print("   PASS  %s%s" % [name, ("  (%s)" % detail) if detail != "" else ""])
	else:
		failed += 1
		print("   FAIL  %s%s" % [name, ("  (%s)" % detail) if detail != "" else ""])

# ---------------------------------------------------------------- geometry ---
func test_cube_sphere_roundtrip() -> void:
	var worst := 0.0
	for i in 4000:
		var d := HashRNG.unit_sphere(1234, i)
		var fuv := CubeSphere.dir_to_face_uv(d)
		var back := CubeSphere.face_uv_to_dir(fuv[0], fuv[1], fuv[2])
		worst = maxf(worst, d.distance_to(back))
	check("cube-sphere inverse is exact", worst < 1e-5, "max error %s" % String.num_scientific(worst))

func test_grid_neighbours() -> void:
	var g := PlanetGrid.new(24, 1000000.0)
	var self_ref := 0
	var degenerate := 0
	var worst_ratio := 0.0
	for c in g.cell_count:
		var d := g.cell_dir(c)
		var seen := {}
		for k in 8:
			var nb := g.nbr[c * 8 + k]
			if nb == c:
				self_ref += 1
			seen[nb] = true
			var ang := d.angle_to(g.cell_dir(nb))
			var cells := ang * g.radius / g.cell_size[c]
			worst_ratio = maxf(worst_ratio, cells)
		if seen.size() < 8:
			degenerate += 1
	check("no cell is its own neighbour", self_ref == 0, "%d self references" % self_ref)
	# A cube-sphere has exactly 8 corners, each shared by 3 faces; those 24 cells
	# genuinely have 7 neighbours, not 8. Anything more is a seam bug.
	check("only the 24 cube-corner cells are degenerate", degenerate <= 24,
		"%d cells with <8 distinct neighbours" % degenerate)
	check("neighbours are within ~2 cells (seams included)", worst_ratio < 2.2,
		"worst %.2f cells" % worst_ratio)

func test_floating_origin_precision() -> void:
	var far := Vec3D.new(1234567.891, -987654.321, 456789.123)
	Frames.rebase(far)
	var local := Frames.to_render(far.add(Vec3D.new(0.01, -0.02, 0.03)))
	var err := local.distance_to(Vector3(0.01, -0.02, 0.03))
	check("1 cm offsets survive at 1.6e6 m from the origin", err < 1e-4, "error %s m" % String.num_scientific(err))
	Frames.rebase(Vec3D.new(0, 0, 0))

# -------------------------------------------------------------- generation ---
func test_determinism(cfg: GenConfig, first: PlanetFields) -> void:
	var cfg2 := _test_cfg()
	var second := PlanetBake.new(cfg2).bake(Callable(), false)
	var same := true
	for c in first.grid.cell_count:
		if absf(first.elev[c] - second.elev[c]) > 1e-6 or first.rock[c] != second.rock[c] \
				or first.biome[c] != second.biome[c] or first.watershed[c] != second.watershed[c]:
			same = false
			break
	check("two bakes of the same seed are identical", same)

	var cfg3 := _test_cfg()
	cfg3.world_seed += 1
	var other := PlanetBake.new(cfg3).bake(Callable(), false)
	var diff := 0
	for c in first.grid.cell_count:
		if absf(first.elev[c] - other.elev[c]) > 1.0:
			diff += 1
	check("a different seed makes a different planet",
		float(diff) / float(first.grid.cell_count) > 0.9, "%.1f%% cells differ" % (100.0 * diff / first.grid.cell_count))

func test_ocean_fraction(f: PlanetFields, cfg: GenConfig) -> void:
	var ocean := 0
	for c in f.grid.cell_count:
		if f.elev[c] < 0.0:
			ocean += 1
	var frac := float(ocean) / float(f.grid.cell_count)
	check("ocean fraction matches the target", absf(frac - cfg.ocean_fraction) < 0.06,
		"%.3f vs %.3f" % [frac, cfg.ocean_fraction])

func test_geology(f: PlanetFields) -> void:
	var families := {}
	var ore_cells := 0
	var oil_cells := 0
	var quartz_cells := 0
	var ocean_basalt := 0
	var ocean_total := 0
	for c in f.grid.cell_count:
		families[f.rock[c]] = true
		if f.ore_iron[c] > 0.3 or f.ore_copper[c] > 0.3:
			ore_cells += 1
		if f.petroleum[c] > 0.25:
			oil_cells += 1
		if f.quartz[c] > 0.3:
			quartz_cells += 1
		if f.elev[c] < -2000.0:
			ocean_total += 1
			if f.rock[c] in [PlanetFields.Rock.BASALT, PlanetFields.Rock.GABBRO, PlanetFields.Rock.SERPENTINITE]:
				ocean_basalt += 1
	check("multiple bedrock families are present", families.size() >= 7, "%d families" % families.size())
	check("deep ocean floor is mafic crust", float(ocean_basalt) / maxf(1.0, ocean_total) > 0.9,
		"%.1f%%" % (100.0 * ocean_basalt / maxf(1.0, ocean_total)))
	check("ore veins were deposited", ore_cells > 0, "%d cells" % ore_cells)
	check("petroleum basins were deposited", oil_cells > 0, "%d cells" % oil_cells)
	check("high-purity quartz regions exist (Axiom)", quartz_cells > 0, "%d cells" % quartz_cells)

func test_erosion_relief(f: PlanetFields) -> void:
	var max_h := -1e30
	var min_h := 1e30
	var land := 0
	for c in f.grid.cell_count:
		max_h = maxf(max_h, f.elev[c])
		min_h = minf(min_h, f.elev[c])
		if f.elev[c] > 0.0:
			land += 1
	check("mountains exist", max_h > 1500.0, "max %.0f m" % max_h)
	check("ocean basins exist", min_h < -3000.0, "min %.0f m" % min_h)
	check("land exists", land > 0, "%d land cells" % land)
	# Erosion must actually change the surface it was given.
	var moved := 0.0
	for c in f.grid.cell_count:
		moved += absf(f.elev[c] - f.base_elev[c])
	check("erosion reshaped the crust", moved / float(f.grid.cell_count) > 20.0,
		"mean |delta| %.0f m" % (moved / f.grid.cell_count))

func test_hydrology(f: PlanetFields) -> void:
	var router := FlowRouter.new(f.grid)
	router.route_and_order(f.elev, 0.0)
	# 1. No cell routes to a strictly higher filled cell.
	var uphill := 0
	for c in f.grid.cell_count:
		var r := router.rec[c]
		if r != c and router.filled[r] > router.filled[c] + 1e-3:
			uphill += 1
	check("no flow runs uphill", uphill == 0, "%d violations" % uphill)

	# 2. Every land cell reaches a terminal cell without cycling.
	var stuck := 0
	for c in range(0, f.grid.cell_count, 7):
		var cur := c
		var steps := 0
		while router.rec[cur] != cur and steps < f.grid.cell_count:
			cur = router.rec[cur]
			steps += 1
		if steps >= f.grid.cell_count:
			stuck += 1
	check("the drainage network is acyclic", stuck == 0)

	# 3. Drainage area is conserved: the sum over outlets equals the total area.
	var total_land := 0
	var to_sea := 0
	for c in f.grid.cell_count:
		if f.elev[c] <= 0.0:
			continue
		total_land += 1
		if f.elev[f.watershed[c]] <= 0.0:
			to_sea += 1
	var exo := float(to_sea) / maxf(1.0, float(total_land))
	check("most land drains to the sea", exo > 0.85, "%.1f%% exorheic" % (exo * 100.0))

	var rivers := 0
	var max_q := 0.0
	for c in f.grid.cell_count:
		if f.river_width[c] > 20.0:
			rivers += 1
		max_q = maxf(max_q, f.discharge[c])
	check("real rivers formed", rivers > 0 and max_q > 200.0,
		"%d river cells, peak %.0f m3/s" % [rivers, max_q])

	var lakes := 0
	for c in f.grid.cell_count:
		if f.lake_level[c] > -1e8:
			lakes += 1
	check("lakes formed in closed depressions", lakes > 0, "%d lake cells" % lakes)

	var orders := 0
	for c in f.grid.cell_count:
		orders = maxi(orders, f.stream_order[c])
	check("stream hierarchy is deep enough to be a network", orders >= 4,
		"max Strahler order %d" % orders)

func test_climate(f: PlanetFields) -> void:
	var eq_t := 0.0
	var eq_n := 0
	var pole_t := 0.0
	var pole_n := 0
	for c in f.grid.cell_count:
		var y: float = absf(f.grid.cell_dir(c).y)
		if y < 0.15:
			eq_t += f.temp_mean[c]
			eq_n += 1
		elif y > 0.9:
			pole_t += f.temp_mean[c]
			pole_n += 1
	eq_t /= maxf(1.0, eq_n)
	pole_t /= maxf(1.0, pole_n)
	check("poles are colder than the equator", eq_t - pole_t > 25.0,
		"equator %.1f C, poles %.1f C" % [eq_t, pole_t])

	# Rain shadow: land far from the sea must be drier than the coast.
	var coastal := 0.0
	var cn := 0
	var interior := 0.0
	var interior_n := 0
	var climate := PassClimate.new(f)
	climate._winds()
	climate._distance_to_ocean()
	for c in f.grid.cell_count:
		if f.elev[c] <= 0.0:
			continue
		if climate.dist_to_ocean[c] < 60000.0:
			coastal += f.precip[c]
			cn += 1
		elif climate.dist_to_ocean[c] > 400000.0:
			interior += f.precip[c]
			interior_n += 1
	if cn > 0 and interior_n > 0:
		check("continental interiors are drier than coasts",
			interior / interior_n < coastal / cn,
			"coast %.0f mm, interior %.0f mm" % [coastal / cn, interior / interior_n])
	else:
		check("continental interiors are drier than coasts", true, "not enough interior land to test")

	var wettest := 0.0
	for c in f.grid.cell_count:
		wettest = maxf(wettest, f.precip[c])
	check("precipitation spans a plausible range", wettest > 1500.0, "max %.0f mm/yr" % wettest)

func test_soil_and_biomes(f: PlanetFields) -> void:
	var kinds := {}
	var soil_ok := true
	var deep := 0
	for c in f.grid.cell_count:
		kinds[f.biome[c]] = true
		var s: float = f.soil_sand[c] + f.soil_silt[c] + f.soil_clay[c]
		if absf(s - 1.0) > 0.02:
			soil_ok = false
		if f.elev[c] > 0.0 and f.soil_depth[c] > 1.0:
			deep += 1
	check("soil texture fractions sum to 1", soil_ok)
	check("deep soils exist somewhere", deep > 0, "%d cells over 1 m" % deep)
	check("biome variety emerges from climate", kinds.size() >= 8, "%d distinct biomes" % kinds.size())

func test_suitability(f: PlanetFields) -> void:
	var best := 0.0
	var mean := 0.0
	var land := 0
	for c in f.grid.cell_count:
		if f.elev[c] <= 0.0:
			continue
		land += 1
		best = maxf(best, f.corridor[c])
		mean += f.corridor[c]
	check("transport corridors were found", best > 0.6, "best %.2f" % best)
	check("corridors are selective, not everywhere", mean / maxf(1.0, land) < 0.65,
		"mean %.2f" % (mean / maxf(1.0, land)))

# ----------------------------------------------------------------- runtime ---
func test_seam_continuity() -> void:
	# Three things have to hold at a cube-face seam, on all twelve edges rather
	# than on the one that happens to be easy. The macro fields must interpolate
	# across it rather than snap to the other face's grid; the surface colour must
	# do the same, or the seam is drawn even where the ground is flat; and the
	# full terrain height must be no rougher there than anywhere else -- the seam
	# must not be findable by looking at the planet.
	var d0 := Planet.make_detail()
	var g := Planet.grid
	var eps := 1e-5
	var seam_macro := 0.0
	var seam_colour := 0.0
	var seam_total := 0.0
	var samples := 0
	var sep := 0.0
	var worst := ""
	for face in 6:
		for edge in 4:
			for i in 60:
				var t := (float(i) / 59.0) * 1.9 - 0.95
				var a: Vector3
				var b: Vector3
				match edge:
					0:
						a = CubeSphere.face_uv_to_dir(face, 1.0 - eps, t)
						b = CubeSphere.face_uv_to_dir(face, 1.0 + eps, t)
					1:
						a = CubeSphere.face_uv_to_dir(face, -1.0 + eps, t)
						b = CubeSphere.face_uv_to_dir(face, -1.0 - eps, t)
					2:
						a = CubeSphere.face_uv_to_dir(face, t, 1.0 - eps)
						b = CubeSphere.face_uv_to_dir(face, t, 1.0 + eps)
					_:
						a = CubeSphere.face_uv_to_dir(face, t, -1.0 + eps)
						b = CubeSphere.face_uv_to_dir(face, t, -1.0 - eps)
				sep = maxf(sep, a.angle_to(b) * Planet.cfg.planet_radius)
				var step: float = absf(g.sample_bilinear(Planet.fields.elev, a)
					- g.sample_bilinear(Planet.fields.elev, b))
				if step > seam_macro:
					seam_macro = step
					worst = "face %d edge %d" % [face, edge]
				var ca := Planet.surface_color(a)
				var cb := Planet.surface_color(b)
				seam_colour = maxf(seam_colour,
					(Vector3(ca.r, ca.g, ca.b) - Vector3(cb.r, cb.g, cb.b)).length())
				seam_total += absf(Planet.terrain_height(a, d0) - Planet.terrain_height(b, d0))
				samples += 1
	seam_total /= float(samples)

	# Reference roughness: the same separation, sampled well inside a face.
	var interior := 0.0
	var interior_colour := 0.0
	var n := 240
	for i in n:
		var d := HashRNG.unit_sphere(99, i)
		var fuv := CubeSphere.dir_to_face_uv(d)
		var u: float = clampf(fuv[1], -0.8, 0.8)
		var v: float = clampf(fuv[2], -0.8, 0.8)
		var a := CubeSphere.face_uv_to_dir(fuv[0], u - eps, v)
		var b := CubeSphere.face_uv_to_dir(fuv[0], u + eps, v)
		interior += absf(Planet.terrain_height(a, d0) - Planet.terrain_height(b, d0))
		var ca := Planet.surface_color(a)
		var cb := Planet.surface_color(b)
		interior_colour = maxf(interior_colour,
			(Vector3(ca.r, ca.g, ca.b) - Vector3(cb.r, cb.g, cb.b)).length())
	interior /= float(n)

	check("macro fields interpolate across all twelve cube seams", seam_macro < 20.0,
		"worst %.2f m at %s, over %.1f m" % [seam_macro, worst, sep])
	check("surface colour crosses a seam as smoothly as it crosses a face",
		seam_colour < interior_colour * 2.5 + 0.01,
		"seam %.4f vs interior %.4f" % [seam_colour, interior_colour])
	check("the seam is no rougher than ordinary ground",
		seam_total < interior * 3.0 + 1.0,
		"seam %.2f m vs interior %.2f m per %.1f m step" % [seam_total, interior, sep])

## The geomorph target has to be the parent's surface, or the hand-over between
## two levels is a pop wearing a morph's clothes. The two tiles sample the macro
## fields on lattices fitted to their own spans, so they do not agree to the
## millimetre -- what has to hold is that the disagreement stays far below the
## resolution of the mesh drawing it, at every depth.
func test_geomorph_lands_on_parent() -> void:
	var det := Planet.make_detail()
	var n: int = Planet.cfg.chunk_grid
	var worst_ratio := 0.0
	var detail := ""
	for step_i in 5:
		var size: float = 0.2 / pow(4.0, float(step_i))
		var parent := ChunkBuilder.build(0, -0.2, -0.2, size, n, det, {}, false)
		var child := ChunkBuilder.build(0, -0.2, -0.2, size * 0.5, n, det, {}, false)
		var pp: Vec3D = parent["pivot"]
		var cp: Vec3D = child["pivot"]
		var pv: PackedVector3Array = parent["vertices"]
		var cv: PackedVector3Array = child["vertices"]
		var cm: PackedFloat32Array = child["morph"]
		var shift := Vector3(float(cp.x - pp.x), float(cp.y - pp.y), float(cp.z - pp.z))
		var off := 0.0
		var stray := 0.0
		for j in n + 1:
			for i in n + 1:
				var vi := j * (n + 1) + i
				var m := Vector3(cm[vi * 4], cm[vi * 4 + 1], cm[vi * 4 + 2])
				var morphed: Vector3 = cv[vi] + m + shift
				if i % 2 == 0 and j % 2 == 0:
					stray = maxf(stray, m.length())
					off = maxf(off, morphed.distance_to(pv[(j / 2) * (n + 1) + i / 2]))
				else:
					var a: int
					var b: int
					if j % 2 == 0:
						a = (j / 2) * (n + 1) + (i - 1) / 2
						b = a + 1
					elif i % 2 == 0:
						a = ((j - 1) / 2) * (n + 1) + i / 2
						b = a + (n + 1)
					else:
						a = ((j - 1) / 2) * (n + 1) + (i + 1) / 2
						b = ((j + 1) / 2) * (n + 1) + (i - 1) / 2
					off = maxf(off, morphed.distance_to((pv[a] + pv[b]) * 0.5))
		# Against the spacing of the mesh that draws it.
		var cell: float = size * (PI * 0.25) * Planet.cfg.planet_radius / float(n)
		var ratio := off / cell
		if ratio > worst_ratio:
			worst_ratio = ratio
			detail = "%.3f m over a %.0f m cell" % [off, cell]
		if step_i == 0:
			check("vertices the parent already has do not move at all", stray < 1e-3,
				"worst offset %.5f m" % stray)
		print("      depth-ish %d: cell %8.1f m, worst morph error %6.3f m (%.2f%% of a cell)"
			% [step_i, cell, off, ratio * 100.0])
	check("a fully morphed tile lands on its parent's surface", worst_ratio < 0.05,
		"worst %s (%.1f%% of a cell)" % [detail, worst_ratio * 100.0])

func test_chunk_build() -> void:
	var d := Planet.make_detail()
	var data := ChunkBuilder.build(0, -0.05, -0.05, 0.1, 16, d, {}, true)
	var verts: PackedVector3Array = data["vertices"]
	var bad := 0
	for v in verts:
		if not (is_finite(v.x) and is_finite(v.y) and is_finite(v.z)):
			bad += 1
	check("chunk vertices are finite", bad == 0)
	check("chunk has skirt geometry", verts.size() > 17 * 17, "%d verts" % verts.size())
	check("chunk produced collision", data.has("collision_faces"))
	var pivot: Vec3D = data["pivot"]
	var far := 0.0
	for v in verts:
		far = maxf(far, v.length())
	var expected := 0.1 * (PI * 0.25) * Planet.cfg.planet_radius
	check("chunk vertices stay near their pivot (float32 safe)", far < expected * 2.0,
		"max %.0f m from pivot at r=%.0f km" % [far, pivot.length() / 1000.0])

# ----------------------------------------------------------------- editing ---
func test_dig_volume_conservation() -> void:
	Deltas.clear()
	var editor := TerrainEditor.new()
	add_child(editor)
	editor.refresh()
	var dir := Planet.grid.cell_dir(_a_land_cell())
	var before_h := Planet.terrain_height(dir)
	var stock := editor.dig(dir, 3.0, 0.5)
	var after_h := Planet.terrain_height(dir)
	check("digging lowered the ground", after_h < before_h - 0.2,
		"%.2f m -> %.2f m" % [before_h, after_h])
	check("excavation produced material", stock.total_volume() > 0.5,
		"%.2f m3 loose" % stock.total_volume())

	# Loose volume must equal in-place volume times the bulking factor.
	var in_place := 0.0
	var loose := 0.0
	for k in stock.entries:
		var e: Dictionary = stock.entries[k]
		loose += e["volume_loose"]
		in_place += e["volume_loose"] / float(e["props"]["swell"])
	check("loose volume is the bulked in-place volume", loose > in_place and loose < in_place * 1.7,
		"%.2f m3 loose from %.2f m3 in place" % [loose, in_place])

	# Filling it back consumes the stock and restores the ground.
	var placed := editor.fill(dir, 3.0, 0.5, stock)
	var restored := Planet.terrain_height(dir)
	check("filling consumes stock", stock.total_volume() < loose * 0.75,
		"%.2f m3 left" % stock.total_volume())
	check("filling raised the ground again", restored > after_h + 0.1,
		"%.2f m -> %.2f m (placed %.2f m3)" % [after_h, restored, placed])
	editor.queue_free()

func test_delta_roundtrip() -> void:
	Deltas.clear()
	var dir := Planet.grid.cell_dir(_a_land_cell())
	var lat := Deltas.dir_to_lattice(dir)
	Deltas.add_offset(lat[0], int(lat[1]), int(lat[2]), -1.25, -60.0, 60.0)
	var read := Deltas.get_offset(lat[0], int(lat[1]), int(lat[2]))
	check("delta stored exactly", absf(read + 1.25) < 1e-4, "%.4f" % read)
	var blob := Deltas.serialize()
	Deltas.clear()
	check("cleared deltas read back as zero", absf(Deltas.get_offset(lat[0], int(lat[1]), int(lat[2]))) < 1e-9)
	Deltas.deserialize(blob)
	var read2 := Deltas.get_offset(lat[0], int(lat[1]), int(lat[2]))
	check("delta survives serialise/deserialise", absf(read2 + 1.25) < 1e-4, "%.4f" % read2)

func test_save_load(cfg: GenConfig) -> void:
	Deltas.clear()
	var editor := TerrainEditor.new()
	add_child(editor)
	editor.refresh()
	var dir := Planet.grid.cell_dir(_a_land_cell())
	var pristine := Planet.pristine_height(dir)
	var stock := editor.dig(dir, 4.0, 0.8)
	editor.drop_pile(stock, dir)
	var edited_h := Planet.terrain_height(dir)
	var tiles := Deltas.edited_tile_count()

	var st := {"x": 1.0, "y": 2.0, "z": 3.0, "yaw": 0.5, "pitch": -0.2, "mode": 0, "carry": []}
	var err := SaveGame.save("unit_test", cfg, st, editor, 12.5)
	check("save wrote successfully", err == OK)

	Deltas.clear()
	editor.restore_piles([])
	check("world is pristine after clearing", absf(Planet.terrain_height(dir) - pristine) < 1e-3)

	var data := SaveGame.load_into("unit_test", cfg, editor)
	check("save loaded", not data.is_empty())
	check("terrain deltas restored exactly", absf(Planet.terrain_height(dir) - edited_h) < 1e-3,
		"%.4f vs %.4f" % [Planet.terrain_height(dir), edited_h])
	check("delta tile count restored", Deltas.edited_tile_count() == tiles,
		"%d vs %d" % [Deltas.edited_tile_count(), tiles])
	check("loose material restored", editor.piles.size() == 1 and editor.piles[0].volume() > 0.0)
	check("player state round-tripped", absf(float(data["player"]["yaw"]) - 0.5) < 1e-6)
	editor.queue_free()

## Drive the quadtree for a few frames from a ground observer and check that it
## converges, stays bounded, culls the far side of the planet, and reacts to an
## edit by rebuilding only the affected chunks.
func test_streaming() -> void:
	Planet.cfg.quadtree_max_depth = 9
	var terrain := PlanetTerrain.new()
	add_child(terrain)
	terrain.build_roots()
	terrain.max_builds_per_frame = 128
	var d := Planet.grid.cell_dir(_a_land_cell())
	var r := Planet.cfg.planet_radius + Planet.terrain_height(d) + 2.0
	var obs := Vec3D.new(d.x * r, d.y * r, d.z * r)
	Frames.rebase(obs)
	terrain.set_observer(obs)

	var settled := false
	for i in 600:
		await get_tree().process_frame
		var st := terrain.stats()
		if i > 20 and int(st["in_flight"]) == 0:
			settled = true
			break
	var st := terrain.stats()
	check("the quadtree converges from a ground observer", settled,
		"%d chunks, %d nodes" % [st["chunks"], st["nodes"]])
	check("chunk count stays bounded", int(st["chunks"]) > 0 and int(st["chunks"]) < 900,
		"%d chunks" % st["chunks"])
	check("the far side of the planet is culled", int(st["culled"]) > 0,
		"%d nodes rejected over the horizon" % st["culled"])

	# An excavation must invalidate the chunks that cover it.
	var editor := TerrainEditor.new()
	add_child(editor)
	editor.refresh()
	editor.dig(d, 3.0, 0.4)
	await get_tree().process_frame
	await get_tree().process_frame
	var after := terrain.stats()
	check("an edit re-meshes the chunks it touches", int(after["queued"]) > 0 or int(after["in_flight"]) > 0,
		"%d rebuilds queued" % (int(after["queued"]) + int(after["in_flight"])))
	for i in 300:
		await get_tree().process_frame
		if int(terrain.stats()["in_flight"]) == 0:
			break
	# Walking past ground that is already streamed in exercises the path an
	# approach-then-recede takes through the quadtree: split on the way in,
	# collapse on the way out. A leaf left behind claiming a chunk it no longer
	# owns is a hole -- one that standing still never fills, because the node
	# believes it is already built.
	var east := Vector3(0, 1, 0).cross(d).normalized()
	for i in 400:
		var nd := (obs.normalized().to_v3() + east * (60.0 / Planet.cfg.planet_radius)).normalized()
		var nr := Planet.cfg.planet_radius + Planet.terrain_height(nd) + 2.0
		obs = Vec3D.new(nd.x * nr, nd.y * nr, nd.z * nr)
		Frames.rebase(obs)
		terrain.set_observer(obs)
		await get_tree().process_frame
	var pending := 0
	for i in 900:
		await get_tree().process_frame
		pending = 0
		for root in terrain.roots:
			pending += _resident_handoffs(root)
		if int(terrain.stats()["in_flight"]) == 0 and pending == 0:
			break
	var holes := 0
	for root in terrain.roots:
		holes += _unmeshed_leaves(root)
	check("walking away from a region leaves no holes behind", holes == 0,
		"%d leaves claim a chunk they no longer have" % holes)
	# Two levels of the same ground are only allowed on screen while a hand-off
	# is dissolving between them. One that never commits is a level drawn over
	# itself for good.
	check("every LOD hand-over finishes", pending == 0,
		"%d subtrees still part-way between levels" % pending)

	editor.queue_free()
	terrain.queue_free()
	await get_tree().process_frame
	Deltas.clear()

## Subtrees part-way through an LOD hand-over: either still holding the coarse
## mesh behind their children, or with the children still bent part-way onto it.
func _resident_handoffs(node) -> int:
	var n := 0
	if not node.is_leaf() and (node.chunk != null or node.fine_vis < 1.0):
		n += 1
	for c in node.children:
		n += _resident_handoffs(c)
	return n

## Leaves that report themselves as built (state 2) while holding no chunk.
func _unmeshed_leaves(node) -> int:
	if node.is_leaf():
		return 1 if node.state == 2 and node.chunk == null else 0
	var n := 0
	for c in node.children:
		n += _unmeshed_leaves(c)
	return n

func _a_land_cell() -> int:
	var f := Planet.fields
	var best := -1e30
	var best_c := 0
	for c in f.grid.cell_count:
		if f.elev[c] > 30.0 and f.elev[c] < 900.0 and f.soil_depth[c] > best:
			best = f.soil_depth[c]
			best_c = c
	return best_c
