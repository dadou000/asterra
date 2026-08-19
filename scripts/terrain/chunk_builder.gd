class_name ChunkBuilder
extends RefCounted
## Builds one terrain chunk's geometry on a worker thread.
##
## Vertices are generated in *chunk-local* space around a double-precision pivot,
## so a 24 m chunk on the far side of a 1000 km planet still has millimetre
## vertex precision once it reaches the float32 scene graph.
##
## The macro fields (elevation, relief, rock hardness, water, colour) vary on an
## ~8 km grid, so they are sampled on a coarse lattice per chunk and interpolated,
## while only the sub-grid detail noise and the player's terrain deltas are
## evaluated per vertex. On a 24 m chunk that is the difference between seven
## cube-sphere inversions per vertex and none.
##
## Produces up to two surfaces: the ground, and (only where needed) the free
## water surface for oceans and lakes.

const SKIRT_MIN := 0.4
const SKIRT_MAX := 300.0
const WATER_SKIRT_MIN := 0.08
const WATER_SKIRT_MAX := 400.0
const WATER_DEPTH_SCALE := 4000.0
const WATER_DEPTH_CURVE := 0.25
const MACRO_SPACING := 800.0
const FACE_EDGE_EPS := 1e-6

static var debug_flat := false

static func build(face: int, u0: float, v0: float, size: float, n: int,
		detail: TerrainDetail, snap: Dictionary, want_collision: bool) -> Dictionary:
	var cfg: GenConfig = Planet.cfg
	var radius := cfg.planet_radius
	var step := size / float(n)
	var arc := size * (PI * 0.25) * radius

	var mres := clampi(int(ceil(arc / MACRO_SPACING)), 1, n)
	var mu0 := u0 - step
	var mspan := size + 2.0 * step
	var mn := mres + 1
	var m_elev := PackedFloat32Array(); m_elev.resize(mn * mn)
	var m_relief := PackedFloat32Array(); m_relief.resize(mn * mn)
	var m_flat := PackedFloat32Array(); m_flat.resize(mn * mn)
	var m_hard := PackedFloat32Array(); m_hard.resize(mn * mn)
	var m_river := PackedFloat32Array(); m_river.resize(mn * mn)
	var m_water := PackedFloat32Array(); m_water.resize(mn * mn)
	var m_wmask := PackedFloat32Array(); m_wmask.resize(mn * mn)
	var m_col := PackedColorArray(); m_col.resize(mn * mn)
	var fields := Planet.fields
	var grid := Planet.grid
	for j in mn:
		var v := mu0_at(v0 - step, mspan, j, mres)
		for i in mn:
			var u := mu0_at(mu0, mspan, i, mres)
			var d := CubeSphere.face_uv_to_dir(face, u, v)
			var mi := j * mn + i
			m_elev[mi] = grid.sample_bilinear(fields.elev, d)
			m_relief[mi] = grid.sample_bilinear(fields.relief, d)
			m_flat[mi] = clampf(grid.sample_bilinear(fields.floodplain, d) * 0.8
				+ grid.sample_bilinear(fields.wetland, d) * 0.5, 0.0, 1.0)
			m_hard[mi] = 1.7 - grid.sample_bilinear(fields.erodibility, d) * 0.55
			m_river[mi] = grid.sample_bilinear(fields.river_width, d)
			m_water[mi] = Planet.water_height(d)
			m_wmask[mi] = Planet.water_coverage(d)
			m_col[mi] = Planet.surface_color(d)

	var cu := u0 + size * 0.5
	var cv := v0 + size * 0.5
	var pivot_dir := CubeSphere.face_uv_to_dir_d(cu_face(face), cu, cv)
	var pivot_r := radius + _height_at(mn, mres, mu0, v0 - step, mspan, step, face,
		cu, cv, m_elev, m_relief, m_flat, m_hard, m_river, detail, snap)
	var pivot := pivot_dir.mul(pivot_r)

	var vcount := (n + 1) * (n + 1)
	var verts := PackedVector3Array(); verts.resize(vcount)
	var norms := PackedVector3Array(); norms.resize(vcount)
	var cols := PackedColorArray(); cols.resize(vcount)
	var uvs := PackedVector2Array(); uvs.resize(vcount)

	var ext := n + 3
	var hgt := PackedFloat32Array(); hgt.resize(ext * ext)
	var dirs := PackedVector3Array(); dirs.resize(ext * ext)
	for j in ext:
		var v := v0 + float(j - 1) * step
		for i in ext:
			var u := u0 + float(i - 1) * step
			var d := CubeSphere.face_uv_to_dir(face, u, v)
			dirs[j * ext + i] = d
			hgt[j * ext + i] = _height_at(mn, mres, mu0, v0 - step, mspan, step, face,
				u, v, m_elev, m_relief, m_flat, m_hard, m_river, detail, snap, d)

	var water_needed := false
	var water_h := PackedFloat32Array(); water_h.resize(vcount)
	var water_conf := PackedFloat32Array(); water_conf.resize(vcount)

	var min_h := 1e30
	var max_h := -1e30
	var max_dh := 0.0
	for j in n + 1:
		var v := v0 + float(j) * step
		for i in n + 1:
			var u := u0 + float(i) * step
			var vi := j * (n + 1) + i
			var ei := (j + 1) * ext + (i + 1)
			var h: float = hgt[ei]
			min_h = minf(min_h, h)
			max_h = maxf(max_h, h)
			var dd := CubeSphere.face_uv_to_dir_d(face, u, v)
			var r := radius + h
			verts[vi] = Vector3(
				float(dd.x * r - pivot.x),
				float(dd.y * r - pivot.y),
				float(dd.z * r - pivot.z))
			uvs[vi] = Vector2(float(i) / float(n), float(j) / float(n))
			var d3: Vector3 = dirs[ei]
			var fx := (u - mu0) / mspan * float(mres)
			var fy := (v - (v0 - step)) / mspan * float(mres)
			# Only the actual outer cube edge needs canonical sampling. Keeping the
			# original chunk-local cache everywhere else avoids doing the expensive
			# global macro lookup for every vertex.
			cols[vi] = Planet.surface_color(d3) if _is_face_edge(u, v) else _bilerp_color(m_col, mn, fx, fy)

			var hl: float = hgt[ei - 1]
			var hr: float = hgt[ei + 1]
			var hd: float = hgt[ei - ext]
			var hu: float = hgt[ei + ext]
			var span := _arc(dirs[ei - 1], dirs[ei + 1], radius)
			var span_v := _arc(dirs[ei - ext], dirs[ei + ext], radius)
			var tb := CubeSphere.tangent_basis(d3)
			var tang: Vector3 = (tb[0] * span + d3 * (hr - hl)).normalized()
			var bitan: Vector3 = (tb[1] * span_v + d3 * (hu - hd)).normalized()
			var nrm := bitan.cross(tang).normalized()
			if nrm.dot(d3) < 0.0:
				nrm = -nrm
			norms[vi] = nrm
			max_dh = maxf(max_dh, maxf(absf(hr - hl), absf(hu - hd)) * 0.5)

			var edge := _is_face_edge(u, v)
			var wl := Planet.water_height(d3) if edge else _bilerp(m_water, mn, fx, fy)
			water_h[vi] = wl
			var coverage := Planet.water_coverage(d3) if edge else _bilerp(m_wmask, mn, fx, fy)
			var conf := 1.0 if wl <= 0.05 else smoothstep(0.30, 0.65, coverage)
			water_conf[vi] = conf
			if wl > h + 0.05 and conf > 0.02:
				water_needed = true

	var idx := PackedInt32Array()
	for j in n:
		for i in n:
			var a := j * (n + 1) + i
			var b := a + 1
			var c := a + (n + 1)
			var e := c + 1
			idx.append_array([a, c, b, b, c, e])

	var cell_arc := arc / float(n)
	var sag := (cell_arc * 2.0) * (cell_arc * 2.0) / (8.0 * radius)
	var skirt_drop := clampf(max_dh * 3.0 + (max_h - min_h) * 0.05 + sag * 3.0 + SKIRT_MIN,
		SKIRT_MIN, SKIRT_MAX)
	var ring: Array[int] = []
	for i in n + 1:
		ring.append(i)
	for j in range(1, n + 1):
		ring.append(j * (n + 1) + n)
	for i in range(n - 1, -1, -1):
		ring.append(n * (n + 1) + i)
	for j in range(n - 1, 0, -1):
		ring.append(j * (n + 1))
	var morph := _morph_offsets(verts, n)
	var base := verts.size()
	var pivot3 := Vector3(float(pivot.x), float(pivot.y), float(pivot.z))
	for k in ring.size():
		var src: int = ring[k]
		var p: Vector3 = verts[src]
		var outward := (p + pivot3).normalized()
		verts.append(p - outward * skirt_drop)
		norms.append(norms[src])
		cols.append(cols[src])
		_append_morph(morph, src)
		uvs.append(uvs[src])
	for k in ring.size():
		var k2 := (k + 1) % ring.size()
		var a: int = ring[k]
		var b: int = ring[k2]
		var sa := base + k
		var sb := base + k2
		idx.append_array([a, b, sa, b, sb, sa])

	var out := {
		"pivot": pivot,
		"vertices": verts,
		"normals": norms,
		"colors": cols,
		"uvs": uvs,
		"morph": morph,
		"indices": idx,
		"min_h": min_h,
		"max_h": max_h,
		"has_water": water_needed,
	}

	if want_collision:
		var faces := PackedVector3Array()
		faces.resize(n * n * 6)
		var w := 0
		for j in n:
			for i in n:
				var a := j * (n + 1) + i
				var b := a + 1
				var c := a + (n + 1)
				var e := c + 1
				faces[w] = verts[a]; faces[w + 1] = verts[c]; faces[w + 2] = verts[b]
				faces[w + 3] = verts[b]; faces[w + 4] = verts[c]; faces[w + 5] = verts[e]
				w += 6
		out["collision_faces"] = faces

	if water_needed:
		var wv := PackedVector3Array(); wv.resize(vcount)
		var wn := PackedVector3Array(); wn.resize(vcount)
		var wuv := PackedVector2Array(); wuv.resize(vcount)
		var wcol := PackedColorArray(); wcol.resize(vcount)
		for j in n + 1:
			var v := v0 + float(j) * step
			for i in n + 1:
				var u := u0 + float(i) * step
				var vi := j * (n + 1) + i
				var dd := CubeSphere.face_uv_to_dir_d(face, u, v)
				var r := radius + water_h[vi]
				wv[vi] = Vector3(
					float(dd.x * r - pivot.x),
					float(dd.y * r - pivot.y),
					float(dd.z * r - pivot.z))
				wn[vi] = dirs[(j + 1) * ext + (i + 1)]
				wuv[vi] = Vector2(float(i) / float(n), float(j) / float(n))
				var depth := maxf(water_h[vi] - _ground_h(hgt, ext, i, j), 0.0)
				wcol[vi] = Color(pow(clampf(depth / WATER_DEPTH_SCALE, 0.0, 1.0), WATER_DEPTH_CURVE),
					water_conf[vi], 0.0, 1.0)

		var widx := PackedInt32Array()
		for j in n:
			for i in n:
				var a := j * (n + 1) + i
				var b := a + 1
				var c := a + (n + 1)
				var e := c + 1
				widx.append_array([a, c, b, b, c, e])

		var wmorph := _morph_offsets(wv, n)
		var water_skirt := clampf(sag * 2.0 + WATER_SKIRT_MIN, WATER_SKIRT_MIN, WATER_SKIRT_MAX)
		var wbase := wv.size()
		for k in ring.size():
			var src: int = ring[k]
			var p: Vector3 = wv[src]
			var outward := (p + pivot3).normalized()
			wv.append(p - outward * water_skirt)
			wn.append(wn[src])
			wuv.append(wuv[src])
			wcol.append(wcol[src])
			_append_morph(wmorph, src)
		for k in ring.size():
			var k2 := (k + 1) % ring.size()
			var a: int = ring[k]
			var b: int = ring[k2]
			var sa := wbase + k
			var sb := wbase + k2
			widx.append_array([a, b, sa, b, sb, sa])

		out["water_vertices"] = wv
		out["water_normals"] = wn
		out["water_uvs"] = wuv
		out["water_colors"] = wcol
		out["water_morph"] = wmorph
		out["water_indices"] = widx
	return out

static func _morph_offsets(verts: PackedVector3Array, n: int) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize((n + 1) * (n + 1) * 4)
	if n % 2 != 0:
		return out
	var row := n + 1
	for j in n + 1:
		for i in n + 1:
			var vi := j * row + i
			var d := Vector3.ZERO
			if i % 2 == 1 and j % 2 == 0:
				d = (verts[vi - 1] + verts[vi + 1]) * 0.5 - verts[vi]
			elif i % 2 == 0 and j % 2 == 1:
				d = (verts[vi - row] + verts[vi + row]) * 0.5 - verts[vi]
			elif i % 2 == 1 and j % 2 == 1:
				d = (verts[vi + 1 - row] + verts[vi - 1 + row]) * 0.5 - verts[vi]
			var o := vi * 4
			out[o] = d.x
			out[o + 1] = d.y
			out[o + 2] = d.z
	return out

static func _append_morph(out: PackedFloat32Array, src: int) -> void:
	var o := src * 4
	out.append(out[o])
	out.append(out[o + 1])
	out.append(out[o + 2])
	out.append(0.0)

static func _ground_h(hgt: PackedFloat32Array, ext: int, i: int, j: int) -> float:
	return hgt[(j + 1) * ext + (i + 1)]

static func cu_face(face: int) -> int:
	return face

static func mu0_at(origin: float, span: float, i: int, res: int) -> float:
	return origin + span * float(i) / float(res)

static func _bilerp(a: PackedFloat32Array, mn: int, fx: float, fy: float) -> float:
	var i0 := clampi(int(floor(fx)), 0, mn - 2) if mn > 1 else 0
	var j0 := clampi(int(floor(fy)), 0, mn - 2) if mn > 1 else 0
	var tx := clampf(fx - float(i0), 0.0, 1.0)
	var ty := clampf(fy - float(j0), 0.0, 1.0)
	if mn == 1:
		return a[0]
	var v00 := a[j0 * mn + i0]
	var v10 := a[j0 * mn + i0 + 1]
	var v01 := a[(j0 + 1) * mn + i0]
	var v11 := a[(j0 + 1) * mn + i0 + 1]
	return lerpf(lerpf(v00, v10, tx), lerpf(v01, v11, tx), ty)

static func _bilerp_color(a: PackedColorArray, mn: int, fx: float, fy: float) -> Color:
	if mn == 1:
		return a[0]
	var i0 := clampi(int(floor(fx)), 0, mn - 2)
	var j0 := clampi(int(floor(fy)), 0, mn - 2)
	var tx := clampf(fx - float(i0), 0.0, 1.0)
	var ty := clampf(fy - float(j0), 0.0, 1.0)
	var c0 := a[j0 * mn + i0].lerp(a[j0 * mn + i0 + 1], tx)
	var c1 := a[(j0 + 1) * mn + i0].lerp(a[(j0 + 1) * mn + i0 + 1], tx)
	return c0.lerp(c1, ty)

static func _height_at(mn: int, mres: int, mu0: float, mv0: float, mspan: float, _step: float,
		face: int, u: float, v: float,
		m_elev: PackedFloat32Array, m_relief: PackedFloat32Array, m_flat: PackedFloat32Array,
		m_hard: PackedFloat32Array, m_river: PackedFloat32Array,
		detail: TerrainDetail, snap: Dictionary, known_dir: Variant = null) -> float:
	if debug_flat:
		return 0.0
	var d: Vector3 = known_dir if known_dir != null else CubeSphere.face_uv_to_dir(face, u, v)
	# Shared outer-face vertices must use the exact global terrain function so
	# both cube faces produce bit-identical radial displacement. Everywhere else
	# keep the original coarse macro cache; evaluating Planet.terrain_height for
	# every vertex caused the streamer backlog seen after the first seam fix.
	if _is_face_edge(u, v):
		return Planet.terrain_height(d, detail, snap)
	var fx := (u - mu0) / mspan * float(mres)
	var fy := (v - mv0) / mspan * float(mres)
	var h := _bilerp(m_elev, mn, fx, fy)
	var relief := _bilerp(m_relief, mn, fx, fy)
	var flat := _bilerp(m_flat, mn, fx, fy)
	var hard := _bilerp(m_hard, mn, fx, fy)
	if h > 0.0:
		h += detail.height(d, relief, flat, hard)
		var w := _bilerp(m_river, mn, fx, fy)
		if w > 4.0:
			h -= clampf(w * 0.045, 0.0, 22.0)
	else:
		h += detail.height(d, relief * 0.5, 0.55, 1.0)
	if snap.is_empty():
		if not Deltas.is_empty():
			h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h

static func _is_face_edge(u: float, v: float) -> bool:
	return absf(absf(u) - 1.0) <= FACE_EDGE_EPS or absf(absf(v) - 1.0) <= FACE_EDGE_EPS

static func _arc(a: Vector3, b: Vector3, radius: float) -> float:
	return maxf(0.001, a.distance_to(b) * radius)
