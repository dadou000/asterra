class_name PlanetStudioMapView
extends Control
## Interactive whole-planet authoring map embedded in Planet Studio.
##
## It reuses the exact PlanetMap environmental layer colour contract, then draws
## authoring overlays from WorldAuthoringSession on top. Mouse clicks return a
## planet-centric direction so the editor can focus the 3D camera, paint, place
## presets or select existing features without a second coordinate system.

signal map_clicked(direction: Vector3, button: int)
signal feature_clicked(slot_id: String)

const PLANET_MAP_SCRIPT := preload("res://scripts/ui/planet_map.gd")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const W := 960
const H := 480
const RENDER_ROWS_PER_FRAME := 14

var session: RefCounted
var workspace: PlanetStudioWorkspace
var base_layer: int = PlanetMap.Layer.ELEVATION
var show_features: bool = true
var show_biomes: bool = true
var show_water: bool = true
var show_bookmarks: bool = true
var show_stale_regions: bool = true
var show_sculpt_diff: bool = false
var staged_sculpt: Dictionary = {}
var applied_sculpt: Dictionary = {}

var _texture_rect: TextureRect
var _overlay: Control
var _image: Image
var _texture: ImageTexture
var _render_y: int = H
var _helper: Object
var _last_feature_centers: Array[Dictionary] = []
var _title: Label


func _ready() -> void:
	custom_minimum_size = Vector2(800.0, 430.0)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_helper = PLANET_MAP_SCRIPT.new()
	var outer := VBoxContainer.new()
	outer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	outer.add_theme_constant_override("separation", 4)
	add_child(outer)
	_title = Label.new()
	_title.text = "Planet map"
	_title.add_theme_font_size_override("font_size", 15)
	_title.modulate = Color(0.68, 0.80, 0.90)
	outer.add_child(_title)
	var aspect := AspectRatioContainer.new()
	aspect.ratio = 2.0
	aspect.stretch_mode = AspectRatioContainer.STRETCH_WIDTH_CONTROLS_HEIGHT
	aspect.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	aspect.size_flags_vertical = Control.SIZE_EXPAND_FILL
	outer.add_child(aspect)
	var frame := PanelContainer.new()
	aspect.add_child(frame)
	_texture_rect = TextureRect.new()
	_texture_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_texture_rect.stretch_mode = TextureRect.STRETCH_SCALE
	_texture_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	frame.add_child(_texture_rect)
	_overlay = Control.new()
	_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_overlay.mouse_filter = Control.MOUSE_FILTER_STOP
	_overlay.draw.connect(_draw_overlay)
	_overlay.gui_input.connect(_on_map_input)
	frame.add_child(_overlay)
	_begin_render()


func bind(p_session: RefCounted, p_workspace: PlanetStudioWorkspace) -> void:
	session = p_session
	workspace = p_workspace
	refresh()


func set_layer(layer_index: int) -> void:
	base_layer = clampi(layer_index, 0, PlanetMap.LAYER_NAMES.size() - 1)
	_begin_render()


func refresh() -> void:
	_begin_render()
	if _overlay != null:
		_overlay.queue_redraw()


func refresh_overlays() -> void:
	if _overlay != null:
		_overlay.queue_redraw()


func set_sculpt_snapshots(staged: Dictionary, applied: Dictionary) -> void:
	staged_sculpt = staged.duplicate(true)
	applied_sculpt = applied.duplicate(true)
	refresh_overlays()


func _process(_delta: float) -> void:
	if _render_y >= H or _image == null or not Planet.ready_state \
			or Planet.fields == null or Planet.grid == null:
		return
	var end_y := mini(H, _render_y + RENDER_ROWS_PER_FRAME)
	for y in range(_render_y, end_y):
		var lat := (0.5 - float(y) / float(H)) * PI
		for x in W:
			var lon := (float(x) / float(W) - 0.5) * TAU
			var direction := CubeSphere.latlon_to_dir(lat, lon)
			var cell := Planet.grid.dir_to_index(direction)
			_image.set_pixel(x, y, _helper.call("_color_for", base_layer, Planet.fields, cell))
	_render_y = end_y
	_title.text = "%s%s" % [PlanetMap.LAYER_NAMES[base_layer], " — rendering…" if _render_y < H else ""]
	if _render_y >= H:
		_texture = ImageTexture.create_from_image(_image)
		_texture_rect.texture = _texture
		_overlay.queue_redraw()


func _begin_render() -> void:
	if _title == null:
		return
	_image = Image.create(W, H, false, Image.FORMAT_RGB8)
	_image.fill(Color(0.015, 0.025, 0.04))
	_render_y = 0
	_title.text = "%s — rendering…" % PlanetMap.LAYER_NAMES[base_layer]


func _on_map_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mouse := event as InputEventMouseButton
		if not mouse.pressed:
			return
		var direction := _local_to_dir(mouse.position)
		if direction.length_squared() < 0.5:
			return
		if mouse.button_index == MOUSE_BUTTON_LEFT:
			var feature_id := _feature_at(mouse.position)
			if not feature_id.is_empty():
				feature_clicked.emit(feature_id)
				accept_event()
				return
		map_clicked.emit(direction, mouse.button_index)
		accept_event()


func _local_to_dir(position: Vector2) -> Vector3:
	if _overlay == null or _overlay.size.x <= 1.0 or _overlay.size.y <= 1.0:
		return Vector3.ZERO
	var u := clampf(position.x / _overlay.size.x, 0.0, 1.0)
	var v := clampf(position.y / _overlay.size.y, 0.0, 1.0)
	var lon := (u - 0.5) * TAU
	var lat := (0.5 - v) * PI
	return CubeSphere.latlon_to_dir(lat, lon)


func _dir_to_local(direction: Vector3) -> Vector2:
	if direction.length_squared() < 0.5 or _overlay == null:
		return Vector2(-10000, -10000)
	var latlon: Vector2 = CubeSphere.dir_to_latlon(direction.normalized())
	return Vector2((latlon.y / TAU + 0.5) * _overlay.size.x,
		(0.5 - latlon.x / PI) * _overlay.size.y)


func _draw_overlay() -> void:
	if session == null or _overlay == null:
		return
	_last_feature_centers.clear()
	if show_features:
		_draw_feature_overlays()
	if show_biomes:
		_draw_biome_overlays()
	if show_water:
		_draw_water_overlays()
	if show_bookmarks and workspace != null:
		_draw_bookmarks()
	if show_stale_regions and workspace != null:
		_draw_stale_regions()
	if show_sculpt_diff:
		_draw_sculpt_diff()


func _draw_feature_overlays() -> void:
	var terrain: Resource = session.call("active_terrain_profile") as Resource
	if terrain == null:
		return
	var radius := _planet_radius()
	for slot_value in terrain.get(&"displacement_slots") as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")):
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null or not GUIDED.is_guided_graph(graph):
			continue
		var config := GUIDED.config_from_graph(graph)
		var area := String(config.get("area_kind", GUIDED.AREA_RADIAL))
		var slot_id := String(slot.get(&"slot_id"))
		var color := Color(0.95, 0.66, 0.24, 0.95)
		if workspace != null and workspace.is_hidden("feature", slot_id):
			color.a = 0.22
		match area:
			GUIDED.AREA_RADIAL, GUIDED.AREA_RING:
				var center := _latlon_deg_to_dir(float(config.get("center_latitude_deg", 0.0)),
					float(config.get("center_longitude_deg", 0.0)))
				var center_px := _dir_to_local(center)
				var outer_deg := float(config.get("outer_radius_deg", 20.0)) if area == GUIDED.AREA_RING \
					else float(config.get("radius_deg", 15.0))
				var rx := deg_to_rad(outer_deg) / TAU * _overlay.size.x
				var ry := deg_to_rad(outer_deg) / PI * _overlay.size.y
				_draw_wrapped_ellipse(center_px, Vector2(rx, ry), color, 2.0)
				if area == GUIDED.AREA_RING:
					var inner_deg := float(config.get("inner_radius_deg", 8.0))
					_draw_wrapped_ellipse(center_px,
						Vector2(deg_to_rad(inner_deg) / TAU * _overlay.size.x,
							deg_to_rad(inner_deg) / PI * _overlay.size.y), color.darkened(0.15), 1.0)
				_last_feature_centers.append({"id": slot_id, "pos": center_px})
			GUIDED.AREA_REGION:
				var south := float(config.get("south_deg", -30.0))
				var north := float(config.get("north_deg", 30.0))
				var west := float(config.get("west_deg", -45.0))
				var east := float(config.get("east_deg", 45.0))
				_draw_geo_rect(south, north, west, east, color)
				var center_lat := (south + north) * 0.5
				var center_lon := _mid_longitude(west, east)
				_last_feature_centers.append({"id": slot_id, "pos": _dir_to_local(_latlon_deg_to_dir(center_lat, center_lon))})
			GUIDED.AREA_LATITUDE:
				var y0 := _lat_to_y(float(config.get("north_deg", 30.0)))
				var y1 := _lat_to_y(float(config.get("south_deg", -30.0)))
				_overlay.draw_rect(Rect2(0.0, minf(y0, y1), _overlay.size.x, absf(y1 - y0)), Color(color, 0.08), true)
				_overlay.draw_line(Vector2(0, y0), Vector2(_overlay.size.x, y0), color, 1.5)
				_overlay.draw_line(Vector2(0, y1), Vector2(_overlay.size.x, y1), color, 1.5)
			GUIDED.AREA_LONGITUDE:
				var x0 := _lon_to_x(float(config.get("west_deg", -45.0)))
				var x1 := _lon_to_x(float(config.get("east_deg", 45.0)))
				_overlay.draw_line(Vector2(x0, 0), Vector2(x0, _overlay.size.y), color, 1.5)
				_overlay.draw_line(Vector2(x1, 0), Vector2(x1, _overlay.size.y), color, 1.5)
			GUIDED.AREA_EVERYWHERE:
				_overlay.draw_rect(Rect2(Vector2.ZERO, _overlay.size), Color(color, 0.04), true)


func _draw_biome_overlays() -> void:
	var terrain: Resource = session.call("active_terrain_profile") as Resource
	if terrain == null:
		return
	var planet_radius := _planet_radius()
	for layer_value in terrain.get(&"biome_override_layers") as Array:
		var layer: Resource = layer_value as Resource
		if layer == null or not bool(layer.get(&"enabled")):
			continue
		for stroke_value in layer.get(&"strokes") as Array:
			var stroke: Dictionary = stroke_value as Dictionary
			var direction: Vector3 = stroke.get("center_dir", Vector3.ZERO)
			if direction.length_squared() < 0.5:
				continue
			var biome_id := clampi(int(stroke.get("biome_id", 0)), 0, PlanetFields.BIOME_COLORS.size() - 1)
			var color: Color = PlanetFields.BIOME_COLORS[biome_id]
			color.a = 0.62 * float(stroke.get("opacity", 1.0))
			var angular := float(stroke.get("radius_m", 1.0)) / planet_radius
			var center_px := _dir_to_local(direction)
			_draw_wrapped_ellipse(center_px,
				Vector2(angular / TAU * _overlay.size.x, angular / PI * _overlay.size.y), color, 1.0)


func _draw_water_overlays() -> void:
	var water: Resource = session.call("active_water_profile") as Resource
	if water == null:
		return
	for feature_value in water.get(&"features") as Array:
		var feature: Resource = feature_value as Resource
		if feature == null or not bool(feature.get(&"enabled")):
			continue
		var points: PackedVector3Array = feature.get(&"lake_polygon_body_m")
		if not points.is_empty():
			var poly := PackedVector2Array()
			for point in points:
				if point.length_squared() > 1.0:
					poly.append(_dir_to_local(point.normalized()))
			if poly.size() >= 2:
				_overlay.draw_polyline(poly, Color(0.20, 0.72, 1.0, 0.95), 2.0)
		var knots: Array = feature.get(&"river_knots") as Array
		if knots.size() >= 2:
			var river := PackedVector2Array()
			for knot_value in knots:
				var knot: Dictionary = knot_value as Dictionary
				var p: Vector3 = knot.get("position_body_m", Vector3.ZERO)
				if p.length_squared() > 1.0:
					river.append(_dir_to_local(p.normalized()))
			if river.size() >= 2:
				_overlay.draw_polyline(river, Color(0.12, 0.62, 1.0, 0.95), 2.5)


func _draw_bookmarks() -> void:
	for entry in workspace.bookmarks:
		var direction := workspace.bookmark_direction(entry)
		if direction.length_squared() < 0.5:
			continue
		var p := _dir_to_local(direction)
		_overlay.draw_circle(p, 5.0, Color(0.95, 0.92, 0.55, 0.95), false, 2.0)
		_overlay.draw_line(p + Vector2(-7, 0), p + Vector2(7, 0), Color(0.95, 0.92, 0.55, 0.9), 1.0)
		_overlay.draw_line(p + Vector2(0, -7), p + Vector2(0, 7), Color(0.95, 0.92, 0.55, 0.9), 1.0)


func _draw_stale_regions() -> void:
	var planet_radius := _planet_radius()
	for entry in workspace.stale_regions:
		var direction := workspace.bookmark_direction(entry)
		if direction.length_squared() < 0.5:
			continue
		var angular := float(entry.get("radius_m", 1.0)) / planet_radius
		var p := _dir_to_local(direction)
		var color := Color(1.0, 0.30, 0.18, 0.80)
		_draw_wrapped_ellipse(p,
			Vector2(angular / TAU * _overlay.size.x, angular / PI * _overlay.size.y), color, 1.5)


func _draw_sculpt_diff() -> void:
	# Sparse diff is intentionally sampled rather than rasterizing every delta tile.
	# It acts as a heatmap of where staged sculpt storage differs from applied state.
	var staged_keys: PackedInt64Array = staged_sculpt.get("keys", PackedInt64Array())
	var applied_keys: PackedInt64Array = applied_sculpt.get("keys", PackedInt64Array())
	var applied_lookup: Dictionary = {}
	for key in applied_keys:
		applied_lookup[int(key)] = true
	for key in staged_keys:
		if applied_lookup.has(int(key)):
			continue
		var face := (int(key) >> 30) & 0x7
		var ti := int(key) & 0x7FFF
		var tj := (int(key) >> 15) & 0x7FFF
		var direction := Deltas.lattice_to_dir(face,
			float(ti * Deltas.TILE) + float(Deltas.TILE) * 0.5,
			float(tj * Deltas.TILE) + float(Deltas.TILE) * 0.5)
		_overlay.draw_circle(_dir_to_local(direction), 2.0, Color(1.0, 0.22, 0.16, 0.65))


func _feature_at(position: Vector2) -> String:
	var best_id := ""
	var best_distance := 22.0
	for entry in _last_feature_centers:
		var d := (position - Vector2(entry.get("pos", Vector2.ZERO))).length()
		if d < best_distance:
			best_distance = d
			best_id = String(entry.get("id", ""))
	return best_id


func _draw_geo_rect(south: float, north: float, west: float, east: float, color: Color) -> void:
	var y0 := _lat_to_y(north)
	var y1 := _lat_to_y(south)
	var x0 := _lon_to_x(west)
	var x1 := _lon_to_x(east)
	if east >= west:
		_overlay.draw_rect(Rect2(Vector2(x0, minf(y0, y1)), Vector2(maxf(x1 - x0, 1.0), absf(y1 - y0))), Color(color, 0.07), true)
		_overlay.draw_rect(Rect2(Vector2(x0, minf(y0, y1)), Vector2(maxf(x1 - x0, 1.0), absf(y1 - y0))), color, false, 1.5)
	else:
		for rect in [
			Rect2(Vector2(x0, minf(y0, y1)), Vector2(_overlay.size.x - x0, absf(y1 - y0))),
			Rect2(Vector2(0, minf(y0, y1)), Vector2(x1, absf(y1 - y0))),
		]:
			_overlay.draw_rect(rect, Color(color, 0.07), true)
			_overlay.draw_rect(rect, color, false, 1.5)


func _draw_wrapped_ellipse(center: Vector2, radius: Vector2, color: Color, width: float) -> void:
	var points := PackedVector2Array()
	var segments := 48
	for i in segments + 1:
		var angle := TAU * float(i) / float(segments)
		points.append(center + Vector2(cos(angle) * radius.x, sin(angle) * radius.y))
	_overlay.draw_polyline(points, color, width)
	if center.x - radius.x < 0.0:
		var shifted := PackedVector2Array()
		for p in points:
			shifted.append(p + Vector2(_overlay.size.x, 0))
		_overlay.draw_polyline(shifted, color, width)
	if center.x + radius.x > _overlay.size.x:
		var shifted2 := PackedVector2Array()
		for p in points:
			shifted2.append(p - Vector2(_overlay.size.x, 0))
		_overlay.draw_polyline(shifted2, color, width)


func _planet_radius() -> float:
	var body: Resource = session.call("active_body") as Resource if session != null else null
	return maxf(float(body.get(&"radius_m")), 1.0) if body != null else \
		(maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1000000.0)


func _latlon_deg_to_dir(lat_deg: float, lon_deg: float) -> Vector3:
	return CubeSphere.latlon_to_dir(deg_to_rad(lat_deg), deg_to_rad(lon_deg))


func _lat_to_y(lat_deg: float) -> float:
	return (0.5 - deg_to_rad(lat_deg) / PI) * _overlay.size.y


func _lon_to_x(lon_deg: float) -> float:
	return (deg_to_rad(lon_deg) / TAU + 0.5) * _overlay.size.x


func _mid_longitude(west: float, east: float) -> float:
	if east >= west:
		return (west + east) * 0.5
	var span := fmod((east - west) + 360.0, 360.0)
	return wrapf(west + span * 0.5, -180.0, 180.0)
