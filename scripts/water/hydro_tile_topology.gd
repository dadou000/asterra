class_name HydroTileTopology
extends RefCounted
## Exact same-level tile adjacency on Asterra's equi-angular cube-sphere.
##
## No seam table is hard-coded. Cross-face neighbors are derived from the same
## CubeSphere face frames/mapping used by terrain: step a fraction of one tile
## across the source boundary in face UV, invert the resulting direction, then
## identify the destination tile/boundary. This keeps hydrology topology coupled
## to the engine's actual cube-sphere convention.
##
## A returned link contains:
##   key                   destination HydroTileKey
##   source_direction      W/E/S/N on source
##   destination_direction W/E/S/N on destination
##   edge_orientation      +1 if increasing edge parameter agrees, -1 if reversed
##   crossed_face          true only for a cube seam

const DIR_WEST := 0
const DIR_EAST := 1
const DIR_SOUTH := 2
const DIR_NORTH := 3

const _CROSS_FRACTION := 0.25
const _ORIENT_FRACTION := 0.22


static func neighbor(key: HydroTileKey, direction: int) -> Dictionary:
	if key == null or direction < DIR_WEST or direction > DIR_NORTH:
		return {}
	var side := 1 << key.level
	var dx := 0
	var dy := 0
	match direction:
		DIR_WEST: dx = -1
		DIR_EAST: dx = 1
		DIR_SOUTH: dy = -1
		DIR_NORTH: dy = 1

	var nx := key.x + dx
	var ny := key.y + dy
	if nx >= 0 and ny >= 0 and nx < side and ny < side:
		return {
			"key": HydroTileKey.new(key.face, key.level, nx, ny),
			"source_direction": direction,
			"destination_direction": opposite_direction(direction),
			"edge_orientation": 1,
			"crossed_face": false,
		}

	return _cross_face_neighbor(key, direction)


static func opposite_direction(direction: int) -> int:
	match direction:
		DIR_WEST: return DIR_EAST
		DIR_EAST: return DIR_WEST
		DIR_SOUTH: return DIR_NORTH
		DIR_NORTH: return DIR_SOUTH
	return -1


static func direction_name(direction: int) -> String:
	match direction:
		DIR_WEST: return "west"
		DIR_EAST: return "east"
		DIR_SOUTH: return "south"
		DIR_NORTH: return "north"
	return "invalid"


static func tile_center_face_uv(key: HydroTileKey) -> Vector2:
	var side := float(1 << key.level)
	return Vector2(
		-1.0 + 2.0 * (float(key.x) + 0.5) / side,
		-1.0 + 2.0 * (float(key.y) + 0.5) / side)


static func tile_bounds_face_uv(key: HydroTileKey) -> Rect2:
	var side := float(1 << key.level)
	var size := 2.0 / side
	return Rect2(Vector2(-1.0 + float(key.x) * size,
		-1.0 + float(key.y) * size), Vector2(size, size))


static func edge_center_direction(key: HydroTileKey, direction: int) -> Vector3:
	var uv := _edge_face_uv(key, direction, 0.0, 0.0)
	return CubeSphere.face_uv_to_dir(key.face, uv.x, uv.y)


static func _cross_face_neighbor(key: HydroTileKey, direction: int) -> Dictionary:
	var side := 1 << key.level
	var tile_uv := 2.0 / float(side)
	# A quarter-cell outward step is large enough to make destination-face choice
	# numerically unambiguous, while remaining inside the one adjacent tile.
	var crossed_uv := _edge_face_uv(key, direction, 0.0,
		tile_uv * _CROSS_FRACTION)
	var mapped := _face_uv_to_face_uv_unclamped(key.face, crossed_uv.x, crossed_uv.y)
	if mapped.is_empty():
		return {}
	var dest_face := int(mapped[0])
	var dest_uv := Vector2(float(mapped[1]), float(mapped[2]))
	var dest_x := clampi(int(floor((dest_uv.x + 1.0) * 0.5 * float(side))), 0, side - 1)
	var dest_y := clampi(int(floor((dest_uv.y + 1.0) * 0.5 * float(side))), 0, side - 1)
	var dest_direction := _nearest_face_boundary(dest_uv)
	if dest_direction < 0:
		return {}

	# Determine seam ordering from geometry rather than face-name assumptions.
	# Source edge parameter increases V on W/E edges and U on S/N edges. Map two
	# nearby points across the seam and compare the destination edge parameter.
	var param_delta := tile_uv * _ORIENT_FRACTION
	var uv_minus := _edge_face_uv(key, direction, -param_delta,
		tile_uv * _CROSS_FRACTION)
	var uv_plus := _edge_face_uv(key, direction, param_delta,
		tile_uv * _CROSS_FRACTION)
	var mapped_minus := _face_uv_to_face_uv_unclamped(key.face, uv_minus.x, uv_minus.y)
	var mapped_plus := _face_uv_to_face_uv_unclamped(key.face, uv_plus.x, uv_plus.y)
	if mapped_minus.is_empty() or mapped_plus.is_empty() \
			or int(mapped_minus[0]) != dest_face or int(mapped_plus[0]) != dest_face:
		return {}
	var minus_uv := Vector2(float(mapped_minus[1]), float(mapped_minus[2]))
	var plus_uv := Vector2(float(mapped_plus[1]), float(mapped_plus[2]))
	var minus_param := minus_uv.y if dest_direction in [DIR_WEST, DIR_EAST] else minus_uv.x
	var plus_param := plus_uv.y if dest_direction in [DIR_WEST, DIR_EAST] else plus_uv.x
	var orientation := 1 if plus_param >= minus_param else -1

	return {
		"key": HydroTileKey.new(dest_face, key.level, dest_x, dest_y),
		"source_direction": direction,
		"destination_direction": dest_direction,
		"edge_orientation": orientation,
		"crossed_face": true,
	}


## Source edge point in face UV. edge_parameter_delta moves along the boundary;
## outward_delta moves perpendicular to it, positive *outside* the source face.
static func _edge_face_uv(key: HydroTileKey, direction: int,
		edge_parameter_delta: float, outward_delta: float) -> Vector2:
	var bounds := tile_bounds_face_uv(key)
	var center := bounds.position + bounds.size * 0.5
	match direction:
		DIR_WEST:
			return Vector2(bounds.position.x - outward_delta,
				center.y + edge_parameter_delta)
		DIR_EAST:
			return Vector2(bounds.end.x + outward_delta,
				center.y + edge_parameter_delta)
		DIR_SOUTH:
			return Vector2(center.x + edge_parameter_delta,
				bounds.position.y - outward_delta)
		DIR_NORTH:
			return Vector2(center.x + edge_parameter_delta,
				bounds.end.y + outward_delta)
	return center


static func _nearest_face_boundary(uv: Vector2) -> int:
	var distances := [
		absf(uv.x + 1.0),
		absf(uv.x - 1.0),
		absf(uv.y + 1.0),
		absf(uv.y - 1.0),
	]
	var best := 0
	for i in range(1, 4):
		if float(distances[i]) < float(distances[best]):
			best = i
	return best


## Double-scalar version of CubeSphere face_uv_to_dir + dir_to_face_uv. Vector3
## is float32; hydrology tile addresses can be much finer than a render vertex, so
## seam topology keeps the mapping ratios in GDScript's 64-bit float domain.
static func _face_uv_to_face_uv_unclamped(face: int, u: float, v: float) -> Array:
	if face < 0 or face >= 6:
		return []
	var tu := tan(u * CubeSphere.Q)
	var tv := tan(v * CubeSphere.Q)
	var axis: Vector3 = CubeSphere.AXIS[face]
	var right: Vector3 = CubeSphere.RIGHT[face]
	var up: Vector3 = CubeSphere.UP[face]
	var x := float(axis.x) + float(right.x) * tu + float(up.x) * tv
	var y := float(axis.y) + float(right.y) * tu + float(up.y) * tv
	var z := float(axis.z) + float(right.z) * tu + float(up.z) * tv
	return _raw_dir_to_face_uv(x, y, z)


static func _raw_dir_to_face_uv(x: float, y: float, z: float) -> Array:
	var ax := absf(x)
	var ay := absf(y)
	var az := absf(z)
	var face: int
	if ax >= ay and ax >= az:
		face = CubeSphere.FACE_PX if x > 0.0 else CubeSphere.FACE_NX
	elif ay >= az:
		face = CubeSphere.FACE_PY if y > 0.0 else CubeSphere.FACE_NY
	else:
		face = CubeSphere.FACE_PZ if z > 0.0 else CubeSphere.FACE_NZ
	var axis: Vector3 = CubeSphere.AXIS[face]
	var right: Vector3 = CubeSphere.RIGHT[face]
	var up: Vector3 = CubeSphere.UP[face]
	var a := float(axis.x) * x + float(axis.y) * y + float(axis.z) * z
	if absf(a) < 1.0e-15:
		a = 1.0e-15 if a >= 0.0 else -1.0e-15
	var ru := float(right.x) * x + float(right.y) * y + float(right.z) * z
	var rv := float(up.x) * x + float(up.y) * y + float(up.z) * z
	return [face, atan(ru / a) / CubeSphere.Q, atan(rv / a) / CubeSphere.Q]
