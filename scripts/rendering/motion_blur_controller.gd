class_name MotionBlurController
extends Node
## Autoload owner for MotionBlurCompositorEffect.
##
## Installs the effect into the same validated Asterra atmosphere WorldEnvironment
## VolumetricCloudController and TerrainOcclusionCompositorEffect use, appended
## after cloud compositing so the blur sees the fully composited frame (clouds
## included). Forwards Frames.origin_shifted so a floating-origin rebase is never
## misread as a frame of camera motion.

const ATMOSPHERE_SHADER_PATH := "res://shaders/atmosphere_sky.gdshader"

var _effect: MotionBlurCompositorEffect
var _installed := false


func _ready() -> void:
	_effect = MotionBlurCompositorEffect.new()
	if _effect == null or not _effect.is_ready():
		return
	# AppSettings is declared earlier in project.godot's autoload list, so it is
	# already loaded from disk by the time this runs. Later user-driven changes
	# push into this controller instead (AppSettings._apply_motion_blur_runtime),
	# since going the other way around at boot would race the autoload order.
	_effect.enabled = AppSettings.motion_blur_enabled
	_effect.strength = AppSettings.motion_blur_strength
	Frames.origin_shifted.connect(_effect.on_origin_shifted)
	get_tree().node_added.connect(_on_node_added)
	call_deferred("_scan_existing_environments")


func _on_node_added(node: Node) -> void:
	if node is WorldEnvironment:
		call_deferred("_try_install", node)


func _scan_existing_environments() -> void:
	_scan_node(get_tree().root)


func _scan_node(node: Node) -> void:
	if node is WorldEnvironment:
		_try_install(node as WorldEnvironment)
	for child: Node in node.get_children():
		_scan_node(child)


func _try_install(world_environment: WorldEnvironment) -> void:
	if world_environment == null or _effect == null or not _effect.is_ready():
		return
	# Only the validated Asterra atmosphere environment gets this pass -- mirrors
	# VolumetricCloudController._try_bind_environment / the terrain occlusion guard.
	var environment: Environment = world_environment.environment
	if environment == null or environment.sky == null \
			or not (environment.sky.sky_material is ShaderMaterial):
		return
	var sky_material := environment.sky.sky_material as ShaderMaterial
	if sky_material.shader == null \
			or sky_material.shader.resource_path != ATMOSPHERE_SHADER_PATH:
		return
	var compositor: Compositor = world_environment.compositor
	if compositor == null:
		compositor = Compositor.new()
	var effects: Array[CompositorEffect] = compositor.compositor_effects
	if not effects.has(_effect):
		# Appended last so it always runs after cloud compositing (installed earlier
		# in the same deferred-call queue, per the VolumetricClouds autoload order).
		effects.append(_effect)
		compositor.compositor_effects = effects
	world_environment.compositor = compositor
	_installed = true


func is_enabled() -> bool:
	return _effect != null and _effect.enabled


func set_enabled(value: bool) -> void:
	if _effect != null:
		_effect.enabled = value


func set_strength(value: float) -> void:
	if _effect != null:
		_effect.strength = maxf(value, 0.0)


func set_max_blur_px(value: float) -> void:
	if _effect != null:
		_effect.max_blur_px = maxf(value, 0.0)


## Discards the stored previous-frame camera pose so the next frame renders
## unblurred. Call after a hard cut that is not a Frames rebase (e.g. a debug/
## editor teleport that does not go through Frames.rebase()).
func reset_history() -> void:
	if _effect != null:
		_effect.reset_history()
