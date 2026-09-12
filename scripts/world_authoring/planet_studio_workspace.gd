class_name PlanetStudioWorkspace
extends RefCounted
## Editor-only persistent metadata for Planet Studio.
##
## None of this data changes the authoritative terrain/water/biome runtime model.
## It stores organization and workflow state that must survive editor restarts:
## groups, hidden/locked authoring objects, bookmarks, dependency-stale regions,
## custom terrain recipes, seed-explorer locks and lightweight timing diagnostics.

const ROOT_DIR := "user://world_authoring/workspace"
const VERSION := 1

var body_id: String = "asterra"
var groups: Array[Dictionary] = []
var hidden: Dictionary = {}
var locked: Dictionary = {}
var bookmarks: Array[Dictionary] = []
var stale_regions: Array[Dictionary] = []
var custom_presets: Array[Dictionary] = []
var seed_locks: Dictionary = {}
var timings_ms: Dictionary = {}
var ui_state: Dictionary = {}


func bind_body(next_body_id: String) -> void:
	var safe := next_body_id.strip_edges()
	if safe.is_empty():
		safe = "asterra"
	if safe == body_id and not groups.is_empty():
		return
	body_id = safe
	_load()


func object_key(kind: String, id: String) -> String:
	return "%s:%s" % [kind, id]


func is_hidden(kind: String, id: String) -> bool:
	return bool(hidden.get(object_key(kind, id), false))


func set_hidden(kind: String, id: String, value: bool) -> void:
	var key := object_key(kind, id)
	if value:
		hidden[key] = true
	else:
		hidden.erase(key)
	_save()


func is_locked(kind: String, id: String) -> bool:
	return bool(locked.get(object_key(kind, id), false))


func set_locked(kind: String, id: String, value: bool) -> void:
	var key := object_key(kind, id)
	if value:
		locked[key] = true
	else:
		locked.erase(key)
	_save()


func add_group(name: String) -> String:
	var id := "group-%d-%d" % [Time.get_ticks_usec(), randi() & 0x7fffffff]
	groups.append({"id": id, "name": _safe_name(name, "New Group"), "items": []})
	_save()
	return id


func remove_group(group_id: String) -> void:
	for index in range(groups.size() - 1, -1, -1):
		if String(groups[index].get("id", "")) == group_id:
			groups.remove_at(index)
	_save()


func rename_group(group_id: String, name: String) -> void:
	for index in groups.size():
		if String(groups[index].get("id", "")) == group_id:
			var group := groups[index]
			group["name"] = _safe_name(name, "Group")
			groups[index] = group
			break
	_save()


func assign_to_group(group_id: String, kind: String, id: String) -> void:
	var key := object_key(kind, id)
	# One organizational parent at a time keeps the outliner deterministic.
	for index in groups.size():
		var group := groups[index]
		var items: Array = group.get("items", []) as Array
		items.erase(key)
		group["items"] = items
		groups[index] = group
	for index in groups.size():
		if String(groups[index].get("id", "")) != group_id:
			continue
		var group := groups[index]
		var items: Array = group.get("items", []) as Array
		if not items.has(key):
			items.append(key)
		group["items"] = items
		groups[index] = group
		break
	_save()


func group_for(kind: String, id: String) -> String:
	var key := object_key(kind, id)
	for group in groups:
		var items: Array = group.get("items", []) as Array
		if items.has(key):
			return String(group.get("id", ""))
	return ""


func add_bookmark(name: String, direction: Vector3, radius_m: float = 0.0) -> String:
	if direction.length_squared() < 0.5:
		return ""
	var id := "bookmark-%d-%d" % [Time.get_ticks_usec(), randi() & 0x7fffffff]
	var d := direction.normalized()
	bookmarks.append({
		"id": id,
		"name": _safe_name(name, "Bookmark"),
		"dir": [d.x, d.y, d.z],
		"radius_m": maxf(radius_m, 0.0),
	})
	_save()
	return id


func remove_bookmark(bookmark_id: String) -> void:
	for index in range(bookmarks.size() - 1, -1, -1):
		if String(bookmarks[index].get("id", "")) == bookmark_id:
			bookmarks.remove_at(index)
	_save()


func bookmark_direction(entry: Dictionary) -> Vector3:
	var raw: Array = entry.get("dir", []) as Array
	if raw.size() < 3:
		return Vector3.ZERO
	return Vector3(float(raw[0]), float(raw[1]), float(raw[2])).normalized()


func mark_region_stale(direction: Vector3, radius_m: float, domains: PackedStringArray,
		reason: String) -> void:
	if direction.length_squared() < 0.5:
		return
	var d := direction.normalized()
	var domain_array: Array[String] = []
	for domain in domains:
		if not domain_array.has(domain):
			domain_array.append(domain)
	# Merge nearby dirty regions rather than filling the workspace with one entry
	# per mouse stamp. The union is deliberately conservative.
	for index in stale_regions.size():
		var entry := stale_regions[index]
		var existing := bookmark_direction(entry)
		if existing.length_squared() < 0.5:
			continue
		var old_radius := float(entry.get("radius_m", 0.0))
		var planet_radius := maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1000000.0
		var separation := existing.angle_to(d) * planet_radius
		if separation > old_radius + radius_m:
			continue
		var old_domains: Array = entry.get("domains", []) as Array
		for domain in domain_array:
			if not old_domains.has(domain):
				old_domains.append(domain)
		entry["domains"] = old_domains
		entry["radius_m"] = maxf(old_radius, separation + radius_m)
		entry["reason"] = reason
		stale_regions[index] = entry
		_save()
		return
	stale_regions.append({
		"id": "stale-%d" % Time.get_ticks_usec(),
		"dir": [d.x, d.y, d.z],
		"radius_m": maxf(radius_m, 1.0),
		"domains": domain_array,
		"reason": reason,
	})
	_save()


func clear_stale_regions() -> void:
	stale_regions.clear()
	_save()


func clear_stale_region_near(direction: Vector3, radius_m: float) -> void:
	if direction.length_squared() < 0.5:
		return
	var planet_radius := maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1000000.0
	for index in range(stale_regions.size() - 1, -1, -1):
		var existing := bookmark_direction(stale_regions[index])
		if existing.length_squared() < 0.5:
			continue
		var separation := existing.angle_to(direction.normalized()) * planet_radius
		if separation <= radius_m + float(stale_regions[index].get("radius_m", 0.0)):
			stale_regions.remove_at(index)
	_save()


func save_custom_preset(name: String, feature_specs: Array[Dictionary]) -> String:
	if feature_specs.is_empty():
		return ""
	var preset_id := _slug(name)
	if preset_id.is_empty():
		preset_id = "custom-%d" % Time.get_ticks_usec()
	var entry := {
		"id": preset_id,
		"name": _safe_name(name, "Custom Terrain"),
		"specs": feature_specs.duplicate(true),
	}
	var replaced := false
	for index in custom_presets.size():
		if String(custom_presets[index].get("id", "")) == preset_id:
			custom_presets[index] = entry
			replaced = true
			break
	if not replaced:
		custom_presets.append(entry)
	_save()
	return preset_id


func remove_custom_preset(preset_id: String) -> void:
	for index in range(custom_presets.size() - 1, -1, -1):
		if String(custom_presets[index].get("id", "")) == preset_id:
			custom_presets.remove_at(index)
	_save()


func custom_preset(preset_id: String) -> Dictionary:
	for entry in custom_presets:
		if String(entry.get("id", "")) == preset_id:
			return entry.duplicate(true)
	return {}


func set_seed_lock(property_name: String, value: bool) -> void:
	if value:
		seed_locks[property_name] = true
	else:
		seed_locks.erase(property_name)
	_save()


func seed_locked(property_name: String) -> bool:
	return bool(seed_locks.get(property_name, false))


func set_timing(label: String, value_ms: float) -> void:
	timings_ms[label] = maxf(value_ms, 0.0)
	_save()


func set_ui_value(key: String, value: Variant) -> void:
	ui_state[key] = value
	_save()


func ui_value(key: String, fallback: Variant = null) -> Variant:
	return ui_state.get(key, fallback)


func _load() -> void:
	groups = []
	hidden = {}
	locked = {}
	bookmarks = []
	stale_regions = []
	custom_presets = []
	seed_locks = {}
	timings_ms = {}
	ui_state = {}
	var path := _path()
	if not FileAccess.file_exists(path):
		return
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	if not (parsed is Dictionary):
		return
	var data := parsed as Dictionary
	groups = _dict_array(data.get("groups", []))
	hidden = (data.get("hidden", {}) as Dictionary).duplicate(true)
	locked = (data.get("locked", {}) as Dictionary).duplicate(true)
	bookmarks = _dict_array(data.get("bookmarks", []))
	stale_regions = _dict_array(data.get("stale_regions", []))
	custom_presets = _dict_array(data.get("custom_presets", []))
	seed_locks = (data.get("seed_locks", {}) as Dictionary).duplicate(true)
	timings_ms = (data.get("timings_ms", {}) as Dictionary).duplicate(true)
	ui_state = (data.get("ui_state", {}) as Dictionary).duplicate(true)


func _save() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(ROOT_DIR))
	var file := FileAccess.open(_path(), FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify({
		"version": VERSION,
		"body_id": body_id,
		"groups": groups,
		"hidden": hidden,
		"locked": locked,
		"bookmarks": bookmarks,
		"stale_regions": stale_regions,
		"custom_presets": custom_presets,
		"seed_locks": seed_locks,
		"timings_ms": timings_ms,
		"ui_state": ui_state,
	}, "\t"))


func _path() -> String:
	return "%s/%s.json" % [ROOT_DIR, _slug(body_id)]


func _safe_name(value: String, fallback: String) -> String:
	var clean := value.strip_edges()
	return fallback if clean.is_empty() else clean


func _slug(value: String) -> String:
	var out := value.strip_edges().to_lower()
	for token in [" ", "/", "\\", ":", ".", ",", "?", "*", "|", "\"", "'"]:
		out = out.replace(token, "-")
	while out.contains("--"):
		out = out.replace("--", "-")
	return out.trim_prefix("-").trim_suffix("-")


func _dict_array(value: Variant) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if value is Array:
		for item in value:
			if item is Dictionary:
				out.append((item as Dictionary).duplicate(true))
	return out
