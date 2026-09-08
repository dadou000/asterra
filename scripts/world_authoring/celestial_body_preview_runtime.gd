extends Node3D
## Multi-body live preview for Planet Studio.
##
## All bodies stay in one stable system/world coordinate frame. Selecting a body
## changes only the editor camera interest point; it never redefines Frames or moves
## the rest of the system underneath renderers that assume a fixed planet centre.
## The one detailed terrestrial runtime can replace its lightweight preview sphere,
## while parents, moons and siblings remain at their staged orbital positions.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const FAR_BODY_SHADER := preload("res://shaders/far_body_surface.gdshader")

const PREVIEW_RADIAL_SEGMENTS: int = 72
const PREVIEW_RINGS: int = 48
const MIN_RADIUS_M: float = 1.0
const FAMILY_FRAME_DISTANCE_RATIO: float = 40.0
## Near-camera clip planes to restore when the player is on/near a surface (M7).
## Mirrors the standalone game's player camera defaults.
const SURFACE_CAMERA_NEAR_M: float = 0.25
const SURFACE_CAMERA_FAR_M: float = 400_000.0
# Fallback-orbit shaping constants moved to OrbitMath (single source of truth).

## Per-body relief elevation feed (see M2 of the seamless-multi-planet design).
## body_id -> { "tex": Texture2DArray, "face_res": float, "amplitude": float }.
## The detailed body is auto-fed from Planet.orbit_elevation_texture; other
## bodies stay smooth lit spheres until a caller (M4/M5) pushes a per-body cache.
var _relief_by_body: Dictionary = {}
## LRU order of `_relief_by_body` keys (most-recent last). Bounded so a dozens-of-
## bodies system never keeps more than MAX_RELIEF_FEEDS relief texture arrays
## resident (M9); the detailed body is exempt.
var _relief_lru: Array[String] = []
const MAX_RELIEF_FEEDS := 3
## body_id -> true for bodies a caller (M5b: those with a live concurrent clipmap)
## wants hidden here so they are not drawn twice.
var _caller_hidden: Dictionary = {}
## Absolute system position of the root star (root body at the origin). Used to
## light every far body's own terminator.
var _star_world: Vec3D = Vec3D.new()

var _system: Resource
var _selected_body_id: String = ""
var _detailed_body_id: String = ""
var _records: Dictionary = {}
## Absolute system positions, root body at the origin. Kept absolute so
## body_system_position() can report them; everything the renderer / camera sees
## is these minus `_anchor_world` (see _anchor_id / _rebuild_system_preview).
var _world_positions: Dictionary = {}
## Absolute position of the anchor body -- the detailed-runtime body when there is
## one, else the selected body. All rendered / reported positions are relative to
## this, so the anchor sits at the Frames origin and its orbital position never
## reaches the anchor-centred terrain/ocean stack.
var _anchor_world: Vec3D = Vec3D.new()
var _selected_visual_radius_m: float = 1.0
var _family_frame_radius_m: float = 1.0
var _system_extent_m: float = 1.0
var _sphere_mesh: SphereMesh
var _corona_mesh: SphereMesh


func _ready() -> void:
	_build_shared_meshes()
	visible = false
	set_process(true)


func _process(_delta: float) -> void:
	if visible:
		_sync_floating_origin()
		_sync_camera_clip()


func show_system(system: Resource, selected_body_id: String,
		detailed_body_id: String = "") -> void:
	_system = system
	_selected_body_id = selected_body_id
	_detailed_body_id = detailed_body_id
	if _system == null:
		hide_preview()
		return
	_system.call("ensure_valid")
	if _sphere_mesh == null:
		_build_shared_meshes()
	_rebuild_system_preview()
	visible = not _records.is_empty()
	_sync_floating_origin()
	_sync_camera_clip()


## Compatibility entry point for older callers. The body is placed at the system
## origin because no hierarchy/orbit information is available through this API.
func show_body(body: Resource) -> void:
	_clear_records()
	_system = null
	_selected_body_id = String(body.get(&"body_id")) if body != null else ""
	_detailed_body_id = ""
	_world_positions.clear()
	_anchor_world = Vec3D.new()
	if body == null:
		hide_preview()
		return
	var world := Vec3D.new()
	_world_positions[_selected_body_id] = world
	_star_world = Vec3D.new()
	var record: Dictionary = _create_body_record(body, world, world, true,
		Frames.helion_dir)
	_records[_selected_body_id] = record
	_selected_visual_radius_m = float(record.get("visual_radius_m", 1.0))
	_family_frame_radius_m = _selected_visual_radius_m
	_system_extent_m = _selected_visual_radius_m
	visible = true
	_sync_floating_origin()
	_sync_camera_clip()


func hide_preview() -> void:
	visible = false


## Hide/show a body's far-LOD sphere on the caller's behalf (M5b: the body has a
## live concurrent clipmap rendering its real terrain -- don't draw it twice).
func set_body_render_hidden(body_id: String, hidden: bool) -> void:
	if hidden:
		_caller_hidden[body_id] = true
	else:
		_caller_hidden.erase(body_id)
	var record: Dictionary = _records.get(body_id, {}) as Dictionary
	var root: Node3D = record.get("root") as Node3D
	if root != null and is_instance_valid(root):
		root.visible = (body_id != _detailed_body_id) and not hidden


func body_id() -> String:
	return _selected_body_id


func detailed_body_id() -> String:
	return _detailed_body_id


func visual_radius_m() -> float:
	return _selected_visual_radius_m


func family_frame_radius_m() -> float:
	return _family_frame_radius_m


func system_extent_m() -> float:
	return _system_extent_m


func preview_body_count() -> int:
	return _records.size()


## Anchor-relative centre of the selected body (0 when the selected body IS the
## anchor). This is what the runtime host feeds Frames.rebase / camera framing.
func selected_center_world() -> Vec3D:
	return _anchor_relative(_selected_body_id)


## Anchor-relative position of any body.
func body_world_position(body_id: String) -> Vec3D:
	return _anchor_relative(body_id)


## Absolute system position (root body at the origin), for callers that need the
## real ephemeris rather than the anchor-relative view.
func body_system_position(body_id: String) -> Vec3D:
	var center: Vec3D = _world_positions.get(body_id) as Vec3D
	return center.dup() if center != null else Vec3D.new()


func _anchor_id() -> String:
	return _detailed_body_id if not _detailed_body_id.is_empty() else _selected_body_id


func _anchor_relative(body_id: String) -> Vec3D:
	var center: Vec3D = _world_positions.get(body_id) as Vec3D
	if center == null:
		return Vec3D.new()
	return center.sub(_anchor_world)


func _build_shared_meshes() -> void:
	_sphere_mesh = SphereMesh.new()
	_sphere_mesh.radius = 1.0
	_sphere_mesh.height = 2.0
	_sphere_mesh.radial_segments = PREVIEW_RADIAL_SEGMENTS
	_sphere_mesh.rings = PREVIEW_RINGS

	_corona_mesh = SphereMesh.new()
	_corona_mesh.radius = 1.0
	_corona_mesh.height = 2.0
	_corona_mesh.radial_segments = PREVIEW_RADIAL_SEGMENTS
	_corona_mesh.rings = PREVIEW_RINGS


func _rebuild_system_preview() -> void:
	_clear_records()
	_world_positions = _compute_world_positions(_system)
	var selected_world: Vec3D = _world_positions.get(_selected_body_id) as Vec3D
	if selected_world == null:
		selected_world = Vec3D.new()
	# Everything the preview renders / reports is relative to the anchor body, so
	# an orbiting anchor still sits at the Frames origin next to its terrain.
	_anchor_world = _world_positions.get(_anchor_id()) as Vec3D
	if _anchor_world == null:
		_anchor_world = Vec3D.new()

	_star_world = _resolve_star_world()

	_selected_visual_radius_m = 1.0
	_family_frame_radius_m = 1.0
	_system_extent_m = 1.0
	var bodies: Array = _system.get(&"bodies")
	for body_value: Variant in bodies:
		var body: Resource = body_value as Resource
		if body == null:
			continue
		var body_id: String = String(body.get(&"body_id"))
		var absolute_world: Vec3D = _world_positions.get(body_id) as Vec3D
		if absolute_world == null:
			absolute_world = Vec3D.new()
		var anchor_relative_world: Vec3D = absolute_world.sub(_anchor_world)
		var offset_from_selected: Vec3D = absolute_world.sub(selected_world)
		# The detailed terrain/ocean renderer already represents this body. Hide only
		# its lightweight duplicate; selecting another body must not hide the detailed
		# body itself or move its centre.
		var body_visible: bool = body_id != _detailed_body_id and not _caller_hidden.has(body_id)
		var record: Dictionary = _create_body_record(
			body, anchor_relative_world, offset_from_selected, body_visible,
			_sun_dir_for(absolute_world))
		_records[body_id] = record
		var visual_radius: float = float(record.get("visual_radius_m", 1.0))
		_system_extent_m = maxf(_system_extent_m, anchor_relative_world.length() + visual_radius)
		if body_id == _selected_body_id:
			_selected_visual_radius_m = visual_radius

	_family_frame_radius_m = _compute_family_frame_radius()


func _create_body_record(body: Resource, world: Vec3D,
		offset_from_selected: Vec3D, body_visible: bool,
		sun_dir: Vector3) -> Dictionary:
	body.call("ensure_children")
	var body_id: String = String(body.get(&"body_id"))
	var body_type: int = int(body.get(&"body_type"))
	var radius_m: float = maxf(float(body.get(&"radius_m")), MIN_RADIUS_M)
	var root := Node3D.new()
	root.name = "CelestialPreview_%s" % body_id
	add_child(root)

	var surface := MeshInstance3D.new()
	surface.name = "Surface"
	surface.mesh = _sphere_mesh
	surface.scale = Vector3.ONE * radius_m
	surface.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	root.add_child(surface)

	var corona: MeshInstance3D = null
	var visual_radius: float = radius_m
	if body_type == BODY_SCRIPT.BodyType.STAR:
		var star_material := StandardMaterial3D.new()
		star_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		star_material.cull_mode = BaseMaterial3D.CULL_BACK
		surface.material_override = star_material
		visual_radius = _configure_star(body, radius_m, root, star_material)
	else:
		var solid_material := ShaderMaterial.new()
		solid_material.shader = FAR_BODY_SHADER
		surface.material_override = solid_material
		_configure_solid_body(body, body_id, body_type, radius_m, solid_material, sun_dir)

	root.visible = body_visible
	return {
		"body": body,
		"root": root,
		"world": world,
		"offset_from_selected": offset_from_selected,
		"surface": surface,
		"corona": corona,
		"visual_radius_m": visual_radius,
		"sun_dir": sun_dir,
	}


## Lit far-LOD sphere with its own terminator, per-body tint, optional atmosphere
## rim and -- when a relief elevation texture is fed -- real relief shading.
func _configure_solid_body(body: Resource, body_id: String, body_type: int,
		radius_m: float, material: ShaderMaterial, sun_dir: Vector3) -> void:
	var color := Color(0.30, 0.56, 0.78)
	match body_type:
		BODY_SCRIPT.BodyType.MOON:
			color = Color(0.56, 0.58, 0.62)
		BODY_SCRIPT.BodyType.DWARF:
			color = Color(0.56, 0.43, 0.32)
		BODY_SCRIPT.BodyType.OTHER:
			color = Color(0.48, 0.42, 0.58)
		_:
			pass

	var atmo_strength: float = 0.0
	var atmo_color := Color(0.35, 0.52, 0.92)
	var ambient_floor := 0.05
	# Archetype (CelestialSystemGenerator) overrides the tint so a scorched rock,
	# a frozen world and a gas giant read differently from far away.
	match StringName(body.get(&"archetype")):
		&"hot_rock", &"moon_rock":
			color = Color(0.40, 0.19, 0.15)
			atmo_strength = 0.12
			atmo_color = Color(0.55, 0.28, 0.18)
		&"ice", &"moon_ice":
			color = Color(0.80, 0.88, 0.96)
			ambient_floor = 0.14
			atmo_strength = 0.35
			atmo_color = Color(0.62, 0.78, 0.95)
		&"gas_giant":
			color = Color(0.78, 0.62, 0.42)
			atmo_strength = 1.4
			atmo_color = Color(0.86, 0.70, 0.48)
		&"terran":
			color = Color(0.26, 0.52, 0.72)

	var profile: Resource = body.get(&"planet_profile") as Resource
	if profile != null:
		var atmosphere: Resource = profile.get(&"atmosphere") as Resource
		if atmosphere != null and bool(atmosphere.get(&"enabled")):
			atmo_strength = maxf(atmo_strength, 0.6)
			var tint: Variant = atmosphere.get(&"sky_tint")
			if tint is Color:
				atmo_color = tint

	material.set_shader_parameter(&"u_albedo", color)
	material.set_shader_parameter(&"u_body_radius_m", maxf(radius_m, MIN_RADIUS_M))
	material.set_shader_parameter(&"u_sun_dir", sun_dir)
	material.set_shader_parameter(&"u_atmo_strength", atmo_strength)
	material.set_shader_parameter(&"u_atmo_color", atmo_color)
	material.set_shader_parameter(&"u_ambient_floor", ambient_floor)
	material.set_shader_parameter(&"u_relief_ready", 0.0)

	if body_id == _detailed_body_id and _relief_by_body.get(body_id) == null:
		_auto_feed_detailed_relief(body_id)
	_apply_body_relief(body_id, material)


func _configure_star(body: Resource, radius_m: float, root: Node3D,
		material: StandardMaterial3D) -> float:
	var star: Resource = body.get(&"star_profile") as Resource
	var surface_color := Color(1.0, 0.93, 0.82, 1.0)
	var surface_intensity: float = 1.0
	var corona_color := Color(0.72, 0.84, 1.0, 1.0)
	var corona_intensity: float = 1.0
	var corona_extent: float = 2.5
	if star != null:
		surface_color = star.get(&"photosphere_color") as Color
		surface_intensity = maxf(float(star.get(&"photosphere_intensity")), 0.0)
		corona_color = star.get(&"corona_color") as Color
		corona_intensity = maxf(float(star.get(&"corona_intensity")), 0.0)
		corona_extent = clampf(float(star.get(&"corona_extent_radii")), 1.0, 12.0)

	material.albedo_color = surface_color
	material.emission_enabled = true
	material.emission = surface_color
	material.emission_energy_multiplier = maxf(surface_intensity, 0.001)

	var corona := MeshInstance3D.new()
	corona.name = "Corona"
	corona.mesh = _corona_mesh
	corona.scale = Vector3.ONE * radius_m * corona_extent
	corona.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	root.add_child(corona)
	var corona_material := StandardMaterial3D.new()
	corona_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	corona_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	corona_material.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	corona_material.cull_mode = BaseMaterial3D.CULL_FRONT
	corona_material.no_depth_test = true
	var corona_alpha: float = clampf(0.035 + corona_intensity * 0.035, 0.02, 0.30)
	corona_material.albedo_color = Color(corona_color.r, corona_color.g, corona_color.b, corona_alpha)
	corona_material.emission_enabled = true
	corona_material.emission = corona_color
	corona_material.emission_energy_multiplier = maxf(corona_intensity * 0.35, 0.01)
	corona.material_override = corona_material
	return radius_m * corona_extent


## Absolute world position of the system's root star (the parentless STAR body),
## or the origin when there is none.
func _resolve_star_world() -> Vec3D:
	if _system == null:
		return Vec3D.new()
	var bodies: Array = _system.get(&"bodies")
	for body_value: Variant in bodies:
		var body: Resource = body_value as Resource
		if body == null:
			continue
		if int(body.get(&"body_type")) != BODY_SCRIPT.BodyType.STAR:
			continue
		if not String(body.get(&"parent_body_id")).is_empty():
			continue
		var world: Vec3D = _world_positions.get(String(body.get(&"body_id"))) as Vec3D
		return world.dup() if world != null else Vec3D.new()
	return Vec3D.new()


## Unit direction from a body at `body_absolute_world` toward the root star.
## Falls back to the anchor body's shipped sun direction.
func _sun_dir_for(body_absolute_world: Vec3D) -> Vector3:
	var to_star: Vec3D = _star_world.sub(body_absolute_world)
	var length: float = to_star.length()
	if length > 1.0:
		return to_star.to_v3() / length
	return Frames.helion_dir


## Feed a body its own orbit-relief elevation texture. `tex` is a FORMAT_RF cube
## Texture2DArray (as built by OrbitSurfaceCache); `face_res` its per-face
## resolution; `amplitude` a shading exaggeration (1.0 = physical). Passing
## tex == null clears the feed. Survives preview rebuilds.
func set_body_relief(body_id: String, tex: Texture2DArray, face_res: float,
		amplitude: float = 1.0) -> void:
	if tex == null:
		_relief_by_body.erase(body_id)
		_relief_lru.erase(body_id)
	else:
		_relief_by_body[body_id] = {
			"tex": tex, "face_res": face_res, "amplitude": maxf(amplitude, 0.01),
		}
		_touch_relief(body_id)
	var record: Dictionary = _records.get(body_id, {}) as Dictionary
	var surface: MeshInstance3D = record.get("surface") as MeshInstance3D
	if surface != null and surface.material_override is ShaderMaterial:
		_apply_body_relief(body_id, surface.material_override as ShaderMaterial)


## Move `body_id` to the front of the relief LRU and evict the oldest feed beyond
## MAX_RELIEF_FEEDS (the current detailed body is never evicted).
func _touch_relief(body_id: String) -> void:
	_relief_lru.erase(body_id)
	_relief_lru.append(body_id)
	while _relief_lru.size() > MAX_RELIEF_FEEDS:
		var victim := ""
		for candidate in _relief_lru:
			if candidate != _detailed_body_id:
				victim = candidate
				break
		if victim.is_empty():
			break
		_relief_lru.erase(victim)
		_relief_by_body.erase(victim)
		var rec: Dictionary = _records.get(victim, {}) as Dictionary
		var surf: MeshInstance3D = rec.get("surface") as MeshInstance3D
		if surf != null and surf.material_override is ShaderMaterial:
			(surf.material_override as ShaderMaterial).set_shader_parameter(&"u_relief_ready", 0.0)


## The resident detailed body already has an orbit-relief texture on the Planet
## autoload; reuse it so its far-LOD duplicate isn't a smooth sphere.
func _auto_feed_detailed_relief(body_id: String) -> void:
	var planet: Node = get_node_or_null(^"/root/Planet")
	if planet == null:
		return
	var tex: Variant = planet.get(&"orbit_elevation_texture")
	if tex is Texture2DArray:
		_relief_by_body[body_id] = {
			"tex": tex,
			"face_res": float(planet.get(&"orbit_texture_face_res")),
			"amplitude": 1.0,
		}
		_touch_relief(body_id)


func _apply_body_relief(body_id: String, material: ShaderMaterial) -> void:
	var feed: Dictionary = _relief_by_body.get(body_id, {}) as Dictionary
	var tex: Variant = feed.get("tex")
	if tex is Texture2DArray and float(feed.get("face_res", 0.0)) > 0.5:
		material.set_shader_parameter(&"u_relief_tex", tex)
		material.set_shader_parameter(&"u_relief_face_res", float(feed.get("face_res")))
		material.set_shader_parameter(&"u_relief_shade_gain", 2.2 * float(feed.get("amplitude", 1.0)))
		material.set_shader_parameter(&"u_relief_ready", 1.0)
	else:
		material.set_shader_parameter(&"u_relief_ready", 0.0)


## Re-evaluate every far body's terminator against the current schematic
## positions. Cheap; call after a clock scrub.
func refresh_lighting() -> void:
	if _system == null:
		return
	_star_world = _resolve_star_world()
	for key: Variant in _records:
		var record: Dictionary = _records[key] as Dictionary
		var surface: MeshInstance3D = record.get("surface") as MeshInstance3D
		if surface == null or not (surface.material_override is ShaderMaterial):
			continue
		var absolute_world: Vec3D = _world_positions.get(String(key)) as Vec3D
		if absolute_world == null:
			continue
		var sun_dir: Vector3 = _sun_dir_for(absolute_world)
		record["sun_dir"] = sun_dir
		(surface.material_override as ShaderMaterial).set_shader_parameter(&"u_sun_dir", sun_dir)


func _compute_world_positions(system: Resource) -> Dictionary:
	var result: Dictionary = {}
	if system == null:
		return result
	var bodies: Array = system.get(&"bodies")
	var unresolved: Array[Resource] = []
	for body_value: Variant in bodies:
		var body: Resource = body_value as Resource
		if body == null:
			continue
		if String(body.get(&"parent_body_id")).is_empty():
			result[String(body.get(&"body_id"))] = Vec3D.new()
		else:
			unresolved.append(body)

	var passes: int = 0
	while not unresolved.is_empty() and passes <= bodies.size():
		passes += 1
		var progressed: bool = false
		for index: int in range(unresolved.size() - 1, -1, -1):
			var body: Resource = unresolved[index]
			var parent_id: String = String(body.get(&"parent_body_id"))
			var parent_world: Vec3D = result.get(parent_id) as Vec3D
			if parent_world == null:
				continue
			var parent: Resource = system.call("find_body", parent_id) as Resource
			result[String(body.get(&"body_id"))] = parent_world.add(_orbit_offset(body, parent))
			unresolved.remove_at(index)
			progressed = true
		if not progressed:
			break

	# ensure_valid() should prevent cycles/missing parents, but keeping unresolved
	# bodies at the origin makes a damaged preset inspectable instead of invisible.
	for body: Resource in unresolved:
		result[String(body.get(&"body_id"))] = Vec3D.new()
	return result


## Parent-centred offset for `body`, evaluated at the shared Frames sim clock so
## the schematic and OrbitalMotionRuntime always agree on where bodies are. The
## Kepler math lives in OrbitMath.
func _orbit_offset(body: Resource, parent: Resource) -> Vec3D:
	var parent_radius: float = maxf(float(parent.get(&"radius_m")), MIN_RADIUS_M) \
		if parent != null else MIN_RADIUS_M
	var child_radius: float = maxf(float(body.get(&"radius_m")), MIN_RADIUS_M)
	return OrbitMath.orbit_offset(body.get(&"orbit") as Resource,
		parent_radius, child_radius, OrbitMath.body_mu(parent), Frames.system_time_s)


func _compute_family_frame_radius() -> float:
	var extent: float = maxf(_selected_visual_radius_m, MIN_RADIUS_M)
	if _system == null:
		return extent
	var selected: Resource = _system.call("find_body", _selected_body_id) as Resource
	if selected == null:
		return extent
	var selected_parent_id: String = String(selected.get(&"parent_body_id"))
	var selected_radius: float = maxf(float(selected.get(&"radius_m")), MIN_RADIUS_M)
	for key: Variant in _records:
		var body_id: String = String(key)
		if body_id == _selected_body_id:
			continue
		var record: Dictionary = _records[key] as Dictionary
		var body: Resource = record.get("body") as Resource
		var offset: Vec3D = record.get("offset_from_selected") as Vec3D
		if body == null or offset == null:
			continue
		var direct_family: bool = body_id == selected_parent_id \
			or String(body.get(&"parent_body_id")) == _selected_body_id
		if not direct_family:
			continue
		var distance: float = offset.length()
		if distance > selected_radius * FAMILY_FRAME_DISTANCE_RATIO:
			continue
		var visual_radius: float = float(record.get("visual_radius_m", 1.0))
		extent = maxf(extent, distance + visual_radius)
	return extent


func _sync_floating_origin() -> void:
	for key: Variant in _records:
		var record: Dictionary = _records[key] as Dictionary
		var root: Node3D = record.get("root") as Node3D
		var world: Vec3D = record.get("world") as Vec3D
		if root == null or world == null:
			continue
		# System-scale compression (M7): pull a far body in along its own line of
		# sight and shrink it by the same factor, so its angular size is unchanged
		# but the scene bounds -- and the near camera's far plane -- stay bounded.
		var render_pos: Vector3 = Frames.to_render(world)
		var d: float = render_pos.length()
		var mul: float = SystemScaleView.scale_for(d)
		root.position = render_pos * mul
		root.scale = Vector3.ONE * mul


func _sync_camera_clip() -> void:
	var viewport: Viewport = get_viewport()
	if viewport == null:
		return
	var camera: Camera3D = viewport.get_camera_3d()
	if camera == null:
		return
	var selected_center_render: Vector3 = Frames.to_render(_anchor_relative(_selected_body_id))
	var center_distance: float = selected_center_render.distance_to(camera.global_position)
	# The near camera covers only the active body's direct family (near planet + a
	# concurrent moon). Every remote planet and the star are distance-compressed by
	# SystemScaleView into a fixed budget so they never reach AU scale here. (M7)
	var family_need: float = center_distance + _family_frame_radius_m * 1.15
	if center_distance <= SystemScaleView.NEAR_M:
		# On or near a surface: only cover the immediate family (a concurrent moon).
		# Resets `far` after a cruise so it does not stay stuck at system scale.
		# The near plane rises slightly with `far` so the far/near ratio stays out
		# of the light-culler's precision-failure range; on a bare surface it is the
		# shipped 0.25 m.
		camera.far = maxf(family_need, SURFACE_CAMERA_FAR_M)
		camera.near = clampf(camera.far * 1.5e-7, SURFACE_CAMERA_NEAR_M, 2.0)
	else:
		# In deep space / high orbit: pull the far plane out to the system budget so
		# every compressed remote body is in view, and raise the near plane to keep
		# the far/near ratio out of the light-culler's precision-failure range
		# (nothing is within tens of metres of the camera out here anyway).
		camera.far = maxf(family_need, SystemScaleView.VIEW_FAR_M)
		camera.near = clampf(camera.far * 8.0e-7, 1.0, 40.0)


func _clear_records() -> void:
	for key: Variant in _records:
		var record: Dictionary = _records[key] as Dictionary
		var root: Node3D = record.get("root") as Node3D
		if root != null and is_instance_valid(root):
			root.queue_free()
	_records.clear()


static func frame_distance_for_radius(radius_m: float, vertical_fov_deg: float,
		margin: float = 1.18) -> float:
	var radius: float = maxf(radius_m, MIN_RADIUS_M)
	var half_fov: float = deg_to_rad(clampf(vertical_fov_deg, 5.0, 170.0) * 0.5)
	return radius / maxf(sin(half_fov), 0.05) * maxf(margin, 1.01)
