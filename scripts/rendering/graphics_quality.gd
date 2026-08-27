class_name GraphicsQuality
extends RefCounted
## One production renderer with scalable feature tiers.
##
## Asterra stays on Forward+ for every desktop preset. Lower presets remove the
## costly passes while keeping materials, lighting, authored content, and the
## physical meaning of the scene shared.

enum Preset {
	PERFORMANCE,
	BALANCED,
	HIGH,
	ULTRA,
}

const DEFAULT_PRESET := Preset.HIGH

## Sun brightness as Godot light energy. Everything else solar is derived from
## this, so the scattering and the surface can never disagree about how bright
## the star is.
const SUN_LIGHT_ENERGY := 1.6


## Top-of-atmosphere solar irradiance, in the same radiance units the shaders
## work in.
##
## This is not a free parameter, and getting it wrong is invisible in isolation
## and ruinous in combination. The scattering integrals build in-scattered
## radiance as irradiance x phase x optical depth, while the surface is lit by
## Godot, which hands `light()` a LIGHT_COLOR already multiplied by energy and by
## PI. A Lambertian surface therefore returns albedo * energy * ndotl, which is
## albedo * (energy * PI) * ndotl / PI -- so the irradiance the surface sees is
## `light_energy * PI`, and the atmosphere has to be told that same number.
static func solar_irradiance() -> float:
	return SUN_LIGHT_ENERGY * PI


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
			return "FSR2 Balanced, SSAO, fast shadows, and reduced water geometry. Global illumination and reflections are disabled."
		Preset.BALANCED:
			return "FSR2 Quality with SSAO, screen-space indirect light, reflections, and medium water interaction range."
		Preset.ULTRA:
			return "Native-resolution FSR2, long-range SDFGI, SSIL, SSR, maximum water spectrum detail, and four-split soft shadows."
		_:
			return "Recommended: FSR2 Ultra Quality, SDFGI, SSIL, SSR, and high-detail reactive water."


## Visual water profile. This never changes authoritative buoyancy or gameplay
## hydrodynamics. A low preset is allowed to change *representation* only:
## wavelengths that are too expensive to displace geometrically are converted
## into normal/slope variance so distant water does not turn into a mirror.
##
## `geometry_spacing_scale` changes the metre spacing of the same concentric
## topology rather than multiplying mesh vertex count. Ultra therefore halves
## triangle size at a given distance and pays roughly one additional clipmap
## level, while Performance can cover the same horizon with fewer vertices.
##
## The interaction values bound the analytical disturbance history consumed by
## the visual shader. Future hull/propeller systems can submit exactly the same
## impulses on every preset; lower presets simply retain fewer of them visually.
static func water_profile(preset: int) -> Dictionary:
	match sanitize(preset):
		Preset.PERFORMANCE:
			return {
				"displacement_band_count": 3,
				"micro_normal_band_count": 1,
				"geometry_spacing_scale": 1.35,
				"displacement_spacing_scale": 1.45,
				"bathymetry_level_limit": 4,
				"foam_quality": 0.45,
				"crest_scatter_strength": 0.35,
				"interaction_budget": 8,
				"interaction_lifetime_s": 12.0,
				"interaction_range_m": 300.0,
				"interaction_vertex_level": 2,
			}
		Preset.BALANCED:
			return {
				"displacement_band_count": 4,
				"micro_normal_band_count": 2,
				"geometry_spacing_scale": 1.0,
				"displacement_spacing_scale": 1.15,
				"bathymetry_level_limit": 6,
				"foam_quality": 0.70,
				"crest_scatter_strength": 0.55,
				"interaction_budget": 16,
				"interaction_lifetime_s": 24.0,
				"interaction_range_m": 700.0,
				"interaction_vertex_level": 3,
			}
		Preset.ULTRA:
			return {
				"displacement_band_count": 6,
				"micro_normal_band_count": 6,
				"geometry_spacing_scale": 0.50,
				"displacement_spacing_scale": 0.82,
				"bathymetry_level_limit": 8,
				"foam_quality": 1.0,
				"crest_scatter_strength": 1.0,
				"interaction_budget": 64,
				"interaction_lifetime_s": 90.0,
				"interaction_range_m": 5000.0,
				"interaction_vertex_level": 5,
			}
		_:
			return {
				"displacement_band_count": 5,
				"micro_normal_band_count": 4,
				"geometry_spacing_scale": 0.72,
				"displacement_spacing_scale": 1.0,
				"bathymetry_level_limit": 7,
				"foam_quality": 0.90,
				"crest_scatter_strength": 0.78,
				"interaction_budget": 32,
				"interaction_lifetime_s": 45.0,
				"interaction_range_m": 2000.0,
				"interaction_vertex_level": 4,
			}


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
	environment.tonemap_agx_contrast = 1.45
	# AgX is a highlight-desaturating curve by design: its inset matrix pulls
	# every channel toward the others so bright saturated colour rolls off
	# gracefully instead of clipping to a primary. That is the right behaviour for
	# highlights and the wrong one for a whole landscape, and this build exposes no
	# saturation control of its own, so it is restored here. Without it a correct
	# foliage reflectance still renders as olive-khaki.
	environment.adjustment_enabled = true
	environment.adjustment_saturation = 1.38
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
