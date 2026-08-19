class_name MaterialStock
extends RefCounted
## A physical quantity of loose material.
##
## Matter is conserved: excavating puts real volume in here, filling takes it back
## out, and nothing is created or destroyed by the editor. Volumes are *loose*
## (post-bulking) cubic metres, because that is what a bucket or a truck bed
## actually holds; the in-place volume it came from is loose / swell.

## key -> { volume_loose, material_id, rock_family, props }
var entries: Dictionary = {}
var capacity: float = -1.0            ## m^3 loose, negative = unlimited

static func key_for(material_id: int, rock_family: int) -> int:
	return material_id * 64 + (rock_family + 1)

func total_volume() -> float:
	var t := 0.0
	for k in entries:
		t += entries[k]["volume_loose"]
	return t

func free_space() -> float:
	return 1e30 if capacity < 0.0 else maxf(0.0, capacity - total_volume())

## Add loose volume. Returns the amount that actually fit.
func add(material_id: int, rock_family: int, volume_loose: float, props: Dictionary) -> float:
	if volume_loose <= 0.0:
		return 0.0
	var accepted := minf(volume_loose, free_space())
	if accepted <= 0.0:
		return 0.0
	var k := key_for(material_id, rock_family)
	if not entries.has(k):
		entries[k] = {"volume_loose": 0.0, "material_id": material_id,
			"rock_family": rock_family, "props": props}
	entries[k]["volume_loose"] += accepted
	return accepted

## Remove loose volume, preferring the largest holding. Returns what was removed
## as a list of {material_id, rock_family, volume_loose, props}.
func take(volume_loose: float) -> Array:
	var out: Array = []
	var remaining := volume_loose
	var keys := entries.keys()
	keys.sort_custom(func(a, b): return entries[a]["volume_loose"] > entries[b]["volume_loose"])
	for k in keys:
		if remaining <= 1e-6:
			break
		var e: Dictionary = entries[k]
		var got: float = minf(e["volume_loose"], remaining)
		e["volume_loose"] -= got
		remaining -= got
		out.append({"material_id": e["material_id"], "rock_family": e["rock_family"],
			"volume_loose": got, "props": e["props"]})
		if e["volume_loose"] <= 1e-6:
			entries.erase(k)
	return out

func dominant() -> Dictionary:
	var best := {}
	var bv := -1.0
	for k in entries:
		if entries[k]["volume_loose"] > bv:
			bv = entries[k]["volume_loose"]
			best = entries[k]
	return best

func describe() -> String:
	if entries.is_empty():
		return "empty"
	var parts: Array[String] = []
	var keys := entries.keys()
	keys.sort_custom(func(a, b): return entries[a]["volume_loose"] > entries[b]["volume_loose"])
	for k in keys:
		var e: Dictionary = entries[k]
		parts.append("%s %.2f m3" % [MaterialDB.display_name(e["material_id"], e["rock_family"]), e["volume_loose"]])
	return String(", ").join(parts)

func serialize() -> Array:
	var out: Array = []
	for k in entries:
		var e: Dictionary = entries[k]
		out.append({"m": e["material_id"], "r": e["rock_family"], "v": e["volume_loose"]})
	return out

func deserialize(data: Array) -> void:
	entries.clear()
	for e in data:
		var props := MaterialDB.get_entry(e["m"])
		add(e["m"], e["r"], e["v"], props)
