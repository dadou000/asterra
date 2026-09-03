class_name TerrainProductionGeomorphSchema
extends RefCounted
## Canonical semantic description of the production GPU geomorph pipeline.
##
## This is deliberately a description of the EXISTING shader, not a replacement
## implementation. Structural graph migration is allowed to consume this schema only
## after the equivalence CI proves that the schema still matches gpu_geomorph and the
## serialized production control defaults.

const SCHEMA_VERSION: int = 1
const FBM_ABS_BOUND: float = 1.0

const GLOBAL_CONTROLS: PackedStringArray = [
	"detail_strength",
	"override_seed",
	"detail_seed",
	"warp_strength",
	"base_elevation_continental",
	"base_elevation_regional",
	"base_elevation_local",
]

const CONTROL_DEFAULTS: Dictionary = {
	"detail_strength": 1.0,
	"override_seed": false,
	"detail_seed": 1337,
	"warp_strength": 1.0,
	"broad_strength": 1.0,
	"broad_wavelength_m": 16000.0,
	"broad_low_amplitude_m": 24.0,
	"broad_mountain_amplitude_m": 125.0,
	"broad_warp": 0.8,
	"mountain_strength": 1.0,
	"mountain_wavelength_m": 6000.0,
	"mountain_amplitude_m": 210.0,
	"mountain_warp": 1.1,
	"mountain_ridge_scale": 1.55,
	"mountain_cell_mix": 0.58,
	"mid_strength": 1.0,
	"mid_wavelength_m": 1400.0,
	"mid_ridge_amplitude_m": 72.0,
	"mid_noise_amplitude_m": 24.0,
	"mid_warp": 0.72,
	"mid_ridge_scale": 1.25,
	"mid_detail_scale": 2.1,
	"channel_strength": 1.0,
	"channel_wavelength_m": 420.0,
	"channel_depth_min_m": 2.0,
	"channel_depth_max_m": 34.0,
	"channel_warp": 0.55,
	"channel_power": 4.6,
	"flow_along_scale": 0.42,
	"flow_across_scale": 1.45,
	"deposit_strength": 1.0,
	"deposit_amplitude_min_m": 1.0,
	"deposit_amplitude_max_m": 12.0,
	"deposit_scale": 0.48,
	"deposit_power": 2.2,
	"fine_strength": 1.0,
	"fine_wavelength_m": 120.0,
	"fine_amplitude_m": 4.5,
	"dune_strength": 1.0,
	"dune_wavelength_m": 180.0,
	"dune_amplitude_m": 9.0,
	"dune_warp": 0.45,
	"micro_wavelength_m": 24.0,
	"micro_amplitude_m": 0.9,
	"glacial_strength": 1.0,
	"glacial_wavelength_m": 2600.0,
	"glacial_amplitude_m": 52.0,
	"glacial_base_scale": 0.62,
	"glacial_mix": 0.72,
	"base_elevation_continental": 1.0,
	"base_elevation_regional": 1.0,
	"base_elevation_local": 1.0,
}

# The order here is part of the production contract. `parent_stage` describes a
# shader branch/sample dependency, not graph ownership: deposition shares the
# channel sample, dunes live in the fine-detail branch, and glacial is a terminal
# transform of the accumulated height rather than another additive layer.
const STAGES: Array[Dictionary] = [
	{
		"id": "broad",
		"title": "Broad Relief",
		"operation": "add_signed",
		"parent_stage": "",
		"parameters": ["broad_strength", "broad_wavelength_m", "broad_low_amplitude_m",
			"broad_mountain_amplitude_m", "broad_warp"],
		"dependencies": ["warp_strength"],
		"seed_offsets": [11, 13],
		"anchor": "float broad_wavelength = max(u_geomorph_broad_wavelength_m, 0.001);",
		"operation_anchor": "h += broad * mix(u_geomorph_broad_low_amplitude_m,",
	},
	{
		"id": "mountain",
		"title": "Mountains",
		"operation": "add_signed",
		"parent_stage": "",
		"parameters": ["mountain_strength", "mountain_wavelength_m", "mountain_amplitude_m",
			"mountain_warp", "mountain_ridge_scale", "mountain_cell_mix"],
		"dependencies": ["warp_strength"],
		"seed_offsets": [31, 37, 41],
		"anchor": "float mountain_wavelength = max(u_geomorph_mountain_wavelength_m, 0.001);",
		"operation_anchor": "h += (skeleton * 2.0 - 1.0) * u_geomorph_mountain_amplitude_m",
	},
	{
		"id": "mid",
		"title": "Mid Relief",
		"operation": "add_signed",
		"parent_stage": "",
		"parameters": ["mid_strength", "mid_wavelength_m", "mid_ridge_amplitude_m",
			"mid_noise_amplitude_m", "mid_warp", "mid_ridge_scale", "mid_detail_scale"],
		"dependencies": ["warp_strength"],
		"seed_offsets": [53, 59, 61],
		"anchor": "float mid_wavelength = max(u_geomorph_mid_wavelength_m, 0.001);",
		"operation_anchor": "h += ((ridge * 2.0 - 1.0) * u_geomorph_mid_ridge_amplitude_m * mountain",
	},
	{
		"id": "channel",
		"title": "Channels / Incision",
		"operation": "subtract_positive",
		"parent_stage": "",
		"parameters": ["channel_strength", "channel_wavelength_m", "channel_depth_min_m",
			"channel_depth_max_m", "channel_warp", "channel_power", "flow_along_scale",
			"flow_across_scale"],
		"dependencies": ["warp_strength"],
		"seed_offsets": [71, 73],
		"anchor": "float channel_wavelength = max(u_geomorph_channel_wavelength_m, 0.001);",
		"operation_anchor": "h -= incision * w420 * u_geomorph_channel_strength;",
	},
	{
		"id": "deposit",
		"title": "Deposition",
		"operation": "add_positive",
		"parent_stage": "channel",
		"parameters": ["deposit_strength", "deposit_amplitude_min_m",
			"deposit_amplitude_max_m", "deposit_scale", "deposit_power"],
		"dependencies": ["channel_wavelength_m"],
		"seed_offsets": [79],
		"anchor": "float depositional_gate = smoothstep(0.0, 0.03, depositional);",
		"operation_anchor": "h += fan * depositional * depositional_gate",
	},
	{
		"id": "fine",
		"title": "Fine Detail",
		"operation": "add_signed",
		"parent_stage": "",
		"parameters": ["fine_strength", "fine_wavelength_m", "fine_amplitude_m"],
		"dependencies": [],
		"seed_offsets": [89],
		"anchor": "float fine_wavelength = max(u_geomorph_fine_wavelength_m, 0.001);",
		"operation_anchor": "h += n * u_geomorph_fine_amplitude_m * (1.0 - glacial * 0.6)",
	},
	{
		"id": "dune",
		"title": "Dunes",
		"operation": "add_signed",
		"parent_stage": "fine",
		"parameters": ["dune_strength", "dune_wavelength_m", "dune_amplitude_m", "dune_warp"],
		"dependencies": ["warp_strength"],
		"seed_offsets": [97, 101],
		"anchor": "float dune_weight = arid * soil.r;",
		"operation_anchor": "h += (dune * 2.0 - 1.0) * u_geomorph_dune_amplitude_m * dune_weight",
	},
	{
		"id": "micro",
		"title": "Micro Relief",
		"operation": "add_signed",
		"parent_stage": "",
		"parameters": ["micro_wavelength_m", "micro_amplitude_m"],
		"dependencies": ["fine_strength"],
		"seed_offsets": [109],
		"anchor": "float micro_wavelength = max(u_geomorph_micro_wavelength_m, 0.001);",
		"operation_anchor": "* u_geomorph_micro_amplitude_m * w24 * u_geomorph_fine_strength;",
	},
	{
		"id": "glacial",
		"title": "Glacial Shaping",
		"operation": "mix_accumulated",
		"parent_stage": "",
		"parameters": ["glacial_strength", "glacial_wavelength_m", "glacial_amplitude_m",
			"glacial_base_scale", "glacial_mix"],
		"dependencies": [],
		"seed_offsets": [127],
		"anchor": "float glacial_gate = smoothstep(0.0, 0.05, glacial);",
		"operation_anchor": "h = mix(h, h * u_geomorph_glacial_base_scale",
	},
]


static func control_defaults() -> Dictionary:
	return CONTROL_DEFAULTS.duplicate(true)


static func stage_specs() -> Array[Dictionary]:
	return STAGES.duplicate(true)


static func ordered_stage_ids() -> PackedStringArray:
	var out := PackedStringArray()
	for stage: Dictionary in STAGES:
		out.append(String(stage.get("id", "")))
	return out


static func parameter_owner_map() -> Dictionary:
	var owners: Dictionary = {}
	for key: String in GLOBAL_CONTROLS:
		owners[key] = "global"
	for stage: Dictionary in STAGES:
		var stage_id: String = String(stage.get("id", ""))
		for key_value: Variant in stage.get("parameters", []) as Array:
			owners[String(key_value)] = stage_id
	return owners


const CACHED_SURFACE_CONTROLS: PackedStringArray = [
	"detail_seed", "detail_strength",
	"base_elevation_continental", "base_elevation_regional", "base_elevation_local",
]


static func uniform_for_control(key: String) -> String:
	match key:
		"override_seed": return ""
		"detail_seed": return "u_detail_seed"
		"detail_strength": return "u_detail_strength"
		"base_elevation_continental", "base_elevation_regional", "base_elevation_local":
			return "u_" + key
		_: return "u_geomorph_" + key


static func shader_source_for_control(key: String) -> String:
	return "cached_surface" if key in CACHED_SURFACE_CONTROLS else "geomorph"


static func validate_schema() -> PackedStringArray:
	var errors := PackedStringArray()
	var ids: Dictionary = {}
	var owners: Dictionary = {}
	for key: String in GLOBAL_CONTROLS:
		if owners.has(key):
			errors.append("duplicate global control: %s" % key)
		owners[key] = "global"
	for index: int in STAGES.size():
		var stage: Dictionary = STAGES[index]
		var stage_id: String = String(stage.get("id", ""))
		if stage_id.is_empty():
			errors.append("stage %d has no id" % index)
			continue
		if ids.has(stage_id):
			errors.append("duplicate stage id: %s" % stage_id)
		ids[stage_id] = true
		if String(stage.get("operation", "")).is_empty():
			errors.append("stage %s has no operation" % stage_id)
		if String(stage.get("anchor", "")).is_empty() or String(stage.get("operation_anchor", "")).is_empty():
			errors.append("stage %s has no shader anchors" % stage_id)
		for key_value: Variant in stage.get("parameters", []) as Array:
			var key: String = String(key_value)
			if owners.has(key):
				errors.append("control %s owned by both %s and %s" % [key, owners[key], stage_id])
			owners[key] = stage_id

	for stage: Dictionary in STAGES:
		var parent: String = String(stage.get("parent_stage", ""))
		if not parent.is_empty() and not ids.has(parent):
			errors.append("stage %s references missing parent %s" % [stage.get("id", ""), parent])
		for dependency_value: Variant in stage.get("dependencies", []) as Array:
			var dependency: String = String(dependency_value)
			if not CONTROL_DEFAULTS.has(dependency):
				errors.append("stage %s references missing dependency %s" % [stage.get("id", ""), dependency])

	for key_value: Variant in CONTROL_DEFAULTS.keys():
		var key: String = String(key_value)
		if not owners.has(key):
			errors.append("control %s has no semantic owner" % key)
	for key_value: Variant in owners.keys():
		var key: String = String(key_value)
		if not CONTROL_DEFAULTS.has(key):
			errors.append("semantic owner references unknown control %s" % key)
	return errors


static func production_guard_m(controls: Dictionary) -> float:
	# Conservative bound derived from the current gpu_geomorph equations. Keep this
	# formula intentionally simple and monotonic: it is a visibility guarantee, not
	# an estimate of typical terrain amplitude.
	var c: Dictionary = CONTROL_DEFAULTS.duplicate(true)
	for key_value: Variant in controls.keys():
		c[key_value] = controls[key_value]
	var broad_amp: float = maxf(
		maxf(float(c.get("broad_low_amplitude_m", 24.0)), 0.0),
		maxf(float(c.get("broad_mountain_amplitude_m", 125.0)), 0.0))
	var broad: float = broad_amp * maxf(float(c.get("broad_strength", 1.0)), 0.0) * FBM_ABS_BOUND
	var mountain: float = maxf(float(c.get("mountain_amplitude_m", 210.0)), 0.0) \
		* maxf(float(c.get("mountain_strength", 1.0)), 0.0) * 1.20
	var mid: float = (
		maxf(float(c.get("mid_ridge_amplitude_m", 72.0)), 0.0)
		+ maxf(float(c.get("mid_noise_amplitude_m", 24.0)), 0.0) * FBM_ABS_BOUND
	) * maxf(float(c.get("mid_strength", 1.0)), 0.0)
	var channels: float = maxf(float(c.get("channel_depth_max_m", 34.0)), 0.0) \
		* maxf(float(c.get("channel_strength", 1.0)), 0.0)
	var deposit: float = maxf(float(c.get("deposit_amplitude_max_m", 12.0)), 0.0) \
		* maxf(float(c.get("deposit_strength", 1.0)), 0.0)
	var fine_strength: float = maxf(float(c.get("fine_strength", 1.0)), 0.0)
	var fine: float = maxf(float(c.get("fine_amplitude_m", 4.5)), 0.0) * fine_strength * FBM_ABS_BOUND
	var dune: float = maxf(float(c.get("dune_amplitude_m", 9.0)), 0.0) \
		* maxf(float(c.get("dune_strength", 1.0)), 0.0)
	var micro: float = maxf(float(c.get("micro_amplitude_m", 0.9)), 0.0) * fine_strength
	var pre_glacial: float = broad + mountain + mid + channels + deposit + fine + dune + micro
	var glacial_strength: float = maxf(float(c.get("glacial_strength", 1.0)), 0.0)
	var glacial_base_scale: float = maxf(float(c.get("glacial_base_scale", 0.62)), 0.0)
	var glacial_extra: float = maxf(float(c.get("glacial_amplitude_m", 52.0)), 0.0) \
		* glacial_strength * FBM_ABS_BOUND
	var after_glacial: float = maxf(pre_glacial,
		pre_glacial * glacial_base_scale + glacial_extra)
	return after_glacial * maxf(float(c.get("detail_strength", 1.0)), 0.0)
