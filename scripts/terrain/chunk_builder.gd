class_name ChunkBuilder
extends RefCounted
## Builds one terrain chunk's geometry on a worker thread.
##
## The macro fields are cached per chunk for speed. Cube-face seams need special
## treatment because neighbouring faces use different local (u,v) axes.

# The old vertical skirts could be hundreds of metres deep on rugged chunks and
# became visible as serrated walls along coasts/chunk boundaries. Keep the skirt
# topology degenerate for now (zero drop) so it contributes no visible geometry.
# If an LOD crack reappears, fix it with edge stitching rather than a vertical wall.
const SKIRT_MIN := 0.0
const SKIRT_MAX := 0.0
const WATER_SKIRT_MIN := 0.0
const WATER_SKIRT_MAX := 0.0
const WATER_DEPTH_SCALE := 4000.0
const WATER_DEPTH_CURVE := 0.25
const MACRO_SPACING := 800.0
const FACE_EDGE_EPS := 1e-6
const STITCH_LEFT := 1
const STITCH_RIGHT := 2
const STITCH_BOTTOM := 4
const STITCH_TOP := 8
# TerrainDetail can pull a macro shoreline hundreds of metres vertically. Any
# chunk whose cached macro field comes this close to sea level gets a sea-level
# water sheet even when none of its coarse terrain vertices happens to cross 0 m.
# The shader's coastline clipmap clips away the dry part per fragment.
const COAST_WATER_CANDIDATE_M := 450.0
## Number of vertex rows over which the fast face-local approximation is
## relaxed toward the canonical planet-space sampler. Exact edge vertices remain
## bit-identical on both faces, while the smoothstep band removes the hard crease.
const SEAM_BLEND_ROWS := 4.0

static var debug_flat := false

static func build(face: int, u0: float, v0: float, size: float, n: int,
		detail: TerrainDetail, snap: Dictionary, want_collision: bool,
		stitch_mask: int = 0, cancel: TerrainBuildCancel = null) -> Dictionary:
	if cancel != null and cancel.is_cancelled():
		return {"cancelled": true}
	var cfg: GenConfig = Planet.cfg
	var radius := cfg.planet_radius
	var step := size / float(n)
	var arc := size * (PI * 0.25) * radius

	# Synthesise only what this chunk's vertex grid can carry. Sampling detail
	# finer than the grid does not add detail, it adds noise: an octave below the
	# sampling interval becomes an uncorrelated per-vertex offset, which reads as
	# crumpled foil at distance, prints the triangulation across the surface as a
	# herringbone of facets, and makes every LOD change pop. The missing band is
	# restored as shading by the terrain shader, from the whole-planet elevation
	# texture, which resolves finer than any distant chunk does.
	detail.set_sample_spacing(arc / float(n))

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
	var m_surface := PackedColorArray(); m_surface.resize(mn * mn)
	var m_geomorph := PackedColorArray(); m_geomorph.resize(mn * mn)
	var m_drainage := PackedColorArray(); m_drainage.resize(mn * mn)
	var fields := Planet.fields
	var grid := Planet.grid
	var ocean_candidate := false
	for j in mn:
		if cancel != null and cancel.is_cancelled():
			return {"cancelled": true}
		var v := mu0_at(v0 - step, mspan, j, mres)
		for i in mn:
			var u := mu0_at(mu0, mspan, i, mres)
			var d := CubeSphere.face_uv_to_dir(face, u, v)
			var mi := j * mn + i
			# Elevation uses Planet's derived smoothed runtime macro field. Keeping
			# the chunk cache on the same source as the canonical edge sampler avoids
			# reintroducing the baked 8 km cell lattice in chunk interiors.
			m_elev[mi] = Planet.macro_height(d)
			if m_elev[mi] <= COAST_WATER_CANDIDATE_M:
				ocean_candidate = true
			m_relief[mi] = Planet.runtime_relief(d)
			m_flat[mi] = clampf(grid.sample_bilinear(fields.floodplain, d) * 0.8
				+ grid.sample_bilinear(fields.wetland, d) * 0.5, 0.0, 1.0)
			m_hard[mi] = 1.7 - grid.sample_bilinear(fields.erodibility, d) * 0.55
			m_river[mi] = grid.sample_bilinear(fields.river_width, d)
			m_water[mi] = Planet.water_height(d)
			m_wmask[mi] = Planet.water_coverage(d)
			# One domain-warp evaluation feeds both material channels. The warp is
			# several noise samples, so sharing it materially reduces build time.
			var surface_lookup := Planet.surface_lookup(d)
			m_col[mi] = Planet.surface_color_from_lookup(surface_lookup)
			m_surface[mi] = Planet.surface_composition_from_lookup(surface_lookup)
			m_geomorph[mi] = Planet.geomorph_composition_from_lookup(surface_lookup)
			m_drainage[mi] = Planet.drainage_composition(d)

	var cu := u0 + size * 0.5
	var cv := v0 + size * 0.5
	var pivot_dir := CubeSphere.face_uv_to_dir_d(cu_face(face), cu, cv)
	var pivot_r := radius + _height_at(mn, mres, mu0, v0 - step, mspan, step, face,
		cu, cv, m_elev, m_relief, m_flat, m_hard, m_river, m_surface, m_geomorph,
		m_drainage,
		detail, snap)
	var pivot := pivot_dir.mul(pivot_r)

	var vcount := (n + 1) * (n + 1)
	var verts := PackedVector3Array(); verts.resize(vcount)
	var norms := PackedVector3Array(); norms.resize(vcount)
	var cols := PackedColorArray(); cols.resize(vcount)
	var uvs := PackedVector2Array(); uvs.resize(vcount)
	var surface := PackedFloat32Array(); surface.resize(vcount * 4)

	# One-vertex border for central-difference normals. Every actual chunk edge
	# vertex and the one sample immediately outside it use the canonical planet
	# sampler. Two independently built neighboring chunks therefore agree exactly
	# at their shared edge instead of each trusting a different local macro cache.
	var ext := n + 3
	var hgt := PackedFloat32Array(); hgt.resize(ext * ext)
	var dirs := PackedVector3Array(); dirs.resize(ext * ext)
	for j in ext:
		if cancel != null and cancel.is_cancelled():
			return {"cancelled": true}
		var v := v0 + float(j - 1) * step
		for i in ext:
			var u := u0 + float(i - 1) * step
			var d := CubeSphere.face_uv_to_dir(face, u, v)
			dirs[j * ext + i] = d
			var canonical_boundary := i <= 1 or i >= n + 1 or j <= 1 or j >= n + 1
			hgt[j * ext + i] = _height_at(mn, mres, mu0, v0 - step, mspan, step, face,
				u, v, m_elev, m_relief, m_flat, m_hard, m_river, m_surface, m_geomorph,
				m_drainage,
				detail, snap, d,
				canonical_boundary)

	# A fine edge bordering a parent-level neighbour must evaluate the exact same
	# band-limited height spectrum as that neighbour. Odd fine-edge vertices are
	# removed by the stitched index topology below; the remaining even vertices
	# now agree mathematically, rather than being pulled together by a skirt.
	var coarse_edge_detail: TerrainDetail = null
	if stitch_mask != 0:
		coarse_edge_detail = Planet.make_detail()
		coarse_edge_detail.set_sample_spacing(arc / float(n) * 2.0)
		for edge_j in n + 1:
			if (stitch_mask & STITCH_LEFT) != 0:
				var left_ei := (edge_j + 1) * ext + 1
				hgt[left_ei] = Planet.terrain_height(dirs[left_ei], coarse_edge_detail, snap)
			if (stitch_mask & STITCH_RIGHT) != 0:
				var right_ei := (edge_j + 1) * ext + n + 1
				hgt[right_ei] = Planet.terrain_height(dirs[right_ei], coarse_edge_detail, snap)
		for edge_i in n + 1:
			if (stitch_mask & STITCH_BOTTOM) != 0:
				var bottom_ei := ext + edge_i + 1
				hgt[bottom_ei] = Planet.terrain_height(dirs[bottom_ei], coarse_edge_detail, snap)
			if (stitch_mask & STITCH_TOP) != 0:
				var top_ei := (n + 1) * ext + edge_i + 1
				hgt[top_ei] = Planet.terrain_height(dirs[top_ei], coarse_edge_detail, snap)

	# Ocean coverage must not depend on whether a coarse chunk vertex happened to
	# fall below sea level. A coast-candidate chunk gets the water sheet now; the
	# LOD-independent shader mask determines the actual visible zero contour.
	var water_needed := ocean_candidate
	var water_h := PackedFloat32Array(); water_h.resize(vcount)
	var water_conf := PackedFloat32Array(); water_conf.resize(vcount)

	var min_h := 1e30
	var max_h := -1e30
	var max_dh := 0.0
	for j in n + 1:
		if cancel != null and cancel.is_cancelled():
			return {"cancelled": true}
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
			var seam_w := _seam_weight(u, v, step)
			var chunk_edge := i == 0 or i == n or j == 0 or j == n

			# Shared chunk edges use the same direct direction-space colour lookup.
			# Interior vertices keep the cached path for speed.
			var local_col := _bilerp_color(m_col, mn, fx, fy)
			if chunk_edge:
				cols[vi] = Planet.surface_color(d3)
			elif seam_w > 0.0:
				cols[vi] = local_col.lerp(Planet.surface_color(d3), seam_w)
			else:
				cols[vi] = local_col
			var local_surface := _bilerp_color(m_surface, mn, fx, fy)
			var surface_value := local_surface
			if chunk_edge:
				surface_value = Planet.surface_composition(d3)
			elif seam_w > 0.0:
				surface_value = local_surface.lerp(Planet.surface_composition(d3), seam_w)
			_set_surface(surface, vi, surface_value)

			var hl: float = hgt[ei - 1]
			var hr: float = hgt[ei + 1]
			var hd: float = hgt[ei - ext]
			var hu: float = hgt[ei + ext]
			var span := _arc(dirs[ei - 1], dirs[ei + 1], radius)
			var span_v := _arc(dirs[ei - ext], dirs[ei + ext], radius)

			# The old code interpreted face-local U/V height differences as
			# global east/north. That rotates the derivative basis at cube edges.
			# Use the actual physical U and V directions of this face instead.
			var du := dirs[ei + 1] - dirs[ei - 1]
			du -= d3 * du.dot(d3)
			if du.length_squared() < 1e-12:
				du = CubeSphere.tangent_basis(d3)[0]
			else:
				du = du.normalized()
			var dv := dirs[ei + ext] - dirs[ei - ext]
			dv -= d3 * dv.dot(d3)
			if dv.length_squared() < 1e-12:
				dv = CubeSphere.tangent_basis(d3)[1]
			else:
				dv = dv.normalized()

			var nrm: Vector3
			if chunk_edge:
				var normal_detail := detail
				var normal_spacing := arc / float(n)
				if coarse_edge_detail != null \
						and _vertex_on_stitched_edge(i, j, n, stitch_mask):
					normal_detail = coarse_edge_detail
					normal_spacing *= 2.0
				nrm = _canonical_surface_normal(d3, normal_detail, snap,
					normal_spacing, radius)
			else:
				var tang: Vector3 = (du * span + d3 * (hr - hl)).normalized()
				var bitan: Vector3 = (dv * span_v + d3 * (hu - hd)).normalized()
				nrm = tang.cross(bitan).normalized()
				if nrm.dot(d3) < 0.0:
					nrm = -nrm
			norms[vi] = nrm
			max_dh = maxf(max_dh, maxf(absf(hr - hl), absf(hu - hd)) * 0.5)

			var local_wl := _bilerp(m_water, mn, fx, fy)
			var local_coverage := _bilerp(m_wmask, mn, fx, fy)
			var wl := local_wl
			var coverage := local_coverage
			if chunk_edge:
				wl = Planet.water_height(d3)
				coverage = Planet.water_coverage(d3)
			elif seam_w > 0.0:
				wl = lerpf(local_wl, Planet.water_height(d3), seam_w)
				coverage = lerpf(local_coverage, Planet.water_coverage(d3), seam_w)
			water_h[vi] = wl
			var conf := 1.0 if wl <= 0.05 else smoothstep(0.30, 0.65, coverage)
			water_conf[vi] = conf
			if wl > h + 0.05 and conf > 0.02:
				water_needed = true

	var idx := _grid_indices(n, stitch_mask)

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
	var geometric_error := _max_morph_error(morph)
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
		_append_surface(surface, src)
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
		"surface": surface,
		"indices": idx,
		"min_h": min_h,
		"max_h": max_h,
		"has_water": water_needed,
		"stitch_mask": stitch_mask,
		"geometric_error": maxf(geometric_error, sag),
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
			if cancel != null and cancel.is_cancelled():
				return {"cancelled": true}
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
				# Preserve the sign of the water-to-ground separation for lakes and
				# for depth fallback. Sea-level shoreline visibility itself now comes
				# from the shared coastline clipmap in the shader.
				var signed_depth := water_h[vi] - _ground_h(hgt, ext, i, j)
				var depth_mag := absf(signed_depth)
				var wet_sign := 1.0 if signed_depth >= 0.0 else 0.0
				var ocean_flag := 1.0 if absf(water_h[vi]) <= 0.05 else 0.0
				wcol[vi] = Color(pow(clampf(depth_mag / WATER_DEPTH_SCALE, 0.0, 1.0), WATER_DEPTH_CURVE),
					water_conf[vi], ocean_flag, wet_sign)

		var widx := _grid_indices(n, stitch_mask)

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


## Index-only 2:1 stitching. On an edge adjacent to a node one level coarser,
## odd boundary vertices collapse onto the preceding even endpoint. Degenerate
## triangles disappear in rasterisation and the surviving boundary is exactly
## the coarse neighbour's polyline, without skirts or duplicate edge geometry.
static func _grid_indices(n: int, stitch_mask: int) -> PackedInt32Array:
	var out := PackedInt32Array()
	for j in n:
		for i in n:
			var a := _stitched_vertex(i, j, n, stitch_mask)
			var b := _stitched_vertex(i + 1, j, n, stitch_mask)
			var c := _stitched_vertex(i, j + 1, n, stitch_mask)
			var e := _stitched_vertex(i + 1, j + 1, n, stitch_mask)
			out.append_array([a, c, b, b, c, e])
	return out


static func _stitched_vertex(i: int, j: int, n: int, stitch_mask: int) -> int:
	if i == 0 and j % 2 == 1 and (stitch_mask & STITCH_LEFT) != 0:
		j -= 1
	elif i == n and j % 2 == 1 and (stitch_mask & STITCH_RIGHT) != 0:
		j -= 1
	if j == 0 and i % 2 == 1 and (stitch_mask & STITCH_BOTTOM) != 0:
		i -= 1
	elif j == n and i % 2 == 1 and (stitch_mask & STITCH_TOP) != 0:
		i -= 1
	return j * (n + 1) + i


static func _vertex_on_stitched_edge(i: int, j: int, n: int, stitch_mask: int) -> bool:
	return (i == 0 and (stitch_mask & STITCH_LEFT) != 0) \
		or (i == n and (stitch_mask & STITCH_RIGHT) != 0) \
		or (j == 0 and (stitch_mask & STITCH_BOTTOM) != 0) \
		or (j == n and (stitch_mask & STITCH_TOP) != 0)


## Direction-space derivative shared by every cube face. Face-local central
## differences rotate at seams; this east/north stencil has the same sample
## directions, height spectrum, and cross-product order on both sides.
static func _canonical_surface_normal(d: Vector3, detail: TerrainDetail,
		snap: Dictionary, sample_m: float, radius: float) -> Vector3:
	var basis := CubeSphere.tangent_basis(d)
	var east: Vector3 = basis[0]
	var north: Vector3 = basis[1]
	var da := maxf(sample_m / maxf(radius, 1.0), 1e-8)
	var dl := (d - east * da).normalized()
	var dr := (d + east * da).normalized()
	var dd := (d - north * da).normalized()
	var du := (d + north * da).normalized()
	var pl := dl * (radius + Planet.terrain_height(dl, detail, snap))
	var pr := dr * (radius + Planet.terrain_height(dr, detail, snap))
	var pd := dd * (radius + Planet.terrain_height(dd, detail, snap))
	var pu := du * (radius + Planet.terrain_height(du, detail, snap))
	var normal := (pr - pl).cross(pu - pd).normalized()
	return normal if normal.dot(d) >= 0.0 else -normal


static func _max_morph_error(morph: PackedFloat32Array) -> float:
	var maximum := 0.0
	for vertex in morph.size() / 4:
		var o := vertex * 4
		maximum = maxf(maximum, Vector3(morph[o], morph[o + 1], morph[o + 2]).length())
	return maximum


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


static func _set_surface(out: PackedFloat32Array, vertex: int, value: Color) -> void:
	var o := vertex * 4
	out[o] = value.r
	out[o + 1] = value.g
	out[o + 2] = value.b
	out[o + 3] = value.a


static func _append_surface(out: PackedFloat32Array, src: int) -> void:
	var o := src * 4
	out.append(out[o])
	out.append(out[o + 1])
	out.append(out[o + 2])
	out.append(out[o + 3])

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


static func _height_at(mn: int, mres: int, mu0: float, mv0: float, mspan: float, step: float,
		face: int, u: float, v: float,
		m_elev: PackedFloat32Array, m_relief: PackedFloat32Array, m_flat: PackedFloat32Array,
		m_hard: PackedFloat32Array, m_river: PackedFloat32Array,
		m_surface: PackedColorArray, m_geomorph: PackedColorArray,
		m_drainage: PackedColorArray,
		detail: TerrainDetail, snap: Dictionary, known_dir: Variant = null,
		force_canonical: bool = false) -> float:
	if debug_flat:
		return 0.0
	var d: Vector3 = known_dir if known_dir != null else CubeSphere.face_uv_to_dir(face, u, v)
	var seam_w := _seam_weight(u, v, step)

	# Exact cube-face seams, every chunk perimeter vertex, and the outer normal
	# sample ring all use one authoritative direction-space height function.
	if force_canonical or seam_w >= 0.999999:
		return Planet.terrain_height(d, detail, snap)

	var fx := (u - mu0) / mspan * float(mres)
	var fy := (v - mv0) / mspan * float(mres)
	var macro_h := _bilerp(m_elev, mn, fx, fy)
	var h := macro_h
	var relief := _bilerp(m_relief, mn, fx, fy)
	var flat := _bilerp(m_flat, mn, fx, fy)
	var hard := _bilerp(m_hard, mn, fx, fy)

	# Keep full detail through the coast and continental shelf. Only below 250 m
	# macro depth do we start approaching the old subdued seabed profile, reaching
	# it gradually by 1500 m. This removes the hard amplitude step at sea level.
	var ocean_depth := maxf(-macro_h, 0.0)
	var deep_ocean := smoothstep(250.0, 1500.0, ocean_depth)
	var used_relief := lerpf(relief, relief * 0.5, deep_ocean)
	var used_flat := lerpf(flat, maxf(flat, 0.55), deep_ocean)
	var used_hard := lerpf(hard, 1.0, deep_ocean)
	var surface := _bilerp_color(m_surface, mn, fx, fy)
	var geomorph := _bilerp_color(m_geomorph, mn, fx, fy)
	var drainage := _bilerp_color(m_drainage, mn, fx, fy)
	h += detail.height(d, used_relief, used_flat, used_hard, surface, geomorph, drainage)
	if Planet.coast_profile_active:
		h += Planet.coast_profile_offset(d, macro_h)

	if macro_h > 0.0:
		var w := _bilerp(m_river, mn, fx, fy)
		# Faded in, not switched on: see Planet.terrain_height. A hard threshold
		# here steps the surface along the contour where the interpolated river
		# width crosses it, and a step in a height field spikes the mesh normals
		# along that contour -- which is a dotted dark hairline tracing every
		# drainage network on the planet from the air.
		h -= smoothstep(3.0, 16.0, w) * clampf(w * 0.045, 0.0, 22.0)
	if snap.is_empty():
		if not Deltas.is_empty():
			h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)

	if seam_w > 0.0:
		var canonical := Planet.terrain_height(d, detail, snap)
		h = lerpf(h, canonical, seam_w)
	return h


## 0 in the normal fast path, 1 exactly on or beyond a cube-face boundary.
## The cubic smoothstep has zero slope at both ends, so blending does not create
## a new derivative discontinuity at the inner edge of the transition strip.
static func _seam_weight(u: float, v: float, step: float) -> float:
	var au := absf(u)
	var av := absf(v)
	if au >= 1.0 - FACE_EDGE_EPS or av >= 1.0 - FACE_EDGE_EPS:
		return 1.0
	var dist_to_edge := minf(1.0 - au, 1.0 - av)
	var band := maxf(step * SEAM_BLEND_ROWS, FACE_EDGE_EPS)
	if dist_to_edge >= band:
		return 0.0
	var t := clampf(1.0 - dist_to_edge / band, 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)


static func _is_face_edge(u: float, v: float) -> bool:
	return absf(absf(u) - 1.0) <= FACE_EDGE_EPS or absf(absf(v) - 1.0) <= FACE_EDGE_EPS


static func _arc(a: Vector3, b: Vector3, radius: float) -> float:
	return maxf(0.001, a.distance_to(b) * radius)
