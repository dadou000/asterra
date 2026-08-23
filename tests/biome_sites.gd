class_name BiomeSites
extends RefCounted
## Finds a representative place to photograph a named biome.
##
## Shared by the one-shot harness (`biome_look.gd`) and the live tuner
## (`biome_tune.gd`) so both aim at the same ground and their images can be
## compared against each other.

static func id_of(biome_name: String) -> int:
	var want := biome_name.to_lower().replace("_", " ")
	for i in PlanetFields.BIOME_NAMES.size():
		var n: String = PlanetFields.BIOME_NAMES[i]
		if n.to_lower() == want:
			return i
	return -1

## The most representative cell of the biome: one whose whole neighbourhood is
## the same biome, so the shot is of that biome rather than of an ecotone.
##
## The baked cell is not enough on its own. Macro smoothing and runtime detail
## both move the surface, so a cell the classifier called forest can sit below
## the runtime waterline -- and a shot of the sea is not a shot of a forest. The
## runtime height is therefore checked directly, and cells with no soil to grow
## anything in are rejected however the classifier labelled them.
static func find(biome_name: String) -> Vector3:
	var id := id_of(biome_name)
	if id < 0:
		print("unknown biome '%s'. Known: %s"
			% [biome_name, ", ".join(PlanetFields.BIOME_NAMES)])
		return Vector3.ZERO
	var f := Planet.fields
	var g := Planet.grid
	var wants_land: bool = not (id in [PlanetFields.Biome.OCEAN,
		PlanetFields.Biome.SHELF_SEA, PlanetFields.Biome.LAKE,
		PlanetFields.Biome.RIVER])
	var best := -1.0
	var best_c := -1
	for c in g.cell_count:
		if f.biome[c] != id:
			continue
		if wants_land and (f.elev[c] < 40.0 or f.vegetation[c] < 0.18):
			continue
		var pure := 0
		for k in 8:
			if f.biome[g.nbr[c * 8 + k]] == id:
				pure += 1
		if pure < 6:
			continue
		var d := g.cell_dir(c)
		if wants_land and Planet.terrain_height(d) < 12.0:
			continue
		# Relief is weighted and floodplain penalised: a dead-flat valley floor is
		# a legitimate part of the biome but tells you nothing about how the
		# material behaves on a slope, which is most of what there is to see.
		var score := float(pure) * 100.0 + f.vegetation[c] * 240.0 \
			+ minf(f.relief[c], 700.0) * 0.55 + f.soil_depth[c] * 12.0 \
			- f.floodplain[c] * 260.0 - f.wetland[c] * 180.0
		if score > best:
			best = score
			best_c = c
	if best_c < 0:
		return Vector3.ZERO
	return g.cell_dir(best_c)
