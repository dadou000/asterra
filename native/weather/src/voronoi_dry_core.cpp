#include "voronoi_dry_core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double CORE_CONSERVATION_REJECT_TOL = 3.0e-10;
constexpr double COORDINATE_ALREADY_ALIGNED_TOL = 5.0e-15;

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}
} // namespace

VoronoiDryCore::VoronoiDryCore(const GeodesicVoronoiGrid &grid,
		double gravity_mps2, double scale_height_m, double top_pressure_pa,
		double rotation_rate_rad_s, Vec3d rotation_axis)
	: grid_(&grid),
	  dynamics_(grid, gravity_mps2, scale_height_m, top_pressure_pa,
		rotation_rate_rad_s, rotation_axis),
	  vertical_(grid, gravity_mps2, scale_height_m, top_pressure_pa) {
	if (grid.cell_count() <= 0 || grid.edge_count() <= 0) {
		throw std::invalid_argument("VoronoiDryCore requires a built geodesic grid");
	}
}

double VoronoiDryCore::max_coordinate_mass_fraction_error(const State &state) const {
	const size_t scalar_count = static_cast<size_t>(LEVELS) * grid_->cell_count();
	if (state.layer_mass_kg_m2.size() != scalar_count) {
		throw std::invalid_argument("Dry-core layer-mass array has wrong size");
	}
	const auto &reference = vertical_.reference_mass_fractions();
	double maximum = 0.0;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		long double column_mass_ld = 0.0L;
		for (int k = 0; k < LEVELS; ++k) {
			const double mass = state.layer_mass_kg_m2[
				static_cast<size_t>(k * grid_->cell_count() + c)];
			if (!(mass > 0.0) || !std::isfinite(mass)) {
				throw std::runtime_error("Dry core has invalid layer mass while checking coordinate");
			}
			column_mass_ld += static_cast<long double>(mass);
		}
		const double column_mass = static_cast<double>(column_mass_ld);
		if (!(column_mass > 0.0) || !std::isfinite(column_mass)) {
			throw std::runtime_error("Dry core has invalid column mass while checking coordinate");
		}
		for (int k = 0; k < LEVELS; ++k) {
			const double actual = state.layer_mass_kg_m2[
				static_cast<size_t>(k * grid_->cell_count() + c)] / column_mass;
			maximum = std::max(maximum,
				std::abs(actual - reference[static_cast<size_t>(k)]));
		}
	}
	return maximum;
}

void VoronoiDryCore::refresh_extrema(const State &state,
		StepDiagnostics &diagnostics) const {
	diagnostics.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
	diagnostics.min_potential_temperature_k = std::numeric_limits<double>::infinity();
	diagnostics.max_speed_mps = 0.0;
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double mass = state.layer_mass_kg_m2[i];
		const double theta_mass = state.theta_mass_kg_k_m2[i];
		if (!(mass > 0.0) || !(theta_mass > 0.0)
				|| !std::isfinite(mass) || !std::isfinite(theta_mass)) {
			throw std::runtime_error("Dry core extrema encountered invalid thermodynamic state");
		}
		diagnostics.min_layer_mass_kg_m2 = std::min(
			diagnostics.min_layer_mass_kg_m2, mass);
		diagnostics.min_potential_temperature_k = std::min(
			diagnostics.min_potential_temperature_k, theta_mass / mass);
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) throw std::runtime_error("Dry core extrema encountered invalid wind");
		diagnostics.max_speed_mps = std::max(diagnostics.max_speed_mps, std::abs(u));
	}
}

VoronoiDryCore::StepDiagnostics VoronoiDryCore::step(State &state,
		double requested_dt_s, double target_cfl, int max_retries) const {
	StepDiagnostics d;
	d.requested_dt_s = requested_dt_s;
	d.dry_mass_before_kg = total_dry_mass_kg(state);
	d.theta_mass_before_kg_k = total_theta_mass_kg_k(state);
	const State original = state;

	const auto horizontal = dynamics_.step(state, requested_dt_s,
		target_cfl, max_retries);
	d.accepted_dt_s = horizontal.accepted_dt_s;
	d.max_courant = horizontal.max_courant;
	d.max_pressure_acceleration_mps2 = horizontal.max_pressure_acceleration_mps2;
	d.rejected_steps = horizontal.rejected_steps;

	if (!(horizontal.accepted_dt_s > 0.0)) {
		state = original;
		d.dry_mass_after_kg = d.dry_mass_before_kg;
		d.theta_mass_after_kg_k = d.theta_mass_before_kg_k;
		d.max_coordinate_mass_fraction_error = max_coordinate_mass_fraction_error(state);
		refresh_extrema(state, d);
		return d;
	}

	try {
		const double coordinate_error_before = max_coordinate_mass_fraction_error(state);
		if (coordinate_error_before > COORDINATE_ALREADY_ALIGNED_TOL) {
			const auto remap = vertical_.remap_to_reference_levels(state);
			d.coordinate_remap_applied = true;
			d.max_coordinate_mass_fraction_error = remap.max_mass_fraction_error;
			d.max_coordinate_column_mass_error = remap.max_column_mass_error;
			d.max_coordinate_column_theta_mass_error = remap.max_column_theta_mass_error;
			d.max_coordinate_edge_momentum_error = remap.max_edge_momentum_error;
		} else {
			d.max_coordinate_mass_fraction_error = coordinate_error_before;
		}

		d.dry_mass_after_kg = total_dry_mass_kg(state);
		d.theta_mass_after_kg_k = total_theta_mass_kg_k(state);
		d.relative_dry_mass_error = relative_error(
			d.dry_mass_after_kg, d.dry_mass_before_kg);
		d.relative_theta_mass_error = relative_error(
			d.theta_mass_after_kg_k, d.theta_mass_before_kg_k);

		if (!std::isfinite(d.relative_dry_mass_error)
				|| !std::isfinite(d.relative_theta_mass_error)
				|| d.relative_dry_mass_error > CORE_CONSERVATION_REJECT_TOL
				|| d.relative_theta_mass_error > CORE_CONSERVATION_REJECT_TOL) {
			throw std::runtime_error("Dry core combined step failed conservation gate");
		}
		refresh_extrema(state, d);
		return d;
	} catch (const std::exception &) {
		// Horizontal dynamics and coordinate maintenance are one transaction.
		state = original;
		d.accepted_dt_s = 0.0;
		d.rejected_steps += 1;
		d.dry_mass_after_kg = d.dry_mass_before_kg;
		d.theta_mass_after_kg_k = d.theta_mass_before_kg_k;
		d.relative_dry_mass_error = 0.0;
		d.relative_theta_mass_error = 0.0;
		d.coordinate_remap_applied = false;
		d.max_coordinate_column_mass_error = 0.0;
		d.max_coordinate_column_theta_mass_error = 0.0;
		d.max_coordinate_edge_momentum_error = 0.0;
		d.max_coordinate_mass_fraction_error = max_coordinate_mass_fraction_error(state);
		refresh_extrema(state, d);
		return d;
	}
}

} // namespace asterra::weather
