class_name AsterraHUD
extends CanvasLayer
## Debug/inspection HUD.
##
## Phase 1's milestone is observational -- you have to be able to see that the
## ground under your feet really is the product of geology, climate and water, so
## the HUD reports the actual field values at the player's position rather than a
## generic coordinate readout.

var info: RichTextLabel
var help: Label
var crosshair: Control
var progress_panel: PanelContainer
var progress_label: Label
var progress_bar: ProgressBar
var toast: Label

var _toast_time := 0.0

func _ready() -> void:
	layer = 10
	var font_size := 13

	info = RichTextLabel.new()
	info.bbcode_enabled = true
	info.scroll_active = false
	info.fit_content = false
	info.set_anchors_preset(Control.PRESET_TOP_LEFT)
	info.position = Vector2(12, 10)
	info.size = Vector2(500, 680)
	info.add_theme_font_size_override("normal_font_size", font_size)
	info.add_theme_color_override("default_color", Color(0.92, 0.95, 1.0))
	add_child(info)

	help = Label.new()
	help.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	help.position = Vector2(12, -132)
	help.add_theme_font_size_override("font_size", 12)
	help.add_theme_color_override("font_color", Color(0.75, 0.80, 0.88))
	help.text = """WASD move · Space/Shift-C up/down · Shift sprint · F walk/fly · Esc debug menu
LMB dig · RMB fill · G grade to aim · Q drop pile · E collect pile · [ ] brush size
M planet map · , . cycle map layer · F5 save · F9 load · T teleport to a good site"""
	add_child(help)

	crosshair = Control.new()
	crosshair.set_anchors_preset(Control.PRESET_CENTER)
	crosshair.custom_minimum_size = Vector2(18, 18)
	crosshair.draw.connect(func():
		crosshair.draw_line(Vector2(-7, 0), Vector2(7, 0), Color(1, 1, 1, 0.55), 1.0)
		crosshair.draw_line(Vector2(0, -7), Vector2(0, 7), Color(1, 1, 1, 0.55), 1.0))
	add_child(crosshair)

	toast = Label.new()
	toast.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	toast.position = Vector2(-200, -180)
	toast.custom_minimum_size = Vector2(400, 24)
	toast.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	toast.add_theme_font_size_override("font_size", 15)
	add_child(toast)

	progress_panel = PanelContainer.new()
	progress_panel.set_anchors_preset(Control.PRESET_CENTER)
	progress_panel.position = Vector2(-260, -50)
	progress_panel.custom_minimum_size = Vector2(520, 90)
	var vb := VBoxContainer.new()
	progress_label = Label.new()
	progress_label.text = "Generating Asterra…"
	progress_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	progress_bar = ProgressBar.new()
	progress_bar.max_value = 1.0
	progress_bar.custom_minimum_size = Vector2(480, 18)
	vb.add_child(progress_label)
	vb.add_child(progress_bar)
	progress_panel.add_child(vb)
	add_child(progress_panel)

func show_progress(stage: String, fraction: float) -> void:
	progress_panel.visible = true
	progress_label.text = stage
	progress_bar.value = fraction

func hide_progress() -> void:
	progress_panel.visible = false

func notify(text: String) -> void:
	toast.text = text
	_toast_time = 3.0

func _process(dt: float) -> void:
	if _toast_time > 0.0:
		_toast_time -= dt
		toast.modulate.a = clampf(_toast_time, 0.0, 1.0)
	else:
		toast.text = ""

func update_info(player: AsterraPlayer, terrain: PlanetTerrain, carry: MaterialStock,
		brush: float, aim_info: Dictionary) -> void:
	if not Planet.ready_state:
		return
	var d := player.up_dir()
	var s := Planet.sample_info(d)
	var lines := PackedStringArray()
	lines.append("[b]ASTERRA[/b]  seed %d   %s" % [Planet.cfg.world_seed,
		"WALK" if player.mode == AsterraPlayer.Mode.WALK else "FLY"])
	lines.append("lat %6.2f°  lon %7.2f°   alt %s   AGL %s" % [
		s["lat_deg"], s["lon_deg"], _m(player.altitude()), _m(player.height_above_ground())])
	lines.append("")
	lines.append("[color=#9fd]TERRAIN[/color] %s   macro %s   relief %s" % [
		_m(s["elevation"]), _m(s["macro_elevation"]), _m(s["relief"])])
	lines.append("[color=#9fd]GEOLOGY[/color] %s   dip %.0f°   fault %.2f   basin %.2f" % [
		s["rock_name"], rad_to_deg(s["strata_dip"]), s["fault"], s["basin"]])
	lines.append("  Fe %.2f  Cu %.2f  coal %.2f  oil %.2f (gas %.0f%%)  quartz %.2f  aquifer %.2f" % [
		s["ore_iron"], s["ore_copper"], s["coal"], s["petroleum"],
		s["gas_fraction"] * 100.0, s["quartz"], s["aquifer"]])
	lines.append("[color=#9fd]WATER[/color] discharge %.1f m³/s  width %.0f m  Strahler %d" % [
		s["discharge"], s["river_width"], s["stream_order"]])
	lines.append("  floodplain %.2f  wetland %.2f  %s" % [
		s["floodplain"], s["wetland"], "LAKE" if s["lake"] else ""])
	lines.append("[color=#9fd]CLIMATE[/color] %.1f °C ±%.0f   %.0f mm/yr   RH %.0f%%" % [
		s["temp_mean"], s["temp_range"] * 0.5, s["precip"], s["humidity"] * 100.0])
	lines.append("  wind %.1f/%.1f m/s   severe %.2f" % [s["wind"].x, s["wind"].y, s["storm_risk"]])
	lines.append("[color=#9fd]SOIL[/color] depth %.2f m   sand %.0f / silt %.0f / clay %.0f %%   org %.0f%%" % [
		s["soil_depth"], s["soil_sand"] * 100.0, s["soil_silt"] * 100.0,
		s["soil_clay"] * 100.0, s["soil_organic"] * 100.0])
	lines.append("  moisture %.2f" % s["soil_moisture"])
	lines.append("[color=#9fd]BIOME[/color] %s   vegetation %.2f" % [s["biome_name"], s["vegetation"]])
	lines.append("[color=#9fd]SITE[/color] buildability %.2f   transport corridor %.2f" % [
		s["suitability"], s["corridor"]])
	lines.append("")
	if not aim_info.is_empty():
		var am: Dictionary = Planet.column_material(aim_info["dir"], 0.25)
		lines.append("[color=#fd9]AIM[/color] %.1f m away · at 0.25 m: %s (dig %.2f)" % [
			aim_info["distance"], am["name"], am["diggability"]])
	else:
		lines.append("[color=#666]AIM  no ground in range[/color]")
	lines.append("[color=#fd9]CARRY[/color] %.2f m³ · %s" % [carry.total_volume(), carry.describe()])
	lines.append("[color=#fd9]BRUSH[/color] %.1f m" % brush)
	var st := terrain.stats()
	lines.append("[color=#666]chunks %d  nodes %d  queued %d  in_flight %d  culled %d  deltas %d tiles  rebases %d[/color]" % [
		st["chunks"], st["nodes"], st["queued"], st["in_flight"], st["culled"],
		Deltas.edited_tile_count(), Frames.rebase_count()])
	var hs := GroundHeightStore.stats()
	var height_total: int = int(hs["in_flight"])
	var height_bake: int = int(hs.get("bake_in_flight", 0))
	var height_io: int = maxi(height_total - height_bake, 0)
	lines.append("[color=#666]height cache RAM %d  memhit %d  disk %d  baked %d  queued %d  io %d  bake %d  dropped %d[/color]" % [
		hs["memory_tiles"], hs["memory_hits"], hs["disk_hits"], hs["tiles_built"],
		hs["queued"], height_io, height_bake, hs["dropped"]])
	if GroundGeometryClipmap.has_method("gpu_stream_stats"):
		var gs: Dictionary = GroundGeometryClipmap.gpu_stream_stats()
		lines.append("[color=#666]height GPU pages %d/%d  uploads %d  reupload %d  evict %d  tablefail %d  coverage %s[/color]" % [
			int(gs.get("page_resident", 0)), int(gs.get("page_capacity", 0)),
			int(gs.get("page_uploads", 0)), int(gs.get("page_reuploads", 0)),
			int(gs.get("page_evictions", 0)), int(gs.get("table_failures", 0)),
			"OK" if bool(gs.get("coverage_ready", false)) else "WAIT"])
		lines.append("[color=#666]GPU upload %.1fk texels  batches %d  table cell %d  full rebuild %d  tomb %d[/color]" % [
			float(gs.get("page_texels", 0)) / 1000.0, int(gs.get("draw_batches", 0)),
			int(gs.get("table_updates", 0)), int(gs.get("table_rebuilds", 0)),
			int(gs.get("table_tombstones", 0))])
	var ps := GroundTerrainPrefetcher.stats()
	lines.append("[color=#666]terrain prefetch %.0f km/h  lookahead %.0f m[/color]" % [
		float(ps["speed_mps"]) * 3.6, float(ps["lookahead_m"])])
	lines.append("[color=#666]horizon %.1f°  %.0f km[/color]" % [st["horizon_deg"], st["horizon_km"]])
	lines.append("[color=#666]fps %d[/color]" % Engine.get_frames_per_second())
	info.text = String("\n").join(lines)

static func _m(v: float) -> String:
	if absf(v) >= 1000.0:
		return "%.2f km" % (v / 1000.0)
	return "%.1f m" % v
