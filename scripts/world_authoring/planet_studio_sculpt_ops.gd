class_name PlanetStudioSculptOps
extends RefCounted
## Advanced non-destructive sculpt operators for Planet Studio.
##
## All operators write the existing Deltas cube-sphere lattice. They never create a
## second heightfield, and every write therefore remains visible to rendering,
## contact, saving and the existing runtime deformation mirror.

enum Mode {
	SMOOTH,
	FLATTEN,
	TERRACE,
	NOISE,
	ERODE,
	RESTORE,
}

const MIN_OFFSET_M := -10000.0
const MAX_OFFSET_M := 10000.0


static func apply_filter(mode: int, center_dir: Vector3, radius_m: float,
		strength: float, hardness: float, planet_radius: float,
		target_height_m: float = 0.0, seed: int = 1337,
		terrace_step_m: float = 2.0) -> int:
	if center_dir.length_squared() < 0.5 or radius_m <= 0.0 or planet_radius <= 1.0:
		return 0
	if mode == Mode.RESTORE:
		return int(Deltas.erase_radial_brush(center_dir.normalized(), radius_m,
			clampf(strength, 0.0, 1.0), hardness, planet_radius))

	var center := center_dir.normalized()
	var samples := _brush_samples(center, radius_m, planet_radius)
	if samples.is_empty():
		return 0
	var writes: Array[Dictionary] = []
	var hard := clampf(hardness, 0.0, 0.98)
	var amount := clampf(strength, 0.0, 1.0)
	var noise := FastNoiseLite.new()
	noise.seed = seed
	noise.frequency = 0.035
	noise.fractal_octaves = 4
	noise.fractal_gain = 0.52
	noise.fractal_lacunarity = 2.0

	for sample in samples:
		var address: Vector3i = sample["address"]
		var direction: Vector3 = sample["dir"]
		var distance_m: float = float(sample["distance_m"])
		var weight := _falloff(distance_m / maxf(radius_m, 0.001), hard) * amount
		if weight <= 0.0001:
			continue
		var current := Deltas.get_offset(address.x, address.y, address.z)
		var desired := current
		match mode:
			Mode.SMOOTH:
				var average := _neighbor_average(address)
				desired = lerpf(current, average, weight)
			Mode.FLATTEN:
				var base_height := TerrainContactSampler.coarse_height(direction) - current
				var target_offset := target_height_m - base_height
				desired = lerpf(current, target_offset, weight)
			Mode.TERRACE:
				var base_height2 := TerrainContactSampler.coarse_height(direction) - current
				var total_height := base_height2 + current
				var step := maxf(terrace_step_m, 0.05)
				var terraced_height := round(total_height / step) * step
				desired = lerpf(current, terraced_height - base_height2, weight)
			Mode.NOISE:
				var p := direction * 1000.0
				var n := noise.get_noise_3d(p.x, p.y, p.z)
				desired = current + n * maxf(0.01, terrace_step_m) * weight
			Mode.ERODE:
				var average2 := _neighbor_average(address)
				# Move peaks harder than pits: a cheap, stable local erosion relaxer.
				var transport := average2 - current
				if transport < 0.0:
					transport *= 0.35
				desired = current + transport * weight
			_:
				continue
		writes.append({"address": address, "value": clampf(desired, MIN_OFFSET_M, MAX_OFFSET_M)})

	var changed := int(Deltas.set_offsets_batch(writes, MIN_OFFSET_M, MAX_OFFSET_M))
	if changed > 0:
		Deltas.notify_changed(center, radius_m + Deltas.sample_spacing(planet_radius) * 3.0)
	return changed


static func apply_corridor_grade(start_dir: Vector3, end_dir: Vector3,
		start_height_m: float, end_height_m: float, half_width_m: float,
		shoulder_m: float, planet_radius: float, strength: float = 1.0) -> int:
	if start_dir.length_squared() < 0.5 or end_dir.length_squared() < 0.5:
		return 0
	var a := start_dir.normalized()
	var b := end_dir.normalized()
	var arc_m := a.angle_to(b) * planet_radius
	if arc_m <= 0.01:
		return 0
	var spacing := maxf(minf(half_width_m * 0.55, 12.0), Deltas.sample_spacing(planet_radius) * 2.0)
	var steps := clampi(int(ceil(arc_m / spacing)), 1, 4096)
	var changed := 0
	var brush_radius := maxf(half_width_m + shoulder_m, 0.5)
	for i in steps + 1:
		var t := float(i) / float(steps)
		var direction := _slerp_dir(a, b, t)
		var target := lerpf(start_height_m, end_height_m, t)
		changed += apply_filter(Mode.FLATTEN, direction, brush_radius,
			clampf(strength, 0.0, 1.0),
			clampf(half_width_m / maxf(brush_radius, 0.001), 0.0, 0.98),
			planet_radius, target)
	return changed


static func apply_image_stamp(image: Image, center_dir: Vector3, radius_m: float,
		height_scale_m: float, planet_radius: float, strength: float = 1.0,
		absolute_height: bool = false, target_mid_height_m: float = 0.0) -> int:
	if image == null or image.is_empty() or center_dir.length_squared() < 0.5 \
			or radius_m <= 0.0 or planet_radius <= 1.0:
		return 0
	var center := center_dir.normalized()
	var basis := _tangent_basis(center)
	var right: Vector3 = basis[0]
	var up: Vector3 = basis[1]
	var spacing := maxf(Deltas.sample_spacing(planet_radius), 0.05)
	var resolution := clampi(int(ceil((radius_m * 2.0) / spacing)) + 1, 3, 2049)
	var writes: Array[Dictionary] = []
	var visited: Dictionary = {}
	var factor := clampf(strength, 0.0, 1.0)
	for y in resolution:
		var v := float(y) / float(resolution - 1)
		var oy := (v * 2.0 - 1.0) * radius_m
		for x in resolution:
			var u := float(x) / float(resolution - 1)
			var ox := (u * 2.0 - 1.0) * radius_m
			var radial := Vector2(ox, oy).length()
			if radial > radius_m:
				continue
			var direction := (center + right * (ox / planet_radius) + up * (oy / planet_radius)).normalized()
			var lattice: Array = Deltas.dir_to_lattice(direction)
			var address := Deltas.canonical_address(int(lattice[0]), int(round(float(lattice[1]))), int(round(float(lattice[2]))))
			var packed := int(Deltas.pack_address(address))
			if packed < 0 or visited.has(packed):
				continue
			visited[packed] = true
			var pixel := image.get_pixelv(Vector2i(
				clampi(int(round(u * float(image.get_width() - 1))), 0, image.get_width() - 1),
				clampi(int(round((1.0 - v) * float(image.get_height() - 1))), 0, image.get_height() - 1)))
			var gray := pixel.get_luminance()
			var current := Deltas.get_offset(address.x, address.y, address.z)
			var desired: float
			if absolute_height:
				var base_height := TerrainContactSampler.coarse_height(direction) - current
				desired = target_mid_height_m + (gray - 0.5) * height_scale_m - base_height
			else:
				desired = current + (gray - 0.5) * height_scale_m
			var edge := 1.0 - smoothstep(0.82, 1.0, radial / radius_m)
			desired = lerpf(current, desired, factor * edge)
			writes.append({"address": address, "value": clampf(desired, MIN_OFFSET_M, MAX_OFFSET_M)})
	var changed := int(Deltas.set_offsets_batch(writes, MIN_OFFSET_M, MAX_OFFSET_M))
	if changed > 0:
		Deltas.notify_changed(center, radius_m + spacing * 3.0)
	return changed


static func sample_current_height(direction: Vector3) -> float:
	if direction.length_squared() < 0.5:
		return 0.0
	return TerrainContactSampler.contact_height(direction.normalized(),
		TerrainContactSampler.coarse_height(direction.normalized()))


static func _brush_samples(center: Vector3, radius_m: float, planet_radius: float) -> Array[Dictionary]:
	var center_lattice: Array = Deltas.dir_to_lattice(center)
	var face := int(center_lattice[0])
	var ci := int(round(float(center_lattice[1])))
	var cj := int(round(float(center_lattice[2])))
	var spacing := maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var extent := maxi(1, int(ceil(radius_m / spacing)) + 2)
	var visited: Dictionary = {}
	var out: Array[Dictionary] = []
	for sj in range(cj - extent, cj + extent + 1):
		for si in range(ci - extent, ci + extent + 1):
			var address: Vector3i = Deltas.canonical_address(face, si, sj)
			if address.x < 0:
				continue
			var packed := int(Deltas.pack_address(address))
			if visited.has(packed):
				continue
			visited[packed] = true
			var direction: Vector3 = Deltas.lattice_to_dir(address.x, float(address.y), float(address.z))
			var distance_m := center.angle_to(direction) * planet_radius
			if distance_m <= radius_m:
				out.append({"address": address, "dir": direction, "distance_m": distance_m})
	return out


static func _neighbor_average(address: Vector3i) -> float:
	var total := 0.0
	var count := 0
	for dj in range(-1, 2):
		for di in range(-1, 2):
			var neighbor: Vector3i = Deltas.canonical_address(address.x, address.y + di, address.z + dj)
			if neighbor.x < 0:
				continue
			total += Deltas.get_offset(neighbor.x, neighbor.y, neighbor.z)
			count += 1
	return total / float(maxi(count, 1))


static func _falloff(t: float, hardness: float) -> float:
	if t >= 1.0:
		return 0.0
	if t <= hardness:
		return 1.0
	var edge_t := (t - hardness) / maxf(1.0 - hardness, 0.001)
	return 1.0 - smoothstep(0.0, 1.0, edge_t)


static func _tangent_basis(normal: Vector3) -> Array[Vector3]:
	var reference := Vector3.UP if absf(normal.dot(Vector3.UP)) < 0.92 else Vector3.RIGHT
	var right := reference.cross(normal).normalized()
	var up := normal.cross(right).normalized()
	return [right, up]


static func _slerp_dir(a: Vector3, b: Vector3, t: float) -> Vector3:
	var dot_value := clampf(a.dot(b), -1.0, 1.0)
	var omega := acos(dot_value)
	if omega < 1e-5:
		return a.lerp(b, t).normalized()
	var sin_omega := sin(omega)
	return (a * (sin((1.0 - t) * omega) / sin_omega) + b * (sin(t * omega) / sin_omega)).normalized()
