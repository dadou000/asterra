#pragma once

#include "voronoi_moist_hydrostatic.h"
#include "voronoi_surface_exchange.h"

#include <vector>

namespace asterra::weather {

// Conservative vertical sedimentation for precipitating rain/snow reservoirs.
//
// Rain and snow are prognostic tracer masses [kg/m2] on the same 30 dry-mass
// layers as the other water species. Sedimentation is a relative fall through
// the air column, independent of the dry-air vertical remap. A single downward
// donor flux per species/interface transfers hydrometeor mass between layers;
// bottom fallout is added exactly to the surface-water reservoir. No UI rain
// rate is prognostic: it is diagnosed from the accepted surface mass flux.
class VoronoiPrecipitation {
public:
	using State = VoronoiDryTransport::State;
	using SurfaceState = VoronoiSurfaceExchange::SurfaceState;
	using TracerIndices = VoronoiMoistThermodynamics::TracerIndices;
	static constexpr int LEVELS = VoronoiDryTransport::LEVELS;

	struct Diagnostics {
		double requested_dt_s = 0.0;
		double accepted_dt_s = 0.0;
		double max_courant = 0.0;
		int rejected_steps = 0;

		double atmosphere_water_before_kg = 0.0;
		double atmosphere_water_after_kg = 0.0;
		double surface_water_before_kg = 0.0;
		double surface_water_after_kg = 0.0;
		double relative_system_water_error = 0.0;

		double rain_before_kg = 0.0;
		double rain_after_kg = 0.0;
		double snow_before_kg = 0.0;
		double snow_after_kg = 0.0;
		double deposited_rain_kg = 0.0;
		double deposited_snow_kg = 0.0;
		double min_rain_kg_m2 = 0.0;
		double min_snow_kg_m2 = 0.0;
		double max_surface_precip_flux_kg_m2_s = 0.0;

		// Cell-centered accepted mean surface fluxes. UI may derive mm/h from
		// kg/m2/s; these fields never feed back into conserved physics.
		std::vector<double> surface_rain_flux_kg_m2_s;
		std::vector<double> surface_snow_flux_kg_m2_s;
	};

	explicit VoronoiPrecipitation(const VoronoiDryTransport &transport,
		TracerIndices indices = {});

	void ensure_precipitation_tracers(State &state) const;

	double total_atmospheric_water_kg(const State &state) const;
	double total_rain_kg(const State &state) const;
	double total_snow_kg(const State &state) const;
	double max_courant(const State &state, double dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const;
	double stable_dt(const State &state, double target_cfl, double maximum_dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const;

	// Default fall speeds are deliberately simple bulk bring-up values. The
	// conservative operator is independent of the later size-distribution/fall-
	// speed closure, which can replace these values without changing budgets.
	Diagnostics step(State &state, SurfaceState &surface,
		double requested_dt_s,
		double rain_fall_speed_mps = 7.0,
		double snow_fall_speed_mps = 1.0,
		double target_cfl = 0.45,
		int max_retries = 10) const;

	const TracerIndices &tracer_indices() const { return indices_; }

private:
	struct Tendencies {
		std::vector<double> rain_dt;
		std::vector<double> snow_dt;
		std::vector<double> surface_rain_dt;
		std::vector<double> surface_snow_dt;
	};

	const VoronoiDryTransport *transport_ = nullptr;
	TracerIndices indices_{};

	int scalar_index(int level, int cell) const {
		return level * transport_->grid().cell_count() + cell;
	}
	int interface_index(int interface_level, int cell) const {
		return interface_level * transport_->grid().cell_count() + cell;
	}

	void validate_state_surface(const State &state, const SurfaceState &surface) const;
	Tendencies compute_tendencies(const State &state,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const;
	bool euler_stage(const State &input, const SurfaceState &surface_input,
		State &output, SurfaceState &surface_output, double dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const;
	bool ssprk3_attempt(const State &initial, const SurfaceState &surface_initial,
		State &candidate, SurfaceState &surface_candidate, double dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const;
};

} // namespace asterra::weather
