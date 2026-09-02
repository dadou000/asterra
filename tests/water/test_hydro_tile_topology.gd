extends Node
## CPU-only exact cube-sphere adjacency and momentum-frame tests for Phase 3.

const LEVELS := [0, 1, 2, 5, 10, 20, HydroTileKey.MAX_LEVEL]


func _ready() -> void:
	_test_same_face_neighbors()
	_test_all_cube_seams()
	_test_momentum_frame_transforms()
	print("HYDRO_TILE_TOPOLOGY: PASS")
	get_tree().quit(0)


func _test_same_face_neighbors() -> void:
	var key := HydroTileKey.new(CubeSphere.FACE_PX, 6, 20, 31)
	for direction in 4:
		var link := HydroTileTopology.neighbor(key, direction)
		_require(not link.is_empty(), "same-face neighbor missing dir=%d" % direction)
		_require(not bool(link["crossed_face"]), "interior neighbor incorrectly crossed face")
		_require(int(link["destination_direction"])
			== HydroTileTopology.opposite_direction(direction),
			"same-face destination boundary mismatch")
		_require(int(link["edge_orientation"]) == 1,
			"same-face edge orientation must be +1")
		var back := HydroTileTopology.neighbor(link["key"], int(link["destination_direction"]))
		_require(not back.is_empty() and (back["key"] as HydroTileKey).equals(key),
			"same-face reciprocity failed")

		var q := Vector2(1.75, -0.625)
		var transformed := HydroEdgeFrame.momentum_across_link(q, direction, link)
		_require(transformed.is_equal_approx(q),
			"same-face momentum transform must be identity dir=%d got=%s" % [
				direction, str(transformed)])


func _test_all_cube_seams() -> void:
	var seam_links := 0
	for level in LEVELS:
		var side := 1 << int(level)
		var indices := _edge_samples(side)
		for face in 6:
			for direction in 4:
				for index in indices:
					var key := _edge_key(face, int(level), direction, index)
					var link := HydroTileTopology.neighbor(key, direction)
					_require(not link.is_empty(),
						"missing seam neighbor %s dir=%s" % [
							str(key), HydroTileTopology.direction_name(direction)])
					var dest := link["key"] as HydroTileKey
					_require(bool(link["crossed_face"]),
						"edge tile did not report crossed_face: %s" % str(key))
					_require(dest.face != key.face and dest.level == key.level,
						"invalid destination face/level: %s -> %s" % [str(key), str(dest)])
					var dest_dir := int(link["destination_direction"])
					_require(dest_dir >= 0 and dest_dir < 4,
						"invalid destination boundary")
					var orientation := int(link["edge_orientation"])
					_require(abs(orientation) == 1, "invalid edge orientation")

					var back := HydroTileTopology.neighbor(dest, dest_dir)
					_require(not back.is_empty(), "reciprocal seam link missing")
					var back_key := back["key"] as HydroTileKey
					_require(back_key.equals(key),
						"seam reciprocity failed: %s -> %s -> %s" % [
							str(key), str(dest), str(back_key)])
					_require(int(back["destination_direction"]) == direction,
						"reciprocal destination boundary mismatch")
					_require(int(back["edge_orientation"]) == orientation,
						"reciprocal edge orientation mismatch")

					# At render-meaningful levels, corresponding edge centres should be
					# the same physical direction to float32 precision.
					if int(level) <= 10:
						var a := HydroTileTopology.edge_center_direction(key, direction)
						var b := HydroTileTopology.edge_center_direction(dest, dest_dir)
						_require(a.dot(b) > 0.999999,
							"physical seam mismatch dot=%.9g %s -> %s" % [
								a.dot(b), str(key), str(dest)])
					seam_links += 1
	_require(seam_links > 0, "no seam links tested")


func _test_momentum_frame_transforms() -> void:
	# Test every seam at a representative level. The transform must preserve
	# magnitude, map destination outward normal to -source outward normal, preserve
	# or reverse the edge tangent according to topology, and be exactly reciprocal.
	var level := 6
	var side := 1 << level
	for face in 6:
		for direction in 4:
			for index in _edge_samples(side):
				var source := _edge_key(face, level, direction, index)
				var link := HydroTileTopology.neighbor(source, direction)
				_require(not link.is_empty(), "momentum test missing topology link")
				var destination := link["key"] as HydroTileKey
				var destination_direction := int(link["destination_direction"])
				var orientation := int(link["edge_orientation"])

				var nd := HydroEdgeFrame.edge_normal(destination_direction)
				var td := HydroEdgeFrame.edge_tangent(destination_direction)
				var ns := HydroEdgeFrame.edge_normal(direction)
				var ts := HydroEdgeFrame.edge_tangent(direction)
				var mapped_normal := HydroEdgeFrame.momentum_to_source(
					nd, direction, destination_direction, orientation)
				var mapped_tangent := HydroEdgeFrame.momentum_to_source(
					td, direction, destination_direction, orientation)
				_require(mapped_normal.distance_to(-ns) < 1.0e-6,
					"destination normal mapped incorrectly %s dir=%d" % [str(source), direction])
				_require(mapped_tangent.distance_to(ts * float(orientation)) < 1.0e-6,
					"destination tangent mapped incorrectly %s dir=%d" % [str(source), direction])

				var q_destination := Vector2(2.375, -0.8125)
				var q_source := HydroEdgeFrame.momentum_to_source(q_destination,
					direction, destination_direction, orientation)
				_require(absf(q_source.length() - q_destination.length()) < 1.0e-6,
					"seam momentum transform changed vector magnitude")

				var back := HydroTileTopology.neighbor(destination, destination_direction)
				var roundtrip := HydroEdgeFrame.momentum_to_source(q_source,
					destination_direction, int(back["destination_direction"]),
					int(back["edge_orientation"]))
				_require(roundtrip.distance_to(q_destination) < 1.0e-6,
					"seam momentum transform not reciprocal: q=%s roundtrip=%s" % [
						str(q_destination), str(roundtrip)])


func _edge_samples(side: int) -> Array[int]:
	var values: Array[int] = []
	for candidate in [0, side / 4, side / 2, (side * 3) / 4, side - 1]:
		var i := clampi(int(candidate), 0, side - 1)
		if not values.has(i):
			values.append(i)
	return values


func _edge_key(face: int, level: int, direction: int, index: int) -> HydroTileKey:
	var side := 1 << level
	match direction:
		HydroTileTopology.DIR_WEST:
			return HydroTileKey.new(face, level, 0, index)
		HydroTileTopology.DIR_EAST:
			return HydroTileKey.new(face, level, side - 1, index)
		HydroTileTopology.DIR_SOUTH:
			return HydroTileKey.new(face, level, index, 0)
		HydroTileTopology.DIR_NORTH:
			return HydroTileKey.new(face, level, index, side - 1)
	return HydroTileKey.new(face, level, 0, 0)


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	push_error("HYDRO_TILE_TOPOLOGY: " + message)
	get_tree().quit(1)
	assert(condition, message)
