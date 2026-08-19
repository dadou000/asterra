class_name PlanetGrid
extends RefCounted
## The discrete global field grid: 6 cube-sphere faces of res x res cells.
##
## All macro-scale generation passes (geology, erosion, hydrology, climate, soil,
## biome, suitability) operate on this single flat index space so that they can
## share neighbour topology. Cross-face adjacency is resolved geometrically.
##
## Continuous field lookup deliberately does *not* pick one cube face and trust
## its 2-D bilinear filter at an edge. Near an edge (or a 3-face cube corner) we
## evaluate every relevant face and blend them in direction space. That is the
## CPU equivalent of seamless cubemap filtering: the result depends only on the
## spherical direction, never on which face happened to win the major-axis tie.

var res: int
var radius: float
var cell_count: int          ## 6 * res * res
## Interleaved xyz unit direction per cell.
var dirs: PackedFloat32Array
## 8 neighbour cell indices per cell, order: E, NE, N, NW, W, SW, S, SE.
var nbr: PackedInt32Array
## Approximate cell edge length in metres.
var cell_size: PackedFloat32Array

const OFFS := [
	Vector2i(1, 0), Vector2i(1, 1), Vector2i(0, 1), Vector2i(-1, 1),
	Vector2i(-1, 0), Vector2i(-1, -1), Vector2i(0, -1), Vector2i(1, -1),
]

## Width of the cross-face blend in macro cells. Two cells is wide enough that
## the first derivative also relaxes instead of merely hiding a one-pixel crack,
## while still being tiny compared with the planetary features in these fields.
const SEAM_BLEND_CELLS := 2.0

func _init(p_res: int, p_radius: float) -> void:
	res = p_res
	radius = p_radius
	cell_count = 6 * res * res
	_build()

func idx(face: int, i: int, j: int) -> int:
	return face * res * res + j * res + i

func cell_uv(i: int, j: int) -> Vector2:
	var s := 2.0 / float(res)
	return Vector2((float(i) + 0.5) * s - 1.0, (float(j) + 0.5) * s - 1.0)

func cell_dir(c: int) -> Vector3:
	var o := c * 3
	return Vector3(dirs[o], dirs[o + 1], dirs[o + 2])

## Flat index of the cell containing a direction.
func dir_to_index(d: Vector3) -> int:
	var fuv := CubeSphere.dir_to_face_uv(d)
	var face: int = fuv[0]
	var i := int(floor((fuv[1] * 0.5 + 0.5) * res))
	var j := int(floor((fuv[2] * 0.5 + 0.5) * res))
	return idx(face, clampi(i, 0, res - 1), clampi(j, 0, res - 1))

func _build() -> void:
	dirs = PackedFloat32Array()
	dirs.resize(cell_count * 3)
	cell_size = PackedFloat32Array()
	cell_size.resize(cell_count)
	nbr = PackedInt32Array()
	nbr.resize(cell_count * 8)

	for face in 6:
		for j in res:
			for i in res:
				var c := idx(face, i, j)
				var uv := cell_uv(i, j)
				var d := CubeSphere.face_uv_to_dir(face, uv.x, uv.y)
				var o := c * 3
				dirs[o] = d.x
				dirs[o + 1] = d.y
				dirs[o + 2] = d.z

	var s := 2.0 / float(res)
	for face in 6:
		for j in res:
			for i in res:
				var c := idx(face, i, j)
				var base := c * 8
				var uv := cell_uv(i, j)
				for k in 8:
					var off: Vector2i = OFFS[k]
					var ni := i + off.x
					var nj := j + off.y
					if ni >= 0 and ni < res and nj >= 0 and nj < res:
						nbr[base + k] = idx(face, ni, nj)
					else:
						# Off-face: build the virtual (u,v), project to the sphere,
						# then invert to find which face actually owns that cell.
						var nu := uv.x + float(off.x) * s
						var nv := uv.y + float(off.y) * s
						var nd := CubeSphere.face_uv_to_dir(face, nu, nv)
						nbr[base + k] = dir_to_index(nd)
				# Cell size from the two axis-aligned neighbours.
				var d0 := cell_dir(c)
				var de := cell_dir(nbr[base + 0])
				var dn := cell_dir(nbr[base + 2])
				var le := d0.distance_to(de) * radius
				var ln := d0.distance_to(dn) * radius
				cell_size[c] = maxf(1.0, (le + ln) * 0.5)

## Continuous scalar lookup on the sphere.
##
## Away from cube boundaries only the dominant face has non-zero weight, so the
## cost is the same four taps as ordinary bilinear filtering. Within roughly two
## macro cells of an edge, the adjacent face joins smoothly. At a cube corner all
## three faces receive equal weight. Because the weights are functions of |d|
## rather than a selected face, crossing a major-axis tie cannot create a seam.
func sample_bilinear(field: PackedFloat32Array, d: Vector3) -> float:
	var w := _seam_weights(d)
	var sum := 0.0
	var total := 0.0
	if w.x > 0.0:
		var face_x := CubeSphere.FACE_PX if d.x >= 0.0 else CubeSphere.FACE_NX
		sum += _sample_face_float(field, d, face_x) * w.x
		total += w.x
	if w.y > 0.0:
		var face_y := CubeSphere.FACE_PY if d.y >= 0.0 else CubeSphere.FACE_NY
		sum += _sample_face_float(field, d, face_y) * w.y
		total += w.y
	if w.z > 0.0:
		var face_z := CubeSphere.FACE_PZ if d.z >= 0.0 else CubeSphere.FACE_NZ
		sum += _sample_face_float(field, d, face_z) * w.z
		total += w.z
	return sum / maxf(total, 1e-12)

## Continuous colour lookup using the exact same seam weights as scalar fields.
func sample_color_bilinear(field: PackedColorArray, d: Vector3) -> Color:
	var w := _seam_weights(d)
	var sum := Color(0.0, 0.0, 0.0, 0.0)
	var total := 0.0
	if w.x > 0.0:
		var face_x := CubeSphere.FACE_PX if d.x >= 0.0 else CubeSphere.FACE_NX
		sum += _sample_face_color(field, d, face_x) * w.x
		total += w.x
	if w.y > 0.0:
		var face_y := CubeSphere.FACE_PY if d.y >= 0.0 else CubeSphere.FACE_NY
		sum += _sample_face_color(field, d, face_y) * w.y
		total += w.y
	if w.z > 0.0:
		var face_z := CubeSphere.FACE_PZ if d.z >= 0.0 else CubeSphere.FACE_NZ
		sum += _sample_face_color(field, d, face_z) * w.z
		total += w.z
	return sum * (1.0 / maxf(total, 1e-12))

## Smooth weights for the three signed axis faces touching `d`.
##
## On an equi-angular cube, one cell next to an edge corresponds approximately to
## a PI/res change in the ratio minor_axis/major_axis around 45 degrees. Using
## that ratio makes the blend width scale automatically with grid resolution.
func _seam_weights(d: Vector3) -> Vector3:
	var a := Vector3(absf(d.x), absf(d.y), absf(d.z))
	var major := maxf(a.x, maxf(a.y, a.z))
	if major <= 1e-12:
		return Vector3(1.0, 0.0, 0.0)
	var ratio_band := clampf(SEAM_BLEND_CELLS * PI / float(res), 0.002, 0.25)
	var start := 1.0 - ratio_band
	return Vector3(
		smoothstep(start, 1.0, a.x / major),
		smoothstep(start, 1.0, a.y / major),
		smoothstep(start, 1.0, a.z / major))

## Project one spherical direction into an explicitly selected cube face. This is
## intentionally separate from CubeSphere.dir_to_face_uv(), which chooses a
## major axis and is exactly the branch we must avoid inside a seam blend.
func _uv_on_face(d: Vector3, face: int) -> Vector2:
	var axis: Vector3 = CubeSphere.AXIS[face]
	var denom := axis.dot(d)
	if absf(denom) < 1e-12:
		denom = 1e-12 if denom >= 0.0 else -1e-12
	var u := atan(CubeSphere.RIGHT[face].dot(d) / denom) / CubeSphere.Q
	var v := atan(CubeSphere.UP[face].dot(d) / denom) / CubeSphere.Q
	return Vector2(u, v)

## Bilinear sample restricted to one face. Coordinates are clamped to that
## face's outer cell centres; the neighbouring face is supplied by the outer
## direction-space blend rather than by a discontinuous major-axis lookup.
func _sample_face_float(field: PackedFloat32Array, d: Vector3, face: int) -> float:
	var uv := _uv_on_face(d, face)
	var fx := clampf((uv.x * 0.5 + 0.5) * res - 0.5, 0.0, float(res - 1))
	var fy := clampf((uv.y * 0.5 + 0.5) * res - 0.5, 0.0, float(res - 1))
	var i0 := int(floor(fx))
	var j0 := int(floor(fy))
	var i1 := mini(i0 + 1, res - 1)
	var j1 := mini(j0 + 1, res - 1)
	var tx := fx - float(i0)
	var ty := fy - float(j0)
	var v00 := field[idx(face, i0, j0)]
	var v10 := field[idx(face, i1, j0)]
	var v01 := field[idx(face, i0, j1)]
	var v11 := field[idx(face, i1, j1)]
	return lerpf(lerpf(v00, v10, tx), lerpf(v01, v11, tx), ty)

func _sample_face_color(field: PackedColorArray, d: Vector3, face: int) -> Color:
	var uv := _uv_on_face(d, face)
	var fx := clampf((uv.x * 0.5 + 0.5) * res - 0.5, 0.0, float(res - 1))
	var fy := clampf((uv.y * 0.5 + 0.5) * res - 0.5, 0.0, float(res - 1))
	var i0 := int(floor(fx))
	var j0 := int(floor(fy))
	var i1 := mini(i0 + 1, res - 1)
	var j1 := mini(j0 + 1, res - 1)
	var tx := fx - float(i0)
	var ty := fy - float(j0)
	var c0 := field[idx(face, i0, j0)].lerp(field[idx(face, i1, j0)], tx)
	var c1 := field[idx(face, i0, j1)].lerp(field[idx(face, i1, j1)], tx)
	return c0.lerp(c1, ty)

## Nearest-cell integer field lookup. Integer/categorical fields are intentionally
## not blended; callers that need a visually continuous quantity use the float
## fields derived from them.
func sample_int(field: PackedByteArray, d: Vector3) -> int:
	return field[dir_to_index(d)]
