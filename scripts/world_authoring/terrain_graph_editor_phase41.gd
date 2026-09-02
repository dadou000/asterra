extends "res://scripts/world_authoring/terrain_graph_editor_phase40.gd"
## Phase 41 graph presentation for deterministic planet-space spatial masks.
##
## Latitude/Longitude Bands, Geographic Region, Radial Area and Ring Area are
## exposed only on generic authored displacement graphs. Base Terrain native stages
## are still resident shader contributions lowered to scalar coefficients, so
## presenting a spatial mask there would imply support that does not exist yet.
## Saved unsupported native topology remains visible but is rejected transactionally
## by Phase 40 lowering.
##
## LONGITUDE_MASK, GEOGRAPHIC_REGION_MASK, RADIAL_MASK and RING_MASK are editor-facing
## picker identities. Serialized graphs keep the canonical LATITUDE_MASK node type
## and opt into richer semantics with parameters.axis. Missing axis therefore remains
## byte-for-byte backward compatible with every original Phase 41 latitude graph.

const LATITUDE_MASK_TYPE := "LATITUDE_MASK"
const LONGITUDE_MASK_PICKER_TYPE := "LONGITUDE_MASK"
const GEOGRAPHIC_REGION_PICKER_TYPE := "GEOGRAPHIC_REGION_MASK"
const RADIAL_MASK_PICKER_TYPE := "RADIAL_MASK"
const RING_MASK_PICKER_TYPE := "RING_MASK"
const LATITUDE_MASK_PICKER_LABEL := "Mask  ·  Latitude Band"
const LONGITUDE_MASK_PICKER_LABEL := "Mask  ·  Longitude Band"
const GEOGRAPHIC_REGION_PICKER_LABEL := "Mask  ·  Geographic Region"
const RADIAL_MASK_PICKER_LABEL := "Mask  ·  Radial Area"
const RING_MASK_PICKER_LABEL := "Mask  ·  Ring Area"


func _build_ui() -> void:
	super._build_ui()
	_sync_phase41_spatial_picker_entries()
	if _status != null and _spatial_mask_available_here():
		_status.text = "Live authored displacement: Latitude / Longitude Bands, Geographic Region, Radial Area and Ring Area use normalized planet-space coordinates and the same CPU/contact + GPU bytecode definition. Invalid edits keep the last valid terrain active."


func _create_graph_node(node_data: Dictionary) -> void:
	if String(node_data.get("type", "")) == LATITUDE_MASK_TYPE:
		var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
		match String(parameters.get("axis", "latitude")):
			"longitude":
				_create_longitude_mask_node(node_data)
			"region":
				_create_geographic_region_node(node_data)
			"radial":
				_create_radial_mask_node(node_data)
			"ring":
				_create_ring_mask_node(node_data)
			_:
				_create_latitude_mask_node(node_data)
		return
	super._create_graph_node(node_data)


func _on_add_node() -> void:
	if _node_type_picker == null or _node_type_picker.item_count == 0:
		return
	var node_type: String = String(_node_type_picker.get_item_metadata(
		_node_type_picker.selected))
	if node_type != LATITUDE_MASK_TYPE \
			and node_type != LONGITUDE_MASK_PICKER_TYPE \
			and node_type != GEOGRAPHIC_REGION_PICKER_TYPE \
			and node_type != RADIAL_MASK_PICKER_TYPE \
			and node_type != RING_MASK_PICKER_TYPE:
		super._on_add_node()
		return
	if not _spatial_mask_available_here() or _session == null or _graph == null:
		if _status != null:
			_status.text = "Planet-space masks are currently available for authored displacement flows, not resident Base Terrain stage composition."
		return
	var node_count: int = (_graph.get(&"nodes") as Array).size()
	var position := Vector2(
		180.0 + float(node_count % 4) * 180.0,
		120.0 + float(node_count / 4) * 120.0)
	match node_type:
		LONGITUDE_MASK_PICKER_TYPE:
			_session.call("stage_action", "Add Longitude Mask", func() -> void:
				_append_longitude_mask_node(position)
			, 2)
		GEOGRAPHIC_REGION_PICKER_TYPE:
			_session.call("stage_action", "Add Geographic Region", func() -> void:
				_append_geographic_region_node(position)
			, 2)
		RADIAL_MASK_PICKER_TYPE:
			_session.call("stage_action", "Add Radial Area", func() -> void:
				_append_radial_mask_node(position)
			, 2)
		RING_MASK_PICKER_TYPE:
			_session.call("stage_action", "Add Ring Area", func() -> void:
				_append_ring_mask_node(position)
			, 2)
		_:
			_session.call("stage_action", "Add Latitude Mask", func() -> void:
				_append_latitude_mask_node(position)
			, 2)
	_request_rebuild()


func _sync_phase41_spatial_picker_entries() -> void:
	if _node_type_picker == null:
		return
	var latitude_index: int = _picker_index_for_metadata(LATITUDE_MASK_TYPE)
	var longitude_index: int = _picker_index_for_metadata(LONGITUDE_MASK_PICKER_TYPE)
	var region_index: int = _picker_index_for_metadata(GEOGRAPHIC_REGION_PICKER_TYPE)
	var radial_index: int = _picker_index_for_metadata(RADIAL_MASK_PICKER_TYPE)
	var ring_index: int = _picker_index_for_metadata(RING_MASK_PICKER_TYPE)

	# LATITUDE_MASK is part of the canonical displacement schema. The richer spatial
	# nodes use parameter variants so old graph resources need no migration. Resident
	# Base Terrain deliberately stays one step behind until native stage provenance
	# can carry a spatial factor identically through render, warm cache and physical/
	# contact evaluation.
	if not _spatial_mask_available_here():
		if ring_index >= 0:
			_node_type_picker.remove_item(ring_index)
		radial_index = _picker_index_for_metadata(RADIAL_MASK_PICKER_TYPE)
		if radial_index >= 0:
			_node_type_picker.remove_item(radial_index)
		region_index = _picker_index_for_metadata(GEOGRAPHIC_REGION_PICKER_TYPE)
		if region_index >= 0:
			_node_type_picker.remove_item(region_index)
		longitude_index = _picker_index_for_metadata(LONGITUDE_MASK_PICKER_TYPE)
		if longitude_index >= 0:
			_node_type_picker.remove_item(longitude_index)
		latitude_index = _picker_index_for_metadata(LATITUDE_MASK_TYPE)
		if latitude_index >= 0:
			_node_type_picker.remove_item(latitude_index)
		return

	if latitude_index < 0:
		_node_type_picker.add_item(LATITUDE_MASK_PICKER_LABEL)
		latitude_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(latitude_index, LATITUDE_MASK_TYPE)
	else:
		_node_type_picker.set_item_text(latitude_index, LATITUDE_MASK_PICKER_LABEL)

	longitude_index = _picker_index_for_metadata(LONGITUDE_MASK_PICKER_TYPE)
	if longitude_index < 0:
		_node_type_picker.add_item(LONGITUDE_MASK_PICKER_LABEL)
		longitude_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(longitude_index, LONGITUDE_MASK_PICKER_TYPE)
	else:
		_node_type_picker.set_item_text(longitude_index, LONGITUDE_MASK_PICKER_LABEL)

	region_index = _picker_index_for_metadata(GEOGRAPHIC_REGION_PICKER_TYPE)
	if region_index < 0:
		_node_type_picker.add_item(GEOGRAPHIC_REGION_PICKER_LABEL)
		region_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(region_index, GEOGRAPHIC_REGION_PICKER_TYPE)
	else:
		_node_type_picker.set_item_text(region_index, GEOGRAPHIC_REGION_PICKER_LABEL)

	radial_index = _picker_index_for_metadata(RADIAL_MASK_PICKER_TYPE)
	if radial_index < 0:
		_node_type_picker.add_item(RADIAL_MASK_PICKER_LABEL)
		radial_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(radial_index, RADIAL_MASK_PICKER_TYPE)
	else:
		_node_type_picker.set_item_text(radial_index, RADIAL_MASK_PICKER_LABEL)

	ring_index = _picker_index_for_metadata(RING_MASK_PICKER_TYPE)
	if ring_index < 0:
		_node_type_picker.add_item(RING_MASK_PICKER_LABEL)
		ring_index = _node_type_picker.item_count - 1
		_node_type_picker.set_item_metadata(ring_index, RING_MASK_PICKER_TYPE)
	else:
		_node_type_picker.set_item_text(ring_index, RING_MASK_PICKER_LABEL)

	_node_type_picker.tooltip_text = "Planet-space masks output 0 to 1 from normalized direction. Geographic Region combines latitude and seam-safe longitude; Radial Area and Ring Area use great-circle angular distance and are seam/pole-safe. Use masks with Multiply or Mix in an authored displacement flow."


func _picker_index_for_metadata(metadata_value: String) -> int:
	if _node_type_picker == null:
		return -1
	for index: int in _node_type_picker.item_count:
		if String(_node_type_picker.get_item_metadata(index)) == metadata_value:
			return index
	return -1


func _spatial_mask_available_here() -> bool:
	return _graph != null and int(_graph.get(&"domain")) == GRAPH_SCRIPT.Domain.DISPLACEMENT \
		and not _is_native_branch_graph()


func _latitude_mask_available_here() -> bool:
	# Kept for Phase 41 regressions and downstream wrappers that used the original
	# helper name before longitude/region/radial/ring support was added.
	return _spatial_mask_available_here()


func _append_latitude_mask_node(position: Vector2) -> void:
	if _graph == null:
		return
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {})


func _append_longitude_mask_node(position: Vector2) -> void:
	if _graph == null:
		return
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {
		"axis":"longitude",
		"south_deg":-45.0,
		"north_deg":45.0,
		"feather_deg":5.0,
		"invert":false,
	})


func _append_geographic_region_node(position: Vector2) -> void:
	if _graph == null:
		return
	# One serialized node lowers to Latitude Band * Longitude Band in bytecode.
	# Region inversion is applied after that intersection, not to each axis.
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {
		"axis":"region",
		"south_deg":-30.0,
		"north_deg":30.0,
		"feather_deg":5.0,
		"west_deg":-45.0,
		"east_deg":45.0,
		"longitude_feather_deg":5.0,
		"invert":false,
	})


func _append_radial_mask_node(position: Vector2) -> void:
	if _graph == null:
		return
	# Radial Area is a true spherical cap: distance is measured along the sphere,
	# not independently in latitude/longitude. This stays correct at poles and seams.
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {
		"axis":"radial",
		"center_latitude_deg":0.0,
		"center_longitude_deg":0.0,
		"radius_deg":15.0,
		"feather_deg":5.0,
		"invert":false,
	})


func _append_ring_mask_node(position: Vector2) -> void:
	if _graph == null:
		return
	# Ring Area uses one center and an angular interval. Feather is outside-only on
	# both boundaries: inward into the hole and outward beyond the outer radius.
	_graph.call("add_node", LATITUDE_MASK_TYPE, position, {
		"axis":"ring",
		"center_latitude_deg":0.0,
		"center_longitude_deg":0.0,
		"inner_radius_deg":10.0,
		"outer_radius_deg":20.0,
		"feather_deg":5.0,
		"invert":false,
	})


func _create_latitude_mask_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "LATITUDE MASK"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	_add_port_row(graph_node, "Mask 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_degree_parameter(graph_node, node_id, "South edge", "south_deg",
		float(parameters.get("south_deg", -30.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "North edge", "north_deg",
		float(parameters.get("north_deg", 30.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Feather", "feather_deg",
		float(parameters.get("feather_deg", 5.0)), 0.0, 90.0, 0.5)

	var invert := CheckButton.new()
	invert.text = "Invert — affect outside the band"
	invert.button_pressed = bool(parameters.get("invert", false))
	invert.tooltip_text = "Off: 1 inside the latitude band. On: 1 outside the band."
	invert.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, "invert", value)
	)
	graph_node.add_child(invert)

	var note := Label.new()
	note.text = "Latitude uses the planet itself: 0° equator, +90° north pole, −90° south pole. Feather fades smoothly outside each edge."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 275.0
	note.modulate = Color(0.60, 0.70, 0.80)
	graph_node.add_child(note)
	_add_node_action_row(graph_node, node_id)


func _create_longitude_mask_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "LONGITUDE MASK"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	_add_port_row(graph_node, "Mask 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	# Legacy south/north keys are intentionally reused inside the serialized
	# LATITUDE_MASK variant. This keeps a single-opcode band ABI.
	_add_degree_parameter(graph_node, node_id, "West edge", "south_deg",
		float(parameters.get("south_deg", -45.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "East edge", "north_deg",
		float(parameters.get("north_deg", 45.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Feather", "feather_deg",
		float(parameters.get("feather_deg", 5.0)), 0.0, 180.0, 0.5)

	var invert := CheckButton.new()
	invert.text = "Invert — affect outside the band"
	invert.button_pressed = bool(parameters.get("invert", false))
	invert.tooltip_text = "Off: 1 inside the longitude arc. On: 1 outside the arc."
	invert.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, "invert", value)
	)
	graph_node.add_child(invert)

	var note := Label.new()
	note.text = "Longitude is measured around the planet with 0° on +Z. The band runs eastward from West edge to East edge; West > East crosses the ±180° seam. Feather is circular and seam-safe."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 300.0
	note.modulate = Color(0.60, 0.70, 0.80)
	graph_node.add_child(note)
	_add_node_action_row(graph_node, node_id)


func _create_geographic_region_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "GEOGRAPHIC REGION MASK"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	_add_port_row(graph_node, "Region 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_degree_parameter(graph_node, node_id, "South edge", "south_deg",
		float(parameters.get("south_deg", -30.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "North edge", "north_deg",
		float(parameters.get("north_deg", 30.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Latitude feather", "feather_deg",
		float(parameters.get("feather_deg", 5.0)), 0.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "West edge", "west_deg",
		float(parameters.get("west_deg", -45.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "East edge", "east_deg",
		float(parameters.get("east_deg", 45.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Longitude feather", "longitude_feather_deg",
		float(parameters.get("longitude_feather_deg", 5.0)), 0.0, 180.0, 0.5)

	var invert := CheckButton.new()
	invert.text = "Invert — affect outside the region"
	invert.button_pressed = bool(parameters.get("invert", false))
	invert.tooltip_text = "Off: 1 where both latitude and longitude are inside. On: complement the finished region."
	invert.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, "invert", value)
	)
	graph_node.add_child(invert)

	var note := Label.new()
	note.text = "A single geographic box on the sphere: latitude intersection × seam-safe longitude intersection. West > East crosses ±180°. Each axis feathers only outside its selected band."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 330.0
	note.modulate = Color(0.60, 0.70, 0.80)
	graph_node.add_child(note)
	_add_node_action_row(graph_node, node_id)


func _create_radial_mask_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "RADIAL AREA MASK"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	_add_port_row(graph_node, "Area 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_degree_parameter(graph_node, node_id, "Center latitude", "center_latitude_deg",
		float(parameters.get("center_latitude_deg", 0.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Center longitude", "center_longitude_deg",
		float(parameters.get("center_longitude_deg", 0.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Radius", "radius_deg",
		float(parameters.get("radius_deg", 15.0)), 0.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Feather", "feather_deg",
		float(parameters.get("feather_deg", 5.0)), 0.0, 180.0, 0.5)

	var invert := CheckButton.new()
	invert.text = "Invert — affect outside the area"
	invert.button_pressed = bool(parameters.get("invert", false))
	invert.tooltip_text = "Off: 1 inside the spherical cap. On: complement the finished radial area."
	invert.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, "invert", value)
	)
	graph_node.add_child(invert)

	var note := Label.new()
	note.text = "True great-circle radius on the planet. 0° longitude is +Z and +90° is +X. The mask stays circular across the ±180° seam and around the poles; feather fades only outside the radius."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 330.0
	note.modulate = Color(0.60, 0.70, 0.80)
	graph_node.add_child(note)
	_add_node_action_row(graph_node, node_id)


func _create_ring_mask_node(node_data: Dictionary) -> void:
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "RING AREA MASK"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)

	_add_port_row(graph_node, "Ring 0 → 1", false, true)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_degree_parameter(graph_node, node_id, "Center latitude", "center_latitude_deg",
		float(parameters.get("center_latitude_deg", 0.0)), -90.0, 90.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Center longitude", "center_longitude_deg",
		float(parameters.get("center_longitude_deg", 0.0)), -180.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Inner radius", "inner_radius_deg",
		float(parameters.get("inner_radius_deg", 10.0)), 0.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Outer radius", "outer_radius_deg",
		float(parameters.get("outer_radius_deg", 20.0)), 0.0, 180.0, 0.5)
	_add_degree_parameter(graph_node, node_id, "Feather", "feather_deg",
		float(parameters.get("feather_deg", 5.0)), 0.0, 180.0, 0.5)

	var invert := CheckButton.new()
	invert.text = "Invert — affect outside the ring"
	invert.button_pressed = bool(parameters.get("invert", false))
	invert.tooltip_text = "Off: 1 between inner and outer great-circle radii. On: complement the finished ring."
	invert.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, "invert", value)
	)
	graph_node.add_child(invert)

	var note := Label.new()
	note.text = "A seam/pole-safe spherical annulus. The requested band is fully selected from Inner radius through Outer radius. Feather fades only outside it: inward into the center hole and outward beyond the outer edge."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 345.0
	note.modulate = Color(0.60, 0.70, 0.80)
	graph_node.add_child(note)
	_add_node_action_row(graph_node, node_id)


func _add_latitude_parameter(node: GraphNode, node_id: String, label_text: String,
		key: String, value: float, minimum: float, maximum: float, step: float) -> void:
	# Backward-compatible helper retained for any downstream Phase 41 wrapper.
	_add_degree_parameter(node, node_id, label_text, key, value, minimum, maximum, step)


func _add_degree_parameter(node: GraphNode, node_id: String, label_text: String,
		key: String, value: float, minimum: float, maximum: float, step: float) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(345.0, 32.0)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 128.0
	row.add_child(label)
	var spin := SpinBox.new()
	spin.min_value = minimum
	spin.max_value = maximum
	spin.step = step
	spin.allow_lesser = false
	spin.allow_greater = false
	spin.suffix = "°"
	spin.value = clampf(value, minimum, maximum)
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spin.value_changed.connect(func(next_value: float) -> void:
		_set_node_parameter(node_id, key, next_value)
	)
	row.add_child(spin)
	node.add_child(row)
