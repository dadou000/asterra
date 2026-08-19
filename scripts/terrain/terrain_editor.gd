class_name TerrainEditor
extends Node
## 1.7 Editable terrain.
##
## Dig, cut, grade and fill, on the sparse delta lattice. Every cubic metre that
## leaves the ground arrives somewhere: in the player's bucket, or in a physical
## pile. Filling consumes real stock. The editor refuses to invent matter, which
## is what makes "keep the excess material" in the Phase 2 mission a property of
## the terrain system rather than mission script.

signal edited(center_dir: Vector3, radius_m: float)
signal material_excavated(stock: MaterialStock)

const MIN_OFFSET := -60.0
const MAX_OFFSET := 60.0

var piles: Array[LoosePile] = []
var pile_parent: Node3D

var _spacing: float = 1.0

func _ready() -> void:
	_spacing = Deltas.sample_spacing(Planet.cfg.planet_radius) if Planet.ready_state else 1.0

func refresh() -> void:
	_spacing = Deltas.sample_spacing(Planet.cfg.planet_radius)

## Excavate a spherical-cap brush. Returns the material removed, as loose volume.
func dig(center_dir: Vector3, radius_m: float, depth_m: float) -> MaterialStock:
	return _apply(center_dir, radius_m, -absf(depth_m), NAN, null)

## Raise ground using material from `source`, consuming it. Returns m^3 in place.
func fill(center_dir: Vector3, radius_m: float, height_m: float, source: MaterialStock) -> float:
	var before := source.total_volume()
	_apply(center_dir, radius_m, absf(height_m), NAN, source)
	return before - source.total_volume()

## Cut or fill toward a target elevation (metres relative to sea level).
func grade(center_dir: Vector3, radius_m: float, target_h: float, source: MaterialStock) -> MaterialStock:
	return _apply(center_dir, radius_m, 0.0, target_h, source)

func _apply(center_dir: Vector3, radius_m: float, amount: float, target_h: float,
		source: MaterialStock) -> MaterialStock:
	refresh()
	var out := MaterialStock.new()
	var lat := Deltas.dir_to_lattice(center_dir)
	var face: int = lat[0]
	var gi: float = lat[1]
	var gj: float = lat[2]
	var reach := int(ceil(radius_m / _spacing)) + 1
	var i0 := int(floor(gi)) - reach
	var j0 := int(floor(gj)) - reach
	var cell_area := _spacing * _spacing
	var detail := Planet._detail_main
	var changed := false

	for j in range(j0, j0 + reach * 2 + 1):
		for i in range(i0, i0 + reach * 2 + 1):
			var d := Deltas.lattice_to_dir(face, float(i), float(j))
			var dist := d.angle_to(center_dir) * Planet.cfg.planet_radius
			if dist > radius_m:
				continue
			var falloff := 1.0 - NoiseKit.smoothstepf(0.35, 1.0, dist / maxf(radius_m, 0.01))
			if falloff <= 1e-3:
				continue

			var pristine := Planet.pristine_height(d, detail)
			var cur_off := Deltas.get_offset(face, i, j)
			var cur_h := pristine + cur_off
			var want: float
			if is_nan(target_h):
				want = amount * falloff
			else:
				want = (target_h - cur_h) * falloff
			if absf(want) < 1e-4:
				continue

			if want < 0.0:
				# --- excavation: charge each slice to the material it came from
				var applied := Deltas.add_offset(face, i, j, want, MIN_OFFSET, MAX_OFFSET)
				if applied == 0.0:
					continue
				changed = true
				var top_depth := maxf(0.0, pristine - cur_h)
				var bot_depth := top_depth + -applied
				var mid := (top_depth + bot_depth) * 0.5
				var props := Planet.column_material(d, mid)
				var in_place := -applied * cell_area
				out.add(props["id"], props["rock_family"], in_place * float(props["swell"]), props)
			else:
				# --- fill: only as much as the stock can pay for
				var in_place_needed := want * cell_area
				if source != null:
					var swell := 1.25
					var dom := source.dominant()
					if not dom.is_empty():
						swell = float(dom["props"].get("swell", 1.25))
					var taken := source.take(in_place_needed * swell)
					var got := 0.0
					for t in taken:
						got += t["volume_loose"] / maxf(1.01, float(t["props"].get("swell", swell)))
					if got <= 1e-7:
						continue
					want = got / cell_area
				var applied2 := Deltas.add_offset(face, i, j, want, MIN_OFFSET, MAX_OFFSET)
				if applied2 != 0.0:
					changed = true

	if changed:
		Deltas.notify_changed(center_dir, radius_m * 1.6)
		edited.emit(center_dir, radius_m * 1.6)
	if out.total_volume() > 0.0:
		material_excavated.emit(out)
	return out

## Drop material on the ground as a physical pile.
func drop_pile(stock: MaterialStock, center_dir: Vector3) -> LoosePile:
	if stock.total_volume() <= 1e-5:
		return null
	var existing := _pile_near(center_dir, 2.5)
	if existing != null:
		existing.add_from(stock)
		return existing
	var pile := LoosePile.new()
	var h := Planet.terrain_height(center_dir)
	var r := Planet.cfg.planet_radius + h
	pile.setup(stock, Vec3D.new(center_dir.x * r, center_dir.y * r, center_dir.z * r), center_dir)
	if pile_parent != null:
		pile_parent.add_child(pile)
	piles.append(pile)
	return pile

func _pile_near(dir: Vector3, radius_m: float) -> LoosePile:
	for p in piles:
		if not is_instance_valid(p):
			continue
		if p.surface_dir.angle_to(dir) * Planet.cfg.planet_radius < radius_m:
			return p
	return null

func collect_pile_near(dir: Vector3, radius_m: float, into: MaterialStock) -> float:
	var p := _pile_near(dir, radius_m)
	if p == null:
		return 0.0
	var moved := 0.0
	for k in p.stock.entries.keys():
		var e: Dictionary = p.stock.entries[k]
		var got := into.add(e["material_id"], e["rock_family"], e["volume_loose"], e["props"])
		e["volume_loose"] -= got
		moved += got
		if e["volume_loose"] <= 1e-6:
			p.stock.entries.erase(k)
	if p.stock.total_volume() <= 1e-6:
		piles.erase(p)
		p.queue_free()
	else:
		p._rebuild()
	return moved

func serialize_piles() -> Array:
	var out: Array = []
	for p in piles:
		if is_instance_valid(p):
			out.append(p.serialize())
	return out

func restore_piles(data: Array) -> void:
	for p in piles:
		if is_instance_valid(p):
			p.queue_free()
	piles.clear()
	for e in data:
		var stock := MaterialStock.new()
		stock.deserialize(e["stock"])
		var wp := Vec3D.new(e["x"], e["y"], e["z"])
		var pile := LoosePile.new()
		pile.setup(stock, wp, wp.normalized().to_v3())
		if pile_parent != null:
			pile_parent.add_child(pile)
		piles.append(pile)
