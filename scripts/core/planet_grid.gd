class_name PlanetGrid
extends RefCounted
## The discrete global field grid: 6 cube-sphere faces of res x res cells.
##
## All macro-scale generation passes (geology, erosion, hydrology, climate, soil,
## biome, suitability) operate on this single flat index space so that they can
## share neighbour topology. Cross-face adjacency is resolved geometrically -- a
## neighbour that falls outside a face is re-projected through the cube-sphere
## inverse -- which removes the usual seam special-casing entirely.

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

## Bilinearly sampled field value at an arbitrary direction. Handles face borders
## by clamping inside the face and blending with the geometric neighbour, which is
## continuous to within a fraction of a cell -- enough for ordinary macro fields.
func sample_bilinear(field: PackedFloat32Array, d: Vector3) -> float:
	var fuv := CubeSphere.dir_to_face_uv(d)
	var face: int = fuv[0]
	var fx: float = (fuv[1] * 0.5 + 0.5) * res - 0.5
	var fy: float = (fuv[2] * 0.5 + 0.5) * res - 0.5
	var i0 := int(floor(fx))
	var j0 := int(floor(fy))
	var tx := fx - float(i0)
	var ty := fy - float(j0)
	var v00 := _safe(field, face, i0, j0)
	var v10 := _safe(field, face, i0 + 1, j0)
	var v01 := _safe(field, face, i0, j0 + 1)
	var v11 := _safe(field, face, i0 + 1, j0 + 1)
	return lerpf(lerpf(v00, v10, tx), lerpf(v01, v11, tx), ty)

## C2-continuous cubic B-spline reconstruction for terrain elevation.
##
## Bilinear interpolation is only C0: height is continuous, but slope changes at
## every ~8 km macro-cell boundary. Once sub-grid noise is composed on top, those
## derivative breaks read as broad square facets. A tensor-product cubic B-spline
## uses the surrounding 4x4 samples and has continuous first and second
## derivatives, while its positive weights avoid Catmull-Rom overshoot around
## coastlines and sharp mountain values.
##
## `_safe()` reprojects taps crossing a cube edge, so the same filter remains
## seam-safe on all six faces.
func sample_cubic_bspline(field: PackedFloat32Array, d: Vector3) -> float:
	var fuv := CubeSphere.dir_to_face_uv(d)
	var face: int = fuv[0]
	var fx: float = (fuv[1] * 0.5 + 0.5) * res - 0.5
	var fy: float = (fuv[2] * 0.5 + 0.5) * res - 0.5
	var i0: int = int(floor(fx))
	var j0: int = int(floor(fy))
	var tx: float = fx - float(i0)
	var ty: float = fy - float(j0)
	var wx := _bspline_weights(tx)
	var wy := _bspline_weights(ty)
	var value := 0.0
	for ky in 4:
		var row := 0.0
		var sy: int = j0 + ky - 1
		for kx in 4:
			row += _safe(field, face, i0 + kx - 1, sy) * wx[kx]
		value += row * wy[ky]
	return value

static func _bspline_weights(t: float) -> Vector4:
	var t2: float = t * t
	var t3: float = t2 * t
	return Vector4(
		(1.0 - 3.0 * t + 3.0 * t2 - t3) / 6.0,
		(4.0 - 6.0 * t2 + 3.0 * t3) / 6.0,
		(1.0 + 3.0 * t + 3.0 * t2 - 3.0 * t3) / 6.0,
		t3 / 6.0)

func _safe(field: PackedFloat32Array, face: int, i: int, j: int) -> float:
	if i >= 0 and i < res and j >= 0 and j < res:
		return field[idx(face, i, j)]
	var s := 2.0 / float(res)
	var u := (float(i) + 0.5) * s - 1.0
	var v := (float(j) + 0.5) * s - 1.0
	return field[dir_to_index(CubeSphere.face_uv_to_dir(face, u, v))]

## Bilinearly sampled colour field. Same border handling as `sample_bilinear` --
## a per-cell palette lookup sampled this way is what keeps the 8 km macro
## lattice from showing up as square tiles on the ground.
func sample_color_bilinear(field: PackedColorArray, d: Vector3) -> Color:
	var fuv := CubeSphere.dir_to_face_uv(d)
	var face: int = fuv[0]
	var fx: float = (fuv[1] * 0.5 + 0.5) * res - 0.5
	var fy: float = (fuv[2] * 0.5 + 0.5) * res - 0.5
	var i0 := int(floor(fx))
	var j0 := int(floor(fy))
	var tx := fx - float(i0)
	var ty := fy - float(j0)
	var c0 := _safe_color(field, face, i0, j0).lerp(_safe_color(field, face, i0 + 1, j0), tx)
	var c1 := _safe_color(field, face, i0, j0 + 1).lerp(_safe_color(field, face, i0 + 1, j0 + 1), tx)
	return c0.lerp(c1, ty)

func _safe_color(field: PackedColorArray, face: int, i: int, j: int) -> Color:
	if i >= 0 and i < res and j >= 0 and j < res:
		return field[idx(face, i, j)]
	var s := 2.0 / float(res)
	var u := (float(i) + 0.5) * s - 1.0
	var v := (float(j) + 0.5) * s - 1.0
	return field[dir_to_index(CubeSphere.face_uv_to_dir(face, u, v))]

## Nearest-cell integer field lookup.
func sample_int(field: PackedByteArray, d: Vector3) -> int:
	return field[dir_to_index(d)]
