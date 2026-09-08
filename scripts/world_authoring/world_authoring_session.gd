class_name WorldAuthoringSession
extends RefCounted
## Transactional staging model for Planet Studio. Editing resources happens only
## in the staged copy; Apply promotes a coherent system snapshot.

const SYSTEM_SCRIPT := preload("res://scripts/world_authoring/model/celestial_system_definition.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const PLANET_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/planet_authoring_profile.gd")
const TERRAIN_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const GENERATION_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const WATER_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/water_authoring_profile.gd")
const ATMOSPHERE_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/atmosphere_profile.gd")
const SHADER_SLOT_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const WATER_FEATURE_SCRIPT := preload("res://scripts/world_authoring/model/water_feature_definition.gd")

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

# Helion is Asterra's star and the root of the system frame (see Frames.gd --
# these mirror helion_radius_m / helion_distance_m and the Sol-like GM the climate
# model assumes). Every Planet Studio system carries it as the parentless root
# STAR, and the primary terrestrial body orbits it at ~1 AU.
const HELION_BODY_ID := "helion"
const HELION_RADIUS_M := 696_340_000.0
const HELION_DISTANCE_M := 149_597_870_700.0
const HELION_MASS_KG := 1.988_416e30
const HELION_GM_M3_S2 := 1.327_124_400_18e20
const HELION_SURFACE_GRAVITY_M_S2 := 274.0
const HELION_SIDEREAL_ROTATION_S := 25.05 * 86_400.0
const HELION_AXIAL_TILT_DEG := 7.25
const PRIMARY_ORBIT_ECCENTRICITY := 0.0167

# Planet Studio's "Biome Terrain" profiles are ordinary scoped displacement slots
# (slot_id "simple-biome-terrain-<n>"). A recovery/preset file written by an older
# build can hold retired node types or more enabled profiles than the shared
# 32-instruction displacement VM can compile, which makes the whole terrain
# program get rejected on load (frozen editor). Loaded systems are repaired here
# before they become the staged/applied state.
const BIOME_TERRAIN_SLOT_PREFIX := "simple-biome-terrain-"
const RETIRED_DISPLACEMENT_NODE_MAP: Dictionary = {
	"BILLOW_NOISE": "NOISE_LAYER",
	"VORONOI_RIDGES": "RIDGED_MOUNTAINS",
}

var applied_system: Resource
var staged_system: Resource
var dirty: bool = false
var apply_scope: int = ApplyScope.NONE

var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []

const CELESTIAL_SYSTEM_GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")

func bootstrap_from_current_world() -> void:
	var generation: Resource = GENERATION_PROFILE_SCRIPT.new()
	if ResourceLoader.exists("res://world.tres"):
		var loaded: Resource = ResourceLoader.load("res://world.tres")
		if loaded != null:
			generation.call("import_from_resource", loaded)
	var env_seed := OS.get_environment("ASTERRA_SYSTEM_SEED")
	if env_seed != "" and env_seed.is_valid_int() and env_seed.to_int() != 0:
		generation.set(&"system_seed", env_seed.to_int())

	# A seeded multi-planet system is deterministic, so regenerate it fresh each
	# open rather than restoring a stale recovery snapshot.
	if int(generation.get(&"system_seed")) != 0:
		bootstrap_from_generated_system(generation)
		return
	if _bootstrap_from_recovery():
		return
	bootstrap_from_generation_profile(generation)


## Build the full CelestialSystemGenerator archetype system (inner hot rocks, the
## terran home, an ice world, a gas giant + moons, a distant pulsar) with every
## body carrying its own archetype-tuned generation profile, so the celestial map
## shows them all and switching the authoring target picks up that body's terrain.
func bootstrap_from_generated_system(baseline: Resource) -> void:
	var minimal: bool = bool(baseline.get(&"minimal_system")) \
		or OS.get_environment("ASTERRA_MINIMAL_SYSTEM") == "1"
	var system: Resource = CELESTIAL_SYSTEM_GENERATOR.generate(
		int(baseline.get(&"system_seed")), float(baseline.get(&"axial_tilt_deg")), 1.0, minimal)
	CELESTIAL_SYSTEM_GENERATOR.apply_archetype_profiles(system, baseline)
	system.set(&"active_body_id", CELESTIAL_SYSTEM_GENERATOR.HOME_BODY_ID)
	system.call("ensure_valid")
	applied_system = system
	staged_system = system.duplicate(true)
	dirty = false
	apply_scope = ApplyScope.NONE
	_undo_stack.clear()
	_redo_stack.clear()


func _bootstrap_from_recovery() -> bool:
	var path: String = _recovery_path()
	if not ResourceLoader.exists(path):
		return false
	var loaded: Resource = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	if loaded == null or loaded.get_script() != SYSTEM_SCRIPT:
		error_reported.emit("Planet Studio recovery is invalid; starting from the current world: %s" % path)
		return false
	loaded.call("ensure_valid")
	var added_star: bool = _ensure_helion_root_star(loaded)
	var repaired: bool = _sanitize_biome_terrain_slots(loaded)
	repaired = repaired or added_star
	applied_system = loaded.duplicate(true)
	staged_system = loaded.duplicate(true)
	dirty = false
	apply_scope = ApplyScope.NONE
	_undo_stack.clear()
	_redo_stack.clear()
	# Heal the on-disk recovery so a repaired biome-terrain save cannot re-freeze
	# the editor on the next launch.
	if repaired:
		_autosave_recovery()
	changed.emit(dirty, apply_scope)
	return true

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
	profile.call("ensure_children")

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
	_ensure_helion_root_star(system)
	system.call("ensure_valid")

	applied_system = system
	staged_system = system.duplicate(true)
	dirty = false
	apply_scope = ApplyScope.NONE
	_undo_stack.clear()
	_redo_stack.clear()
	_autosave_recovery()
	changed.emit(dirty, apply_scope)


## Guarantees the system has Helion as a parentless root STAR AND that every
## parentless terrestrial body orbits it (~1 AU by default). Returns true when it
## changed the system, so callers can re-persist / migrate a legacy save.
##
## The detailed terrain/ocean/contact renderer is body-centre-aware
## (OrbitalMotionRuntime keeps Frames anchored to the selected body), so a
## terrestrial body being a child of Helion no longer disables it.
func _ensure_helion_root_star(system: Resource) -> bool:
	if system == null:
		return false
	var bodies: Array = system.get(&"bodies")
	var changed: bool = false

	var star: Resource = _find_root_star(bodies)
	if star == null:
		star = _make_helion_body()
		system.call("add_body", star)
		bodies = system.get(&"bodies")
		changed = true

	var star_id: String = String(star.get(&"body_id"))
	for body_value: Variant in bodies:
		var body: Resource = body_value as Resource
		if body == null or body == star:
			continue
		if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
			continue
		var parent_id: String = String(body.get(&"parent_body_id"))
		if parent_id.is_empty():
			body.set(&"parent_body_id", star_id)
			_seed_primary_orbit(body)
			changed = true
		elif parent_id == star_id and _seed_primary_orbit(body):
			# Already a direct child of the star (migrated by an earlier build) but
			# missing the day-0 sun-direction calibration -- apply it now.
			changed = true

	if changed:
		system.call("ensure_valid")
	return changed


func _find_root_star(bodies: Array) -> Resource:
	for body_value: Variant in bodies:
		var body: Resource = body_value as Resource
		if body != null and int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR \
				and String(body.get(&"parent_body_id")).is_empty():
			return body
	return null


## Ensure the primary body has a real ~1 AU orbit AND that system_time_s == 0
## reproduces the historical fixed sun direction, so the planet's day-0 look is
## unchanged and only scrubbing the clock moves the sun. Returns true if it
## changed anything (idempotent -- a calibrated / authored orbit is left alone).
func _seed_primary_orbit(body: Resource) -> bool:
	var orbit: Resource = body.get(&"orbit") as Resource
	if orbit == null:
		body.call("ensure_children")
		orbit = body.get(&"orbit") as Resource
	if orbit == null:
		return false
	var changed: bool = false
	if float(orbit.get(&"semi_major_axis_m")) <= 0.0:
		orbit.set(&"semi_major_axis_m", HELION_DISTANCE_M)
		orbit.set(&"eccentricity", PRIMARY_ORBIT_ECCENTRICITY)
		orbit.set(&"mean_anomaly_at_epoch_deg", 0.0)
		changed = true
	# Calibrate the day-0 sun direction only when the phase is still untouched.
	if is_equal_approx(float(orbit.get(&"argument_periapsis_deg")), 0.0) \
			and is_equal_approx(float(orbit.get(&"mean_anomaly_at_epoch_deg")), 0.0) \
			and is_equal_approx(float(body.get(&"rotation_phase_at_epoch_deg")), 0.0):
		var ORBIT_MATH := preload("res://scripts/world_authoring/model/orbit_math.gd")
		var align: Dictionary = ORBIT_MATH.epoch_alignment_for(
			ORBIT_MATH.LEGACY_SUNWARD, float(body.get(&"axial_tilt_deg")))
		orbit.set(&"argument_periapsis_deg", float(align["argp_deg"]))
		body.set(&"rotation_phase_at_epoch_deg", float(align["rotation_phase0_deg"]))
		changed = true
	return changed


func _make_helion_body() -> Resource:
	var star: Resource = BODY_SCRIPT.new()
	star.set(&"body_id", HELION_BODY_ID)
	star.set(&"display_name", "Helion")
	star.set(&"body_type", BODY_SCRIPT.BodyType.STAR)
	star.set(&"parent_body_id", "")
	star.set(&"radius_m", HELION_RADIUS_M)
	star.set(&"mass_kg", HELION_MASS_KG)
	star.set(&"gravitational_parameter_m3_s2", HELION_GM_M3_S2)
	star.set(&"surface_gravity_m_s2", HELION_SURFACE_GRAVITY_M_S2)
	star.set(&"sidereal_rotation_period_s", HELION_SIDEREAL_ROTATION_S)
	star.set(&"axial_tilt_deg", HELION_AXIAL_TILT_DEG)
	star.call("ensure_children")
	# Star profile defaults are already Sol/G-class (5772 K, 1 L_sun, 0.27 deg
	# apparent radius), which is exactly Helion -- only re-validate.
	var star_profile: Resource = star.get(&"star_profile") as Resource
	if star_profile != null and star_profile.has_method("ensure_valid"):
		star_profile.call("ensure_valid")
	return star


func active_body() -> Resource:
	if staged_system == null:
		return null
	return staged_system.call("active_body") as Resource

func active_planet_profile() -> Resource:
	var body := active_body()
	if body == null:
		return null
	return body.get(&"planet_profile") as Resource

func active_star_profile() -> Resource:
	var body := active_body()
	if body == null or int(body.get(&"body_type")) != BODY_SCRIPT.BodyType.STAR:
		return null
	return body.get(&"star_profile") as Resource

func active_terrain_profile() -> Resource:
	var profile := active_planet_profile()
	if profile == null:
		return null
	return profile.get(&"terrain") as Resource

func active_water_profile() -> Resource:
	var profile := active_planet_profile()
	if profile == null:
		return null
	return profile.get(&"water") as Resource

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
	staged_system.call("ensure_valid")
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

func set_active_body_parent(parent_body_id: String) -> bool:
	var body := active_body()
	if body == null or staged_system == null:
		return false
	var body_id := String(body.get(&"body_id"))
	if not bool(staged_system.call("can_parent_body", body_id, parent_body_id)):
		error_reported.emit("Invalid orbit parent: the celestial hierarchy cannot contain self-parenting or cycles.")
		return false
	stage_action("Change orbit parent", func() -> void:
		staged_system.call("set_parent_body", body_id, parent_body_id)
	, ApplyScope.FULL_REBUILD)
	return true

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

func create_biome_layer(display_name: String = "Biome Paint") -> Resource:
	var terrain := active_terrain_profile()
	if terrain == null:
		return null
	stage_action("Create biome paint layer", func() -> void:
		terrain.call("create_biome_layer", display_name)
	, ApplyScope.TILES)
	var layers: Array = terrain.get(&"biome_override_layers")
	return layers.back() as Resource if not layers.is_empty() else null

func remove_biome_layer(layer_id: String) -> bool:
	var terrain := active_terrain_profile()
	if terrain == null or terrain.call("find_biome_layer", layer_id) == null:
		return false
	stage_action("Delete biome paint layer", func() -> void:
		terrain.call("remove_biome_layer", layer_id)
	, ApplyScope.TILES)
	return true

func add_biome_stroke(layer_id: String, center_direction: Vector3, biome_id: int, radius_m: float, hardness: float, opacity: float) -> bool:
	var terrain := active_terrain_profile()
	if terrain == null:
		return false
	var layer: Resource = terrain.call("find_biome_layer", layer_id) as Resource
	if layer == null:
		return false
	if center_direction.length_squared() < 1e-9:
		return false
	stage_action("Paint biome", func() -> void:
		layer.call("add_stroke", center_direction, biome_id, radius_m, hardness, opacity)
	, ApplyScope.TILES)
	return true

func create_terrain_shader_slot(domain: int, display_name: String = "Terrain Slot") -> Resource:
	var terrain := active_terrain_profile()
	if terrain == null:
		return null
	var resolved_domain := SHADER_SLOT_SCRIPT.Domain.MATERIAL if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL else SHADER_SLOT_SCRIPT.Domain.DISPLACEMENT
	stage_action("Create terrain shader slot", func() -> void:
		terrain.call("create_shader_slot", resolved_domain, display_name)
	, ApplyScope.GRAPH)
	var collection: Array = terrain.get(&"material_slots") if resolved_domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL else terrain.get(&"displacement_slots")
	return collection.back() as Resource if not collection.is_empty() else null

func remove_terrain_shader_slot(slot_id: String) -> bool:
	var terrain := active_terrain_profile()
	if terrain == null or terrain.call("find_shader_slot", slot_id) == null:
		return false
	stage_action("Delete terrain shader slot", func() -> void:
		terrain.call("remove_shader_slot", slot_id)
	, ApplyScope.GRAPH)
	return true

func create_water_feature(feature_type: int, display_name: String = "Water Feature") -> Resource:
	var water := active_water_profile()
	if water == null:
		return null
	var resolved_type := WATER_FEATURE_SCRIPT.FeatureType.RIVER if feature_type == WATER_FEATURE_SCRIPT.FeatureType.RIVER else WATER_FEATURE_SCRIPT.FeatureType.LAKE
	stage_action("Create water feature", func() -> void:
		water.call("create_feature", resolved_type, display_name)
	, ApplyScope.TILES)
	var features: Array = water.get(&"authored_features")
	return features.back() as Resource if not features.is_empty() else null

func remove_water_feature(feature_id: String) -> bool:
	var water := active_water_profile()
	if water == null or water.call("find_feature", feature_id) == null:
		return false
	stage_action("Delete water feature", func() -> void:
		water.call("remove_feature", feature_id)
	, ApplyScope.TILES)
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
	_ensure_helion_root_star(staged_system)
	_sanitize_biome_terrain_slots(staged_system)
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
	if body_type == BODY_SCRIPT.BodyType.STAR:
		# Solar-like defaults are useful without constraining the authoring range.
		body.set(&"radius_m", 696340000.0)
		body.set(&"mass_kg", 1.98847e30)
		body.set(&"gravitational_parameter_m3_s2", 1.32712440018e20)
		body.set(&"surface_gravity_m_s2", 274.0)
		body.set(&"sidereal_rotation_period_s", 25.05 * 86400.0)
	elif body_type == BODY_SCRIPT.BodyType.MOON:
		body.set(&"radius_m", 600000.0)
		body.set(&"sidereal_rotation_period_s", 24.0 * 3600.0)
	else:
		body.set(&"radius_m", 1000000.0)
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
	var path: String = _recovery_path()
	_ensure_parent_directory(path)
	var err := ResourceSaver.save(staged_system, path)
	if err != OK:
		error_reported.emit("Planet Studio recovery autosave failed: %s" % error_string(err))


func _recovery_path() -> String:
	var override: String = OS.get_environment("ASTERRA_AUTHORING_RECOVERY_PATH").strip_edges()
	return override if override.begins_with("user://") else RECOVERY_PATH


## Repair biome-terrain profiles in a just-loaded system so the shared displacement
## VM always has a compilable program:
##  - rewrite retired displacement node types to their supported equivalents;
##  - disable every biome-terrain profile. Planet Studio's Biome Terrain tab
##    re-enables the profile for whichever biome is being viewed, so no authored
##    terrain shader is compiled merely by loading a stale multi-biome save.
func _sanitize_biome_terrain_slots(system: Resource) -> bool:
	if system == null:
		return false
	var changed: bool = false
	var bodies_value: Variant = system.get(&"bodies")
	if not (bodies_value is Array):
		return false
	for body_value: Variant in bodies_value as Array:
		var body: Resource = body_value as Resource
		if body == null:
			continue
		var profile: Resource = body.get(&"planet_profile") as Resource
		var terrain: Resource = profile.get(&"terrain") as Resource if profile != null else null
		if terrain == null:
			continue
		var slots_value: Variant = terrain.get(&"displacement_slots")
		if not (slots_value is Array):
			continue
		for slot_value: Variant in slots_value as Array:
			var slot: Resource = slot_value as Resource
			if slot == null:
				continue
			if not String(slot.get(&"slot_id")).begins_with(BIOME_TERRAIN_SLOT_PREFIX):
				continue
			var graph: Resource = slot.get(&"graph") as Resource
			if graph != null:
				var nodes_value: Variant = graph.get(&"nodes")
				if nodes_value is Array:
					var mutated: bool = false
					for node_value: Variant in nodes_value as Array:
						if not (node_value is Dictionary):
							continue
						var node: Dictionary = node_value as Dictionary
						var node_type: String = String(node.get("type", ""))
						if RETIRED_DISPLACEMENT_NODE_MAP.has(node_type):
							node["type"] = RETIRED_DISPLACEMENT_NODE_MAP[node_type]
							mutated = true
					if mutated:
						graph.set(&"nodes", nodes_value)
						graph.set(&"revision", int(graph.get(&"revision")) + 1)
						changed = true
			if bool(slot.get(&"enabled")):
				slot.set(&"enabled", false)
				changed = true
	return changed

func _ensure_parent_directory(path: String) -> void:
	var absolute := ProjectSettings.globalize_path(path.get_base_dir())
	var err := DirAccess.make_dir_recursive_absolute(absolute)
	if err != OK and err != ERR_ALREADY_EXISTS:
		error_reported.emit("Could not create Planet Studio directory: %s" % error_string(err))
