class_name GasGiantModel
extends RefCounted
## Coarse analytic vertical structure for a gas giant, deterministic from a seed.
##
## Not real Jovian physics -- a feel-tuned model that gives every gas giant a
## consistent set of reference radii and a monotonic density / pressure / temp
## profile through its outer envelope:
##
##   cloud_top_radius_m   the visible 1-bar-ish cloud deck == CelestialBody.radius_m
##                        (orbits, far-LOD, camera framing keep using this)
##   one_bar_radius_m     P = 1 bar
##   deadly_radius_m      envelope too dense/hot to survive (gameplay warning)
##   core_radius_m        rho == liquid-water density (1000 kg/m^3): the SOLID
##                        surface the terrain bake / collision / altitude use
##
## The envelope between core_radius_m and cloud_top_radius_m is the "shell" the
## volumetric renderer (GG2/GG3) fills and the drag/buoyancy model (GG4) reads.

## Liquid-water density -- the agreed datum for "this is now a surface".
const WATER_DENSITY := 1000.0
const GAS_CONSTANT := 8.314462618              ## J/(mol*K)
const MEAN_MOLAR_MASS := 2.3e-3               ## kg/mol, H2/He envelope

var cloud_top_radius_m: float = 1.0
var one_bar_radius_m: float = 1.0
var deadly_radius_m: float = 1.0
var core_radius_m: float = 1.0

## Shell geometry / profile constants (derived once in _build).
var _shell_thickness_m: float = 1.0
var _rho_top: float = 0.18                    ## kg/m^3 at the cloud tops
var _t_ref: float = 125.0                     ## K, roughly isothermal for readouts
var _falloff_k: float = 1.0                   ## rho(r) = _rho_top * exp(_falloff_k * (cloud_top - r))
var _gm_m3_s2: float = 1.0


## Build from a body's cloud-top radius + standard gravitational parameter, jittered
## by `seed` so every gas giant in a system is distinct but stable across runs.
static func from_body(cloud_top_radius_m: float, gm_m3_s2: float, seed: int) -> GasGiantModel:
	var m := GasGiantModel.new()
	m._build(maxf(cloud_top_radius_m, 1000.0), maxf(gm_m3_s2, 1.0), seed)
	return m


func _build(cloud_top: float, gm: float, seed: int) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = seed
	cloud_top_radius_m = cloud_top
	_gm_m3_s2 = gm
	_t_ref = 110.0 + rng.randf() * 45.0                       ## 110..155 K
	_rho_top = 0.13 + rng.randf() * 0.10                      ## 0.13..0.23 kg/m^3
	# The solid core is a small terrestrial rock deep under a vast gas envelope
	# (the shell is 84..93 % of the radius, KSP-Jool-like). Far shallower than a
	# real Jupiter analogue's water-density level, but the model is feel-tuned and
	# a small core is both the right look and a sane size for the terrain clipmap.
	var shell_fraction: float = 0.84 + rng.randf() * 0.09     ## 84%..93% of the radius
	core_radius_m = cloud_top_radius_m * (1.0 - shell_fraction)
	_shell_thickness_m = maxf(cloud_top_radius_m - core_radius_m, 1.0)
	# Pick the falloff so density is exactly WATER_DENSITY at the core surface.
	_falloff_k = log(WATER_DENSITY / _rho_top) / _shell_thickness_m

	# P = rho * R * T / M ; treat T as ~_t_ref for the threshold solves.
	var rho_one_bar: float = 1.0e5 * MEAN_MOLAR_MASS / (GAS_CONSTANT * _t_ref)
	var rho_deadly: float = 60.0                              ## kg/m^3, feel-tuned
	one_bar_radius_m = _radius_for_density(rho_one_bar)
	deadly_radius_m = _radius_for_density(rho_deadly)


## Radius where the exponential envelope reaches `rho` (clamped to the shell).
func _radius_for_density(rho: float) -> float:
	if rho <= _rho_top:
		return cloud_top_radius_m
	var depth: float = log(rho / _rho_top) / _falloff_k
	return clampf(cloud_top_radius_m - depth, core_radius_m, cloud_top_radius_m)


## Mass density (kg/m^3) at radius `r`. Monotonic: rises from ~_rho_top at the
## cloud tops to WATER_DENSITY at the core surface, thin exponential decay above.
func density_at(r: float) -> float:
	if r >= cloud_top_radius_m:
		var h: float = maxf(scale_height_at(cloud_top_radius_m), 1.0)
		return _rho_top * exp(-(r - cloud_top_radius_m) / h)
	var rr: float = maxf(r, core_radius_m)
	return _rho_top * exp(_falloff_k * (cloud_top_radius_m - rr))


## Temperature (K): ~_t_ref at the cloud tops, warming inward through the shell.
func temperature_at(r: float) -> float:
	var rr: float = clampf(r, core_radius_m, cloud_top_radius_m)
	var depth_frac: float = (cloud_top_radius_m - rr) / _shell_thickness_m
	return _t_ref * (1.0 + 0.6 * depth_frac)


func pressure_at(r: float) -> float:
	return density_at(r) * GAS_CONSTANT * temperature_at(r) / MEAN_MOLAR_MASS


func gravity_at(r: float) -> float:
	return _gm_m3_s2 / maxf(r * r, 1.0)


func scale_height_at(r: float) -> float:
	return GAS_CONSTANT * temperature_at(r) / (MEAN_MOLAR_MASS * maxf(gravity_at(r), 1.0e-6))


## Shell thickness (m) between the solid core surface and the cloud tops.
func shell_thickness_m() -> float:
	return _shell_thickness_m


## Profile constants for a renderer that wants the analytic density directly
## (density_at(r) == rho_top * exp(falloff_k * (cloud_top_radius_m - r)) in-shell).
func rho_top() -> float:
	return _rho_top


func falloff_k() -> float:
	return _falloff_k


## The volumetric envelope as TWO ordered (outer -> inner) decks:
##   [0] "atmo"  : core -> just above the cloud tops. Cheap analytic march, green
##                 Rayleigh-ish tint; always closes the view (no seams / no stars),
##                 stays a visible glowing haze deep instead of going black.
##   [1] "cloud" : a bounded deck near the tops. Grey clouds; coverage from a
##                 procedural equirect texture (built in gas_giant_shell.gd) sampled
##                 with an animated flow -- crisp bands that swirl, one texture
##                 fetch per step instead of a stack of FBM octaves.
## Fields: outer_m/inner_m radial span; kind; steps; density_mul; band_count
## (feeds the coverage texture); deck_altitude/deck_width (shell-fraction centre +
## half-width of the cloud concentration); detail (erosion strength); flow (uv
## flow speed); rot_speed_mul.
func decks() -> Array:
	var rng := RandomNumberGenerator.new()
	rng.seed = int(_rho_top * 1.0e7) ^ int(cloud_top_radius_m)
	var top: float = cloud_top_radius_m
	var sh: float = _shell_thickness_m
	var bands: int = 5 + (rng.randi() % 5)              ## 5..9 bands
	var deck_alt: float = 0.87 + rng.randf() * 0.06     ## shell fraction of the deck
	var deck_w: float = 0.09 + rng.randf() * 0.06
	return [
		{
			"outer_m": top + sh * 0.04, "inner_m": core_radius_m,
			"kind": "atmo", "steps": 14, "density_mul": 1.0,
			"band_count": bands, "detail": 0.0,
			"deck_altitude": deck_alt, "deck_width": deck_w, "flow": 0.0,
			"rot_speed_mul": 1.0,
		},
		{
			"outer_m": top + sh * 0.03,
			"inner_m": maxf(top - sh * (deck_w * 3.2 + 0.05), core_radius_m),
			"kind": "cloud", "steps": 18, "density_mul": 1.0,
			"band_count": bands,
			"detail": 0.55 + rng.randf() * 0.30,
			"deck_altitude": deck_alt, "deck_width": deck_w,
			"flow": 0.02 + rng.randf() * 0.03,
			"rot_speed_mul": 1.0 + rng.randf() * 0.2,
		},
	]


## Deterministic seed for this body's cloud-coverage map (same for the volumetric
## shell and the far-LOD sphere so a descent never sees the pattern change).
func coverage_seed() -> int:
	return int(_rho_top * 1.0e7) ^ int(cloud_top_radius_m)


func band_hint() -> int:
	return 5 + (coverage_seed() & 3) + 1


## Procedural equirect cloud map shared by the volumetric shell (as coverage) and
## the far-LOD sphere (as the band pattern): irregular latitudinal bands +
## domain-warped swirls + storm ovals, deterministic from `seed_v`.
##   R = zone value      0 = dark belt .. 1 = bright zone
##   G = mid detail      (erosion / relief)
##   B = storm mask      bright ovals
static func build_coverage_image(seed_v: int, band_hint_v: int,
		w: int = 512, h: int = 256) -> Image:
	var img := Image.create(w, h, false, Image.FORMAT_RGBA8)
	var rng := RandomNumberGenerator.new()
	rng.seed = seed_v

	var warp := FastNoiseLite.new()
	warp.seed = seed_v ^ 0x11
	warp.noise_type = FastNoiseLite.TYPE_SIMPLEX
	warp.frequency = 0.9
	warp.fractal_octaves = 2
	var swirl := FastNoiseLite.new()
	swirl.seed = seed_v ^ 0x22
	swirl.noise_type = FastNoiseLite.TYPE_SIMPLEX
	swirl.frequency = 1.8
	swirl.fractal_octaves = 4
	var detail := FastNoiseLite.new()
	detail.seed = seed_v ^ 0x33
	detail.noise_type = FastNoiseLite.TYPE_SIMPLEX
	detail.frequency = 5.5
	detail.fractal_octaves = 3

	# A clean primary stripe rate + two smaller harmonics for irregular spacing;
	# NOT a sum of many random sinusoids (that lumps into blobs, not belts).
	var nb: int = clampi(band_hint_v, 5, 11)
	var f0: float = float(nb) * 2.2                      # ~22-40 thin belts across the disc
	var f1: float = f0 * (2.1 + rng.randf() * 0.7)
	var f2: float = f0 * (0.45 + rng.randf() * 0.2)
	var p0: float = rng.randf() * TAU
	var p1: float = rng.randf() * TAU
	var p2: float = rng.randf() * TAU
	# Bands stay HORIZONTAL and THIN. The displacement of the belt edge must be
	# small vs the band spacing (~1/f0) or the boundary folds into lobes -- so the
	# festoon look comes from a FINE wobble of the band VALUE near its zero
	# crossing, not a big latitude shift.
	var lat_wobble: float = 0.015 + rng.randf() * 0.02
	var festoon_amp: float = 0.07 + rng.randf() * 0.05
	# Soft threshold: belts and zones blend at their boundary (Jool bands are
	# gradients, not hard stripes).
	var edge_w: float = 0.10 + rng.randf() * 0.05
	var swirl2 := FastNoiseLite.new()
	swirl2.seed = seed_v ^ 0x44
	swirl2.noise_type = FastNoiseLite.TYPE_SIMPLEX
	swirl2.frequency = 1.6
	swirl2.fractal_octaves = 3

	for y in h:
		var lat: float = (float(y) / float(h - 1) - 0.5) * PI
		var slat: float = sin(lat)
		var clat: float = cos(lat)
		for x in w:
			var lon: float = float(x) / float(w) * TAU
			var p := Vector3(clat * cos(lon), slat, clat * sin(lon))
			var wv := Vector3(
				warp.get_noise_3d(p.x * 1.7 + 3.0, p.y * 1.7, p.z * 1.7),
				warp.get_noise_3d(p.x * 1.7, p.y * 1.7 + 5.0, p.z * 1.7),
				warp.get_noise_3d(p.x * 1.7, p.y * 1.7, p.z * 1.7 + 7.0)) * 0.4
			var wl: float = slat * (1.0 + wv.y * lat_wobble)
			var band: float = (0.52 * sin(wl * f0 * PI + p0)
				+ 0.33 * sin(wl * f1 * PI + p1)
				+ 0.15 * sin(wl * f2 * PI + p2))       # -1..1, several thin belt rates
			# Fine festoon wobble of the band value (finer than a band -> curly
			# edges, not folded lobes).
			var festoon: float = (swirl.get_noise_3d(p.x * 2.2 + wv.x, p.y * 2.2, p.z * 2.2 + wv.z)
				+ 0.5 * swirl2.get_noise_3d(p.x * 1.3, p.y * 1.3, p.z * 1.3)) * festoon_amp
			var band_v: float = 0.5 + 0.5 * band + festoon
			var cov: float = smoothstep(0.5 - edge_w, 0.5 + edge_w, band_v)
			var det: float = 0.5 + 0.5 * detail.get_noise_3d(
				p.x + wv.x * 0.5, p.y + wv.y * 0.5, p.z + wv.z * 0.5)
			var storm: float = clampf((swirl2.get_noise_3d(
				p.x * 0.9 + 31.0, p.y * 0.9 + 7.0, p.z * 0.9) - 0.5) * 3.5, 0.0, 1.0)
			img.set_pixel(x, y, Color(cov, det, storm, 1.0))
	return img


func describe() -> String:
	return "GasGiantModel(cloud_top=%.0f km, 1bar=%.0f km, deadly=%.0f km, core=%.0f km, shell=%.0f km)" % [
		cloud_top_radius_m / 1000.0, one_bar_radius_m / 1000.0,
		deadly_radius_m / 1000.0, core_radius_m / 1000.0, _shell_thickness_m / 1000.0]


## One-line HUD / status readout for an observer `r_from_center_m` from the body
## centre (GG5). Above the cloud tops it is a plain altitude tagged "(cloud tops)";
## inside the envelope it adds depth below the tops, height above the solid core,
## pressure and density from the analytic profile. Shared by the game HUD and the
## Planet Studio status line so both phrase a gas giant the same way.
func readout_at(r_from_center_m: float) -> String:
	var above_tops: float = r_from_center_m - cloud_top_radius_m
	if above_tops >= 0.0:
		return "alt %s (cloud tops)" % _fmt_len(above_tops)
	var above_core: float = maxf(r_from_center_m - core_radius_m, 0.0)
	var p_bar: float = pressure_at(r_from_center_m) / 1.0e5
	return "alt %s above core • depth %s • P %s bar • ρ %.1f kg/m³" % [
		_fmt_len(above_core), _fmt_len(-above_tops), _fmt_bar(p_bar),
		density_at(r_from_center_m)]


static func _fmt_len(m: float) -> String:
	if absf(m) >= 1000.0:
		return "%.1f km" % (m / 1000.0)
	return "%.0f m" % m


static func _fmt_bar(bar: float) -> String:
	if bar >= 100.0:
		return "%.0f" % bar
	if bar >= 1.0:
		return "%.1f" % bar
	return "%.3f" % bar
