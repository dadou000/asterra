extends "res://scripts/world_authoring/world_authoring_runtime_host.gd"
## Phase 24: persistent root-detail ownership for the multi-body Planet Studio view.
##
## Body selection is an editor interest/focus operation only. It must never hide the
## already-resident root terrain/ocean stack or rebind the single global atmosphere
## to an orbital child. The detailed root body remains a persistent member of the
## staged celestial system while moons/stars use lightweight orbital previews.

const BODY_DEFINITION_SCRIPT := preload(
	"res://scripts/world_authoring/model/celestial_body_definition.gd")

var _sky_owner_body_id: String = ""
var _sky_owner_center_world: Vec3D = Vec3D.new()
var _sky_owner_radius_m: float = 1.0


func _flush_active_body_preview() -> void:
	_preview_sync_pending = false
	if _authoring_session == null or _main == null:
		_preview_focus_pending = false
		return
	var system: Resource = _authoring_session.staged_system as Resource
	var body: Resource = _authoring_session.active_body() as Resource
	if system == null or body == null:
		_preview_focus_pending = false
		return

	var body_id: String = String(body.get(&"body_id"))
	var body_changed: bool = body_id != _preview_body_id
	_preview_body_id = body_id
	_selected_radius_m = maxf(float(body.get(&"radius_m")), 1.0)

	var rebaking: bool = bool(_main.get("_rebaking"))
	var detailed_available: bool = detailed_runtime_should_be_visible(system, rebaking)
	_selected_uses_detailed_surface = detailed_available \
		and body_id == _detailed_runtime_body_id

	# Critical invariant: the root runtime is system-owned, not selection-owned.
	# Selecting a moon therefore leaves Asterra terrain/ocean resident and hides its
	# blue lightweight duplicate for the entire time that detailed runtime is valid.
	_set_terrestrial_runtime_visible(detailed_available)
	var family_radius_m: float = _selected_radius_m
	if _celestial_preview != null:
		var hidden_detailed_id: String = _detailed_runtime_body_id \
			if detailed_available else ""
		_celestial_preview.call("show_system", system, body_id, hidden_detailed_id)
		_selected_center_world = _celestial_preview.call("selected_center_world") as Vec3D
		family_radius_m = maxf(
			_selected_radius_m,
			float(_celestial_preview.call("family_frame_radius_m")))
	if _selected_center_world == null:
		_selected_center_world = Vec3D.new()

	if _editor != null and _editor.has_method("set_camera_interest"):
		# Selected-body radius controls framing/editing. Family radius is only a clip
		# visibility requirement; it must not zoom normal selection out to the family.
		_editor.call("set_camera_interest", _selected_center_world,
			_selected_radius_m, _selected_uses_detailed_surface, family_radius_m)

	_update_sky_owner(system, body, detailed_available)
	var sky_owner: Resource = _sky_owner_body()
	if sky_owner != null:
		_apply_preview_atmosphere(sky_owner)
	_sync_depth_cloud_preview()

	var should_focus: bool = _preview_focus_pending or body_changed
	_preview_focus_pending = false
	if should_focus:
		_focus_camera_on_body(body)
		if int(body.get(&"body_type")) == BODY_DEFINITION_SCRIPT.BodyType.STAR:
			_set_editor_status("Focused %s — stellar target selected; resident root planet remains in the system view." % String(body.get(&"display_name")))
		elif _selected_uses_detailed_surface:
			_set_editor_status("Focused %s — detailed root terrain active; orbital companions remain at absolute positions." % String(body.get(&"display_name")))
		else:
			_set_editor_status("Focused %s — orbital target selected; resident root terrain and atmosphere remain visible at their system position." % String(body.get(&"display_name")))


func detailed_runtime_should_be_visible(system: Resource, rebaking: bool = false) -> bool:
	# Kept as an explicit testable invariant: changing active/selected body is not an
	# input. Only root-runtime validity and an in-progress replacement bake matter.
	return not rebaking and _detailed_runtime_is_root_usable(system)


func _update_sky_owner(system: Resource, selected_body: Resource,
		detailed_available: bool) -> void:
	var owner: Resource = null
	if detailed_available and not _detailed_runtime_body_id.is_empty():
		owner = system.call("find_body", _detailed_runtime_body_id) as Resource
	if owner == null:
		owner = selected_body
	if owner == null:
		_sky_owner_body_id = ""
		_sky_owner_center_world = Vec3D.new()
		_sky_owner_radius_m = 1.0
		return

	_sky_owner_body_id = String(owner.get(&"body_id"))
	_sky_owner_radius_m = maxf(float(owner.get(&"radius_m")), 1.0)
	_sky_owner_center_world = Vec3D.new()
	if _celestial_preview != null and not _sky_owner_body_id.is_empty():
		var center: Vec3D = _celestial_preview.call(
			"body_world_position", _sky_owner_body_id) as Vec3D
		if center != null:
			_sky_owner_center_world = center


func _sky_owner_body() -> Resource:
	if _authoring_session == null or _authoring_session.staged_system == null:
		return null
	if not _sky_owner_body_id.is_empty():
		var owner: Resource = _authoring_session.staged_system.call(
			"find_body", _sky_owner_body_id) as Resource
		if owner != null:
			return owner
	return _authoring_session.active_body() as Resource


func _sync_selected_preview_environment() -> void:
	if _preview_player == null or _main == null:
		return
	var sky_material: ShaderMaterial = _main.get("sky_mat") as ShaderMaterial
	if sky_material == null:
		return
	var world_pos: Vec3D = _preview_player.get("world_pos") as Vec3D
	if world_pos == null:
		return

	# The global sky shader has one spherical atmosphere. It follows the persistent
	# sky owner (normally the detailed root Asterra), not the editor selection.
	var local: Vec3D = world_pos.sub(_sky_owner_center_world)
	var up: Vector3 = local.normalized().to_v3() if local.length_sq() > 1.0 \
		else Vector3.UP
	var height_m: float = local.length() - _sky_owner_radius_m
	sky_material.set_shader_parameter("u_up", up)
	sky_material.set_shader_parameter("u_camera_height", height_m)


func _sky_owner_cloud_enabled() -> bool:
	var body: Resource = _sky_owner_body()
	if body == null or int(body.get(&"body_type")) == BODY_DEFINITION_SCRIPT.BodyType.STAR:
		return false
	var profile: Resource = body.get(&"planet_profile") as Resource
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource if profile != null else null
	return atmosphere != null and bool(atmosphere.get(&"enabled"))


func _sync_depth_cloud_preview() -> void:
	var clouds: Node = get_node_or_null("/root/VolumetricClouds")
	var effect: Object = clouds.get("_depth_effect") as Object if clouds != null else null
	var depth_ready: bool = false
	if effect != null and effect.has_method("is_ready"):
		depth_ready = bool(effect.call("is_ready"))

	# Depth clouds are spatially coupled to the root terrain compositor and are only
	# valid when that root is the current close editing target. From a moon/system
	# view, the sky raymarch fallback still renders the root body's atmosphere/clouds.
	var use_depth: bool = _selected_uses_detailed_surface \
		and _sky_owner_body_id == _detailed_runtime_body_id \
		and depth_ready
	if effect != null:
		effect.set("enabled", use_depth)
	var sky_material: ShaderMaterial = _main.get("sky_mat") as ShaderMaterial
	if sky_material != null:
		sky_material.set_shader_parameter("u_cloud_enabled",
			0.0 if use_depth else (1.0 if _sky_owner_cloud_enabled() else 0.0))


func _focus_camera_on_body(body: Resource) -> void:
	if _preview_player == null or body == null:
		return
	var camera: Camera3D = _preview_player.get("camera") as Camera3D
	if camera == null:
		return
	var radius_m: float = maxf(float(body.get(&"radius_m")), 1.0)
	var selected_visual_radius: float = radius_m * BODY_FRAME_SURFACE_MARGIN
	if _celestial_preview != null:
		selected_visual_radius = maxf(
			float(_celestial_preview.call("visual_radius_m")), radius_m)

	# Normal body selection frames only the selected target. Family extent is kept
	# exclusively for far-clip visibility so selecting a moon does not make it tiny.
	var frame_distance: float = float(CELESTIAL_PREVIEW_SCRIPT.frame_distance_for_radius(
		selected_visual_radius, camera.fov, BODY_FRAME_MARGIN))
	var radial_axis := Vector3(1.0, 0.18, 0.32).normalized()
	var current_world: Vec3D = _preview_player.get("world_pos") as Vec3D
	if current_world != null:
		var current_radial: Vec3D = current_world.sub(_selected_center_world)
		if current_radial.length_sq() > 1.0:
			radial_axis = current_radial.normalized().to_v3()
	var next_world: Vec3D = _selected_center_world.add(
		Vec3D.from_v3(radial_axis).mul(frame_distance))
	_preview_player.set("world_pos", next_world)
	_preview_player.set("pitch", -PI * 0.5)
	Frames.rebase(next_world)
	if _editor != null and _editor.has_method("_sync_interest_camera_transform"):
		_editor.call("_sync_interest_camera_transform")
	_preview_player.emit_signal("moved", next_world)
