class_name WorldAuthoringSession
extends RefCounted
## Transactional staging model for Planet Studio.
## Phase 0 intentionally does not mutate the live terrain runtime on Apply.

const SYSTEM_SCRIPT := preload("res://scripts/world_authoring/model/celestial_system_definition.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const PLANET_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/planet_authoring_profile.gd")
const TERRAIN_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const GENERATION_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const WATER_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/water_authoring_profile.gd")
const ATMOSPHERE_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/atmosphere_profile.gd")

signal changed(dirty: bool, apply_scope: int)
signal applied(system: Resource)
signal preset_saved(path: String)
signal preset_loaded(path: String)
signal error_reported(message: String)

enum ApplyScope {
	NONE,
	HOT,
	GRAPH,
	TILES,
	CLIPMAP,
	FULL_REBUILD,
}

const RECOVERY_PATH := "user://world_authoring/recovery.tres"
const DEFAULT_PRESET_PATH := "user://world_authoring/presets/last_preset.tres"

var applied_system: Resource
var staged_system: Resource
var dirty: bool = false
var apply_scope: int = ApplyScope.NONE

var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []

func bootstrap_from_current_world() -> void:
	var generation: Resource = GENERATION_PROFILE_SCRIPT.new()
	if ResourceLoader.exists("res://world.tres"):
		var loaded: Resource = ResourceLoader.load("res://world.tres")
		if loaded != null:
			generation.call("import_from_resource", loaded)
	bootstrap_from_generation_profile(generation)

func bootstrap_from_generation_profile(generation: Resource) -> void:
	if generation == null:
		generation = GENERATION_PROFILE_SCRIPT.new()
	var terrain: Resource = TERRAIN_PROFILE_SCRIPT.new()
	terrain.set(&"generation_profile", generation.duplicate(true))
	var water: Resource = WATER_PROFILE_SCRIPT.new()
	water.set(&"sea_level_m", 0.0)
	var atmosphere: Resource = ATMOSPHERE_PROFILE_SCRIPT.new()
	atmosphere.set(&"atmosphere_height_m", float(generation.get(&"atmosphere_height")))
	var profile: Resource = PLANET_PROFILE_SCRIPT.new()
	profile.set(&"terrain", terrain)
	profile.set(&"water", water)
	profile.set(&"atmosphere", atmosphere)
	profile.set(&"reference_sea_level_m", 0.0)

	var body: Resource = BODY_SCRIPT.new()
	body.set(&"body_id", "asterra")
	body.set(&"display_name", "Asterra")
	body.set(&"body_type", BODY_SCRIPT.BodyType.PLANET)
	body.set(&"radius_m", float(generation.get(&"planet_radius")))
	body.set(&"sidereal_rotation_period_s", 24.0 * 3600.0)
	body.set(&"axial_tilt_deg", float(generation.get(&"axial_tilt_deg")))
	body.set(&"planet_profile", profile)
	body.call("ensure_children")

	var system: Resource = SYSTEM_SCRIPT.new()
	system.set(&"system_id", "asterra-system")
	system.set(&"display_name", "Asterra System")
	var body_array: Array[Resource] = []
	body_array.append(body)
	system.set(&"bodies", body_array)
	system.set(&"active_body_id", String(body.get(&"body_id")))
	system.call("ensure_valid")

	applied_system = system
	staged_system = system.duplicate(true)
	dirty = false
	apply_scope = ApplyScope.NONE
	_undo_stack.clear()
	_redo_stack.clear()
	_autosave_recovery()
	changed.emit(dirty, apply_scope)

func active_body() -> Resource:
	if staged_system == null:
		return null
	return staged_system.call("active_body") as Resource

func can_undo() -> bool:
	return not _undo_stack.is_empty()

func can_redo() -> bool:
	return not _redo_stack.is_empty()

func stage_set(target: Object, property_name: StringName, value: Variant, scope: int, _action_name: String = "Edit") -> void:
	if target == null or staged_system == null:
		return
	_push_undo_state()
	_redo_stack.clear()
	target.set(property_name, value)
	_mark_dirty(scope)

func stage_action(_action_name: String, action: Callable, scope: int) -> void:
	if staged_system == null or not action.is_valid():
		return
	_push_undo_state()
	_redo_stack.clear()
	action.call()
	staged_system.call("ensure_valid")
	_mark_dirty(scope)

func select_body(body_id: String) -> void:
	if staged_system == null or staged_system.call("find_body", body_id) == null:
		return
	if String(staged_system.get(&"active_body_id")) == body_id:
		return
	stage_set(staged_system, &"active_body_id", body_id, ApplyScope.CLIPMAP, "Select body")

func create_body(display_name: String, body_type: int, parent_body_id: String = "") -> Resource:
	if staged_system == null:
		return null
	var body := _make_default_body(display_name, body_type, parent_body_id)
	stage_action("Create body", func() -> void:
		staged_system.call("add_body", body)
		staged_system.set(&"active_body_id", String(body.get(&"body_id")))
	, ApplyScope.FULL_REBUILD)
	return body

func duplicate_active_body() -> Resource:
	var source: Resource = active_body()
	if source == null:
		return null
	var copy: Resource = source.duplicate(true)
	copy.set(&"body_id", BODY_SCRIPT.make_body_id(String(source.get(&"display_name"))))
	copy.set(&"display_name", "%s Copy" % String(source.get(&"display_name")))
	stage_action("Duplicate body", func() -> void:
		staged_system.call("add_body", copy)
		staged_system.set(&"active_body_id", String(copy.get(&"body_id")))
	, ApplyScope.FULL_REBUILD)
	return copy

func delete_active_body() -> bool:
	if staged_system == null:
		return false
	var bodies: Array = staged_system.get(&"bodies")
	if bodies.size() <= 1:
		return false
	var body: Resource = active_body()
	if body == null:
		return false
	var body_id := String(body.get(&"body_id"))
	stage_action("Delete body", func() -> void:
		staged_system.call("remove_body", body_id)
	, ApplyScope.FULL_REBUILD)
	return true

func undo() -> void:
	if _undo_stack.is_empty() or staged_system == null:
		return
	_redo_stack.append(_capture_state())
	_restore_state(_undo_stack.pop_back())
	_autosave_recovery()
	changed.emit(dirty, apply_scope)

func redo() -> void:
	if _redo_stack.is_empty() or staged_system == null:
		return
	_undo_stack.append(_capture_state())
	_restore_state(_redo_stack.pop_back())
	_autosave_recovery()
	changed.emit(dirty, apply_scope)

func apply() -> void:
	if staged_system == null:
		return
	staged_system.call("ensure_valid")
	applied_system = staged_system.duplicate(true)
	dirty = false
	apply_scope = ApplyScope.NONE
	_undo_stack.clear()
	_redo_stack.clear()
	_autosave_recovery()
	changed.emit(dirty, apply_scope)
	applied.emit(applied_system)

func revert() -> void:
	if applied_system == null:
		return
	staged_system = applied_system.duplicate(true)
	dirty = false
	apply_scope = ApplyScope.NONE
	_undo_stack.clear()
	_redo_stack.clear()
	_autosave_recovery()
	changed.emit(dirty, apply_scope)

func save_preset(path: String = DEFAULT_PRESET_PATH) -> Error:
	if staged_system == null:
		return ERR_UNCONFIGURED
	_ensure_parent_directory(path)
	var err := ResourceSaver.save(staged_system, path)
	if err != OK:
		error_reported.emit("Could not save Planet Studio preset: %s" % error_string(err))
		return err
	preset_saved.emit(path)
	return OK

func load_preset(path: String = DEFAULT_PRESET_PATH) -> Error:
	if not ResourceLoader.exists(path):
		error_reported.emit("Planet Studio preset does not exist: %s" % path)
		return ERR_FILE_NOT_FOUND
	var loaded: Resource = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	if loaded == null or loaded.get_script() != SYSTEM_SCRIPT:
		error_reported.emit("Preset is not a Planet Studio celestial system: %s" % path)
		return ERR_INVALID_DATA
	_push_undo_state()
	_redo_stack.clear()
	staged_system = loaded.duplicate(true)
	staged_system.call("ensure_valid")
	dirty = true
	apply_scope = ApplyScope.FULL_REBUILD
	_autosave_recovery()
	changed.emit(dirty, apply_scope)
	preset_loaded.emit(path)
	return OK

func _make_default_body(display_name: String, body_type: int, parent_body_id: String) -> Resource:
	var body: Resource = BODY_SCRIPT.new()
	body.set(&"display_name", display_name)
	body.set(&"body_type", body_type)
	body.set(&"parent_body_id", parent_body_id)
	body.set(&"body_id", BODY_SCRIPT.make_body_id(display_name))
	body.set(&"radius_m", 600000.0 if body_type == BODY_SCRIPT.BodyType.MOON else 1000000.0)
	body.set(&"sidereal_rotation_period_s", 24.0 * 3600.0)
	body.call("ensure_children")
	return body

func _push_undo_state() -> void:
	_undo_stack.append(_capture_state())
	if _undo_stack.size() > 96:
		_undo_stack.pop_front()

func _capture_state() -> Dictionary:
	return {
		"system": staged_system.duplicate(true),
		"dirty": dirty,
		"scope": apply_scope,
	}

func _restore_state(state: Dictionary) -> void:
	var restored: Variant = state.get("system")
	if restored is Resource:
		staged_system = restored as Resource
	dirty = bool(state.get("dirty", true))
	apply_scope = int(state.get("scope", ApplyScope.FULL_REBUILD))

func _mark_dirty(scope: int) -> void:
	dirty = true
	apply_scope = maxi(apply_scope, scope)
	_autosave_recovery()
	changed.emit(dirty, apply_scope)

func _autosave_recovery() -> void:
	if staged_system == null:
		return
	_ensure_parent_directory(RECOVERY_PATH)
	var err := ResourceSaver.save(staged_system, RECOVERY_PATH)
	if err != OK:
		error_reported.emit("Planet Studio recovery autosave failed: %s" % error_string(err))

func _ensure_parent_directory(path: String) -> void:
	var absolute := ProjectSettings.globalize_path(path.get_base_dir())
	var err := DirAccess.make_dir_recursive_absolute(absolute)
	if err != OK and err != ERR_ALREADY_EXISTS:
		error_reported.emit("Could not create Planet Studio directory: %s" % error_string(err))
