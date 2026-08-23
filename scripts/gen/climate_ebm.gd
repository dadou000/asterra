class_name ClimateEBM
extends RefCounted
## Zonally averaged energy-balance climate model (Budyko / Sellers / North).
##
## This is where Asterra's temperatures come from. Nothing here is an authored
## equator-to-pole curve: the model closes an actual energy budget,
##
##     d/dx [ D (1 - x^2) dT/dx ]  -  (A + B T)  +  S(x) (1 - alpha(x, T))  =  0
##
## with x = sin(latitude), so absorbed sunlight, thermal emission to space and
## meridional heat transport have to balance at every latitude. The ice-albedo
## feedback is inside the loop, which means the ice caps find their own latitude
## instead of being dialled in -- cool the planet and they grow on their own.
##
## Insolation S(x) is integrated from the orbit rather than approximated by
## sin^2(latitude), so `axial_tilt_deg` is the thing that actually sets the
## profile, and the same integration yields the annual harmonic that drives the
## seasonal range.
##
## The linear OLR law A + B*T is the standard shortcut that lets an energy
## balance work without a radiative-transfer column: A and B are Earth-calibrated
## and carry the greenhouse effect implicitly. `greenhouse_offset` shifts A, and
## is the physically meaningful "make this planet warmer" knob.

## Equal-area latitude bands (uniform in x = sin(lat)).
const BANDS := 180
## Samples per orbit for the insolation integral.
const ORBIT_STEPS := 256
## Land elevation histogram resolution, so high ground can ice over while the
## lowland in the same band does not.
const ELEV_BUCKETS := 10
const ELEV_BUCKET_M := 600.0

const MAX_OUTER := 300
const TOL := 5e-5
## Under-relaxation on the albedo. The ice-albedo feedback is the one genuinely
## unstable part of the system -- it is what produces the snowball bifurcation --
## so the outer loop is damped rather than allowed to flip-flop.
const RELAX := 0.30

var cfg: GenConfig

# --- band geometry ---
var x_center := PackedFloat32Array()      ## sin(latitude) at band centre
var x_edge := PackedFloat32Array()        ## BANDS + 1 band edges

# --- insolation, W/m^2 ---
var insol_mean := PackedFloat32Array()    ## annual mean top-of-atmosphere
var insol_amp := PackedFloat32Array()     ## amplitude of the annual harmonic

# --- geography per band, area weighted ---
var ocean_frac := PackedFloat32Array()
var land_frac := PackedFloat32Array()
var land_elev_hist := PackedFloat32Array()  ## BANDS * ELEV_BUCKETS, fraction of band area
var band_weight := PackedFloat32Array()     ## raw area weight, 0 if the band held no cells

# --- solution ---
var temp := PackedFloat32Array()          ## deg C, sea-level reference
var albedo := PackedFloat32Array()        ## planetary albedo actually used
var ice_frac := PackedFloat32Array()      ## 0..1 area under permanent ice
var transport := PackedFloat32Array()     ## W/m^2 convergence of meridional heat flux
var iterations := 0
var converged := false

func _init(p_cfg: GenConfig) -> void:
	cfg = p_cfg
	_build_bands()
	_build_insolation()

# ------------------------------------------------------------------ bands ---
func _build_bands() -> void:
	x_center.resize(BANDS)
	x_edge.resize(BANDS + 1)
	var dx := 2.0 / float(BANDS)
	for b in BANDS + 1:
		x_edge[b] = -1.0 + dx * float(b)
	for b in BANDS:
		x_center[b] = -1.0 + dx * (float(b) + 0.5)
	for f in ["insol_mean", "insol_amp", "ocean_frac", "land_frac", "band_weight",
			"temp", "albedo", "ice_frac", "transport"]:
		var a := PackedFloat32Array()
		a.resize(BANDS)
		set(f, a)
	land_elev_hist.resize(BANDS * ELEV_BUCKETS)

## Band index for a signed sin(latitude). Static: callers often need to bin
## cells before there is a solved model to bin them against.
static func band_of(x: float) -> int:
	return clampi(int((clampf(x, -1.0, 1.0) + 1.0) * 0.5 * float(BANDS)), 0, BANDS - 1)

# ------------------------------------------------------------- insolation ---
## Daily-mean insolation at the top of the atmosphere, W/m^2. The orbit is taken
## as circular: eccentricity is a precession-timescale detail that averages out
## of the climate this model describes.
static func daily_insolation(s0: float, sin_lat: float, sin_decl: float) -> float:
	var cos_lat := sqrt(maxf(0.0, 1.0 - sin_lat * sin_lat))
	var cos_decl := sqrt(maxf(0.0, 1.0 - sin_decl * sin_decl))
	var h0: float
	if cos_lat < 1e-7:
		# Exactly at the pole: polar day or polar night, nothing in between.
		h0 = PI if sin_lat * sin_decl > 0.0 else 0.0
	else:
		var c := -(sin_lat * sin_decl) / (cos_lat * cos_decl)
		if c <= -1.0:
			h0 = PI          # the sun never sets
		elif c >= 1.0:
			h0 = 0.0         # the sun never rises
		else:
			h0 = acos(c)
	return (s0 / PI) * (h0 * sin_lat * sin_decl + cos_lat * cos_decl * sin(h0))

func _build_insolation() -> void:
	var s0: float = cfg.solar_constant
	var sin_obl := sin(deg_to_rad(cfg.axial_tilt_deg))
	for b in BANDS:
		var x: float = x_center[b]
		var mean := 0.0
		var harm := 0.0
		for m in ORBIT_STEPS:
			# lam = 0 at the northern vernal equinox, pi/2 at northern midsummer.
			var lam := TAU * (float(m) + 0.5) / float(ORBIT_STEPS)
			var q := daily_insolation(s0, x, sin_obl * sin(lam))
			mean += q
			harm += q * sin(lam)
		insol_mean[b] = mean / float(ORBIT_STEPS)
		# Projection onto the annual harmonic. It comes out negative in the
		# southern hemisphere -- its summer is the north's winter -- and only the
		# magnitude matters for a seasonal range.
		insol_amp[b] = absf(2.0 * harm / float(ORBIT_STEPS))

# -------------------------------------------------------------- geography ---
## Bin the real surface into bands: what fraction is ocean, and how the land is
## distributed in elevation. Geography enters the energy balance through albedo,
## which is why a band full of continent solves differently from a band of sea.
func bind_geography(fields: PlanetFields) -> void:
	var grid := fields.grid
	for i in land_elev_hist.size():
		land_elev_hist[i] = 0.0
	for b in BANDS:
		ocean_frac[b] = 0.0
		land_frac[b] = 0.0
		band_weight[b] = 0.0
	for c in grid.cell_count:
		var cs: float = grid.cell_size[c]
		var w := cs * cs
		var b := band_of(grid.cell_dir(c).y)
		band_weight[b] += w
		if fields.elev[c] < 0.0:
			ocean_frac[b] += w
		else:
			land_frac[b] += w
			var k := clampi(int(fields.elev[c] / ELEV_BUCKET_M), 0, ELEV_BUCKETS - 1)
			land_elev_hist[b * ELEV_BUCKETS + k] += w
	for b in BANDS:
		var w: float = band_weight[b]
		if w <= 0.0:
			# Empty band, only reachable on a very coarse grid. Treat it as open
			# ocean so the solve stays well posed.
			ocean_frac[b] = 1.0
			land_frac[b] = 0.0
			continue
		ocean_frac[b] /= w
		land_frac[b] /= w
		for k in ELEV_BUCKETS:
			land_elev_hist[b * ELEV_BUCKETS + k] /= w

# ------------------------------------------------------------------ solve ---
## Fraction of a surface under permanent ice at temperature `t`.
func ice_at(t: float) -> float:
	return 1.0 - NoiseKit.smoothstepf(cfg.ice_full_temp, cfg.ice_onset_temp, t)

## Area-weighted planetary albedo of a band whose sea-level temperature is `t`,
## returned as [albedo, ice fraction].
func _band_albedo(b: int, t: float) -> Array:
	var a_ocean: float = cfg.albedo_ocean
	var a_land: float = cfg.albedo_land
	var a_ice: float = cfg.albedo_ice
	var lapse: float = cfg.lapse_rate

	var ice_o := ice_at(t)
	var alpha: float = ocean_frac[b] * lerpf(a_ocean, a_ice, ice_o)
	var ice_total: float = ocean_frac[b] * ice_o
	var base := b * ELEV_BUCKETS
	for k in ELEV_BUCKETS:
		var frac: float = land_elev_hist[base + k]
		if frac <= 0.0:
			continue
		var h := (float(k) + 0.5) * ELEV_BUCKET_M
		var ice_l := ice_at(t - h * lapse)
		alpha += frac * lerpf(a_land, a_ice, ice_l)
		ice_total += frac * ice_l
	return [alpha, ice_total]

## Steady-state solve. The diffusion operator is linear and tridiagonal, so each
## inner solve is exact (Thomas); only the albedo has to be iterated.
func solve() -> void:
	var a_olr: float = cfg.olr_intercept - cfg.greenhouse_offset
	var b_olr: float = maxf(0.05, cfg.olr_slope)
	var d: float = maxf(0.0, cfg.heat_diffusion)
	var dx := 2.0 / float(BANDS)
	var inv_dx2 := 1.0 / (dx * dx)

	# Face conductances. (1 - x^2) vanishes at the poles, so the no-flux boundary
	# condition is built into the operator rather than imposed on top of it.
	var k_face := PackedFloat32Array()
	k_face.resize(BANDS + 1)
	for b in BANDS + 1:
		var xe: float = x_edge[b]
		k_face[b] = d * (1.0 - xe * xe) * inv_dx2

	# Start from a plausible profile so the feedback has something to bite on.
	for b in BANDS:
		temp[b] = 30.0 - 55.0 * absf(x_center[b])

	var lower := PackedFloat32Array(); lower.resize(BANDS)
	var diag := PackedFloat32Array(); diag.resize(BANDS)
	var upper := PackedFloat32Array(); upper.resize(BANDS)
	var rhs := PackedFloat32Array(); rhs.resize(BANDS)
	var alpha_prev := PackedFloat32Array(); alpha_prev.resize(BANDS)
	for b in BANDS:
		alpha_prev[b] = cfg.albedo_land

	converged = false
	iterations = 0
	for it in MAX_OUTER:
		iterations = it + 1
		for b in BANDS:
			var res := _band_albedo(b, temp[b])
			var a: float = lerpf(alpha_prev[b], res[0], RELAX)
			alpha_prev[b] = a
			albedo[b] = a
			ice_frac[b] = res[1]

			var kl: float = k_face[b]
			var ku: float = k_face[b + 1]
			lower[b] = -kl
			upper[b] = -ku
			diag[b] = kl + ku + b_olr
			rhs[b] = insol_mean[b] * (1.0 - a) - a_olr

		var t_new := _thomas(lower, diag, upper, rhs)
		var worst := 0.0
		for b in BANDS:
			worst = maxf(worst, absf(t_new[b] - temp[b]))
			temp[b] = t_new[b]
		if worst < TOL:
			converged = true
			break

	# Convergence of the meridional heat flux, W/m^2. Positive where the
	# circulation delivers heat, negative where it exports it.
	for b in BANDS:
		var t_lo: float = temp[b - 1] if b > 0 else temp[b]
		var t_hi: float = temp[b + 1] if b < BANDS - 1 else temp[b]
		transport[b] = k_face[b] * (t_lo - temp[b]) + k_face[b + 1] * (t_hi - temp[b])

## Tridiagonal solve (Thomas). The matrix is diagonally dominant by
## construction, so no pivoting is needed.
func _thomas(lower: PackedFloat32Array, diag: PackedFloat32Array,
		upper: PackedFloat32Array, rhs: PackedFloat32Array) -> PackedFloat32Array:
	var n := diag.size()
	var cp := PackedFloat32Array(); cp.resize(n)
	var dp := PackedFloat32Array(); dp.resize(n)
	var beta: float = diag[0]
	cp[0] = upper[0] / beta
	dp[0] = rhs[0] / beta
	for i in range(1, n):
		beta = diag[i] - lower[i] * cp[i - 1]
		cp[i] = upper[i] / beta
		dp[i] = (rhs[i] - lower[i] * dp[i - 1]) / beta
	var out := PackedFloat32Array(); out.resize(n)
	out[n - 1] = dp[n - 1]
	for i in range(n - 2, -1, -1):
		out[i] = dp[i] - cp[i] * out[i + 1]
	return out

# ----------------------------------------------------------- interpolation ---
func _lerp_band(field: PackedFloat32Array, x: float) -> float:
	var f := (clampf(x, -1.0, 1.0) + 1.0) * 0.5 * float(BANDS) - 0.5
	var i := int(floor(f))
	var i0 := clampi(i, 0, BANDS - 1)
	var i1 := clampi(i + 1, 0, BANDS - 1)
	return lerpf(field[i0], field[i1], clampf(f - float(i), 0.0, 1.0))

## Zonal-mean sea-level temperature, deg C.
func temp_at(x: float) -> float:
	return _lerp_band(temp, x)

## Annual-mean insolation, W/m^2.
func insol_at(x: float) -> float:
	return _lerp_band(insol_mean, x)

## Amplitude of the annual insolation harmonic, W/m^2.
func insol_amp_at(x: float) -> float:
	return _lerp_band(insol_amp, x)

## Zonal-mean planetary albedo.
func albedo_at(x: float) -> float:
	return _lerp_band(albedo, x)

## Zonal-mean convergence of the meridional heat flux, W/m^2.
func transport_at(x: float) -> float:
	return _lerp_band(transport, x)

# -------------------------------------------------------------- diagnostic ---
## Bands are equal area by construction, so the global mean is a plain mean.
func global_mean_temp() -> float:
	var s := 0.0
	for b in BANDS:
		s += temp[b]
	return s / float(BANDS)

func global_ice_fraction() -> float:
	var s := 0.0
	for b in BANDS:
		s += ice_frac[b]
	return s / float(BANDS)

## Insolation-weighted planetary albedo -- the number a distant observer measures.
func global_albedo() -> float:
	var num := 0.0
	var den := 0.0
	for b in BANDS:
		num += insol_mean[b] * albedo[b]
		den += insol_mean[b]
	return num / maxf(1e-6, den)
