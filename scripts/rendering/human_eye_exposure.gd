class_name HumanEyeExposure
extends Node
## Asymmetric, two-stage exposure adaptation for the planetary world.
##
## Godot performs the actual full-frame HDR luminance reduction on the GPU. This
## controller supplies physiology-inspired timing and explicit scene-linear
## metering limits. Bright adaptation is fast; dark adaptation first follows the
## cone response, then gradually unlocks the deeper rod threshold.

# The renderer's AgX white point is 12.0, so values at and above this level do
# not drive the exposure farther down. The two darkness floors span six stops
# for cone vision and nine stops for fully adapted rod vision.
const HIGHLIGHT_CLIP_LUMINANCE := 12.0
const CONE_DARKNESS_LUMINANCE := 0.015625      # 1 / 64
const ROD_DARKNESS_LUMINANCE := 0.001953125    # 1 / 512
const MIDDLE_GREY := 0.18

# Godot's luminance reducer is a full-frame arithmetic average. A mostly black
# orbital view can therefore overpower a much smaller sunlit planet. This guard
# gives bright content a nonlinear, highlight-priority vote and raises the
# meter's lower bound before the average can expose the limb to white.
const PLANET_HIGHLIGHT_FLOOR := 0.24
const DIRECT_SUN_HIGHLIGHT_FLOOR := 0.60
const HIGHLIGHT_PRIORITY_GAIN := 8.0

# CameraAttributesPractical converts ISO sensitivity to renderer luminance as
# sensitivity * ((12.5 / 100) / ISO). At the fixed ISO 100 baseline this is
# exactly sensitivity / 800.
const BASE_ISO := 100.0
const SENSITIVITY_PER_LUMINANCE := 800.0

# Research-calibrated response constants.
const LIGHT_ADAPTATION_RATE := 5.0             # 0.20 s time constant
const CONE_DARK_BASE := 0.996787282
const ROD_DARK_BASE := 0.999637396
const ADAPTATION_SAMPLE_SECONDS := 0.28
const ROD_ONSET_SECONDS := 420.0
const NIGHT_RESET_LUMINANCE := 0.05
const DIRECTION_HYSTERESIS_STOPS := 0.10

var observer: AsterraPlayer
var attributes: CameraAttributesPractical

var _adapted_luminance := MIDDLE_GREY
var _dark_seconds := 0.0
var _initialized := false

var _cone_dark_rate := -log(CONE_DARK_BASE) / ADAPTATION_SAMPLE_SECONDS
var _rod_dark_rate := -log(ROD_DARK_BASE) / ADAPTATION_SAMPLE_SECONDS

# Debug controls are deliberately neutral at their defaults. They are runtime
# diagnostics only and are not persisted into world or graphics configuration.
var _debug_auto_exposure_enabled := true
var _debug_freeze_adaptation := false
var _debug_rate_scale := 1.0
var _debug_exposure_compensation_ev := 0.0

# Last-frame diagnostics exposed to the debug menu. The metered value here is the
# analytic proxy used to choose the asymmetric response rate; the actual rendered
# luminance distribution is shown separately by ScreenHistogramCompositorEffect.
var _last_metered_luminance := MIDDLE_GREY
var _last_highlight_floor := ROD_DARKNESS_LUMINANCE
var _last_meter_floor := CONE_DARKNESS_LUMINANCE
var _last_rate := LIGHT_ADAPTATION_RATE
var _last_direction := "steady"


func configure(world_environment: WorldEnvironment) -> void:
	attributes = CameraAttributesPractical.new()
	attributes.exposure_sensitivity = BASE_ISO
	attributes.exposure_multiplier = 1.0
	attributes.auto_exposure_scale = MIDDLE_GREY
	attributes.auto_exposure_min_sensitivity = _to_sensitivity(CONE_DARKNESS_LUMINANCE)
	attributes.auto_exposure_max_sensitivity = _to_sensitivity(HIGHLIGHT_CLIP_LUMINANCE)
	attributes.auto_exposure_speed = LIGHT_ADAPTATION_RATE
	attributes.auto_exposure_enabled = true
	world_environment.camera_attributes = attributes


func observe(p_observer: AsterraPlayer) -> void:
	observer = p_observer
	_initialized = false
	_dark_seconds = 0.0


func _process(delta: float) -> void:
	if attributes == null or observer == null or not is_instance_valid(observer):
		return

	var highlight_floor := _highlight_protection_floor()
	var metered := _metered_luminance_proxy(highlight_floor)
	_last_highlight_floor = highlight_floor
	_last_metered_luminance = metered

	if not _initialized:
		_adapted_luminance = metered
		_initialized = true

	attributes.auto_exposure_enabled = _debug_auto_exposure_enabled
	attributes.exposure_multiplier = pow(2.0, _debug_exposure_compensation_ev)

	# Disabling auto exposure or freezing adaptation pauses the physiological state
	# too. Returning to live mode therefore resumes from the exact diagnostic state
	# that was visible when it was paused.
	if not _debug_auto_exposure_enabled:
		_last_direction = "disabled"
		return
	if _debug_freeze_adaptation:
		attributes.auto_exposure_speed = 0.0
		_last_rate = 0.0
		_last_direction = "frozen"
		return

	# Sustained low light unlocks rod sensitivity only after the cone phase. Any
	# meaningful return to daylight rapidly bleaches that accumulated adaptation.
	if metered <= NIGHT_RESET_LUMINANCE:
		_dark_seconds += delta
	else:
		_dark_seconds = 0.0

	var hysteresis := pow(2.0, DIRECTION_HYSTERESIS_STOPS)
	var becoming_brighter := metered > _adapted_luminance * hysteresis
	var becoming_darker := metered < _adapted_luminance / hysteresis
	var base_rate := LIGHT_ADAPTATION_RATE if becoming_brighter else _dark_adaptation_rate()
	var rate := base_rate * _debug_rate_scale
	var weight := 1.0 - exp(-rate * delta)
	_adapted_luminance = exp(lerpf(
		log(maxf(_adapted_luminance, ROD_DARKNESS_LUMINANCE)),
		log(maxf(metered, ROD_DARKNESS_LUMINANCE)),
		weight
	))

	_last_rate = rate
	if becoming_brighter:
		_last_direction = "light adapting"
	elif becoming_darker:
		_last_direction = "dark adapting"
	else:
		_last_direction = "hysteresis hold"

	if not is_equal_approx(attributes.auto_exposure_speed, rate):
		attributes.auto_exposure_speed = rate
	var meter_floor := maxf(_current_darkness_floor(), highlight_floor)
	_last_meter_floor = meter_floor
	var min_sensitivity := _to_sensitivity(meter_floor)
	if not is_equal_approx(attributes.auto_exposure_min_sensitivity, min_sensitivity):
		attributes.auto_exposure_min_sensitivity = min_sensitivity


func set_debug_auto_exposure_enabled(value: bool) -> void:
	_debug_auto_exposure_enabled = value
	if attributes != null:
		attributes.auto_exposure_enabled = value


func set_debug_freeze_adaptation(value: bool) -> void:
	_debug_freeze_adaptation = value
	if attributes != null and value:
		attributes.auto_exposure_speed = 0.0


func set_debug_rate_scale(value: float) -> void:
	_debug_rate_scale = clampf(value, 0.05, 8.0)


func set_debug_exposure_compensation_ev(value: float) -> void:
	_debug_exposure_compensation_ev = clampf(value, -6.0, 6.0)
	if attributes != null:
		attributes.exposure_multiplier = pow(2.0, _debug_exposure_compensation_ev)


func reset_debug_controls() -> void:
	_debug_auto_exposure_enabled = true
	_debug_freeze_adaptation = false
	_debug_rate_scale = 1.0
	_debug_exposure_compensation_ev = 0.0
	reset_adaptation_state()
	if attributes != null:
		attributes.auto_exposure_enabled = true
		attributes.exposure_multiplier = 1.0
		attributes.auto_exposure_speed = LIGHT_ADAPTATION_RATE


func reset_adaptation_state() -> void:
	_initialized = false
	_dark_seconds = 0.0
	_adapted_luminance = MIDDLE_GREY
	_last_direction = "reset"


func debug_snapshot() -> Dictionary:
	var stage := "cones"
	if _dark_seconds >= ROD_ONSET_SECONDS:
		stage = "rods"
	elif _last_metered_luminance > NIGHT_RESET_LUMINANCE:
		stage = "photopic"

	var nominal_ev := log(MIDDLE_GREY / maxf(_adapted_luminance,
		ROD_DARKNESS_LUMINANCE)) / log(2.0)
	return {
		"auto_exposure_enabled": _debug_auto_exposure_enabled,
		"frozen": _debug_freeze_adaptation,
		"stage": stage,
		"direction": _last_direction,
		"metered_proxy": _last_metered_luminance,
		"adapted_luminance": _adapted_luminance,
		"highlight_floor": _last_highlight_floor,
		"darkness_floor": _current_darkness_floor(),
		"meter_floor": _last_meter_floor,
		"dark_seconds": _dark_seconds,
		"rod_onset_seconds": ROD_ONSET_SECONDS,
		"adaptation_rate": _last_rate,
		"rate_scale": _debug_rate_scale,
		"exposure_compensation_ev": _debug_exposure_compensation_ev,
		"nominal_adaptation_ev": nominal_ev,
		"auto_exposure_speed": attributes.auto_exposure_speed if attributes != null else 0.0,
		"min_sensitivity": attributes.auto_exposure_min_sensitivity if attributes != null else 0.0,
		"max_sensitivity": attributes.auto_exposure_max_sensitivity if attributes != null else 0.0,
		"exposure_multiplier": attributes.exposure_multiplier if attributes != null else 1.0,
	}


func _dark_adaptation_rate() -> float:
	return _rod_dark_rate if _dark_seconds >= ROD_ONSET_SECONDS else _cone_dark_rate


func _current_darkness_floor() -> float:
	if _dark_seconds <= ROD_ONSET_SECONDS:
		return CONE_DARKNESS_LUMINANCE
	var rod_time := _dark_seconds - ROD_ONSET_SECONDS
	var rod_progress := 1.0 - exp(-_rod_dark_rate * rod_time)
	# Sensitivity changes logarithmically, so interpolate the threshold in stops.
	return exp(lerpf(
		log(CONE_DARKNESS_LUMINANCE),
		log(ROD_DARKNESS_LUMINANCE),
		rod_progress
	))


func _highlight_protection_floor() -> float:
	var up := observer.up_dir()
	var view := observer.view_dir().normalized()
	var sun_dir := Frames.helion_dir.normalized()
	var planet_importance := _sunlit_planet_importance(up, view, sun_dir)

	# A saturating curve is deliberately used instead of an area average: ten
	# percent of bright imagery receives more than half of the available weight.
	# This is the behavior needed for a bright limb against a large black sky.
	var priority := 1.0 - exp(-HIGHLIGHT_PRIORITY_GAIN * planet_importance)
	var floor := lerpf(ROD_DARKNESS_LUMINANCE, PLANET_HIGHLIGHT_FLOOR, priority)

	# The solar disc covers very few pixels but is the strongest possible visual
	# adaptation cue. Give it an independent protective ceiling when unoccluded.
	var sun_alignment := view.dot(sun_dir)
	var solar_view := _smoothstep(cos(0.035), cos(0.006), sun_alignment)
	if _sun_is_visible(up, sun_dir):
		floor = maxf(floor, lerpf(ROD_DARKNESS_LUMINANCE,
			DIRECT_SUN_HIGHLIGHT_FLOOR, solar_view))
	return floor


func _sunlit_planet_importance(up: Vector3, view: Vector3, sun_dir: Vector3) -> float:
	if observer.camera == null or Planet.cfg == null:
		return 0.0

	var planet_radius: float = Planet.cfg.planet_radius
	var observer_radius := planet_radius + maxf(observer.altitude(), 0.01)
	var angular_radius := asin(clampf(planet_radius / observer_radius, 0.0, 1.0))
	var vertical_half_fov := deg_to_rad(observer.camera.fov) * 0.5
	var viewport_size := get_viewport().get_visible_rect().size
	var aspect := viewport_size.x / maxf(viewport_size.y, 1.0)
	var horizontal_half_fov := atan(tan(vertical_half_fov) * aspect)

	# Approximate the projected disc area and its overlap with the camera frame.
	# The overlap has a soft edge so protection never pops while orbiting/panning.
	var frame_radius := sqrt(vertical_half_fov * vertical_half_fov \
		+ horizontal_half_fov * horizontal_half_fov)
	var center_angle := acos(clampf(view.dot(-up), -1.0, 1.0))
	var inner_edge := maxf(frame_radius - angular_radius, 0.0)
	var outer_edge := frame_radius + angular_radius
	var frame_overlap := 1.0 - _smoothstep(inner_edge, outer_edge, center_angle)
	var frame_area := maxf(4.0 * vertical_half_fov * horizontal_half_fov, 1.0e-6)
	var disc_fraction := clampf(PI * angular_radius * angular_radius / frame_area, 0.0, 1.0)

	# Square-root weighting keeps a narrow crescent important: its small area is
	# still made of day-side radiance and should not be sacrificed to black space.
	var illuminated_fraction := clampf(0.5 * (1.0 + up.dot(sun_dir)), 0.0, 1.0)
	return clampf(disc_fraction * frame_overlap * sqrt(illuminated_fraction), 0.0, 1.0)


func _sun_is_visible(up: Vector3, sun_dir: Vector3) -> bool:
	# Ray/sphere occultation in canonical planet space. This avoids treating a
	# below-horizon sun as a highlight while still handling orbital views where
	# the observer can see both the dark side and the solar disc.
	var planet_radius: float = Planet.cfg.planet_radius
	var observer_radius := planet_radius + maxf(observer.altitude(), 0.01)
	var ray_origin := up * observer_radius
	var b := ray_origin.dot(sun_dir)
	var c := observer_radius * observer_radius - planet_radius * planet_radius
	var discriminant := b * b - c
	if discriminant <= 0.0:
		return true
	var nearest_hit := -b - sqrt(discriminant)
	return nearest_hit <= 0.0


func _metered_luminance_proxy(highlight_floor: float) -> float:
	# Godot meters the real rendered HDR frame. This analytic proxy is used only
	# to choose the correct directional time constant, because the renderer offers
	# one speed rather than separate light-to-dark and dark-to-light rates.
	var up := observer.up_dir()
	var view := observer.view_dir().normalized()
	var sun_dir := Frames.helion_dir.normalized()
	var solar_height := up.dot(sun_dir)
	var daylight := _smoothstep(-0.08, 0.20, solar_height)
	var twilight := _smoothstep(-0.22, -0.015, solar_height) \
		* (1.0 - _smoothstep(-0.015, 0.16, solar_height))
	var sky_fraction := _smoothstep(-0.20, 0.45, view.dot(up))
	var scene_luminance := ROD_DARKNESS_LUMINANCE
	scene_luminance += daylight * lerpf(0.12, 0.42, sky_fraction)
	scene_luminance += twilight * 0.025

	# Highlight protection also selects fast light adaptation. The actual exposure
	# is still computed from the rendered HDR frame; this proxy only selects speed.
	scene_luminance = maxf(scene_luminance, highlight_floor)
	return clampf(scene_luminance, ROD_DARKNESS_LUMINANCE, HIGHLIGHT_CLIP_LUMINANCE)


func _to_sensitivity(luminance: float) -> float:
	return luminance * SENSITIVITY_PER_LUMINANCE


func _smoothstep(edge0: float, edge1: float, value: float) -> float:
	var t := clampf((value - edge0) / maxf(edge1 - edge0, 1.0e-6), 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)
