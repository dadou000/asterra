class_name SaveGame
extends RefCounted
## Persistence and save-state layer.
##
## The save holds only what cannot be regenerated: the seed(s), the player, the
## sparse terrain deltas and the loose material that exists in the world. The
## planets themselves are never serialised -- they come back from their seeds,
## byte for byte.
##
## v3 (seamless multi-planet): also stores the system seed, which body the player
## was standing on, and every visited body's own terrain-edit blob. A v2 save
## (single planet) still loads -- `active_body_id` defaults to "asterra" and
## `body_deltas` to just that one body.

const VERSION := 3
const LEGACY_VERSIONS := [2]
const DIR := "user://asterra/saves"
const DEFAULT_BODY_ID := "asterra"

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

## `system_seed` 0 => single-planet save. `active_body_id` "" => DEFAULT_BODY_ID.
## `body_deltas` maps body_id -> a Deltas.serialize() dict; the active body's live
## deltas are also written to the top-level "deltas" for v2 back-compat.
static func save(name: String, cfg: GenConfig, player_state: Dictionary,
		editor: TerrainEditor, elapsed: float,
		system_seed: int = 0, active_body_id: String = "",
		body_deltas: Dictionary = {}) -> Error:
	DirAccess.make_dir_recursive_absolute(DIR)
	var active_id: String = active_body_id if not active_body_id.is_empty() else DEFAULT_BODY_ID
	var active_blob: Variant = body_deltas.get(active_id, Deltas.serialize())
	var data := {
		"version": VERSION,
		"saved_at": Time.get_datetime_string_from_system(true),
		"elapsed": elapsed,
		"system_seed": system_seed,
		"active_body_id": active_id,
		"world": {
			"seed": cfg.world_seed,
			"radius": cfg.planet_radius,
			"face_res": cfg.face_res,
			"cache_key": cfg.cache_key(),
		},
		"player": player_state,
		"deltas": active_blob,
		"body_deltas": body_deltas if not body_deltas.is_empty() else {active_id: active_blob},
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

## Returns the loaded payload, having already restored the ACTIVE body's deltas
## into the `Deltas` autoload and the loose piles into `editor`. Callers that
## support multiple bodies then hand `data["body_deltas"]` to
## `restore_multibody()`. Accepts v2 and v3 saves.
static func load_into(name: String, cfg: GenConfig, editor: TerrainEditor) -> Dictionary:
	var data := peek(name)
	var version := int(data.get("version", 0))
	if data.is_empty() or (version != VERSION and version not in LEGACY_VERSIONS):
		return {}
	var w: Dictionary = data["world"]
	if int(w["seed"]) != cfg.world_seed or String(w["cache_key"]) != cfg.cache_key():
		push_warning("Save '%s' was made with different world parameters." % name)
	Deltas.deserialize(data["deltas"])
	if editor != null:
		editor.restore_piles(data.get("piles", []))
	# Normalise so a v2 payload looks like a v3 one to callers.
	if not data.has("active_body_id"):
		data["active_body_id"] = DEFAULT_BODY_ID
	if not data.has("body_deltas"):
		data["body_deltas"] = {String(data["active_body_id"]): data["deltas"]}
	if not data.has("system_seed"):
		data["system_seed"] = 0
	return data

## Replay every visited body's saved edit blob onto its BodyRuntime, and make the
## saved active body active. `bodies` is the `Bodies` autoload; `frames` is
## `Frames`. Non-active bodies get their blob stashed on `_delta_blob` (replayed
## when that body next activates); the active body's blob is deserialised live.
static func restore_multibody(data: Dictionary, bodies: Node, frames: Node) -> void:
	if data.is_empty() or bodies == null:
		return
	var body_deltas: Dictionary = data.get("body_deltas", {})
	var active_id: String = String(data.get("active_body_id", DEFAULT_BODY_ID))

	for id_key: Variant in body_deltas:
		var rt: Object = bodies.call(&"slot", StringName(id_key))
		if rt == null:
			continue
		if String(id_key) == active_id:
			continue
		rt.set(&"_delta_blob", (body_deltas[id_key] as Dictionary).duplicate(true))

	var current: Object = bodies.get(&"active")
	if current != null and String(current.id) == active_id:
		# Already on the right body -- load_into() already deserialised its deltas.
		return
	var target: Object = bodies.call(&"slot", StringName(active_id))
	if target == null:
		return
	# Stash the active body's blob so activate_singleton() replays it, then make it
	# active with the player at its saved body-local position (no re-expression).
	target.set(&"_delta_blob", (body_deltas.get(active_id, {}) as Dictionary).duplicate(true))
	bodies.call(&"load_active", StringName(active_id), _player_world_from(data))


static func _player_world_from(data: Dictionary) -> Vec3D:
	var p: Dictionary = data.get("player", {})
	return Vec3D.new(float(p.get("x", 0.0)), float(p.get("y", 0.0)), float(p.get("z", 0.0)))
