extends "res://scripts/character/dense_facial_hair_groom.gd"

## Visible-shell correction for the direct surface beard binder.
##
## The first direct-triangle sampler correctly removed per-hair projection, but
## an (x,y)-only anatomical mask could still select triangles on the side/back
## of the neck that happen to share the same screen-space coordinates as the
## jaw. It could also spend too much of the requested follicle population on
## those hidden layers, leaving the true chin sparse.
##
## This pass builds a cheap front-depth atlas once from the neutral face
## triangles. Only the outer visible face/jaw shell is admitted to the beard
## sampler. The actual follicles are still born directly on source triangles,
## so there are no per-hair raycasts and every root retains its exact triangle
## and barycentric coordinates for facial animation.

const BEARD_SHELL_GRID_MIN := 0.010
const BEARD_SHELL_GRID_HEIGHT_SCALE := 0.007
const BEARD_SHELL_FRONT_TOLERANCE := 0.010
const BEARD_SHELL_SIDE_TOLERANCE := 0.030

var _beard_shell_grid: Dictionary = {}
var _beard_shell_candidates := 0
var _beard_shell_rejected := 0

func _clear_surface_beard_cache() -> void:
	_beard_shell_grid.clear()
	_beard_shell_candidates = 0
	_beard_shell_rejected = 0
	super._clear_surface_beard_cache()

func _beard_shell_cell(point: Vector3, step: float) -> Vector2i:
	return Vector2i(
		int(floor(point.x / step)),
		int(floor(point.y / step))
	)

func _triangle_shell_cells(a: Vector3, b: Vector3, c: Vector3, step: float) -> Array[Vector2i]:
	var min_x: float = minf(a.x, minf(b.x, c.x))
	var max_x: float = maxf(a.x, maxf(b.x, c.x))
	var min_y: float = minf(a.y, minf(b.y, c.y))
	var max_y: float = maxf(a.y, maxf(b.y, c.y))
	var min_cell: Vector2i = _beard_shell_cell(Vector3(min_x, min_y, 0.0), step)
	var max_cell: Vector2i = _beard_shell_cell(Vector3(max_x, max_y, 0.0), step)
	var cells: Array[Vector2i] = []
	for gx in range(min_cell.x, max_cell.x + 1):
		for gy in range(min_cell.y, max_cell.y + 1):
			cells.append(Vector2i(gx, gy))
	return cells

func _ensure_beard_surface_sampler() -> bool:
	if _beard_sampler_ready:
		return not _beard_sampler_triangles.is_empty()
	var started_usec: int = Time.get_ticks_usec()
	_beard_sampler_ready = true

	if not _ensure_morph_source_cache() or _head.is_empty() or _mount == null:
		_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
		return false

	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var rz: float = maxf(float(_head["rz"]), 0.001)
	var front: Vector3 = Vector3(0.0, 0.0, _front_sign)
	var shell_step: float = maxf(BEARD_SHELL_GRID_MIN, _character_height * BEARD_SHELL_GRID_HEIGHT_SCALE)
	var candidates: Array[Dictionary] = []
	_beard_shell_grid.clear()

	# Pass 1: collect plausible lower-face triangles and build an XY depth atlas
	# containing the front-most neutral skin depth for each small cell.
	for tri_index in _morph_triangles.size():
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var centroid: Vector3 = (a + b + c) / 3.0
		var ax: float = absf(centroid.x - center.x) / rx
		var down: float = (center.y - centroid.y) / ry
		if ax > 0.96 or down < 0.005 or down > 0.90:
			continue

		var cross_value: Vector3 = (b - a).cross(c - a)
		var twice_area: float = cross_value.length()
		if twice_area < 0.00000001:
			continue
		var normal: Vector3 = cross_value / twice_area
		var radial: Vector3 = centroid - center
		if radial.length_squared() > 0.000001 and normal.dot(radial) < 0.0:
			normal = -normal
		if normal.dot(front) < -0.48:
			continue

		# A beard may wrap around the mandibular angle, but it should not continue
		# down the lateral neck. At very low Y only the central/front chin shell is
		# valid; this removes the vertical neck patch seen in the first binder test.
		var front_depth_norm: float = (centroid.z - center.z) * _front_sign / rz
		if down > 0.72 and ax > 0.50:
			continue
		if down > 0.58:
			var side_mix: float = smoothstep(0.38, 0.84, ax)
			var min_front_depth: float = lerpf(0.18, -0.08, side_mix)
			if front_depth_norm < min_front_depth:
				continue

		var cells: Array[Vector2i] = _triangle_shell_cells(a, b, c, shell_step)
		var front_depth: float = maxf(a.dot(front), maxf(b.dot(front), c.dot(front)))
		for cell in cells:
			var existing: float = float(_beard_shell_grid.get(cell, -INF))
			if front_depth > existing:
				_beard_shell_grid[cell] = front_depth

		candidates.append({
			"tri": tri_index,
			"area": twice_area * 0.5,
			"depth": centroid.dot(front),
			"ax": ax,
			"down": down,
			"cells": cells
		})

	_beard_shell_candidates = candidates.size()
	var total_area := 0.0

	# Pass 2: retain only triangles close to the front-most layer in at least one
	# cell they cover. Tolerance grows toward the side of the face so sideburn and
	# mandibular-angle triangles survive while recessed neck layers do not.
	for candidate in candidates:
		var ax: float = float(candidate["ax"])
		var down: float = float(candidate["down"])
		var depth: float = float(candidate["depth"])
		var cells: Array[Vector2i] = candidate["cells"]
		var nearest_front_gap := INF
		for cell in cells:
			var shell_depth: float = float(_beard_shell_grid.get(cell, depth))
			nearest_front_gap = minf(nearest_front_gap, shell_depth - depth)

		var side_factor: float = smoothstep(0.42, 0.88, ax)
		var tolerance: float = lerpf(BEARD_SHELL_FRONT_TOLERANCE, BEARD_SHELL_SIDE_TOLERANCE, side_factor)
		if down > 0.56:
			tolerance += 0.004
		if nearest_front_gap > tolerance:
			_beard_shell_rejected += 1
			continue

		var tri_index: int = int(candidate["tri"])
		var area: float = float(candidate["area"])
		total_area += area
		_beard_sampler_triangles.append(tri_index)
		_beard_sampler_cumulative_area.append(total_area)

	_beard_sampler_total_area = total_area
	_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return not _beard_sampler_triangles.is_empty() and total_area > 0.0

## x = follicle acceptance weight
## y = desired lateral flow in world X
## z = desired vertical flow in world Y
## w = chin-length blend
func _beard_field(world_point: Vector3, coverage: float, fullness: float) -> Vector4:
	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var rz: float = maxf(float(_head["rz"]), 0.001)
	var dx: float = world_point.x - center.x
	var ax: float = absf(dx) / rx
	var down: float = (center.y - world_point.y) / ry
	if ax > 0.90 or down < 0.02 or down > 0.87:
		return Vector4.ZERO

	# Depth is part of the anatomical coordinate now. This prevents a point on
	# the neck behind the jaw from receiving the same beard weight as a cheek or
	# chin point with identical X/Y.
	var front_depth: float = (world_point.z - center.z) * _front_sign / rz
	if down > 0.60:
		var lower_side_mix: float = smoothstep(0.34, 0.82, ax)
		var lower_min_front: float = lerpf(0.16, -0.08, lower_side_mix)
		if front_depth < lower_min_front:
			return Vector4.ZERO
	if down > 0.73 and ax > 0.48:
		return Vector4.ZERO

	var mouth_center_y: float = center.y - ry * 0.22
	var mouth_x: float = absf(dx) / maxf(rx * 0.40, 0.001)
	var mouth_y: float = absf(world_point.y - mouth_center_y) / maxf(ry * 0.125, 0.001)
	if mouth_x * mouth_x + mouth_y * mouth_y < 1.0:
		return Vector4.ZERO

	var side: float = -1.0 if dx < 0.0 else 1.0
	var outerness: float = clampf((ax - 0.25) / 0.60, 0.0, 1.0)

	# Cheek: broad continuous field with a soft upper edge that rises toward the
	# sideburn. It overlaps the jaw instead of ending before it.
	var cheek_top: float = lerpf(0.235, 0.085, coverage) - 0.050 * outerness
	var cheek_bottom := 0.625
	var cheek_vertical: float = smoothstep(cheek_top, cheek_top + 0.050, down)
	cheek_vertical *= 1.0 - smoothstep(cheek_bottom - 0.055, cheek_bottom, down)
	var cheek_side: float = smoothstep(0.235, 0.315, ax)
	cheek_side *= 1.0 - smoothstep(0.82, 0.90, ax)
	var cheek: float = cheek_vertical * cheek_side * lerpf(0.74, 1.0, coverage)

	# Jaw: widen the band slightly toward the central chin and extend it lower so
	# the front jaw remains fully populated in three-quarter views.
	var jaw_u: float = clampf(ax / 0.84, 0.0, 1.0)
	var jaw_center: float = lerpf(0.745, 0.515, pow(jaw_u, 1.30))
	var jaw_half_width: float = 0.092 * lerpf(0.86, 1.18, clampf(fullness, 0.15, 1.35))
	var jaw_distance: float = absf(down - jaw_center)
	var jaw: float = 1.0 - smoothstep(jaw_half_width * 0.62, jaw_half_width, jaw_distance)
	jaw *= 1.0 - smoothstep(0.83, 0.90, ax)

	# Chin/goatee: deliberately wider and lower than the first direct-surface
	# version. This is what restores the missing front chin while keeping the neck
	# gate above independent from beard density.
	var chin_x: float = 1.0 - smoothstep(0.34, 0.43, ax)
	var chin_y: float = smoothstep(0.305, 0.375, down)
	chin_y *= 1.0 - smoothstep(0.82, 0.87, down)
	var chin: float = chin_x * chin_y

	var connector_x: float = smoothstep(0.14, 0.22, ax) * (1.0 - smoothstep(0.52, 0.60, ax))
	var connector_y: float = smoothstep(0.36, 0.43, down) * (1.0 - smoothstep(0.66, 0.72, down))
	var connector: float = connector_x * connector_y

	var weight: float = maxf(maxf(cheek, jaw), maxf(chin, connector))
	weight *= clampf(0.80 + fullness * 0.18, 0.72, 1.0)
	if weight <= 0.0001:
		return Vector4.ZERO

	var lateral := 0.0
	if chin >= jaw and chin >= cheek and chin >= connector:
		lateral = side * ax * 0.08
	elif connector >= cheek and connector >= jaw:
		lateral = -side * 0.055
	elif jaw >= cheek:
		lateral = -side * 0.20 * jaw_u
	else:
		var cheek_down: float = clampf((down - cheek_top) / maxf(cheek_bottom - cheek_top, 0.001), 0.0, 1.0)
		lateral = -side * lerpf(0.17, 0.07, cheek_down)

	var chin_mix: float = maxf(chin, connector * 0.55)
	chin_mix *= smoothstep(0.40, 0.78, down)
	return Vector4(clampf(weight, 0.0, 1.0), lateral, -1.0, clampf(chin_mix, 0.0, 1.0))

func diagnostics() -> String:
	var base: String = super.diagnostics()
	return "%s • beard shell %d candidates/%d rear layers rejected" % [
		base,
		_beard_shell_candidates,
		_beard_shell_rejected
	]
