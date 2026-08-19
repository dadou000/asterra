extends Node
func _ready() -> void:
	var cfg := GenConfig.new()
	var f := PlanetBake.new(cfg).bake(Callable(), true)
	var land := PackedFloat32Array()
	var sea := PackedFloat32Array()
	var lakes := 0
	var ice := 0
	var deep := 0.0
	for c in f.grid.cell_count:
		if f.elev[c] > 0.0: land.append(f.elev[c])
		else: sea.append(f.elev[c])
		if f.lake_level[c] > -1e8: lakes += 1
		if f.biome[c] == PlanetFields.Biome.ICE_CAP: ice += 1
	land.sort(); sea.sort()
	var n := land.size()
	print("land cells %d (%.1f%%)  mean %.0f m" % [n, 100.0*n/f.grid.cell_count, Array(land).reduce(func(a,b): return a+b, 0.0)/n])
	for q in [0.1,0.25,0.5,0.75,0.9,0.99]:
		print("   land p%02d = %7.0f m" % [q*100, land[int(q*(n-1))]])
	print("sea mean %.0f m   p10 %.0f  p50 %.0f" % [Array(sea).reduce(func(a,b): return a+b,0.0)/sea.size(), sea[int(0.1*sea.size())], sea[int(0.5*sea.size())]])
	print("lakes %d (%.1f%% of land)   ice biome %d (%.1f%% of all)" % [lakes, 100.0*lakes/n, ice, 100.0*ice/f.grid.cell_count])
	var lake_depth := PackedFloat32Array()
	for c in f.grid.cell_count:
		if f.lake_level[c] > -1e8: lake_depth.append(f.lake_level[c]-f.elev[c])
	lake_depth.sort()
	if lake_depth.size() > 0:
		print("lake depth p50 %.1f m  p90 %.1f m  max %.1f m" % [lake_depth[lake_depth.size()/2], lake_depth[int(0.9*lake_depth.size())], lake_depth[-1]])
	get_tree().quit()
