from pathlib import Path

path = Path("native/weather/src/weather_native_oklahoma.cpp")
text = path.read_text(encoding="utf-8")

old_call = '''\t// Keep the proven transport/momentum/saturation solver, then replace its old
\t// 68%-RH *persistent* sub-grid cloud state with the much sparser closure below.
\t// The baseline pass may momentarily create broad condensate, but it is not
\t// allowed to survive unless the post-advection total-water RH supports it.
\thorizontal_pass_original(a, is_global, dt);
'''
new_call = '''\t// Run the proven transport/momentum core without its legacy 68%-RH cloud
\t// microphysics. That branch was the source of the runaway: it converted broad
\t// climatological humidity into condensate every step while the humidity nudger
\t// simultaneously refilled vapour. The sparse closure below now owns all cloud
\t// phase changes. Keep only a very weak global humidity climatology as a slow
\t// large-scale anchor; the coupled surface evaporation is the real water source.
\tconst float requested_humidity_weight = tuning_weights[HUMIDITY];
\tconst float requested_cloud_weight = tuning_weights[CLOUD_MICROPHYSICS];
\ttuning_weights[HUMIDITY] = requested_humidity_weight * (is_global ? 0.12f : 0.0f);
\ttuning_weights[CLOUD_MICROPHYSICS] = 0.0f;
\thorizontal_pass_original(a, is_global, dt);
\ttuning_weights[HUMIDITY] = requested_humidity_weight;
\ttuning_weights[CLOUD_MICROPHYSICS] = requested_cloud_weight;
'''
if old_call not in text:
    raise SystemExit("legacy horizontal-pass call block not found")
text = text.replace(old_call, new_call, 1)

start_marker = '''\t// Sparse-cloud budget. The original core still performs resolved saturation,
'''
end_marker = '''\t}\n}\n\nvoid WeatherNative::vertical_pass'''
start = text.find(start_marker)
end = text.find(end_marker, start)
if start < 0 or end < 0:
    raise SystemExit("sparse-cloud block markers not found")

new_microphysics = r'''\t// Single-owner sparse cloud microphysics. Vapour RH determines whether a new
\t// sub-grid cloud fraction may form; existing condensate is not allowed to
\t// inflate its own formation RH. Total water is used only to enforce saturated
\t// equilibrium and therefore remains conserved through condensation/evaporation.
\tconst float microphysics_scale = std::clamp(requested_cloud_weight, 0.0f, 2.0f);
\tfor (int layer = 0; layer < LAYERS; ++layer) {
\t\tconst int off = a.layer_offset(layer);
\t\tconst float sf = sigma_temperature_factor(layer);
\t\tconst float fair_onset_rh = layer <= 1 ? 0.90f : 0.93f;
\t\tconst float fair_full_rh = layer <= 1 ? 0.985f : 0.995f;
\t\tfor (int c = 0; c < a.cells; ++c) {
\t\t\tconst int i = off + c;
\t\t\tfloat qv = std::clamp(a.nq[i], 0.00001f, 0.032f);
\t\t\tfloat liquid = std::max(a.nliquid[i], 0.0f);
\t\t\tfloat ice = std::max(a.nice[i], 0.0f);
\t\t\tfloat cloud_total = liquid + ice;
\t\t\tfloat temperature = a.ntheta[i] * sf;
\t\t\tconst float pabs = std::clamp(P0 * SIGMA[layer] + a.npressure[i], 12000.0f, 115000.0f);
\t\t\tconst float saturation = std::max(qsat_scalar(temperature, pabs), 2e-5f);
\t\t\tconst float total_water = qv + cloud_total;
\t\t\tconst float vapor_rh = qv / saturation;
\n\t\t\tconst float active = a.convective_activation.size() == size_t(a.cells)
\t\t\t\t? std::clamp(a.convective_activation[c], 0.0f, 1.0f) : 0.0f;
\t\t\tconst float precip_active = severe_smooth01((a.nprecip[c] - 0.055f) / 0.24f);
\t\t\tconst float storm_keep = std::clamp(std::max(active, precip_active), 0.0f, 1.0f);
\n\t\t\t// If total water exceeds saturation, enough condensate must remain to
\t\t\t// keep vapour at qsat. This is the resolved cloud component.
\t\t\tconst float resolved_target = std::max(total_water - saturation, 0.0f);
\t\t\tfloat target_cloud = resolved_target;
\n\t\t\t// Fair-weather cloud fraction exists only when *vapour* RH is already
\t\t\t// close to saturation. A released storm lowers the onset and turns much
\t\t\t// more of the near-saturated reservoir into optically deep condensate.
\t\t\tconst float onset_rh = std::lerp(fair_onset_rh, 0.80f, storm_keep);
\t\t\tconst float full_rh = std::lerp(fair_full_rh, 0.95f, storm_keep);
\t\t\tif (vapor_rh > onset_rh) {
\t\t\t\tconst float partial = severe_smooth01(
\t\t\t\t\t(vapor_rh - onset_rh) / std::max(full_rh - onset_rh, 0.001f));
\t\t\t\tconst float available = std::max(
\t\t\t\t\ttotal_water - saturation * onset_rh - resolved_target, 0.0f);
\t\t\t\tconst float fair_fraction = 0.012f + 0.070f * partial;
\t\t\t\tconst float storm_fraction = 0.18f + 0.72f * partial;
\t\t\t\tconst float retained_fraction = std::lerp(fair_fraction, storm_fraction, storm_keep);
\t\t\t\ttarget_cloud += available * retained_fraction;
\t\t\t}
\t\t\ttarget_cloud = std::clamp(target_cloud, 0.0f, std::max(total_water - 0.00001f, 0.0f));
\n\t\t\tif (target_cloud > cloud_total) {
\t\t\t\t// New cloud forms quickly only when actually supersaturated or when a
\t\t\t\t// released convective column is close to saturation.
\t\t\t\tconst float formation_tau = std::lerp(
\t\t\t\t\tis_global ? 900.0f : 420.0f,
\t\t\t\t\tis_global ? 120.0f : 60.0f,
\t\t\t\t\tstorm_keep);
\t\t\t\tconst float formation_fraction = 1.0f - std::exp(
\t\t\t\t\t-microphysics_scale * dt / std::max(formation_tau, 1.0f));
\t\t\t\tconst float formed = std::min(
\t\t\t\t\t(target_cloud - cloud_total) * formation_fraction,
\t\t\t\t\tstd::max(qv - 0.00001f, 0.0f));
\t\t\t\tif (formed > 0.0f) {
\t\t\t\t\tconst float ice_frac = std::clamp((268.0f - temperature) / 20.0f, 0.0f, 1.0f);
\t\t\t\t\tqv -= formed;
\t\t\t\t\tliquid += formed * (1.0f - ice_frac);
\t\t\t\t\tice += formed * ice_frac;
\t\t\t\t\ta.ntheta[i] = std::clamp(
\t\t\t\t\t\ta.ntheta[i] + formed * (2500.0f / sf), 220.0f, 430.0f);
\t\t\t\t}
\t\t\t} else if (target_cloud < cloud_total) {
\t\t\t\t// Fair cloud clears rapidly; anvils/active storm condensate retain a
\t\t\t\t// much longer memory. Evaporation returns exactly the removed mass to q.
\t\t\t\tconst float liquid_tau = std::lerp(layer <= 1 ? 480.0f : 720.0f,
\t\t\t\t\t4800.0f, storm_keep);
\t\t\t\tconst float ice_tau = std::lerp(1500.0f, 14400.0f, storm_keep);
\t\t\t\tconst float liquid_share = cloud_total > 1e-9f ? liquid / cloud_total : 0.0f;
\t\t\t\tconst float effective_tau = std::lerp(ice_tau, liquid_tau, liquid_share);
\t\t\t\tconst float evap_fraction = 1.0f - std::exp(
\t\t\t\t\t-microphysics_scale * dt / std::max(effective_tau, 1.0f));
\t\t\t\tconst float evaporated = (cloud_total - target_cloud) * evap_fraction;
\t\t\t\tif (evaporated > 0.0f) {
\t\t\t\t\tconst float remove_liquid = evaporated * liquid_share;
\t\t\t\t\tliquid = std::max(liquid - remove_liquid, 0.0f);
\t\t\t\t\tice = std::max(ice - (evaporated - remove_liquid), 0.0f);
\t\t\t\t\tqv = std::min(qv + evaporated, 0.032f);
\t\t\t\t\ta.ntheta[i] = std::clamp(
\t\t\t\t\t\ta.ntheta[i] - evaporated * (2488.0f / sf), 220.0f, 430.0f);
\t\t\t\t}
\t\t\t}
\n\t\t\t// Phase conversion remains prognostic. This is intentionally separate
\t\t\t// from cloud amount so cold anvils can glaciate without creating water.
\t\t\ttemperature = a.ntheta[i] * sf;
\t\t\tconst float phase_scale = std::max(microphysics_scale, 0.05f);
\t\t\tconst float freeze_strength = std::clamp((260.0f - temperature) / 14.0f, 0.0f, 1.0f);
\t\t\tconst float freeze = liquid * freeze_strength
\t\t\t\t* (1.0f - std::exp(-phase_scale * dt / 900.0f));
\t\t\tliquid -= freeze;
\t\t\tice += freeze;
\t\t\tconst float melt_strength = std::clamp((temperature - 273.15f) / 8.0f, 0.0f, 1.0f);
\t\t\tconst float melt = ice * melt_strength
\t\t\t\t* (1.0f - std::exp(-phase_scale * dt / 700.0f));
\t\t\tice -= melt;
\t\t\tliquid += melt;
\t\t\ta.ntheta[i] = std::clamp(
\t\t\t\ta.ntheta[i] + (freeze - melt) * (333.0f / sf), 220.0f, 430.0f);
\n\t\t\ta.nq[i] = std::clamp(qv, 0.00001f, 0.032f);
\t\t\ta.nliquid[i] = std::clamp(liquid, 0.0f, 0.012f);
\t\t\ta.nice[i] = std::clamp(ice, 0.0f, 0.012f);
\t\t}
\t}
'''
text = text[:start] + new_microphysics + text[end:]
path.write_text(text, encoding="utf-8")
print("patched", path)
