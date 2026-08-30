extends Node3D
## Lightweight live preview for staged celestial bodies that do not own the
## currently applied terrestrial runtime.
##
## The production terrain renderer is intentionally not reused for a newly created
## procedural body because doing so would display the previous body's generated
## height/material fields. Instead, Planet Studio shows an explicit staged sphere.
## Stars use the same preview node with an emissive photosphere + corona.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")

const PREVIEW_RADIAL_SEGMENTS: int = 96
const PREVIEW_RINGS: int = 64
const MIN_RADIUS_M: float = 1.0

var _body: Resource
var _photosphere: MeshInstance3D
var _corona: MeshInstance3D
var _photosphere_material: StandardMaterial3D
var _corona_material: StandardMaterial3D
var _visual_radius_m: float = 1.0


func _ready() -> void:
	_build_preview_meshes()
	visible = false
	set_process(true)


func _process(_delta: float) -> void:
	if visible:
		_sync_floating_origin()
		_sync_camera_clip()


func show_body(body: Resource) -> void:
	_body = body
	if _body == null:
		hide_preview()
		return
	_body.call("ensure_children")
	_configure_from_body(_body)
	visible = true
	_sync_floating_origin()
	_sync_camera_clip()


func hide_preview() -> void:
	_body = null
	visible = false


func body_id() -> String:
	return String(_body.get(&"body_id")) if _body != null else ""


func visual_radius_m() -> float:
	return _visual_radius_m


func _build_preview_meshes() -> void:
	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = PREVIEW_RADIAL_SEGMENTS
	sphere.rings = PREVIEW_RINGS

	_photosphere = MeshInstance3D.new()
	_photosphere.name = "StagedBodySurface"
	_photosphere.mesh = sphere
	_photosphere.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_photosphere)

	var corona_sphere := SphereMesh.new()
	corona_sphere.radius = 1.0
	corona_sphere.height = 2.0
	corona_sphere.radial_segments = PREVIEW_RADIAL_SEGMENTS
	corona_sphere.rings = PREVIEW_RINGS
	_corona = MeshInstance3D.new()
	_corona.name = "StagedStarCorona"
	_corona.mesh = corona_sphere
	_corona.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_corona.visible = false
	add_child(_corona)

	_photosphere_material = StandardMaterial3D.new()
	_photosphere_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_photosphere_material.cull_mode = BaseMaterial3D.CULL_BACK
	_photosphere.material_override = _photosphere_material

	_corona_material = StandardMaterial3D.new()
	_corona_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_corona_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_corona_material.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	_corona_material.cull_mode = BaseMaterial3D.CULL_FRONT
	_corona_material.no_depth_test = true
	_corona.material_override = _corona_material


func _configure_from_body(body: Resource) -> void:
	var radius_m: float = maxf(float(body.get(&"radius_m")), MIN_RADIUS_M)
	var body_type: int = int(body.get(&"body_type"))
	var is_star: bool = body_type == BODY_SCRIPT.BodyType.STAR
	_visual_radius_m = radius_m
	_photosphere.scale = Vector3.ONE * radius_m
	_corona.visible = is_star

	if is_star:
		_configure_star(body, radius_m)
	else:
		_configure_solid_body(body_type, radius_m)


func _configure_solid_body(body_type: int, radius_m: float) -> void:
	var color := Color(0.30, 0.56, 0.78, 1.0)
	match body_type:
		BODY_SCRIPT.BodyType.MOON:
			color = Color(0.58, 0.60, 0.64, 1.0)
		BODY_SCRIPT.BodyType.DWARF:
			color = Color(0.56, 0.43, 0.32, 1.0)
		BODY_SCRIPT.BodyType.OTHER:
			color = Color(0.48, 0.42, 0.58, 1.0)
		_:
			pass
	_photosphere_material.albedo_color = color
	_photosphere_material.emission_enabled = false
	_corona.visible = false
	_visual_radius_m = radius_m


func _configure_star(body: Resource, radius_m: float) -> void:
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

	_photosphere_material.albedo_color = surface_color
	_photosphere_material.emission_enabled = true
	_photosphere_material.emission = surface_color
	_photosphere_material.emission_energy_multiplier = maxf(surface_intensity, 0.001)

	var corona_alpha: float = clampf(0.035 + corona_intensity * 0.035, 0.02, 0.30)
	_corona_material.albedo_color = Color(corona_color.r, corona_color.g, corona_color.b, corona_alpha)
	_corona_material.emission_enabled = true
	_corona_material.emission = corona_color
	_corona_material.emission_energy_multiplier = maxf(corona_intensity * 0.35, 0.01)
	_corona.scale = Vector3.ONE * radius_m * corona_extent
	_visual_radius_m = radius_m * minf(corona_extent, 2.0)


func _sync_floating_origin() -> void:
	# Preview bodies are body-centred just like the current production planet. The
	# mesh follows the local floating-origin frame while canonical position remains 0.
	position = Frames.to_render(Vec3D.new())


func _sync_camera_clip() -> void:
	var viewport: Viewport = get_viewport()
	if viewport == null:
		return
	var camera: Camera3D = viewport.get_camera_3d()
	if camera == null:
		return
	# AsterraPlayer recomputes its normal terrestrial far plane on every movement
	# and RMB look. Reassert the preview requirement afterwards so a solar-sized
	# staged star cannot disappear as soon as the author starts navigating.
	var center_distance: float = global_position.distance_to(camera.global_position)
	var required_far: float = center_distance + _visual_radius_m * 4.0
	camera.far = maxf(camera.far, required_far)


static func frame_distance_for_radius(radius_m: float, vertical_fov_deg: float,
		margin: float = 1.18) -> float:
	var radius: float = maxf(radius_m, MIN_RADIUS_M)
	var half_fov: float = deg_to_rad(clampf(vertical_fov_deg, 5.0, 170.0) * 0.5)
	return radius / maxf(sin(half_fov), 0.05) * maxf(margin, 1.01)
