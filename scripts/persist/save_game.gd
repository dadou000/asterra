class_name SaveGame
extends RefCounted
## Persistence and save-state layer.
##
## The save holds only what cannot be regenerated: the seed, the player, the
## sparse terrain deltas and the loose material that exists in the world. The
## planet itself is never serialised -- it comes back from the seed, byte for
## byte. That is the property the Phase 1 milestone actually tests.

const VERSION := 2
const DIR := "user://asterra/saves"

static func path_for(name: String) -> String:
	return "%s/%s.asv" % [DIR, name]

static func list_saves() -> PackedStringArray:
	var out := PackedStringArray()
	var d := DirAccess.open(DIR)
	if d == null:
		return out
	for f in d.get_files():
		if f.ends_with(".asv"):
			out.append(f.get_basename())
	return out

static func save(name: String, cfg: GenConfig, player_state: Dictionary,
		editor: TerrainEditor, elapsed: float) -> Error:
	DirAccess.make_dir_recursive_absolute(DIR)
	var data := {
		"version": VERSION,
		"saved_at": Time.get_datetime_string_from_system(true),
		"elapsed": elapsed,
		"world": {
			"seed": cfg.world_seed,
			"radius": cfg.planet_radius,
			"face_res": cfg.face_res,
			"cache_key": cfg.cache_key(),
		},
		"player": player_state,
		"deltas": Deltas.serialize(),
		"piles": editor.serialize_piles() if editor != null else [],
	}
	var f := FileAccess.open_compressed(path_for(name), FileAccess.WRITE, FileAccess.COMPRESSION_ZSTD)
	if f == null:
		return FileAccess.get_open_error()
	f.store_var(data, true)
	f.close()
	return OK

static func peek(name: String) -> Dictionary:
	var f := FileAccess.open_compressed(path_for(name), FileAccess.READ, FileAccess.COMPRESSION_ZSTD)
	if f == null:
		return {}
	var data = f.get_var(true)
	f.close()
	return data if data is Dictionary else {}

## Returns the loaded payload, having already restored deltas and piles.
static func load_into(name: String, cfg: GenConfig, editor: TerrainEditor) -> Dictionary:
	var data := peek(name)
	if data.is_empty() or int(data.get("version", 0)) != VERSION:
		return {}
	var w: Dictionary = data["world"]
	if int(w["seed"]) != cfg.world_seed or String(w["cache_key"]) != cfg.cache_key():
		push_warning("Save '%s' was made with different world parameters." % name)
	Deltas.deserialize(data["deltas"])
	if editor != null:
		editor.restore_piles(data.get("piles", []))
	return data
