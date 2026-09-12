extends Node
## CPU-only validation of the GPU connectivity buffer encoding.
## Exercises every cube face/edge without requiring a RenderingDevice.


func _ready() -> void:
	_test_resident_seam_links()
	_test_nonresident_neighbor_stays_closed()
	print("SPARSE_HYDRO_CONNECTIVITY: PASS")
	get_tree().quit(0)


func _test_resident_seam_links() -> void:
	const LEVEL := 5
	var side := 1 << LEVEL
	var reversed_seen := 0
	var links_tested := 0
	for face in 6:
		for direction in 4:
			for edge_index in [0, side / 3, side / 2, side - 1]:
				var source := _edge_key(face, LEVEL, direction, int(edge_index))
				var topology := HydroTileTopology.neighbor(source, direction)
				_require(not topology.is_empty(), "missing topology seam link")
				var destination := topology["key"] as HydroTileKey
				var pool := HydroTilePool.new(2)
				_require(pool.allocate(source, 0) == 0, "source slot allocation failed")
				_require(pool.allocate(destination, 0) == 1, "destination slot allocation failed")

				var arrays := SparseHydroConnectivityGPU.build_arrays(pool)
				var slots := arrays["neighbor_slots"] as PackedInt32Array
				var encoded := arrays["neighbor_links"] as PackedInt32Array
				var source_index := direction
				_require(slots[source_index] == 1,
					"resident seam neighbor not encoded face=%d dir=%d" % [face, direction])
				var link_value := encoded[source_index]
				_require(SparseHydroConnectivityGPU.unpack_destination_direction(link_value)
					== int(topology["destination_direction"]),
					"destination edge encoding mismatch")
				var expected_reversed := int(topology["edge_orientation"]) < 0
				_require(SparseHydroConnectivityGPU.unpack_reversed(link_value) == expected_reversed,
					"edge reversal encoding mismatch")
				if expected_reversed:
					reversed_seen += 1

				var destination_direction := int(topology["destination_direction"])
				var destination_index := 4 + destination_direction
				_require(slots[destination_index] == 0,
					"reciprocal resident seam slot missing")
				links_tested += 1

	_require(links_tested > 0, "no seam connectivity links tested")
	_require(reversed_seen > 0,
		"cube seam suite did not encounter a reversed edge; reversal bit remains untested")


func _test_nonresident_neighbor_stays_closed() -> void:
	var pool := HydroTilePool.new(2)
	var source := HydroTileKey.new(CubeSphere.FACE_PZ, 5, (1 << 5) - 1, 9)
	_require(pool.allocate(source, 0) == 0, "nonresident source allocation failed")
	var arrays := SparseHydroConnectivityGPU.build_arrays(pool)
	var slots := arrays["neighbor_slots"] as PackedInt32Array
	var encoded := arrays["neighbor_links"] as PackedInt32Array
	_require(slots[HydroTileTopology.DIR_EAST] == -1,
		"nonresident topological neighbor must stay disconnected")
	_require(encoded[HydroTileTopology.DIR_EAST] == -1,
		"nonresident neighbor must not publish a stale link descriptor")


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
	push_error("SPARSE_HYDRO_CONNECTIVITY: " + message)
	get_tree().quit(1)
	assert(condition, message)
