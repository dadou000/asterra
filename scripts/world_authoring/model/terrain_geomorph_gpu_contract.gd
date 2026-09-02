extends RefCounted
## Shared normalized production-geomorph contract for render and cache synthesis.
##
## The cached compute path and the visible analytic shader must never own separate
## defaults/clamps. This file is the single CPU-side packing contract for the exact
## production controls. The 12 vec4 layout is std430-safe and intentionally leaves
## spatial-mask state for a later contract version without changing existing keys.

const GRAPH_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const GEOMORPH_SETTINGS := "PRODUCTION_GEOMORPH_SETTINGS"
const CONTRACT_VERSION: int = 1
const VEC4_COUNT: int = 12
const BYTE_SIZE: int = VEC4_COUNT * 16

# Order is ABI. Keep shader binding comments synchronized with this table.
const PACKED_KEYS: Array[String] = [
	"detail_strength", "warp_strength", "broad_strength", "mountain_strength",
	"mid_strength", "channel_strength", "deposit_strength", "fine_strength",
	"dune_strength", "glacial_strength", "broad_wavelength_m", "broad_low_amplitude_m",
	"broad_mountain_amplitude_m", "broad_warp", "mountain_wavelength_m", "mountain_amplitude_m",
	"mountain_warp", "mountain_ridge_scale", "mountain_cell_mix", "mid_wavelength_m",
	"mid_ridge_amplitude_m", "mid_noise_amplitude_m", "mid_warp", "mid_ridge_scale",
	"mid_detail_scale", "channel_wavelength_m", "channel_depth_min_m", "channel_depth_max_m",
	"channel_warp", "channel_power", "flow_along_scale", "flow_across_scale",
	"deposit_amplitude_min_m", "deposit_amplitude_max_m", "deposit_scale", "deposit_power",
	"fine_wavelength_m", "fine_amplitude_m", "dune_wavelength_m", "dune_amplitude_m",
	"dune_warp", "micro_wavelength_m", "micro_amplitude_m", "glacial_wavelength_m",
	"glacial_amplitude_m", "glacial_base_scale", "glacial_mix", "reserved_0",
]


static func normalized_controls(source: Dictionary) -> Dictionary:
	var defaults: Dictionary = GRAPH_SCRIPT.production_control_defaults(GEOMORPH_SETTINGS)
	var merged: Dictionary = defaults.duplicate(true)
	for key_value: Variant in source.keys():
		merged[key_value] = source[key_value]

	var out: Dictionary = {}
	out["detail_strength"] = _nonnegative(merged, "detail_strength", 1.0)
	out["warp_strength"] = _nonnegative(merged, "warp_strength", 1.0)
	out["broad_strength"] = _nonnegative(merged, "broad_strength", 1.0)
	out["mountain_strength"] = _nonnegative(merged, "mountain_strength", 1.0)
	out["mid_strength"] = _nonnegative(merged, "mid_strength", 1.0)
	out["channel_strength"] = _nonnegative(merged, "channel_strength", 1.0)
	out["deposit_strength"] = _nonnegative(merged, "deposit_strength", 1.0)
	out["fine_strength"] = _nonnegative(merged, "fine_strength", 1.0)
	out["dune_strength"] = _nonnegative(merged, "dune_strength", 1.0)
	out["glacial_strength"] = _nonnegative(merged, "glacial_strength", 1.0)

	out["broad_wavelength_m"] = _positive(merged, "broad_wavelength_m", 16000.0, 0.001)
	out["broad_low_amplitude_m"] = _nonnegative(merged, "broad_low_amplitude_m", 24.0)
	out["broad_mountain_amplitude_m"] = _nonnegative(merged, "broad_mountain_amplitude_m", 125.0)
	out["broad_warp"] = _nonnegative(merged, "broad_warp", 0.8)
	out["mountain_wavelength_m"] = _positive(merged, "mountain_wavelength_m", 6000.0, 0.001)
	out["mountain_amplitude_m"] = _nonnegative(merged, "mountain_amplitude_m", 210.0)
	out["mountain_warp"] = _nonnegative(merged, "mountain_warp", 1.1)
	out["mountain_ridge_scale"] = _positive(merged, "mountain_ridge_scale", 1.55, 0.001)
	out["mountain_cell_mix"] = clampf(float(merged.get("mountain_cell_mix", 0.58)), 0.0, 1.0)

	out["mid_wavelength_m"] = _positive(merged, "mid_wavelength_m", 1400.0, 0.001)
	out["mid_ridge_amplitude_m"] = _nonnegative(merged, "mid_ridge_amplitude_m", 72.0)
	out["mid_noise_amplitude_m"] = _nonnegative(merged, "mid_noise_amplitude_m", 24.0)
	out["mid_warp"] = _nonnegative(merged, "mid_warp", 0.72)
	out["mid_ridge_scale"] = _positive(merged, "mid_ridge_scale", 1.25, 0.001)
	out["mid_detail_scale"] = _positive(merged, "mid_detail_scale", 2.1, 0.001)

	out["channel_wavelength_m"] = _positive(merged, "channel_wavelength_m", 420.0, 0.001)
	out["channel_depth_min_m"] = _nonnegative(merged, "channel_depth_min_m", 2.0)
	out["channel_depth_max_m"] = _nonnegative(merged, "channel_depth_max_m", 34.0)
	out["channel_warp"] = _nonnegative(merged, "channel_warp", 0.55)
	out["channel_power"] = _positive(merged, "channel_power", 4.6, 0.01)
	out["flow_along_scale"] = _positive(merged, "flow_along_scale", 0.42, 0.001)
	out["flow_across_scale"] = _positive(merged, "flow_across_scale", 1.45, 0.001)
	out["deposit_amplitude_min_m"] = _nonnegative(merged, "deposit_amplitude_min_m", 1.0)
	out["deposit_amplitude_max_m"] = _nonnegative(merged, "deposit_amplitude_max_m", 12.0)
	out["deposit_scale"] = _positive(merged, "deposit_scale", 0.48, 0.001)
	out["deposit_power"] = _positive(merged, "deposit_power", 2.2, 0.01)

	out["fine_wavelength_m"] = _positive(merged, "fine_wavelength_m", 120.0, 0.001)
	out["fine_amplitude_m"] = _nonnegative(merged, "fine_amplitude_m", 4.5)
	out["dune_wavelength_m"] = _positive(merged, "dune_wavelength_m", 180.0, 0.001)
	out["dune_amplitude_m"] = _nonnegative(merged, "dune_amplitude_m", 9.0)
	out["dune_warp"] = _nonnegative(merged, "dune_warp", 0.45)
	out["micro_wavelength_m"] = _positive(merged, "micro_wavelength_m", 24.0, 0.001)
	out["micro_amplitude_m"] = _nonnegative(merged, "micro_amplitude_m", 0.9)
	out["glacial_wavelength_m"] = _positive(merged, "glacial_wavelength_m", 2600.0, 0.001)
	out["glacial_amplitude_m"] = _nonnegative(merged, "glacial_amplitude_m", 52.0)
	out["glacial_base_scale"] = _nonnegative(merged, "glacial_base_scale", 0.62)
	out["glacial_mix"] = _nonnegative(merged, "glacial_mix", 0.72)

	out["override_seed"] = bool(merged.get("override_seed", false))
	out["detail_seed"] = maxi(1, int(merged.get("detail_seed", 1337)))
	out["reserved_0"] = 0.0
	return out


static func pack_controls(source: Dictionary) -> PackedByteArray:
	var controls: Dictionary = normalized_controls(source)
	var bytes := PackedByteArray()
	bytes.resize(BYTE_SIZE)
	for index: int in PACKED_KEYS.size():
		bytes.encode_float(index * 4, float(controls.get(PACKED_KEYS[index], 0.0)))
	return bytes


static func effective_seed(source: Dictionary, fallback_seed: int) -> int:
	var controls: Dictionary = normalized_controls(source)
	if bool(controls.get("override_seed", false)):
		return maxi(1, int(controls.get("detail_seed", 1337))) & 0x00ffffff
	return maxi(1, fallback_seed & 0x00ffffff)


static func fingerprint(source: Dictionary, fallback_seed: int) -> String:
	var packed: PackedByteArray = pack_controls(source)
	var seed: int = effective_seed(source, fallback_seed)
	return (packed.hex_encode() + ":" + str(seed) + ":v" + str(CONTRACT_VERSION)).sha256_text()


static func packed_values(source: Dictionary) -> PackedFloat32Array:
	var controls: Dictionary = normalized_controls(source)
	var values := PackedFloat32Array()
	values.resize(PACKED_KEYS.size())
	for index: int in PACKED_KEYS.size():
		values[index] = float(controls.get(PACKED_KEYS[index], 0.0))
	return values


static func _nonnegative(source: Dictionary, key: String, fallback: float) -> float:
	return maxf(float(source.get(key, fallback)), 0.0)


static func _positive(source: Dictionary, key: String, fallback: float,
		minimum: float) -> float:
	return maxf(float(source.get(key, fallback)), minimum)
