#pragma once

#include "voronoi_moist_thermodynamics.h"

#include <vector>

namespace asterra::weather {

// Conservative atmosphere <-> surface source operator for the replacement core.
//
// This layer deliberately accepts prescribed physical fluxes rather than yet
// choosing a bulk-aerodynamic closure. Positive evaporation moves liquid water
// from the surface reservoir into bottom-layer water vapor [kg/m2/s]. Positive
// sensible heat moves energy from the surface into bottom-layer dry enthalpy
// [W/m2]. Surface ice is a separate non-evaporable reservoir so snowfall cannot
// silently become liquid water. Every transfer is equal/opposite and
// transactional; unavailable donor water, invalid temperatures, or any
// non-finite result restores both states.
class VoronoiSurfaceExchange {
public:
	using State = VoronoiDryTransport::State;
	using TracerIndices = VoronoiMoistThermodynamics::TracerIndices;

	struct SurfaceState {
		std::vector<double> water_kg_m2;   // liquid reservoir, [cell]
		std::vector<double> ice_kg_m2;     // frozen reservoir, [cell]
		std::vector<double> energy_j_m2;   // thermodynamic energy, [cell]
	};

	struct Diagnostics {
		double requested_dt_s = 0.0;
		double atmosphere_water_before_kg = 0.0;
		double atmosphere_water_after_kg = 0.0;
		double surface_water_before_kg = 0.0;
		double surface_water_after_kg = 0.0;
		double relative_system_water_error = 0.0;
		double atmosphere_thermo_before_j = 0.0;
		double atmosphere_thermo_after_j = 0.0;
		double surface_energy_before_j = 0.0;
		double surface_energy_after_j = 0.0;
		double relative_system_energy_error = 0.0;
		double evaporated_to_atmosphere_kg = 0.0;
		double condensed_to_surface_kg = 0.0;
		double sensible_to_atmosphere_j = 0.0;
		double latent_to_atmosphere_j = 0.0;
		double min_surface_water_kg_m2 = 0.0;
		double min_surface_ice_kg_m2 = 0.0;
		double min_bottom_temperature_k = 0.0;
		double max_bottom_temperature_k = 0.0;
	};

	explicit VoronoiSurfaceExchange(const VoronoiDryTransport &transport,
		TracerIndices indices = {});

	SurfaceState make_uniform_surface_state(double water_kg_m2,
		double energy_j_m2 = 0.0) const;

	// Flux arrays are [cell]. Positive evaporation transfers liquid surface water
	// -> vapor; negative evaporation is dew/condensation vapor -> liquid surface.
	// Frozen surface water is not available to this operator. Positive sensible
	// heat transfers surface -> atmosphere. No implicit saturation adjustment is
	// performed here; call VoronoiMoistThermodynamics after this operator.
	Diagnostics apply_fluxes(State &atmosphere, SurfaceState &surface,
		const std::vector<double> &evaporation_kg_m2_s,
		const std::vector<double> &sensible_heat_w_m2,
		double dt_s) const;

	double total_surface_water_kg(const SurfaceState &surface) const;
	double total_surface_liquid_water_kg(const SurfaceState &surface) const;
	double total_surface_ice_kg(const SurfaceState &surface) const;
	double total_surface_energy_j(const SurfaceState &surface) const;
	double total_atmospheric_water_kg(const State &atmosphere) const;
	double atmospheric_thermodynamic_energy_j(const State &atmosphere) const;

	const TracerIndices &tracer_indices() const { return indices_; }

private:
	const VoronoiDryTransport *transport_ = nullptr;
	TracerIndices indices_{};

	void validate_surface(const SurfaceState &surface) const;
	void validate_fluxes(const std::vector<double> &evaporation_kg_m2_s,
		const std::vector<double> &sensible_heat_w_m2, double dt_s) const;
};

} // namespace asterra::weather
