extends Node3D
## Streaming / performance probe: how many chunks does standing on the ground
## cost, how long does the first fill take, and does moving away release them?

var terrain: PlanetTerrain
var frames := 0
var phase := 0
var t0 := 0
var obs: Vec3D
var _stable := 0
var _last_chunks := -1

func _ready() -> void:
	var cfg := GenConfig.new()
	var fields := PlanetBake.new(cfg).bake(Callable(), true)
	Planet.adopt(fields)
	terrain = PlanetTerrain.new()
	add_child(terrain)
	terrain.build_roots()
	terrain.max_builds_per_frame = 64
	var g := Planet.grid
	var best := -1.0
	var bc := 0
	for c in g.cell_count:
		if fields.elev[c] > 50.0 and fields.elev[c] < 900.0:
			var s: float = fields.corridor[c] * fields.suitability[c]
			if s > best:
				best = s
				bc = c
	var d := g.cell_dir(bc)
	var r := Planet.cfg.planet_radius + Planet.terrain_height(d) + 2.0
	obs = Vec3D.new(d.x * r, d.y * r, d.z * r)
	Frames.rebase(obs)
	terrain.set_observer(obs)
	t0 = Time.get_ticks_msec()
	print("observer at lat %.2f lon %.2f, ground %.1f m" % [
		rad_to_deg(CubeSphere.dir_to_latlon(d).x), rad_to_deg(CubeSphere.dir_to_latlon(d).y),
		Planet.terrain_height(d)])

func _process(_dt: float) -> void:
	frames += 1
	var st := terrain.stats()
	if frames % 30 == 0:
		print("   f%-5d phase %d  chunks %d  nodes %d  in-flight %d  t=%.1fs" % [
			frames, phase, st["chunks"], st["nodes"], st["in_flight"], (Time.get_ticks_msec() - t0) / 1000.0])
	if int(st["in_flight"]) == 0 and int(st["chunks"]) == _last_chunks:
		_stable += 1
	else:
		_stable = 0
	_last_chunks = int(st["chunks"])
	if phase == 0 and _stable > 20 and frames > 30:
		var tris: int = int(st["chunks"]) * (Planet.cfg.chunk_grid * Planet.cfg.chunk_grid * 2 + Planet.cfg.chunk_grid * 4 * 2)
		print("GROUND  settled after %d frames / %.2f s: %d chunks, %d quadtree nodes, ~%d triangles" % [
			frames, (Time.get_ticks_msec() - t0) / 1000.0, st["chunks"], st["nodes"], tris])
		phase = 1
		t0 = Time.get_ticks_msec()
		frames = 0
		_stable = 0
		# Jump to orbit.
		var d := obs.normalized()
		obs = d.mul(Planet.cfg.planet_radius + 900000.0)
		Frames.rebase(obs)
		terrain.set_observer(obs)
	elif phase == 1 and _stable > 20 and frames > 30:
		print("ORBIT   settled after %d frames / %.2f s: %d chunks, %d quadtree nodes" % [
			frames, (Time.get_ticks_msec() - t0) / 1000.0, st["chunks"], st["nodes"]])
		print("rebases: %d" % Frames.rebase_count())
		get_tree().quit()
	elif frames > 3000:
		print("TIMEOUT chunks=%d queued=%d" % [st["chunks"], st["queued"]])
		get_tree().quit(1)
