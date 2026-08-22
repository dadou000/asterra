class_name GraphicsQuality
extends RefCounted
## One production renderer with scalable feature tiers.
##
## Asterra stays on Forward+ for every desktop preset. Lower presets remove the
## costly passes while keeping materials, lighting, and authored content shared.

enum Preset {
	PERFORMANCE,
	BALANCED,
	HIGH,
	ULTRA,
}

const DEFAULT_PRESET := Preset.HIGH


static func sanitize(preset: int) -> int:
	return clampi(preset, Preset.PERFORMANCE, Preset.ULTRA)


static func preset_name(preset: int) -> String:
	match sanitize(preset):
		Preset.PERFORMANCE:
			return "Performance"
		Preset.BALANCED:
			return "Balanced"
		Preset.ULTRA:
			return "Ultra"
		_:
			return "High"


static func preset_description(preset: int) -> String:
	match sanitize(preset):
		Preset.PERFORMANCE:
			return "FSR2 Balanced, SSAO, and fast shadows. Global illumination and reflections are disabled."
		Preset.BALANCED:
			return "FSR2 Quality with SSAO, screen-space indirect light, reflections, and two-split shadows."
		Preset.ULTRA:
			return "Native-resolution FSR2, long-range SDFGI, SSIL, SSR, glow, and four-split soft shadows."
		_:
			return "Recommended: FSR2 Ultra Quality, SDFGI, SSIL, SSR, and four-split soft shadows."


static func configure_viewport(viewport: Viewport, preset: int) -> void:
	if viewport == null:
		return
	var quality := sanitize(preset)
	# FSR2 at 1.0 is also Godot's temporal AA path. Do not stack MSAA, FXAA, or
	# the separate TAA pass on top of it; that costs more and softens the image.
	viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
	viewport.scaling_3d_scale = _render_scale(quality)
	viewport.fsr_sharpness = 0.18 if quality >= Preset.HIGH else 0.25
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	viewport.use_taa = false


static func configure_world_environment(environment: Environment, preset: int) -> void:
	if environment == null:
		return
	var quality := sanitize(preset)
	_configure_common_environment(environment, quality)

	# SDFGI is the native GI method suited to a camera moving over procedural
	# terrain. VoxelGI and LightmapGI both require a bounded offline bake.
	environment.sdfgi_enabled = quality >= Preset.HIGH
	environment.sdfgi_cascades = 6 if quality == Preset.ULTRA else 4
	environment.sdfgi_min_cell_size = 0.45 if quality == Preset.ULTRA else 0.65
	environment.sdfgi_use_occlusion = quality == Preset.ULTRA
	environment.sdfgi_bounce_feedback = 0.45
	environment.sdfgi_energy = 1.0
	environment.sdfgi_read_sky_light = true

	# Godot's local volumetric fog sees the global DirectionalLight even when the
	# planet is between the camera and the sun, which lights the night side gray.
	# Planet shaders already provide horizon-aware atmospheric perspective.
	environment.volumetric_fog_enabled = false


static func configure_studio_environment(environment: Environment, preset: int) -> void:
	if environment == null:
		return
	var quality := sanitize(preset)
	_configure_common_environment(environment, quality)
	# The character studio is a small, mostly dynamic rig. SSIL gives useful
	# contact bounce without voxelizing a tiny preview stage with SDFGI.
	environment.sdfgi_enabled = false
	environment.volumetric_fog_enabled = false
	environment.ssil_radius = 1.5
	environment.ssao_radius = 0.8
	environment.ssr_fade_out = 1.2


static func configure_sun(light: DirectionalLight3D, preset: int, studio := false) -> void:
	if light == null:
		return
	var quality := sanitize(preset)
	light.shadow_enabled = true
	if quality == Preset.PERFORMANCE:
		light.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
		light.directional_shadow_blend_splits = false
		light.light_angular_distance = 0.0
	elif quality == Preset.BALANCED:
		light.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_2_SPLITS
		light.directional_shadow_blend_splits = true
		light.light_angular_distance = 0.35 if studio else 0.45
	else:
		light.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
		light.directional_shadow_blend_splits = true
		light.light_angular_distance = 0.35 if studio else 0.53
	light.shadow_blur = 1.0


static func _configure_common_environment(environment: Environment, quality: int) -> void:
	# AgX preserves hue in bright highlights better than the older Filmic curve.
	environment.tonemap_mode = Environment.TONE_MAPPER_AGX
	environment.tonemap_agx_contrast = 1.2
	environment.tonemap_agx_white = 12.0
	environment.tonemap_exposure = 1.0

	environment.ssao_enabled = true
	environment.ssao_radius = 1.4
	environment.ssao_intensity = 1.6
	environment.ssao_power = 1.35
	environment.ssao_detail = 0.65
	environment.ssao_sharpness = 0.96

	environment.ssil_enabled = quality >= Preset.BALANCED
	environment.ssil_radius = 4.0
	environment.ssil_intensity = 1.0
	environment.ssil_sharpness = 0.96
	environment.ssil_normal_rejection = 1.0

	environment.ssr_enabled = quality >= Preset.BALANCED
	environment.ssr_max_steps = 96 if quality == Preset.ULTRA else 64
	environment.ssr_fade_in = 0.12
	environment.ssr_fade_out = 2.5
	environment.ssr_depth_tolerance = 0.35

	environment.glow_enabled = quality >= Preset.HIGH
	environment.glow_intensity = 0.12
	environment.glow_strength = 0.7
	environment.glow_bloom = 0.05
	environment.glow_hdr_threshold = 1.6
	environment.glow_hdr_scale = 1.3


static func _render_scale(preset: int) -> float:
	match preset:
		Preset.PERFORMANCE:
			return 0.59
		Preset.BALANCED:
			return 0.67
		Preset.ULTRA:
			return 1.0
		_:
			return 0.77
