#pragma once

#include "voronoi_dry_transport.h"

#include <vector>

namespace asterra::weather {

// Local moist thermodynamics for the conservative dry-mass-coordinate core.
//
// Water species are stored as tracer masses [kg/m2] per model layer while dry
// layer mass remains the prognostic vertical-coordinate mass. Mechanical
// pressure, however, is the weight of dry air plus vapor/liquid/ice. The
// prognostic theta is standard potential temperature and therefore converts to
// sensible temperature using that full mechanical pressure everywhere in this
// module and in the moist hydrostatic/surface source operators.
class VoronoiMoistThermodynamics {
public:
	using State = VoronoiDryTransport::State;
	static constexpr int LEVELS = VoronoiDryTransport::LEVELS;
	static constexpr int INTERFACES = LEVELS + 1;

	struct TracerIndices {
		int vapor = 0;
		int cloud_liquid = 1;
		int cloud_ice = 2;
	};

	struct ThermodynamicDiagnostics {
		std::vector<double> surface_pressure_pa;     // [cell], full mechanical pressure
		std::vector<double> interface_pressure_pa;   // [interface][cell]
		std::vector<double> layer_pressure_pa;       // [level][cell], geometric mean
		std::vector<double> potential_temperature_k; // [level][cell]
		std::vector<double> temperature_k;           // [level][cell], theta*Exner(full p)
	};

	struct AdjustmentDiagnostics {
		double total_water_before_kg = 0.0;
		double total_water_after_kg = 0.0;
		double relative_total_water_error = 0.0;
		double max_relative_cell_water_error = 0.0;
		double max_specific_enthalpy_error_j_kg = 0.0;
		double max_relative_humidity_before = 0.0;
		double max_relative_humidity_after = 0.0;
		double max_abs_temperature_change_k = 0.0;
		double condensed_water_kg = 0.0;
		double evaporated_water_kg = 0.0;
		double min_temperature_k = 0.0;
		double max_temperature_k = 0.0;
		int saturated_cell_count = 0;
	};

	static constexpr double EPSILON = 0.622;
	static constexpr double CP_DRY = VoronoiDryHydrostatic::CP;
	static constexpr double LV0_J_KG = 2.500e6;
	static constexpr double LF0_J_KG = 3.34e5;
	static constexpr double LS0_J_KG = LV0_J_KG + LF0_J_KG;
	static constexpr double T_FREEZE_K = 273.15;
	static constexpr double T_ALL_ICE_K = 253.15;

	explicit VoronoiMoistThermodynamics(const VoronoiDryTransport &transport,
		TracerIndices indices = {});

	const TracerIndices &tracer_indices() const { return indices_; }

	// Ensure the configured tracer slots exist. Existing tracer fields are never
	// reordered or overwritten; newly-created fields are initialized to zero.
	void ensure_water_tracers(State &state) const;

	// Diagnose total mechanical pressure and the single canonical theta->T
	// conversion for a moist state. Pressure includes dry + vapor + liquid + ice
	// layer weight while the vertical coordinate itself remains dry-mass based.
	ThermodynamicDiagnostics diagnose_thermodynamics(const State &state) const;

	// Saturation vapor pressure and dry-air-basis saturation mixing ratio.
	// Pressure must be the full mechanical pressure and exceed saturation vapor
	// pressure.
	static double saturation_vapor_pressure_pa(double temperature_k);
	static double saturation_mixing_ratio(double pressure_pa,
		double temperature_k);
	static double ice_fraction(double temperature_k);

	// Exact ideal-mixture virtual temperature for water mixing ratios expressed
	// per kg of dry air, with condensate volume neglected:
	//   Tv = T (1 + qv/epsilon) / (1 + qv + ql + qi).
	// With total mechanical pressure p this gives rho_total = p/(Rd Tv).
	static double virtual_temperature_k(double temperature_k,
		double vapor_mixing_ratio,
		double liquid_mixing_ratio = 0.0,
		double ice_mixing_ratio = 0.0);
	static double mixture_density_kg_m3(double total_pressure_pa,
		double temperature_k,
		double vapor_mixing_ratio,
		double liquid_mixing_ratio = 0.0,
		double ice_mixing_ratio = 0.0);

	// Diagnose Tv for every model scalar from the same full-pressure temperature
	// convention used by saturation adjustment and moist hydrostatics.
	std::vector<double> diagnose_virtual_temperature_k(const State &state) const;

	// Convenience initialization: set vapor to RH * qsat and zero cloud water at
	// every cell/level while preserving dry mass, wind and theta. The initializer
	// iterates the added vapor weight to a self-consistent full-pressure RH.
	void initialize_uniform_relative_humidity(State &state,
		double relative_humidity) const;

	// Instantaneous local saturation adjustment. Each cell/level independently
	// conserves total water and the approximate moist specific enthalpy
	//   h = Cp_d T + Lv qv - Lf qi
	// while enforcing qv <= qsat(T,p). Because phase repartition preserves total
	// layer water mass, p is fixed during each local enthalpy root. Existing
	// condensate evaporates when subsaturated; equilibrium condensate is split
	// between liquid and ice by a 253.15..273.15 K mixed-phase ramp.
	AdjustmentDiagnostics saturation_adjust(State &state) const;

private:
	const VoronoiDryTransport *transport_ = nullptr;
	TracerIndices indices_{};

	int scalar_count() const;
	void validate_indices() const;
	void validate_water_state(const State &state) const;
	double total_water_mass_kg(const State &state) const;
};

} // namespace asterra::weather
