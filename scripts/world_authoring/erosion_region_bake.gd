class_name ErosionRegionBake
extends RefCounted
## Bakes a real hydraulic-erosion pass (HydraulicErosionBaker) over one region
## of the planet and writes the result into `Deltas` -- the project's existing
## sparse terrain-edit lattice. `Deltas` is already wired to the renderer
## (gpu_terrain_edit_delta.gd mirrors it into `u_edit_delta`) and to CPU
## contact (`Deltas.offset_at`, composed into contact height in
## gpu_terrain_height_query_contact.gd) and already persists with the rest of
## the world's edits (serialize/deserialize). Baking into it means a real
## erosion pass gets render + CPU-contact + persistence for free instead of a
## parallel texture/uniform pipeline: an erosion bake IS an edit here,
## produced by simulation instead of a sculpt-tool stroke.
##
## `Deltas`' own lattice is extremely fine (~0.75 m on a 1000 km planet --
## see terrain_deltas.gd's sample_spacing) and simulating a droplet erosion
## pass directly at that resolution is computationally impractical for
## anything but a tiny region. So this simulates at a much coarser working
## resolution (`resolution`, e.g. 256) covering the same physical area, then
## bilinearly upsamples that coarse result onto every native lattice point
## the region covers when writing back -- the terrain reads as smooth at full
## render resolution regardless of how coarse the simulation itself ran, the
## same principle `Deltas.sample_tangent_rect` already relies on for its own
## GPU mirror.
##
## Runs synchronously on the calling thread. A bake over a large radius means
## writing many native lattice points (roughly (2 * radius_m / spacing_m)^2 of
## them) and will visibly block whatever called it -- fine for an authoring
## "click Bake and wait" action over a modest region (some tens to a couple
## hundred metres), but keep radius small or thread this call if it needs to
## cover a much larger area without freezing the caller.


## Runs one bake and writes the eroded delta into Deltas.
## `center_dir`: planet-relative direction at the centre of the region.
## `radius_m`: region radius in metres.
## `resolution`: the SIMULATION grid's resolution (not the native edit-lattice
## resolution -- see the module comment).
## `hardness`: 0..1, mirrors Deltas.apply_radial_brush's edge softness -- how
## much of the radius stays at full strength before feathering to zero at the
## boundary, so a bake never leaves a hard seam at its edge.
## `sim_params`: forwarded to HydraulicErosionBaker.simulate (droplet_count,
## inertia, etc. -- see HydraulicErosionBaker.DEFAULT_PARAMS).
## Returns stats: {ok, native_points_written, sim_min_delta_m,
## sim_max_delta_m, sim_mean_delta_m}.
static func bake(center_dir: Vector3, radius_m: float, resolution: int = 256,
		hardness: float = 0.6, sim_params: Dictionary = {}) -> Dictionary:
	if center_dir.length_squared() < 0.5 or radius_m <= 0.0 or resolution < 8:
		return {"ok": false, "error": "invalid region"}
	if Planet.cfg == null:
		return {"ok": false, "error": "planet not ready"}
	var planet_radius: float = Planet.cfg.planet_radius
	if planet_radius <= 1.0:
		return {"ok": false, "error": "invalid planet radius"}

	var center: Vector3 = center_dir.normalized()
	var basis: Array = CubeSphere.tangent_basis(center)
	var right: Vector3 = (basis[0] as Vector3).normalized()
	var up: Vector3 = (basis[1] as Vector3).normalized()
	var runtime: Node = _find_displacement_runtime()

	# 1. Rasterize the current total height (coarse elevation + biome
	# authoring + any existing edits) over the simulation grid.
	var cell_m: float = (radius_m * 2.0) / float(resolution)
	var half: float = (float(resolution) - 1.0) * 0.5
	var heights := PackedFloat32Array()
	heights.resize(resolution * resolution)
	for gy in resolution:
		var oy: float = (float(gy) - half) * cell_m
		for gx in resolution:
			var ox: float = (float(gx) - half) * cell_m
			var d: Vector3 = (center + right * (ox / planet_radius)
				+ up * (oy / planet_radius)).normalized()
			heights[gy * resolution + gx] = _sample_current_height(d, runtime)

	# 2. Run the real (simplified) hydraulic erosion simulation.
	var eroded: PackedFloat32Array = HydraulicErosionBaker.simulate(
		heights, resolution, cell_m, sim_params)

	var delta := PackedFloat32Array()
	delta.resize(heights.size())
	var min_d: float = INF
	var max_d: float = -INF
	var sum_d: float = 0.0
	for i in heights.size():
		var v: float = eroded[i] - heights[i]
		delta[i] = v
		min_d = minf(min_d, v)
		max_d = maxf(max_d, v)
		sum_d += v

	# 3. Upsample the simulated delta onto every native Deltas lattice point
	# the region covers, and write it as an addition to whatever edit offset
	# was already there (mirrors Deltas.apply_radial_brush's own face/i/j
	# enumeration and edge-feather convention).
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var center_lattice: Array = Deltas.dir_to_lattice(center)
	var face: int = int(center_lattice[0])
	var center_i: int = int(round(float(center_lattice[1])))
	var center_j: int = int(round(float(center_lattice[2])))
	var extent: int = maxi(1, int(ceil(radius_m / spacing_m)) + 2)
	var hard: float = clampf(hardness, 0.0, 0.98)

	var addresses := PackedInt64Array()
	var values := PackedFloat32Array()
	var visited: Dictionary = {}
	for source_j in range(center_j - extent, center_j + extent + 1):
		for source_i in range(center_i - extent, center_i + extent + 1):
			var address: Vector3i = Deltas.canonical_address(face, source_i, source_j)
			if address.x < 0:
				continue
			var address_key: String = "%d:%d:%d" % [address.x, address.y, address.z]
			if visited.has(address_key):
				continue
			visited[address_key] = true
			var sample_dir: Vector3 = Deltas.lattice_to_dir(
				address.x, float(address.y), float(address.z))
			var distance_m: float = acos(clampf(center.dot(sample_dir), -1.0, 1.0)) * planet_radius
			if distance_m > radius_m:
				continue
			var normalized_distance: float = distance_m / maxf(radius_m, 0.001)
			var weight: float = 1.0
			if normalized_distance > hard:
				var edge_t: float = (normalized_distance - hard) / maxf(1.0 - hard, 0.001)
				weight = 1.0 - smoothstep(0.0, 1.0, edge_t)
			if weight <= 0.0001:
				continue
			var rel: Vector3 = sample_dir - center
			var px: float = rel.dot(right) * planet_radius
			var py: float = rel.dot(up) * planet_radius
			var erosion_delta_m: float = _bilinear_sample(delta, resolution, px / cell_m + half,
				py / cell_m + half) * weight
			if absf(erosion_delta_m) <= 1e-6:
				continue
			var existing: float = Deltas.offset_at(sample_dir)
			addresses.append(Deltas.pack_address(address))
			values.append(existing + erosion_delta_m)

	var written: int = Deltas.set_packed_offsets_batch(addresses, values)
	if written > 0:
		Deltas.notify_changed(center, radius_m + spacing_m * 2.0)

	return {
		"ok": true,
		"native_points_written": written,
		"sim_min_delta_m": min_d,
		"sim_max_delta_m": max_d,
		"sim_mean_delta_m": sum_d / float(maxi(heights.size(), 1)),
	}


static func _find_displacement_runtime() -> Node:
	var loop: SceneTree = Engine.get_main_loop() as SceneTree
	if loop == null:
		return null
	return loop.get_first_node_in_group(&"terrain_displacement_runtime")


## Coarse elevation + biome authoring + existing edits, matching how the
## render and contact height compose (see gpu_terrain_height_query_contact.gd
## / cached_surface.gdshader). Deliberately synchronous/CPU-only -- a bake
## samples many points and shouldn't wait on an async GPU height query per
## sample.
static func _sample_current_height(d: Vector3, runtime: Node) -> float:
	var macro_h: float = Planet.terrain_height(d) if Planet.has_method("terrain_height") else 0.0
	var biome_h: float = 0.0
	if runtime != null and is_instance_valid(runtime) and runtime.has_method("_biome_profiles_height"):
		biome_h = float(runtime.call("_biome_profiles_height", d, -1))
	var edit_h: float = Deltas.offset_at(d)
	return macro_h + biome_h + edit_h


static func _bilinear_sample(field: PackedFloat32Array, resolution: int, gx: float, gy: float) -> float:
	var x0: int = clampi(int(floor(gx)), 0, resolution - 1)
	var y0: int = clampi(int(floor(gy)), 0, resolution - 1)
	var x1: int = clampi(x0 + 1, 0, resolution - 1)
	var y1: int = clampi(y0 + 1, 0, resolution - 1)
	var fx: float = clampf(gx - float(x0), 0.0, 1.0)
	var fy: float = clampf(gy - float(y0), 0.0, 1.0)
	var v00: float = field[y0 * resolution + x0]
	var v10: float = field[y0 * resolution + x1]
	var v01: float = field[y1 * resolution + x0]
	var v11: float = field[y1 * resolution + x1]
	return lerpf(lerpf(v00, v10, fx), lerpf(v01, v11, fx), fy)
