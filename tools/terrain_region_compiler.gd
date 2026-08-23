extends SceneTree
## Headless regional terrain cache compiler.
##
## This is the first offline-world-compiler entry point. It deliberately writes
## the exact same GroundHeightStore .ghz format used by runtime, so no renderer or
## streamer special case is required for precompiled regions.
##
## Example:
##   godot --headless --path . --script res://tools/terrain_region_compiler.gd -- \
##     --lat=34.5 --lon=-96.0 --radius-m=500 --finest-level=0
##
## `finest-level` follows GroundHeightStore:
##   0 ~0.75 m, 1 ~1.5 m, 2 ~3 m, ... 6 ~48 m.
##
## The current .ghz-per-tile format is intentionally a migration format. This
## compiler proves the offline/runtime split before the later packed residual
## archive format replaces millions of small files.

const TARGET_FINE_DEPTH := 16
const TILE_CELLS := 32
const MAX_LEVEL := 6

var _args: Dictionary = {}


func _init() -> void:
	_args = _parse_args(OS.get_cmdline_user_args())
	call_deferred("_run")


func _run() -> void:
	if bool(_args.get("help", false)):
		_print_help()
		quit(0)
		return

	var cfg_resource := load("res://world.tres")
	if not (cfg_resource is GenConfig):
		printerr("terrain compiler: res://world.tres is not a GenConfig")
		quit(2)
		return
	var cfg: GenConfig = cfg_resource
	Planet.configure(cfg)

	var use_world_cache := not bool(_args.get("rebake_world", false))
	print("terrain compiler: preparing world seed %d" % cfg.world_seed)
	var bake := PlanetBake.new(cfg)
	var fields := bake.bake(Callable(), use_world_cache)
	Planet.adopt(fields)

	var lat_deg := float(_args.get("lat", 0.0))
	var lon_deg := float(_args.get("lon", 0.0))
	var radius_m := maxf(float(_args.get("radius_m", 250.0)), 1.0)
	var finest_level := clampi(int(_args.get("finest_level", 0)), 0, MAX_LEVEL)
	var center := _lat_lon_dir(lat_deg, lon_deg)

	print("terrain compiler: center %.5f°, %.5f°  radius %.0f m  levels %d..%d" % [
		lat_deg, lon_deg, radius_m, finest_level, MAX_LEVEL])
	_compile_region(center, radius_m, finest_level, cfg)

	var st := GroundHeightStore.stats()
	print("terrain compiler: done  RAM=%d disk_hits=%d newly_baked=%d" % [
		int(st["memory_tiles"]), int(st["disk_hits"]), int(st["tiles_built"])])
	print("terrain compiler: output is under the project user://terrain_height_cache directory")
	quit(0)


func _compile_region(center: Vector3, radius_m: float, finest_level: int,
		cfg: GenConfig) -> void:
	var tangent := CubeSphere.tangent_basis(center)
	var east: Vector3 = tangent[0]
	var north: Vector3 = tangent[1]
	var base_spacing := PI * 0.5 * cfg.planet_radius \
		/ (float(cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))

	# Coarse-to-fine makes an interrupted compile immediately useful to runtime.
	for level in range(MAX_LEVEL, finest_level - 1, -1):
		var spacing := base_spacing * pow(2.0, float(level))
		var tile_span := spacing * float(TILE_CELLS)
		# Half-tile probe spacing deliberately overlaps coverage. sample_pristine()
		# bilinearly touches neighbouring tiles at boundaries, so this conservative
		# grid is much simpler and safer than duplicating private cache addressing
		# rules in the compiler.
		var step := maxf(tile_span * 0.5, spacing)
		var reach := int(ceil((radius_m + tile_span) / step))
		var probes: Array[Vector2] = []
		for gy in range(-reach, reach + 1):
			for gx in range(-reach, reach + 1):
				var plane := Vector2(float(gx) * step, float(gy) * step)
				if plane.length() > radius_m + tile_span * 0.75:
					continue
				probes.append(plane)

		print("  L%d  spacing %.3f m  tile %.1f m  probes %d" % [
			level, spacing, tile_span, probes.size()])
		var last_percent := -1
		for i in probes.size():
			var plane := probes[i]
			var d := (center + east * (plane.x / cfg.planet_radius)
				+ north * (plane.y / cfg.planet_radius)).normalized()
			# Blocking is intentional here: this is the offline compiler.
			GroundHeightStore.sample_pristine(d, level)
			var percent := int(floor(100.0 * float(i + 1) / maxf(float(probes.size()), 1.0)))
			if percent >= last_percent + 10:
				last_percent = percent
				print("    %d%%" % percent)


static func _lat_lon_dir(lat_deg: float, lon_deg: float) -> Vector3:
	var lat := deg_to_rad(lat_deg)
	var lon := deg_to_rad(lon_deg)
	var c := cos(lat)
	return Vector3(c * cos(lon), sin(lat), c * sin(lon)).normalized()


static func _parse_args(raw: PackedStringArray) -> Dictionary:
	var out := {}
	for arg in raw:
		if arg == "--help" or arg == "-h":
			out["help"] = true
		elif arg == "--rebake-world":
			out["rebake_world"] = true
		elif arg.begins_with("--lat="):
			out["lat"] = arg.substr(6).to_float()
		elif arg.begins_with("--lon="):
			out["lon"] = arg.substr(6).to_float()
		elif arg.begins_with("--radius-m="):
			out["radius_m"] = arg.substr(11).to_float()
		elif arg.begins_with("--radius-km="):
			out["radius_m"] = arg.substr(12).to_float() * 1000.0
		elif arg.begins_with("--finest-level="):
			out["finest_level"] = arg.substr(15).to_int()
	return out


static func _print_help() -> void:
	print("""
Asterra regional terrain compiler

Usage:
  godot --headless --path . --script res://tools/terrain_region_compiler.gd -- [options]

Options:
  --lat=DEG            center latitude, default 0
  --lon=DEG            center longitude, default 0
  --radius-m=M         compile radius in metres, default 250
  --radius-km=KM       compile radius in kilometres
  --finest-level=N     0=~0.75m ... 6=~48m, default 0
  --rebake-world       ignore the cached macro PlanetBake
  --help               show this text

Start with a few hundred metres at level 0. The present full-float .ghz format is
for architecture testing; large production regions will move to packed quantized
residual archives.
""")
