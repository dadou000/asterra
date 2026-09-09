class_name PlanetStudioRegionalHydrology
extends RefCounted
## Regional coarse hydrology re-analysis for Planet Studio.
##
## Flow routing and accumulation have non-local dependencies, so receiver topology
## is solved globally against the current coarse elevation field. Only hydrology
## cells intersecting the requested spherical region are then replaced. Soil and
## biome passes are deterministic consequences and are rerun afterwards; outside
## the edited hydrology region their inputs are unchanged and therefore their
## outputs remain unchanged.

const SECONDS_PER_YEAR := 31556736.0
const RUNOFF_COEFFICIENT := 0.38

signal progress(label: String, fraction: float)
signal completed(cell_count: int, elapsed_ms: float)


func reanalyse(center_dir: Vector3, radius_m: float) -> Dictionary:
	if not Planet.ready_state or Planet.fields == null or Planet.grid == null \
			or Planet.cfg == null or center_dir.length_squared() < 0.5:
		return {"ok": false, "reason": "Planet fields are not ready."}
	var started := Time.get_ticks_usec()
	var center := center_dir.normalized()
	var planet_radius := maxf(float(Planet.cfg.planet_radius), 1.0)
	var angular_radius := clampf(maxf(radius_m, 1.0) / planet_radius, 0.0, PI)
	var fields: PlanetFields = Planet.fields
	var grid: PlanetGrid = Planet.grid
	var n := grid.cell_count
	progress.emit("Flow topology", 0.05)
	var router := FlowRouter.new(grid)
	router.route_and_order(fields.elev, 0.0)

	# Global accumulation is required even for a local edit because upstream area
	# may enter the selected region from arbitrarily far away.
	var runoff_weight := PackedFloat32Array()
	var area := PackedFloat32Array()
	runoff_weight.resize(n)
	area.resize(n)
	for c in n:
		var cell_area := grid.cell_size[c] * grid.cell_size[c]
		area[c] = cell_area
		var runoff := RUNOFF_COEFFICIENT * clampf(fields.precip[c] / 900.0, 0.15, 2.2)
		runoff_weight[c] = cell_area * (fields.precip[c] * 0.001) * runoff
	var volume := router.accumulate(runoff_weight)
	var accumulation := router.accumulate(area)
	progress.emit("Watersheds", 0.25)

	var watershed := PackedInt32Array()
	watershed.resize(n)
	for i in n:
		var c := router.order[i]
		var r := router.rec[c]
		watershed[c] = c if r == c else watershed[r]

	var stream_order := PackedByteArray()
	stream_order.resize(n)
	var max1 := PackedByteArray()
	var count1 := PackedByteArray()
	max1.resize(n)
	count1.resize(n)
	for i in range(n - 1, -1, -1):
		var c := router.order[i]
		var order_value := 1
		if count1[c] > 0:
			order_value = int(max1[c]) + (1 if count1[c] >= 2 else 0)
		stream_order[c] = mini(order_value, 255)
		var r := router.rec[c]
		if r != c:
			if order_value > max1[r]:
				max1[r] = order_value
				count1[r] = 1
			elif order_value == max1[r]:
				count1[r] = mini(255, int(count1[r]) + 1)
			elif count1[r] == 0:
				count1[r] = 1

	# Temporary global widths are needed by the local floodplain neighborhood test.
	var width := PackedFloat32Array()
	width.resize(n)
	for c in n:
		var q := volume[c] / SECONDS_PER_YEAR
		width[c] = 0.0 if q < 1.5 else clampf(7.2 * sqrt(q), 2.0, 2600.0)

	progress.emit("Regional hydrology", 0.50)
	var changed_cells := 0
	for c in n:
		var direction := grid.cell_dir(c)
		if center.angle_to(direction) > angular_radius:
			continue
		changed_cells += 1
		fields.flow_accum[c] = accumulation[c]
		fields.discharge[c] = volume[c] / SECONDS_PER_YEAR
		fields.flow_dir[c] = router.rec_slot[c]
		fields.watershed[c] = watershed[c]
		fields.stream_order[c] = stream_order[c]
		var h := fields.elev[c]
		var filled := router.filled[c]
		var wet_enough := fields.precip[c] > 180.0
		fields.lake_level[c] = filled if h >= 0.0 and filled > h + 4.0 and wet_enough else -1e9
		fields.river_width[c] = width[c]
		if h < 0.0:
			fields.floodplain[c] = 0.0
			fields.wetland[c] = 0.0
			continue
		var base := c * 8
		var relief := 0.0
		var near_river := width[c]
		var near_level := h
		for k in 8:
			var nb := grid.nbr[base + k]
			relief = maxf(relief, absf(h - fields.elev[nb]))
			if width[nb] > near_river:
				near_river = width[nb]
				near_level = fields.elev[nb]
		var slope := relief / maxf(grid.cell_size[c], 0.001)
		var flat := clampf(1.0 - slope * 34.0, 0.0, 1.0)
		var above := h - near_level
		var fp := flat * NoiseKit.smoothstepf(0.0, 45.0, near_river) \
			* clampf(1.0 - above / 55.0, 0.0, 1.0)
		fields.floodplain[c] = clampf(fp, 0.0, 1.0)
		var wet := flat * clampf(fields.precip[c] / 1400.0, 0.0, 1.4) \
			* (1.0 - fields.aquifer[c] * 0.55)
		if fields.lake_level[c] > -1e8:
			wet = maxf(wet, 0.85)
		fields.wetland[c] = clampf(wet * 0.9 + fp * 0.35, 0.0, 1.0)

	# These passes are deterministic and depend on hydrology. Re-running them keeps
	# wetlands/soil/vegetation coherent while avoiding a complete geology/climate bake.
	progress.emit("Soil consequences", 0.72)
	PassSoil.new(fields).run()
	progress.emit("Biome consequences", 0.86)
	PassBiome.new(fields).run()

	# Runtime hydrology owns additional reach/state caches derived from Planet.fields.
	var root := Engine.get_main_loop()
	if root is SceneTree:
		var hydro := (root as SceneTree).root.get_node_or_null(^"PersistentHydrologySystem")
		if hydro != null and hydro.has_method("_rebuild_store"):
			hydro.call_deferred("_rebuild_store")
	progress.emit("Complete", 1.0)
	var elapsed_ms := float(Time.get_ticks_usec() - started) / 1000.0
	completed.emit(changed_cells, elapsed_ms)
	return {
		"ok": true,
		"cells": changed_cells,
		"elapsed_ms": elapsed_ms,
		"radius_m": radius_m,
	}
