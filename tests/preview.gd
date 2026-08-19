extends Node
## Bakes the default world and writes every map layer to user://preview as PNG.
func _ready() -> void:
	var cfg := GenConfig.new()
	var t0 := Time.get_ticks_msec()
	var bake := PlanetBake.new(cfg)
	var last := ""
	var fields := bake.bake(func(stage, frac):
		if stage != last:
			last = stage
			print("  [%5.1f%%] %s" % [frac * 100.0, stage]), true)
	print("bake: %d cells, %.1f s" % [fields.grid.cell_count, (Time.get_ticks_msec() - t0) / 1000.0])
	Planet.adopt(fields)
	var map := PlanetMap.new()
	add_child(map)
	var dir := "user://preview"
	DirAccess.make_dir_recursive_absolute(dir)
	for i in PlanetMap.LAYER_NAMES.size():
		var img := map._render(i)
		var name := String(PlanetMap.LAYER_NAMES[i]).to_lower().replace(" ", "_").replace("&", "and")
		img.save_png("%s/%02d_%s.png" % [dir, i, name])
	print("wrote %d layers to %s" % [PlanetMap.LAYER_NAMES.size(), ProjectSettings.globalize_path(dir)])
	get_tree().quit()
