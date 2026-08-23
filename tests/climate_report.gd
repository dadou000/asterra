extends Node
## Diagnostic for the energy-balance climate model.
##
## Two halves. The first checks the physics against Earth with Earth's numbers,
## which is the only external ground truth available: insolation at known
## latitudes, then the solved profile, global mean and planetary albedo. The
## second bakes Asterra and reports what the biome classifier actually receives.
##
## Run: godot --headless --path . res://tests/ClimateReport.tscn

const FAILSAFE_FRAMES := 6000
var _frames := 0

func _process(_dt: float) -> void:
	# Headless scenes that error out never reach quit(); never spin forever.
	_frames += 1
	if _frames > FAILSAFE_FRAMES:
		get_tree().quit(1)

func _ready() -> void:
	_insolation_check()
	_earth_check()
	_asterra_profile()
	_asterra_bake()
	get_tree().quit()

# ---------------------------------------------------------------------------
func _insolation_check() -> void:
	print("\n== insolation, integrated from the orbit (Earth: S0=1361, tilt=23.44) ==")
	var cfg := GenConfig.new()
	cfg.axial_tilt_deg = 23.44
	var e := ClimateEBM.new(cfg)
	print("  latitude   annual mean W/m^2   annual harmonic W/m^2   (published mean)")
	# Published annual-mean TOA insolation (Legendre expansion, S0/4 = 340.25).
	var refs := {0: 416.0, 30: 361.0, 45: 301.0, 60: 238.0, 90: 176.0}
	for lat in [0, 15, 30, 45, 60, 75, 90]:
		var x := sin(deg_to_rad(float(lat)))
		var ref: String = "%.0f" % refs[lat] if refs.has(lat) else "-"
		print("  %5d      %8.1f            %8.1f              %s"
				% [lat, e.insol_at(x), e.insol_amp_at(x), ref])
	# Solstice extremes, the two numbers a reader can check by hand.
	var sd := sin(deg_to_rad(23.44))
	print("  pole at summer solstice: %.0f W/m^2 (S0*sin(tilt) = %.0f)"
			% [ClimateEBM.daily_insolation(1361.0, 1.0, sd), 1361.0 * sd])
	print("  pole at winter solstice: %.0f W/m^2 (polar night)"
			% ClimateEBM.daily_insolation(1361.0, 1.0, -sd))
	# The whole sphere must intercept exactly a disc of sunlight: mean = S0/4.
	var tot := 0.0
	for b in ClimateEBM.BANDS:
		tot += e.insol_mean[b]
	print("  global mean %.2f W/m^2 (S0/4 = %.2f, error %.3f%%)"
			% [tot / ClimateEBM.BANDS, 1361.0 / 4.0,
			100.0 * absf(tot / ClimateEBM.BANDS - 340.25) / 340.25])

func _earth_check() -> void:
	print("\n== energy balance run with Earth's parameters ==")
	var cfg := GenConfig.new()
	cfg.axial_tilt_deg = 23.44
	var e := ClimateEBM.new(cfg)
	# Earth-like geography: ~71% ocean, land skewed to the north, mostly lowland.
	for b in ClimateEBM.BANDS:
		var x: float = e.x_center[b]
		var land: float = clampf(0.29 + 0.34 * x, 0.02, 0.72)
		e.ocean_frac[b] = 1.0 - land
		e.land_frac[b] = land
		e.band_weight[b] = 1.0
		for k in ClimateEBM.ELEV_BUCKETS:
			e.land_elev_hist[b * ClimateEBM.ELEV_BUCKETS + k] = 0.0
		e.land_elev_hist[b * ClimateEBM.ELEV_BUCKETS + 0] = land * 0.80
		e.land_elev_hist[b * ClimateEBM.ELEV_BUCKETS + 1] = land * 0.15
		e.land_elev_hist[b * ClimateEBM.ELEV_BUCKETS + 3] = land * 0.05
	e.solve()
	print("  converged=%s in %d iterations" % [e.converged, e.iterations])
	print("  lat    T(degC)   albedo   ice    transport W/m^2      (Earth zonal mean T)")
	var refs := {-60: 3.0, -30: 17.0, 0: 26.0, 30: 20.0, 60: 0.0, 80: -16.0}
	for lat in [-80, -60, -30, 0, 30, 60, 80]:
		var x := sin(deg_to_rad(float(lat)))
		var b := ClimateEBM.band_of(x)
		var ref: String = "%.0f" % refs[lat] if refs.has(lat) else "-"
		print("  %4d   %7.1f   %6.3f   %5.2f   %8.1f              %s"
				% [lat, e.temp_at(x), e.albedo[b], e.ice_frac[b], e.transport[b], ref])
	print("  global mean %.1f C   (Earth 14.0)" % e.global_mean_temp())
	print("  planetary albedo %.3f   (Earth 0.294)" % e.global_albedo())
	# Permanent ice only: ice sheets plus perennial sea ice, about 4% of Earth.
	# Seasonal snow reaches far wider, but an annual-mean model cannot see it.
	print("  permanent ice %.1f%% of the surface   (Earth ~4%%)"
			% (100.0 * e.global_ice_fraction()))

	print("
  -- calibration sweep: OLR intercept A and transport D, Earth geography --")
	print("     A      D    global   equator   60deg   80deg    ice%   albedo")
	print("     target        14.0      26.0     0.0   -16.0    ~4.0    0.294")
	for a_v in [203.3, 206.0, 209.0, 212.0]:
		for d_v in [0.30, 0.40, 0.50, 0.60]:
			var c3 := GenConfig.new()
			c3.axial_tilt_deg = 23.44
			c3.olr_intercept = a_v
			c3.heat_diffusion = d_v
			c3.ice_onset_temp = -4.0
			c3.ice_full_temp = -16.0
			var e3 := ClimateEBM.new(c3)
			e3.ocean_frac = e.ocean_frac.duplicate()
			e3.land_frac = e.land_frac.duplicate()
			e3.land_elev_hist = e.land_elev_hist.duplicate()
			e3.band_weight = e.band_weight.duplicate()
			e3.solve()
			print("  %6.1f  %4.2f  %7.1f  %8.1f  %6.1f  %6.1f  %6.1f   %6.3f"
					% [a_v, d_v, e3.global_mean_temp(), e3.temp_at(0.0),
					e3.temp_at(sin(deg_to_rad(60.0))), e3.temp_at(sin(deg_to_rad(80.0))),
					100.0 * e3.global_ice_fraction(), e3.global_albedo()])

	print("\n  -- the feedback is live: shift the greenhouse and the caps move --")
	for off in [-12.0, -6.0, 0.0, 6.0, 12.0]:
		var c2 := GenConfig.new()
		c2.axial_tilt_deg = 23.44
		c2.greenhouse_offset = off
		var e2 := ClimateEBM.new(c2)
		e2.ocean_frac = e.ocean_frac.duplicate()
		e2.land_frac = e.land_frac.duplicate()
		e2.land_elev_hist = e.land_elev_hist.duplicate()
		e2.band_weight = e.band_weight.duplicate()
		e2.solve()
		print("     greenhouse %+5.1f W/m^2 -> global %6.2f C, ice %5.1f%%, poles %6.1f C"
				% [off, e2.global_mean_temp(), 100.0 * e2.global_ice_fraction(),
				e2.temp_at(1.0)])

func _asterra_profile() -> void:
	print("\n== Asterra's own tilt (%.1f deg) against Earth's ==" % GenConfig.new().axial_tilt_deg)
	var a := ClimateEBM.new(GenConfig.new())
	var b := GenConfig.new()
	b.axial_tilt_deg = 23.44
	var e := ClimateEBM.new(b)
	print("  lat    Asterra mean   Earth mean   Asterra harmonic   Earth harmonic")
	for lat in [0, 30, 60, 90]:
		var x := sin(deg_to_rad(float(lat)))
		print("  %4d   %10.1f   %10.1f   %14.1f   %12.1f"
				% [lat, a.insol_at(x), e.insol_at(x), a.insol_amp_at(x), e.insol_amp_at(x)])

# ---------------------------------------------------------------------------
func _asterra_bake() -> void:
	print("\n== baked world ==")
	var cfg := GenConfig.new()
	cfg.face_res = 64
	cfg.erosion_iterations = 12
	var t0 := Time.get_ticks_msec()
	var f := PlanetBake.new(cfg).bake(Callable(), false)
	print("  bake: %d cells in %.1f s" % [f.grid.cell_count, (Time.get_ticks_msec() - t0) / 1000.0])

	var g := f.grid
	print("\n  lat band   mean T   land T   sea T   range(land)   precip(land)")
	for lo_v in [-90, -70, -50, -30, -10, 10, 30, 50, 70]:
		var lo: int = lo_v
		var hi := lo + 20
		var all_t := 0.0
		var all_n := 0
		var lt := 0.0
		var ln := 0
		var st := 0.0
		var sn := 0
		var lr := 0.0
		var lp := 0.0
		for c in g.cell_count:
			var lat := rad_to_deg(asin(clampf(g.cell_dir(c).y, -1.0, 1.0)))
			if lat < float(lo) or lat >= float(hi):
				continue
			all_t += f.temp_mean[c]
			all_n += 1
			if f.elev[c] > 0.0:
				lt += f.temp_mean[c]
				lr += f.temp_range[c]
				lp += f.precip[c]
				ln += 1
			else:
				st += f.temp_mean[c]
				sn += 1
		if all_n == 0:
			continue
		print("  %3d..%3d   %6.1f   %6.1f   %5.1f   %11.1f   %10.0f"
				% [lo, hi, all_t / all_n,
				lt / maxf(1.0, ln), st / maxf(1.0, sn),
				lr / maxf(1.0, ln), lp / maxf(1.0, ln)])

	# Maritime versus continental at the same latitude -- the thing the old model
	# could not express at all.
	var clim := PassClimate.new(f)
	clim._winds()
	clim._distance_to_ocean()
	clim.ebm = ClimateEBM.new(cfg)
	clim.ebm.bind_geography(f)
	clim.ebm.solve()
	print("
  zonal solve: global mean %.1f C, planetary albedo %.3f, permanent ice %.1f%%"
			% [clim.ebm.global_mean_temp(), clim.ebm.global_albedo(),
			100.0 * clim.ebm.global_ice_fraction()])
	var coast_r := 0.0
	var coast_n := 0
	var inland_r := 0.0
	var inland_n := 0
	for c in g.cell_count:
		if f.elev[c] <= 0.0:
			continue
		var lat := absf(rad_to_deg(asin(clampf(g.cell_dir(c).y, -1.0, 1.0))))
		if lat < 35.0 or lat > 60.0:
			continue
		if clim.dist_to_ocean[c] < 60000.0:
			coast_r += f.temp_range[c]
			coast_n += 1
		elif clim.dist_to_ocean[c] > 300000.0:
			inland_r += f.temp_range[c]
			inland_n += 1
	if coast_n > 0 and inland_n > 0:
		print("\n  mid-latitude land, annual range:  coast %.1f C   interior %.1f C"
				% [coast_r / coast_n, inland_r / inland_n])

	# Asterra is not Earth: 62% ocean against 71%, and 21.4 deg of tilt against
	# 23.44. Both push it colder, and low obliquity in particular is a known
	# glaciation driver -- cool summers fail to melt what the winter laid down.
	# This is what that costs, and what the greenhouse knob buys back.
	print("\n  greenhouse offset on the real baked geography:")
	print("     W/m^2   global    60deg    poles     ice%")
	for off_v in [0.0, 2.0, 4.0, 6.0, 8.0]:
		var cg := GenConfig.new()
		cg.face_res = cfg.face_res
		cg.greenhouse_offset = off_v
		var eg := ClimateEBM.new(cg)
		eg.bind_geography(f)
		eg.solve()
		print("     %+5.1f   %6.1f   %6.1f   %6.1f   %6.1f"
				% [off_v, eg.global_mean_temp(), eg.temp_at(sin(deg_to_rad(60.0))),
				eg.temp_at(1.0), 100.0 * eg.global_ice_fraction()])

	# Aridity at a fixed latitude: wet ground should have a flat year.
	var dry_r := 0.0
	var dry_t := 0.0
	var dry_h := 0.0
	var dry_n := 0
	var wet_r := 0.0
	var wet_t := 0.0
	var wet_h := 0.0
	var wet_n := 0
	for c in g.cell_count:
		if f.elev[c] <= 0.0 or f.elev[c] > 900.0:
			continue
		if absf(rad_to_deg(asin(clampf(g.cell_dir(c).y, -1.0, 1.0)))) > 25.0:
			continue
		var dep: float = f.temp_mean[c] - clim.ebm.temp_at(clampf(g.cell_dir(c).y, -1.0, 1.0))
		if f.precip[c] < 300.0:
			dry_r += f.temp_range[c]
			dry_t += dep
			dry_h += f.elev[c]
			dry_n += 1
		elif f.precip[c] > 1600.0:
			wet_r += f.temp_range[c]
			wet_t += dep
			wet_h += f.elev[c]
			wet_n += 1
	if dry_n > 0 and wet_n > 0:
		print("  tropical lowland, departure from its own zonal mean:")
		print("     arid   %+.1f C   range %4.1f C   mean elevation %.0f m" % [dry_t / dry_n, dry_r / dry_n, dry_h / dry_n])
		print("     humid  %+.1f C   range %4.1f C   mean elevation %.0f m" % [wet_t / wet_n, wet_r / wet_n, wet_h / wet_n])

	# Biomes.
	var counts := {}
	for c in g.cell_count:
		var k: int = f.biome[c]
		counts[k] = counts.get(k, 0) + 1
	var keys := counts.keys()
	keys.sort_custom(func(p, q): return counts[p] > counts[q])
	print("\n  biomes (%d distinct):" % keys.size())
	for k in keys:
		print("    %-26s %6d  %5.2f%%"
				% [PlanetFields.BIOME_NAMES[k], counts[k], 100.0 * counts[k] / g.cell_count])
