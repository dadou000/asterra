extends Node
## M2 verification: far-LOD celestial bodies render as lit, optionally
## relief-shaded spheres (not flat unshaded discs), and OrbitSurfaceCache can
## build a body's relief elevation texture from an arbitrary sampler.
##   godot --headless --path . res://tests/validate_far_body_relief.tscn

const SESSION_SCRIPT := preload("res://scripts/world_authoring/world_authoring_session.gd")
const GEN_PROFILE := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const PLANET_SAMPLER := preload("res://scripts/gen/planet_sampler_gpu_runtime.gd")
const PREVIEW_RUNTIME := preload("res://scripts/world_authoring/celestial_body_preview_runtime.gd")
const FAR_BODY_SHADER_PATH := "res://shaders/far_body_surface.gdshader"

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	_test_orbit_surface_cache_from_arbitrary_sampler()
	_test_far_body_preview_materials()

	if _failed:
		get_tree().quit(1)
		return
	print("FAR_BODY_RELIEF_OK")
	get_tree().quit(0)


## OrbitSurfaceCache builds a relief elevation texture from a sampler that is NOT
## the resident Planet autoload.
func _test_orbit_surface_cache_from_arbitrary_sampler() -> void:
	var cfg := GEN_CONFIG.new()
	cfg.face_res = 24
	cfg.erosion_iterations = 6
	cfg.world_seed = 0x46415242534642
	cfg.planet_radius = 600000.0
	var fields := PlanetBake.new(cfg).bake(Callable(), false)
	_assert(fields != null, "bake produced fields")

	var sampler: Node = PLANET_SAMPLER.new()
	sampler.name = "Sampler_farbody"
	add_child(sampler)
	sampler.adopt(fields)
	_assert(bool(sampler.get(&"ready_state")), "throwaway sampler is ready after adopt")

	var built: Dictionary = OrbitSurfaceCache.build_images_from(sampler)
	_assert(not built.is_empty(), "build_images_from returned images")
	_assert((built.get("images", []) as Array).size() == 6,
		"orbit surface cache built all six cube faces (got %d)" % (built.get("images", []) as Array).size())
	var expected_face_res: int = mini(fields.grid.res * OrbitSurfaceCache.UPSAMPLE, OrbitSurfaceCache.MAX_FACE_RES)
	_assert(int(built.get("face_res", 0)) == expected_face_res,
		"face_res == grid.res*UPSAMPLE capped at MAX_FACE_RES (%d vs %d)" % [built.get("face_res", 0), expected_face_res])
	var first_image: Image = (built["images"] as Array)[0]
	_assert(first_image != null and first_image.get_format() == Image.FORMAT_RF,
		"each face is a single-channel float elevation image")
	_assert(first_image.get_width() == expected_face_res + 2,
		"each face carries the one-texel cross-face gutter (%d vs %d)" % [first_image.get_width(), expected_face_res + 2])

	var tex: Texture2DArray = OrbitSurfaceCache.create_texture(built)
	_assert(tex != null, "create_texture uploaded a Texture2DArray")
	_assert(tex.get_layers() == 6, "the relief texture array has six layers (got %d)" % tex.get_layers())

	# Relief actually varies -- an amplified terrain must not be a constant plane.
	var lo := 1.0e30
	var hi := -1.0e30
	for y in range(1, first_image.get_height() - 1, 3):
		for x in range(1, first_image.get_width() - 1, 3):
			var e := first_image.get_pixel(x, y).r
			lo = minf(lo, e)
			hi = maxf(hi, e)
	_assert(hi - lo > 1.0, "the relief image has real elevation variation (span %.2f m)" % (hi - lo))

	sampler.free()


## CelestialBodyPreviewRuntime gives solid bodies a lit far_body_surface material
## with a valid terminator direction; stars keep their emissive material; a relief
## feed flips the body to relief-shaded.
func _test_far_body_preview_materials() -> void:
	var session: RefCounted = SESSION_SCRIPT.new()
	session.bootstrap_from_generation_profile(GEN_PROFILE.new())
	var system: Resource = session.staged_system

	var preview: Node = PREVIEW_RUNTIME.new()
	add_child(preview)
	preview.call("show_system", system, "asterra")

	_assert(int(preview.call("preview_body_count")) >= 2,
		"the preview built at least the star + planet (got %d)" % preview.call("preview_body_count"))

	var star_mat: Material = _surface_material(preview, "helion")
	_assert(star_mat is StandardMaterial3D, "the star keeps a StandardMaterial3D emissive surface")
	_assert((star_mat as StandardMaterial3D).emission_enabled, "the star surface is emissive")

	var planet_mat: Material = _surface_material(preview, "asterra")
	_assert(planet_mat is ShaderMaterial, "a solid body uses a ShaderMaterial far-LOD surface")
	var shader: Shader = (planet_mat as ShaderMaterial).shader
	_assert(shader != null and shader.resource_path == FAR_BODY_SHADER_PATH,
		"the solid-body material runs far_body_surface.gdshader (got %s)" % (shader.resource_path if shader else "<null>"))

	var sun_dir: Variant = (planet_mat as ShaderMaterial).get_shader_parameter(&"u_sun_dir")
	_assert(sun_dir is Vector3 and (sun_dir as Vector3).is_finite() and (sun_dir as Vector3).length() > 0.5,
		"the far body has a finite, unit-ish terminator direction (got %s)" % [sun_dir])
	_assert(float((planet_mat as ShaderMaterial).get_shader_parameter(&"u_relief_ready")) == 0.0
		or float((planet_mat as ShaderMaterial).get_shader_parameter(&"u_relief_ready")) == 1.0,
		"u_relief_ready is a clean 0/1 flag")

	# Feed a relief texture and confirm the body switches to relief-shaded.
	var img := Image.create(18, 18, false, Image.FORMAT_RF)
	img.fill(Color(0.0, 0.0, 0.0, 1.0))
	var layers: Array[Image] = []
	for _i in 6:
		layers.append(img.duplicate())
	var relief := Texture2DArray.new()
	relief.create_from_images(layers)
	preview.call("set_body_relief", "asterra", relief, 16.0, 1.0)
	_assert(float((planet_mat as ShaderMaterial).get_shader_parameter(&"u_relief_ready")) == 1.0,
		"set_body_relief flips u_relief_ready to 1")
	_assert((planet_mat as ShaderMaterial).get_shader_parameter(&"u_relief_tex") == relief,
		"set_body_relief binds the supplied texture array")

	preview.call("set_body_relief", "asterra", null, 0.0)
	_assert(float((planet_mat as ShaderMaterial).get_shader_parameter(&"u_relief_ready")) == 0.0,
		"clearing the relief feed returns the body to a smooth lit sphere")

	preview.free()


func _surface_material(preview: Node, body_id: String) -> Material:
	var root: Node = preview.find_child("CelestialPreview_%s" % body_id, true, false)
	if root == null:
		return null
	var surface: MeshInstance3D = root.find_child("Surface", true, false) as MeshInstance3D
	return surface.material_override if surface != null else null


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("FAR_BODY_RELIEF_FAILED: %s" % message)
