#pragma once

#include "voronoi_dry_transport.h"

#include <array>
#include <vector>

namespace asterra::weather {

// Conservative vertical mass exchange and pressure-coordinate remap for the
// 30-level atmosphere. Dry mass, theta mass and every passive tracer use the
// same donor transfer across interfaces and the same overlap remap in dry-mass
// coordinate.
class VoronoiDryVerticalTransport {
public:
	static constexpr int LEVELS = VoronoiDryTransport::LEVELS;
	static constexpr int INTERFACES = LEVELS - 1;
	using State = VoronoiDryTransport::State;

	struct StepDiagnostics {
		double requested_dt_s = 0.0;
		double accepted_dt_s = 0.0;
		double max_vertical_courant = 0.0;
		double dry_mass_before_kg = 0.0;
		double dry_mass_after_kg = 0.0;
		double relative_dry_mass_error = 0.0;
		double theta_mass_before_kg_k = 0.0;
		double theta_mass_after_kg_k = 0.0;
		double relative_theta_mass_error = 0.0;
		double max_relative_tracer_mass_error = 0.0;
		double min_layer_mass_kg_m2 = 0.0;
		double min_potential_temperature_k = 0.0;
		int rejected_steps = 0;
	};

	struct RemapDiagnostics {
		double relative_dry_mass_error = 0.0;
		double relative_theta_mass_error = 0.0;
		double max_relative_tracer_mass_error = 0.0;
		double max_column_mass_error = 0.0;
		double max_column_theta_mass_error = 0.0;
		double max_column_tracer_mass_error = 0.0;
		double max_edge_momentum_error = 0.0;
		double max_mass_fraction_error = 0.0;
		double min_layer_mass_kg_m2 = 0.0;
		double min_potential_temperature_k = 0.0;
	};

	explicit VoronoiDryVerticalTransport(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double scale_height_m = 8000.0,
		double top_pressure_pa = 7500.0);

	void set_surface_height_m(const std::vector<double> &height_m) {
		horizontal_.set_surface_height_m(height_m);
	}
	void set_surface_geopotential_m2_s2(const std::vector<double> &geopotential) {
		horizontal_.set_surface_geopotential_m2_s2(geopotential);
	}

	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const {
		return horizontal_.make_isothermal_reference(surface_pressure_pa, temperature_k);
	}
	State make_isothermal_terrain_balanced_reference(double reference_surface_pressure_pa,
		double temperature_k) const {
		return horizontal_.make_isothermal_terrain_balanced_reference(
			reference_surface_pressure_pa, temperature_k);
	}

	double max_courant(const State &state,
		const std::vector<double> &interface_mass_flux, double dt_s) const;
	double stable_dt(const State &state,
		const std::vector<double> &interface_mass_flux,
		double target_cfl, double maximum_dt_s) const;

	StepDiagnostics step(State &state,
		const std::vector<double> &interface_mass_flux,
		double requested_dt_s, double target_cfl = 0.45,
		int max_retries = 10) const;

	RemapDiagnostics remap_to_reference_levels(State &state) const;

	const std::array<double, LEVELS> &reference_mass_fractions() const {
		return reference_mass_fraction_;
	}
	const VoronoiDryTransport &horizontal_transport() const { return horizontal_; }

private:
	struct Tendencies {
		std::vector<double> mass_dt;
		std::vector<double> theta_mass_dt;
		std::vector<std::vector<double>> tracer_mass_dt;
	};

	const GeodesicVoronoiGrid *grid_ = nullptr;
	VoronoiDryTransport horizontal_;
	std::array<double, LEVELS> reference_mass_fraction_{};

	int scalar_index(int level, int cell) const {
		return level * grid_->cell_count() + cell;
	}
	int interface_index(int interface_level, int cell) const {
		return interface_level * grid_->cell_count() + cell;
	}
	int edge_index(int level, int edge) const {
		return level * grid_->edge_count() + edge;
	}

	void validate_state(const State &state) const;
	void validate_flux(const std::vector<double> &interface_mass_flux) const;
	bool finite_positive(const State &state) const;
	Tendencies compute_tendencies(const State &state,
		const std::vector<double> &interface_mass_flux) const;
	bool euler_stage(const State &input, State &output,
		const std::vector<double> &interface_mass_flux, double dt_s) const;
	bool ssprk3_attempt(const State &initial, State &candidate,
		const std::vector<double> &interface_mass_flux, double dt_s) const;
};

} // namespace asterra::weather
