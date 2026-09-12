class_name PlanetStudioRegionalHydrology
extends RefCounted
## Regional coarse hydrology re-analysis for Planet Studio. Receiver topology is
## solved globally for correct upstream context; derived fields are replaced only
## inside the requested spherical region before soil/biome consequences refresh.

const SECONDS_PER_YEAR := 31556736.0
const RUNOFF_COEFFICIENT := 0.38
signal progress(label: String, fraction: float)
signal completed(cell_count: int, elapsed_ms: float)

func reanalyse(center_dir: Vector3, radius_m: float) -> Dictionary:
	if not Planet.ready_state or Planet.fields == null or Planet.grid == null or Planet.cfg == null or center_dir.length_squared() < 0.5:
		return {"ok": false, "reason": "Planet fields are not ready."}
	var started := Time.get_ticks_usec()
	var center := center_dir.normalized()
	var planet_radius := maxf(float(Planet.cfg.planet_radius), 1.0)
	var angular_radius := clampf(maxf(radius_m, 1.0) / planet_radius, 0.0, PI)
	# Planet is an autoload whose CI substitute intentionally exposes Resource. Keep
	# this single boundary dynamic; production generator/pass classes remain typed.
	var fields: Variant = Planet.fields
	var grid: Variant = Planet.grid
	if fields == null or grid == null:
		return {"ok": false, "reason": "Planet fields are unavailable."}
	var n := int(grid.cell_count)
	progress.emit("Flow topology", 0.05)
	var router := FlowRouter.new(grid)
	router.route_and_order(fields.elev, 0.0)

	var runoff_weight := PackedFloat32Array()
	var area := PackedFloat32Array()
	runoff_weight.resize(n); area.resize(n)
	for c in n:
		var cell_area := float(grid.cell_size[c]) * float(grid.cell_size[c])
		area[c] = cell_area
		var runoff := RUNOFF_COEFFICIENT * clampf(float(fields.precip[c]) / 900.0, 0.15, 2.2)
		runoff_weight[c] = cell_area * (float(fields.precip[c]) * 0.001) * runoff
	var volume: PackedFloat32Array = router.accumulate(runoff_weight)
	var accumulation: PackedFloat32Array = router.accumulate(area)
	progress.emit("Watersheds", 0.25)

	var watershed := PackedInt32Array(); watershed.resize(n)
	for i in n:
		var c := int(router.order[i]); var receiver := int(router.rec[c])
		watershed[c] = c if receiver == c else watershed[receiver]

	var stream_order := PackedByteArray(); var max1 := PackedByteArray(); var count1 := PackedByteArray()
	stream_order.resize(n); max1.resize(n); count1.resize(n)
	for i in range(n - 1, -1, -1):
		var c := int(router.order[i]); var order_value := 1
		if count1[c] > 0: order_value = int(max1[c]) + (1 if count1[c] >= 2 else 0)
		stream_order[c] = mini(order_value, 255)
		var receiver := int(router.rec[c])
		if receiver == c: continue
		if order_value > max1[receiver]:
			max1[receiver] = order_value; count1[receiver] = 1
		elif order_value == max1[receiver]: count1[receiver] = mini(255, int(count1[receiver]) + 1)
		elif count1[receiver] == 0: count1[receiver] = 1

	var width := PackedFloat32Array(); width.resize(n)
	for c in n:
		var discharge := float(volume[c]) / SECONDS_PER_YEAR
		width[c] = 0.0 if discharge < 1.5 else clampf(7.2 * sqrt(discharge), 2.0, 2600.0)

	progress.emit("Regional hydrology", 0.50)
	var changed_cells := 0
	for c in n:
		var direction: Vector3 = grid.cell_dir(c)
		if center.angle_to(direction) > angular_radius: continue
		changed_cells += 1
		fields.flow_accum[c] = accumulation[c]; fields.discharge[c] = volume[c] / SECONDS_PER_YEAR
		fields.flow_dir[c] = router.rec_slot[c]; fields.watershed[c] = watershed[c]; fields.stream_order[c] = stream_order[c]
		var height := float(fields.elev[c]); var filled := float(router.filled[c]); var wet_enough := float(fields.precip[c]) > 180.0
		fields.lake_level[c] = filled if height >= 0.0 and filled > height + 4.0 and wet_enough else -1e9
		fields.river_width[c] = width[c]
		if height < 0.0:
			fields.floodplain[c] = 0.0; fields.wetland[c] = 0.0; continue
		var base := c * 8; var relief := 0.0; var near_river := float(width[c]); var near_level := height
		for k in 8:
			var neighbor := int(grid.nbr[base + k])
			relief = maxf(relief, absf(height - float(fields.elev[neighbor])))
			if width[neighbor] > near_river: near_river = width[neighbor]; near_level = float(fields.elev[neighbor])
		var slope := relief / maxf(float(grid.cell_size[c]), 0.001); var flat := clampf(1.0 - slope * 34.0, 0.0, 1.0)
		var floodplain := flat * NoiseKit.smoothstepf(0.0, 45.0, near_river) * clampf(1.0 - (height - near_level) / 55.0, 0.0, 1.0)
		fields.floodplain[c] = clampf(floodplain, 0.0, 1.0)
		var wet := flat * clampf(float(fields.precip[c]) / 1400.0, 0.0, 1.4) * (1.0 - float(fields.aquifer[c]) * 0.55)
		if float(fields.lake_level[c]) > -1e8: wet = maxf(wet, 0.85)
		fields.wetland[c] = clampf(wet * 0.9 + floodplain * 0.35, 0.0, 1.0)

	progress.emit("Soil consequences", 0.72); PassSoil.new(fields).run()
	progress.emit("Biome consequences", 0.86); PassBiome.new(fields).run()
	var loop := Engine.get_main_loop()
	if loop is SceneTree:
		var hydro := (loop as SceneTree).root.get_node_or_null(^"PersistentHydrologySystem")
		if hydro != null and hydro.has_method("_rebuild_store"): hydro.call_deferred("_rebuild_store")
	progress.emit("Complete", 1.0)
	var elapsed_ms := float(Time.get_ticks_usec() - started) / 1000.0
	completed.emit(changed_cells, elapsed_ms)
	return {"ok": true, "cells": changed_cells, "elapsed_ms": elapsed_ms, "radius_m": radius_m}
