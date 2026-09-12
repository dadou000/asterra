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

## Upscaler / temporal anti-aliasing choice. Independent of Preset: any preset can
## be paired with any of these, though FSR2 is the recommended default (see
## configure_viewport's note on not stacking a second temporal pass on top of it).
enum UpscaleMode {
	BILINEAR, ## No upscale filtering, no temporal AA. Cheapest, aliased below 1.0 scale.
	FSR1,     ## Spatial-only sharpening upscale. No temporal stability.
	FSR2,     ## Temporal upscale + anti-aliasing.
	TAA,      ## Godot's native temporal AA at full (1.0) resolution. No upscaling.
}

const DEFAULT_UPSCALE_MODE := UpscaleMode.FSR2

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
##
## It previously was not. The shaders defaulted to 22 against a sun of 1.6, so
## every scattering term was more than four times too bright relative to the
## ground it was scattering in front of. That does not read as a bright sky, it
## reads as a pale wash over everything past a few kilometres, and no amount of
## work on surface colour survives it.
static func solar_irradiance() -> float:
	return SUN_LIGHT_ENERGY * PI * solar_distance_scale()


## Inverse-square brightness factor for the current Asterra-Helion distance
## (1.0 at REFERENCE_HELION_DISTANCE_M). The surface (`sun.light_energy`) and the
## scattering (this) MUST scale by the same number -- see the docstring above.
## Reads the Frames autoload when it exists; returns 1.0 in isolated/headless CI.
static func solar_distance_scale() -> float:
	var loop := Engine.get_main_loop()
	if loop is SceneTree:
		var frames := (loop as SceneTree).root.get_node_or_null(^"Frames")
		if frames != null and frames.has_method(&"solar_distance_scale"):
			return float(frames.call(&"solar_distance_scale"))
	return 1.0


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

	# Re-enabled 2026-09-11 to try porting godot-forest-demo's tuned volumetric
	# fog. Previously forced off because Godot's local volumetric fog sees the
	# global DirectionalLight even when the planet is between the camera and the
	# sun, lighting the night side gray -- pending live verification at a planet
	# terminator (low/grazing sun angle). If that bug reproduces, revert this to
	# false and record the outcome in planning/PROBLEMS.md (P-012); planet
	# shaders already provide horizon-aware atmospheric perspective as a fallback.
	environment.volumetric_fog_enabled = true
	environment.volumetric_fog_density = 0.015
	environment.volumetric_fog_albedo = Color(0.7734375, 0.7425537, 0.703949, 1.0)
	environment.volumetric_fog_anisotropy = 0.35
	environment.volumetric_fog_length = 6.23
	environment.volumetric_fog_detail_spread = 1.5157164
	environment.volumetric_fog_ambient_inject = 0.11
	environment.volumetric_fog_sky_affect = 0.768
	environment.volumetric_fog_temporal_reprojection_amount = 0.951


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


static func advanced_defaults(preset: int) -> Dictionary:
	var quality := sanitize(preset)
	return {
		"render_scale": _render_scale(quality),
		"sdfgi_enabled": quality >= Preset.HIGH,
		"sdfgi_cascades": 6 if quality == Preset.ULTRA else 4,
		"sdfgi_cell_size": 0.45 if quality == Preset.ULTRA else 0.65,
		"ssao_enabled": true,
		"ssil_enabled": quality >= Preset.BALANCED,
		"ssr_enabled": quality >= Preset.BALANCED,
		"glow_enabled": quality >= Preset.HIGH,
		"shadow_splits": 1 if quality == Preset.PERFORMANCE \
			else (2 if quality == Preset.BALANCED else 4),
		"cloud_quality": quality,
	}


static func apply_advanced_viewport(viewport: Viewport, options: Dictionary) -> void:
	if viewport == null:
		return
	# Render scale is applied by apply_upscaler() instead, which also owns
	# scaling_3d_mode/fsr_sharpness/use_taa and must be the single writer of all
	# four so they never fall out of sync with each other.
	pass


static func sanitize_upscale_mode(mode: int) -> int:
	return clampi(mode, UpscaleMode.BILINEAR, UpscaleMode.TAA)


static func upscale_mode_name(mode: int) -> String:
	match sanitize_upscale_mode(mode):
		UpscaleMode.BILINEAR:
			return "Bilinear"
		UpscaleMode.FSR1:
			return "FSR 1.0"
		UpscaleMode.TAA:
			return "Native TAA"
		_:
			return "FSR 2.0"


static func upscale_mode_description(mode: int) -> String:
	match sanitize_upscale_mode(mode):
		UpscaleMode.BILINEAR:
			return "Cheapest. No temporal anti-aliasing; render scale below 100% looks soft and aliased."
		UpscaleMode.FSR1:
			return "Spatial sharpening upscale. Cheaper than FSR 2.0 but has no temporal anti-aliasing."
		UpscaleMode.TAA:
			return "Godot's native temporal anti-aliasing at full resolution. No upscaling."
		_:
			return "Temporal upscale and anti-aliasing. Recommended -- stays sharp and stable below 100% scale."


## Only Bilinear/FSR1/FSR2 render at less than full resolution; Native TAA always
## runs at scale 1.0 (apply_upscaler enforces this regardless of the input value).
static func supports_render_scale(mode: int) -> bool:
	return sanitize_upscale_mode(mode) != UpscaleMode.TAA


## Only FSR1/FSR2 have an edge-sharpening pass to tune.
static func supports_sharpness(mode: int) -> bool:
	var sanitized := sanitize_upscale_mode(mode)
	return sanitized == UpscaleMode.FSR1 or sanitized == UpscaleMode.FSR2


## Single writer of scaling_3d_mode / scaling_3d_scale / fsr_sharpness / use_taa.
## Called after configure_viewport()/apply_advanced_viewport() so an explicit user
## upscaler choice always wins over the preset's default. Godot's FSR2 already is a
## temporal pass, so use_taa only ever turns on for UpscaleMode.TAA -- stacking a
## second temporal accumulation on top of FSR2 would cost more and soften the image.
static func apply_upscaler(viewport: Viewport, mode: int, render_scale: float,
		sharpness: float) -> void:
	if viewport == null:
		return
	var sanitized_mode := sanitize_upscale_mode(mode)
	var scale := clampf(render_scale, 0.50, 1.00)
	var sharp := clampf(sharpness, 0.0, 2.0)
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	match sanitized_mode:
		UpscaleMode.TAA:
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			viewport.scaling_3d_scale = 1.0
			viewport.use_taa = true
		UpscaleMode.BILINEAR:
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			viewport.scaling_3d_scale = scale
			viewport.use_taa = false
		UpscaleMode.FSR1:
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR
			viewport.scaling_3d_scale = scale
			viewport.fsr_sharpness = sharp
			viewport.use_taa = false
		_:
			viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
			viewport.scaling_3d_scale = scale
			viewport.fsr_sharpness = sharp
			viewport.use_taa = false


static func apply_advanced_world_environment(environment: Environment,
		options: Dictionary) -> void:
	if environment == null:
		return
	environment.sdfgi_enabled = bool(options.get("sdfgi_enabled", false))
	if environment.sdfgi_enabled:
		environment.sdfgi_cascades = clampi(int(options.get("sdfgi_cascades", 4)), 2, 8)
		environment.sdfgi_min_cell_size = clampf(
			float(options.get("sdfgi_cell_size", 0.65)), 0.25, 2.00)
	environment.ssao_enabled = bool(options.get("ssao_enabled", true))
	environment.ssil_enabled = bool(options.get("ssil_enabled", true))
	environment.ssr_enabled = bool(options.get("ssr_enabled", true))
	environment.glow_enabled = bool(options.get("glow_enabled", true))


static func apply_advanced_sun(light: DirectionalLight3D, options: Dictionary,
		_studio := false) -> void:
	if light == null:
		return
	var splits := int(options.get("shadow_splits", 4))
	light.shadow_enabled = splits > 0
	match splits:
		1:
			light.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
			light.directional_shadow_blend_splits = false
		2:
			light.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_2_SPLITS
			light.directional_shadow_blend_splits = true
		4:
			light.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
			light.directional_shadow_blend_splits = true


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
