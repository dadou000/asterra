class_name PlanetStudioMapView
extends Control
## Interactive whole-planet authoring map. Environmental colour layers reuse the
## existing PlanetMap contract; authoring overlays are rendered from staged data.

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
var show_features := true
var show_biomes := true
var show_water := true
var show_bookmarks := true
var show_stale_regions := true
var show_sculpt_diff := false
var staged_sculpt: Dictionary = {}
var applied_sculpt: Dictionary = {}

var _texture_rect: TextureRect
var _overlay: Control
var _image: Image
var _texture: ImageTexture
var _render_y := H
var _helper: PlanetMap
var _title: Label
var _feature_points: Array[Dictionary] = []


func _ready() -> void:
	custom_minimum_size = Vector2(800, 430)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_helper = PLANET_MAP_SCRIPT.new() as PlanetMap
	var outer := VBoxContainer.new()
	outer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(outer)
	_title = Label.new()
	_title.add_theme_font_size_override("font_size", 15)
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
	refresh_overlays()


func refresh_overlays() -> void:
	if _overlay != null:
		_overlay.queue_redraw()


func set_sculpt_snapshots(staged: Dictionary, applied: Dictionary) -> void:
	staged_sculpt = staged.duplicate(true)
	applied_sculpt = applied.duplicate(true)
	refresh_overlays()


func _process(_delta: float) -> void:
	if _render_y >= H or _image == null or not Planet.ready_state or Planet.fields == null or Planet.grid == null:
		return
	var fields: Variant = Planet.fields
	var grid: Variant = Planet.grid
	if fields == null or grid == null:
		return
	var end_y := mini(H, _render_y + RENDER_ROWS_PER_FRAME)
	for y in range(_render_y, end_y):
		var lat := (0.5 - float(y) / float(H)) * PI
		for x in W:
			var lon := (float(x) / float(W) - 0.5) * TAU
			var direction := CubeSphere.latlon_to_dir(lat, lon)
			var cell: int = int(grid.dir_to_index(direction))
			_image.set_pixel(x, y, _helper._color_for(base_layer, fields, cell))
	_render_y = end_y
	_title.text = "%s%s" % [PlanetMap.LAYER_NAMES[base_layer], " — rendering…" if _render_y < H else ""]
	if _render_y >= H:
		_texture = ImageTexture.create_from_image(_image)
		_texture_rect.texture = _texture
		refresh_overlays()


func _begin_render() -> void:
	if _title == null:
		return
	_image = Image.create(W, H, false, Image.FORMAT_RGB8)
	_image.fill(Color(0.015, 0.025, 0.04))
	_render_y = 0
	_title.text = "%s — rendering…" % PlanetMap.LAYER_NAMES[base_layer]


func _on_map_input(event: InputEvent) -> void:
	if not (event is InputEventMouseButton):
		return
	var mouse := event as InputEventMouseButton
	if not mouse.pressed:
		return
	var direction := _local_to_dir(mouse.position)
	if direction.length_squared() < 0.5:
		return
	if mouse.button_index == MOUSE_BUTTON_LEFT:
		var picked := _feature_at(mouse.position)
		if not picked.is_empty():
			feature_clicked.emit(picked)
			accept_event()
			return
	map_clicked.emit(direction, mouse.button_index)
	accept_event()


func _local_to_dir(position: Vector2) -> Vector3:
	if _overlay == null or _overlay.size.x <= 1.0 or _overlay.size.y <= 1.0:
		return Vector3.ZERO
	var u := clampf(position.x / _overlay.size.x, 0.0, 1.0)
	var v := clampf(position.y / _overlay.size.y, 0.0, 1.0)
	return CubeSphere.latlon_to_dir((0.5 - v) * PI, (u - 0.5) * TAU)


func _dir_to_local(direction: Vector3) -> Vector2:
	if direction.length_squared() < 0.5 or _overlay == null:
		return Vector2(-10000, -10000)
	var latlon := CubeSphere.dir_to_latlon(direction.normalized())
	return Vector2((latlon.y / TAU + 0.5) * _overlay.size.x,
		(0.5 - latlon.x / PI) * _overlay.size.y)


func _draw_overlay() -> void:
	if session == null or _overlay == null:
		return
	_feature_points.clear()
	if show_features:
		_draw_feature_overlays()
	if show_biomes:
		_draw_biome_overlays()
	if show_water:
		_draw_water_overlays()
	if workspace != null and show_bookmarks:
		for entry: Dictionary in workspace.bookmarks:
			var d := workspace.bookmark_direction(entry)
			if d.length_squared() > 0.5:
				var p := _dir_to_local(d)
				_overlay.draw_circle(p, 5.0, Color(0.95, 0.92, 0.55, 0.95), false, 2.0)
	if workspace != null and show_stale_regions:
		for entry: Dictionary in workspace.stale_regions:
			var d := workspace.bookmark_direction(entry)
			if d.length_squared() > 0.5:
				_draw_region_circle(d, float(entry.get("radius_m", 1.0)), Color(1.0, 0.30, 0.18, 0.8))
	if show_sculpt_diff:
		_draw_sculpt_diff()


func _draw_feature_overlays() -> void:
	var terrain := session.call("active_terrain_profile") as Resource
	if terrain == null:
		return
	for value: Variant in terrain.get(&"displacement_slots") as Array:
		var slot := value as Resource
		var graph := slot.get(&"graph") as Resource if slot != null else null
		if slot == null or graph == null or not bool(slot.get(&"enabled")) or not GUIDED.is_guided_graph(graph):
			continue
		var config := GUIDED.config_from_graph(graph)
		var area := String(config.get("area_kind", GUIDED.AREA_RADIAL))
		var id := String(slot.get(&"slot_id"))
		var color := Color(0.95, 0.66, 0.24, 0.95)
		if workspace != null and workspace.is_hidden("feature", id):
			color.a = 0.22
		if area in [GUIDED.AREA_RADIAL, GUIDED.AREA_RING]:
			var center := CubeSphere.latlon_to_dir(deg_to_rad(float(config.get("center_latitude_deg", 0.0))), deg_to_rad(float(config.get("center_longitude_deg", 0.0))))
			var radius_deg := float(config.get("outer_radius_deg", 20.0)) if area == GUIDED.AREA_RING else float(config.get("radius_deg", 15.0))
			_draw_angular_circle(center, deg_to_rad(radius_deg), color)
			_feature_points.append({"id": id, "pos": _dir_to_local(center)})
		elif area == GUIDED.AREA_REGION:
			var south := float(config.get("south_deg", -30.0))
			var north := float(config.get("north_deg", 30.0))
			var west := float(config.get("west_deg", -45.0))
			var east := float(config.get("east_deg", 45.0))
			var p0 := _dir_to_local(CubeSphere.latlon_to_dir(deg_to_rad(north), deg_to_rad(west)))
			var p1 := _dir_to_local(CubeSphere.latlon_to_dir(deg_to_rad(south), deg_to_rad(east)))
			_overlay.draw_rect(Rect2(Vector2(minf(p0.x, p1.x), minf(p0.y, p1.y)), Vector2(absf(p1.x-p0.x), absf(p1.y-p0.y))), color, false, 1.5)
			_feature_points.append({"id": id, "pos": (p0 + p1) * 0.5})


func _draw_biome_overlays() -> void:
	var terrain := session.call("active_terrain_profile") as Resource
	if terrain == null:
		return
	for layer_value: Variant in terrain.get(&"biome_override_layers") as Array:
		var layer := layer_value as Resource
		if layer == null or not bool(layer.get(&"enabled")):
			continue
		for stroke_value: Variant in layer.get(&"strokes") as Array:
			var stroke := stroke_value as Dictionary
			var d: Vector3 = stroke.get("center_dir", Vector3.ZERO)
			if d.length_squared() < 0.5:
				continue
			var biome_id := clampi(int(stroke.get("biome_id", 0)), 0, PlanetFields.BIOME_COLORS.size() - 1)
			var color: Color = PlanetFields.BIOME_COLORS[biome_id]
			color.a = 0.6 * float(stroke.get("opacity", 1.0))
			_draw_region_circle(d, float(stroke.get("radius_m", 1.0)), color)


func _draw_water_overlays() -> void:
	pass


func _draw_region_circle(direction: Vector3, radius_m: float, color: Color) -> void:
	_draw_angular_circle(direction, radius_m / _planet_radius(), color)


func _draw_angular_circle(direction: Vector3, angular_radius: float, color: Color) -> void:
	var p := _dir_to_local(direction)
	var radii := Vector2(angular_radius / TAU * _overlay.size.x, angular_radius / PI * _overlay.size.y)
	var points := PackedVector2Array()
	for i in 49:
		var a := TAU * float(i) / 48.0
		points.append(p + Vector2(cos(a) * radii.x, sin(a) * radii.y))
	_overlay.draw_polyline(points, color, 1.5)
	if p.x - radii.x < 0.0:
		var shifted := PackedVector2Array()
		for point in points: shifted.append(point + Vector2(_overlay.size.x, 0))
		_overlay.draw_polyline(shifted, color, 1.5)
	if p.x + radii.x > _overlay.size.x:
		var shifted2 := PackedVector2Array()
		for point in points: shifted2.append(point - Vector2(_overlay.size.x, 0))
		_overlay.draw_polyline(shifted2, color, 1.5)


func _draw_sculpt_diff() -> void:
	var staged_keys: PackedInt64Array = staged_sculpt.get("keys", PackedInt64Array())
	var applied_keys: PackedInt64Array = applied_sculpt.get("keys", PackedInt64Array())
	var applied_lookup: Dictionary = {}
	for key: int in applied_keys:
		applied_lookup[key] = true
	for key: int in staged_keys:
		if applied_lookup.has(key):
			continue
		var face := (key >> 30) & 0x7
		var ti := key & 0x7FFF
		var tj := (key >> 15) & 0x7FFF
		var d := Deltas.lattice_to_dir(face, float(ti * Deltas.TILE) + Deltas.TILE * 0.5, float(tj * Deltas.TILE) + Deltas.TILE * 0.5)
		_overlay.draw_circle(_dir_to_local(d), 2.0, Color(1.0, 0.22, 0.16, 0.65))


func _feature_at(position: Vector2) -> String:
	var result := ""
	var best := 22.0
	for entry: Dictionary in _feature_points:
		var distance := position.distance_to(entry.get("pos", Vector2.ZERO))
		if distance < best:
			best = distance
			result = String(entry.get("id", ""))
	return result


func _planet_radius() -> float:
	var body := session.call("active_body") as Resource if session != null else null
	if body != null:
		return maxf(float(body.get(&"radius_m")), 1.0)
	return maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1000000.0
