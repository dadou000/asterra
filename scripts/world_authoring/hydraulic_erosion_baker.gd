class_name HydraulicErosionBaker
extends RefCounted
## Real (simplified) hydraulic erosion: simulates individual water droplets
## flowing downhill across a heightmap, picking up sediment where the flow is
## fast or the ground is steep, and dropping it where the flow slows or the
## ground flattens out -- the actual physical mechanism rain/river erosion
## works by. This is the standard particle-based technique behind terrain
## tools like Gaea and World Machine (Beyer, "Implementation of a method for
## Hydraulic Erosion", 2015).
##
## This is the offline/baked counterpart to Biome Terrain's Erosion Channels /
## Sediment Deposit layers (terrain_biome_profile.gdshaderinc's
## bp_erosion_channels/bp_sediment_deposit), which fake the same *look* with
## domain-warped noise because they run per-vertex, live, with no memory of
## neighbouring vertices. A real simulation needs the whole local height grid
## as shared, mutable state that thousands of droplets read and write in
## sequence -- global state a per-vertex shader fundamentally does not have --
## so it has to run once, offline, over a bounded region and bake its result
## into a heightmap texture (see erosion_region_bake.gd) rather than evaluate
## live like the rest of Biome Terrain.
##
## Runs on the CPU, sequentially, by design: each droplet reads and writes the
## shared heightmap as it moves, so parallel droplets race on the same cells.
## A GPU port needs float atomics (an int-quantised fixed-point workaround, in
## practice) to avoid that -- solvable, but this project has already hit two
## separate cases of a fancier shader path causing Vulkan device loss or a
## hung compile (see [[authored-displacement-cpu-gpu-split]]), and a bake is a
## one-shot "click Bake and wait a few seconds" action, not a per-frame cost,
## so CPU sequential is the safer choice until bake times prove it isn't fast
## enough.

## Matches the widely-used, empirically-stable reference parameters (Beyer
## 2015 / Sebastian Lague's Hydraulic-Erosion implementation) as closely as
## possible. Those were tuned against a HEIGHT FIELD NORMALISED TO [0, 1] --
## simulate() normalises this bake's real-metre heights the same way before
## running (and denormalises the result after) specifically so these numbers
## stay meaningful regardless of the actual relief of whatever region is
## baked. Do not tune these against a metre-scale heightmap directly: the
## first version of this did, inherited these same nominal values, and the
## capacity/erosion math -- calibrated for O(1) height differences -- was
## effectively ~200x too aggressive against O(200 m) real relief, which blew
## the simulation up into a runaway pit-digging feedback loop within a few
## thousand droplets (confirmed with a headless probe: heights that started
## in a 0-220 range ended up spanning roughly -400,000 to +340,000).
const DEFAULT_PARAMS := {
	"droplet_count": 40000,
	"max_lifetime": 30,       # steps before a droplet is forcibly retired
	"inertia": 0.05,          # 0 = always turn straight downhill, 1 = never turn
	"capacity_factor": 4.0,   # how much sediment one unit of fast/steep flow can carry
	"min_slope": 0.01,        # capacity floor so flat ground doesn't stop eroding entirely
	"erode_speed": 0.3,       # fraction of the capacity gap picked up per step
	"deposit_speed": 0.3,     # fraction of the excess sediment dropped per step
	"evaporate_speed": 0.01,  # fraction of carried water lost per step
	"gravity": 4.0,
	"initial_speed": 1.0,
	"initial_water": 1.0,
	"min_water": 0.01,        # droplet retires once its water drops below this
	"erode_radius": 3,        # brush radius (cells) sediment is picked up over
	"seed": 1337,
}


## Runs the simulation over `heights` (a PackedFloat32Array of length
## grid_size * grid_size, row-major, in metres) and returns the eroded result
## -- `heights` itself is left untouched; the caller diffs the two to get a
## deposit-positive/erode-negative delta. `cell_size_m` is currently unused by
## the physics (see DEFAULT_PARAMS) and reserved for future distance-aware
## tuning; kept in the signature so callers don't need to change when that
## lands.
static func simulate(heights: PackedFloat32Array, grid_size: int, _cell_size_m: float,
		params: Dictionary = {}) -> PackedFloat32Array:
	var p := DEFAULT_PARAMS.duplicate()
	for key: String in params:
		p[key] = params[key]
	if grid_size < 4 or heights.size() != grid_size * grid_size:
		return PackedFloat32Array(heights)

	# Normalise to [0, 1] -- see the DEFAULT_PARAMS comment for why.
	var min_h: float = heights[0]
	var max_h: float = heights[0]
	for v in heights:
		min_h = minf(min_h, v)
		max_h = maxf(max_h, v)
	var relief: float = maxf(max_h - min_h, 0.0001)
	var h := PackedFloat32Array()
	h.resize(heights.size())
	for i in heights.size():
		h[i] = (heights[i] - min_h) / relief

	var rng := RandomNumberGenerator.new()
	rng.seed = int(p["seed"])
	var brush: Array[Dictionary] = _build_brush(int(p["erode_radius"]))
	var droplet_count: int = maxi(0, int(p["droplet_count"]))
	var max_lifetime: int = maxi(1, int(p["max_lifetime"]))
	var inertia: float = clampf(float(p["inertia"]), 0.0, 1.0)
	var capacity_factor: float = maxf(float(p["capacity_factor"]), 0.0001)
	var min_slope: float = maxf(float(p["min_slope"]), 0.0)
	var erode_speed: float = clampf(float(p["erode_speed"]), 0.0, 1.0)
	var deposit_speed: float = clampf(float(p["deposit_speed"]), 0.0, 1.0)
	var evaporate_speed: float = clampf(float(p["evaporate_speed"]), 0.0, 0.999)
	var gravity: float = maxf(float(p["gravity"]), 0.0001)
	var min_water: float = maxf(float(p["min_water"]), 0.0001)

	for i in droplet_count:
		var pos := Vector2(rng.randf_range(0.0, float(grid_size - 1)),
			rng.randf_range(0.0, float(grid_size - 1)))
		var dir := Vector2.ZERO
		var speed: float = float(p["initial_speed"])
		var water: float = float(p["initial_water"])
		var sediment: float = 0.0

		for _step in max_lifetime:
			var gh := _height_and_gradient(h, grid_size, pos)
			var grad: Vector2 = gh["gradient"]
			var height: float = gh["height"]

			dir = dir * inertia - grad * (1.0 - inertia)
			var dir_len := dir.length()
			if dir_len > 1e-8:
				dir /= dir_len
			else:
				# Flat/pit: no gradient to follow -- pick a fresh random
				# direction rather than stalling in place forever.
				dir = Vector2.from_angle(rng.randf_range(0.0, TAU))

			var new_pos := pos + dir
			if new_pos.x < 0.0 or new_pos.x >= float(grid_size - 1) \
					or new_pos.y < 0.0 or new_pos.y >= float(grid_size - 1):
				break

			var new_height: float = _height_and_gradient(h, grid_size, new_pos)["height"]
			var delta_height := new_height - height

			var capacity: float = maxf(-delta_height, min_slope) \
				* speed * water * capacity_factor

			if sediment > capacity or delta_height > 0.0:
				# Uphill, or already carrying more than it can hold: drop some.
				var amount: float = (sediment - capacity) * deposit_speed if delta_height <= 0.0 \
					else minf(delta_height, sediment)
				amount = maxf(amount, 0.0)
				sediment -= amount
				_deposit_bilinear(h, grid_size, pos, amount)
			else:
				# Downhill with spare capacity: pick some up.
				var amount: float = minf((capacity - sediment) * erode_speed, -delta_height)
				amount = maxf(amount, 0.0)
				_erode_brush(h, grid_size, pos, amount, brush)
				sediment += amount

			speed = sqrt(maxf(speed * speed + delta_height * gravity, 0.0))
			water *= (1.0 - evaporate_speed)
			pos = new_pos
			if water < min_water:
				break

	var result := PackedFloat32Array()
	result.resize(h.size())
	for i in h.size():
		result[i] = h[i] * relief + min_h
	return result


## Bilinearly interpolated height and gradient (finite difference in cell
## units) at a fractional grid position.
static func _height_and_gradient(h: PackedFloat32Array, grid_size: int, pos: Vector2) -> Dictionary:
	var x0: int = clampi(int(floor(pos.x)), 0, grid_size - 2)
	var y0: int = clampi(int(floor(pos.y)), 0, grid_size - 2)
	var fx: float = clampf(pos.x - float(x0), 0.0, 1.0)
	var fy: float = clampf(pos.y - float(y0), 0.0, 1.0)
	var h00: float = h[y0 * grid_size + x0]
	var h10: float = h[y0 * grid_size + x0 + 1]
	var h01: float = h[(y0 + 1) * grid_size + x0]
	var h11: float = h[(y0 + 1) * grid_size + x0 + 1]
	var gx: float = (h10 - h00) * (1.0 - fy) + (h11 - h01) * fy
	var gy: float = (h01 - h00) * (1.0 - fx) + (h11 - h10) * fx
	var height: float = h00 * (1.0 - fx) * (1.0 - fy) + h10 * fx * (1.0 - fy) \
		+ h01 * (1.0 - fx) * fy + h11 * fx * fy
	return {"height": height, "gradient": Vector2(gx, gy)}


## Adds `amount` at `pos`, split across its 4 surrounding cells by the same
## bilinear weights _height_and_gradient reads with -- deposition exactly
## undoes an equal-and-opposite erosion pass at the same position.
static func _deposit_bilinear(h: PackedFloat32Array, grid_size: int, pos: Vector2, amount: float) -> void:
	if amount <= 0.0:
		return
	var x0: int = clampi(int(floor(pos.x)), 0, grid_size - 2)
	var y0: int = clampi(int(floor(pos.y)), 0, grid_size - 2)
	var fx: float = clampf(pos.x - float(x0), 0.0, 1.0)
	var fy: float = clampf(pos.y - float(y0), 0.0, 1.0)
	h[y0 * grid_size + x0] += amount * (1.0 - fx) * (1.0 - fy)
	h[y0 * grid_size + x0 + 1] += amount * fx * (1.0 - fy)
	h[(y0 + 1) * grid_size + x0] += amount * (1.0 - fx) * fy
	h[(y0 + 1) * grid_size + x0 + 1] += amount * fx * fy


## Precomputes a normalised (weights sum to 1), falloff-weighted disc of
## offsets used to spread erosion pickup over an area instead of one cell --
## real flow erodes a patch of streambed under and around the droplet, not a
## single infinitely small point, and picking up from just one cell (or its 4
## bilinear neighbours) carves single-cell spikes/pits instead of a channel.
static func _build_brush(radius: int) -> Array[Dictionary]:
	var r: int = maxi(1, radius)
	var offsets: Array[Dictionary] = []
	var total: float = 0.0
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var dist: float = sqrt(float(dx * dx + dy * dy))
			if dist > float(r):
				continue
			var w: float = maxf(float(r) - dist, 0.0)
			if w <= 0.0:
				continue
			offsets.append({"dx": dx, "dy": dy, "w": w})
			total += w
	if total > 0.0:
		for entry: Dictionary in offsets:
			entry["w"] = float(entry["w"]) / total
	return offsets


## Removes `amount` from the height field, spread over the brush footprint
## centred on `pos` and clamped to the grid.
static func _erode_brush(h: PackedFloat32Array, grid_size: int, pos: Vector2, amount: float,
		brush: Array[Dictionary]) -> void:
	if amount <= 0.0 or brush.is_empty():
		return
	var cx: int = clampi(int(round(pos.x)), 0, grid_size - 1)
	var cy: int = clampi(int(round(pos.y)), 0, grid_size - 1)
	for entry: Dictionary in brush:
		var x: int = cx + int(entry["dx"])
		var y: int = cy + int(entry["dy"])
		if x < 0 or x >= grid_size or y < 0 or y >= grid_size:
			continue
		var idx: int = y * grid_size + x
		h[idx] -= amount * float(entry["w"])
