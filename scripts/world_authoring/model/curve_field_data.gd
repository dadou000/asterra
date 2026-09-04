class_name CurveFieldData
## Shared data model + pure evaluation for the small (2-4 control point)
## response curves used by Biome Terrain's noise layers and Biome Texture's
## gradient bands -- see CurveFieldControl for the interactive widget that
## edits this data. Kept separate from both so the CPU displacement runtime
## (which must evaluate Biome Terrain's curve identically to the GPU, since
## that height feeds contact/AGL, not just rendering) does not need to depend
## on any UI Control script.
##
## A curve is a flat PackedFloat32Array of (x0,y0,x1,y1,...) pairs, sorted by
## x, x in [0,1], with the first point's x always 0 and the last point's x
## always 1 (CurveFieldControl enforces this while editing -- a caller reading
## raw authored data from elsewhere should not assume it holds). Segments
## between consecutive points interpolate via smoothstep (the standard
## 3t^2-2t^3 Hermite ease), which gives an automatic ease-in/ease-out with no
## separate tangent-handle authoring: GLSL's smoothstep() and Godot's global
## smoothstep() are the same curve, so the GPU and CPU sides need no
## correction term the way e.g. bp_noise's FMA fix does for raw multiplication.
##
## Capped at MAX_POINTS (4) rather than an arbitrary spline point count: two
## endpoints plus up to two interior shape points already covers the
## practical range for "gradient distribution" and "noise response" (an
## S-curve, a plateau, a sharpened peak) while keeping the GPU packing small
## (2 vec4 uniforms per curve) and the CPU/GPU parity trivial to keep exact.

const MAX_POINTS: int = 4
const MIN_POINTS: int = 2


static func identity() -> PackedFloat32Array:
	return PackedFloat32Array([0.0, 0.0, 1.0, 1.0])


static func point_count(points: PackedFloat32Array) -> int:
	return points.size() / 2


static func get_point(points: PackedFloat32Array, index: int) -> Vector2:
	return Vector2(points[index * 2], points[index * 2 + 1])


## Piecewise-smoothstep evaluation -- mirrored exactly by asterra_curve_eval
## in terrain_biome_profile.gdshaderinc and terrain_biome_texture.gdshaderinc
## (duplicated there rather than shared via #include, following this
## codebase's existing convention for code both shaderincs need: neither file
## has an include guard, and both end up included together in the same
## fragment shader, so a shared #include would double-declare it).
static func evaluate(points: PackedFloat32Array, x: float) -> float:
	var count: int = point_count(points)
	if count < MIN_POINTS:
		return clampf(x, 0.0, 1.0)
	x = clampf(x, 0.0, 1.0)
	for i: int in count - 1:
		var a: Vector2 = get_point(points, i)
		var b: Vector2 = get_point(points, i + 1)
		if x <= b.x or i == count - 2:
			var t: float = clampf((x - a.x) / maxf(b.x - a.x, 1e-5), 0.0, 1.0)
			return lerpf(a.y, b.y, smoothstep(0.0, 1.0, t))
	return get_point(points, count - 1).y


static func is_identity(points: PackedFloat32Array) -> bool:
	return point_count(points) == MIN_POINTS \
		and is_equal_approx(get_point(points, 0).x, 0.0) and is_zero_approx(get_point(points, 0).y) \
		and is_equal_approx(get_point(points, 1).x, 1.0) and is_equal_approx(get_point(points, 1).y, 1.0)


## Packs into the two vec4 uniforms terrain_biome_profile.gdshaderinc /
## terrain_biome_texture.gdshaderinc's asterra_curve_eval expects: (x0,y0,x1,y1)
## and (x2,y2,x3,y3), padding unused trailing slots by repeating the last
## authored point (never read past `point_count`, but keeps every array
## element defined rather than relying on driver-dependent uninitialized
## uniform behaviour).
static func pack_to_vec4_pair(points: PackedFloat32Array) -> Array:
	var padded: PackedFloat32Array = points.duplicate()
	while padded.size() < MAX_POINTS * 2:
		padded.append(padded[padded.size() - 2])
		padded.append(padded[padded.size() - 1])
	return [
		Vector4(padded[0], padded[1], padded[2], padded[3]),
		Vector4(padded[4], padded[5], padded[6], padded[7]),
	]


static func can_add_point(points: PackedFloat32Array) -> bool:
	return point_count(points) < MAX_POINTS


static func insert_point(points: PackedFloat32Array, x: float, y: float) -> PackedFloat32Array:
	var result: PackedFloat32Array = points.duplicate()
	if point_count(result) >= MAX_POINTS:
		return result
	x = clampf(x, 0.001, 0.999)
	var insert_index: int = point_count(result) - 1
	for i: int in point_count(result):
		if get_point(result, i).x > x:
			insert_index = i
			break
	result.insert(insert_index * 2, y)
	result.insert(insert_index * 2, x)
	return result


static func remove_point(points: PackedFloat32Array, index: int) -> PackedFloat32Array:
	if index <= 0 or index >= point_count(points) - 1:
		return points.duplicate() # endpoints are never removable
	var result: PackedFloat32Array = points.duplicate()
	result.remove_at(index * 2)
	result.remove_at(index * 2)
	return result


## Moves one control point, clamping its x between its neighbours (or to 0/1
## exactly for an endpoint, which never changes x) and its y to [0, 1].
static func move_point(points: PackedFloat32Array, index: int, x: float, y: float) -> PackedFloat32Array:
	var result: PackedFloat32Array = points.duplicate()
	var count: int = point_count(result)
	var clamped_x: float = x
	if index == 0:
		clamped_x = 0.0
	elif index == count - 1:
		clamped_x = 1.0
	else:
		var min_x: float = get_point(result, index - 1).x + 0.001
		var max_x: float = get_point(result, index + 1).x - 0.001
		clamped_x = clampf(x, minf(min_x, max_x), maxf(min_x, max_x))
	result[index * 2] = clamped_x
	result[index * 2 + 1] = clampf(y, 0.0, 1.0)
	return result
