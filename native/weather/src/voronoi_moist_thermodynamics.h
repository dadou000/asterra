#pragma once

#include "voronoi_dry_transport.h"

#include <vector>

namespace asterra::weather {

// Local moist phase-equilibrium operator for the conservative dry-core state.
//
// Water species are stored as tracer masses [kg/m2] on the same dry-mass
// coordinate as all other transported scalars. This operator performs no
// advection and never changes dry mass or wind. It only repartitions total
// water between vapor/cloud liquid/cloud ice and updates theta mass for latent
// heating/cooling.
//
// The present thermodynamic closure is deliberately dilute: pressure is the
// dry-core diagnosed pressure and Cp is dry-air Cp. Moist hydrostatic mass and
// virtual-temperature effects belong to the next coupling phase; keeping those
// separate makes this saturation-adjustment budget independently testable.
class VoronoiMoistThermodynamics {
public:
	using State = VoronoiDryTransport::State;

	struct TracerIndices {
		int vapor = 0;
		int cloud_liquid = 1;
		int cloud_ice = 2;
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

	// Saturation vapor pressure and dry-air-basis saturation mixing ratio.
	// Pressure must exceed saturation vapor pressure.
	static double saturation_vapor_pressure_pa(double temperature_k);
	static double saturation_mixing_ratio(double pressure_pa,
		double temperature_k);
	static double ice_fraction(double temperature_k);

	// Convenience initialization: set vapor to RH * qsat and zero cloud water at
	// every cell/level while preserving dry state and theta. RH must be >= 0.
	void initialize_uniform_relative_humidity(State &state,
		double relative_humidity) const;

	// Instantaneous local saturation adjustment. Each cell/level independently
	// conserves total water and the approximate moist specific enthalpy
	//   h = Cp_d T + Lv qv - Lf qi
	// while enforcing qv <= qsat(T,p). Existing condensate evaporates when
	// subsaturated; equilibrium condensate is partitioned between liquid and ice
	// by a 253.15..273.15 K mixed-phase ramp.
	AdjustmentDiagnostics saturation_adjust(State &state) const;

private:
	const VoronoiDryTransport *transport_ = nullptr;
	TracerIndices indices_{};

	int scalar_count() const;
	void validate_indices() const;
	double total_water_mass_kg(const State &state) const;
};

} // namespace asterra::weather
