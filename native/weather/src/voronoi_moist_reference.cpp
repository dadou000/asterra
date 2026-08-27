#include "voronoi_moist_reference.h"

#include "voronoi_moist_hydrostatic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace asterra::weather {

namespace {
constexpr int Q_FIXED_POINT_ITERS = 80;
constexpr int ROOT_ITERS = 100;
constexpr double Q_TOL = 2.0e-14;
constexpr double ROOT_REL_TOL = 2.0e-13;

struct ColumnBuild {
	std::array<double, VoronoiDryCore::LEVELS> dry_mass{};
	std::array<double, VoronoiDryCore::LEVELS> qv{};
	std::array<double, VoronoiDryCore::LEVELS> pressure_center{};
	std::array<double, VoronoiDryCore::LEVELS> theta{};
	double surface_pressure_pa = 0.0;
	double top_geopotential_m2_s2 = 0.0;
};

double relative_error(double a, double b) {
	return std::abs(a - b) / std::max({std::abs(a), std::abs(b), 1.0});
}

} // namespace

VoronoiMoistReference::VoronoiMoistReference(const VoronoiDryCore &core,
		TracerIndices indices)
	: core_(&core), indices_(indices) {
	VoronoiMoistThermodynamics validate(core.transport(), indices);
	(void)validate;
}

VoronoiMoistReference::State VoronoiMoistReference::make_isothermal_terrain_balanced(
		double reference_surface_pressure_pa, double temperature_k,
		double relative_humidity, Diagnostics *diagnostics) const {
	const auto &transport = core_->transport();
	const auto &grid = core_->grid();
	const double top_pressure = transport.top_pressure_pa();
	const double gravity = transport.gravity_mps2();
	if (!(reference_surface_pressure_pa > top_pressure)
			|| !std::isfinite(reference_surface_pressure_pa)) {
		throw std::invalid_argument("Moist reference surface pressure must exceed model-top pressure");
	}
	if (!(temperature_k > 150.0) || !(temperature_k < 400.0)
			|| !std::isfinite(temperature_k)) {
		throw std::invalid_argument("Moist reference temperature is invalid");
	}
	if (!(relative_humidity >= 0.0) || !(relative_humidity <= 1.0)
			|| !std::isfinite(relative_humidity)) {
		throw std::invalid_argument("Terrain-balanced moist reference RH must be in [0,1]");
	}
	// Ensure the configured top remains inside the dilute vapor regime.
	if (!(top_pressure > VoronoiMoistThermodynamics::saturation_vapor_pressure_pa(temperature_k))) {
		throw std::invalid_argument("Moist reference model top is below saturation vapor pressure");
	}

	const auto &fractions = core_->reference_mass_fractions();
	const auto &surface_phi = transport.surface_geopotential_m2_s2();

	auto build_column = [&](double dry_column_mass, double phi_surface) -> ColumnBuild {
		if (!(dry_column_mass > 0.0) || !std::isfinite(dry_column_mass)) {
			throw std::runtime_error("Moist reference column dry mass became invalid");
		}
		ColumnBuild col;
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			col.dry_mass[static_cast<size_t>(k)] =
				dry_column_mass * fractions[static_cast<size_t>(k)];
		}

		double p_upper = top_pressure;
		for (int k = VoronoiDryCore::LEVELS - 1; k >= 0; --k) {
			const double md = col.dry_mass[static_cast<size_t>(k)];
			double q = 0.0;
			bool converged = relative_humidity == 0.0;
			for (int iter = 0; iter < Q_FIXED_POINT_ITERS && !converged; ++iter) {
				const double p_lower = p_upper + gravity * md * (1.0 + q);
				const double p_center = std::sqrt(p_lower * p_upper);
				const double q_new = relative_humidity
					* VoronoiMoistThermodynamics::saturation_mixing_ratio(
						p_center, temperature_k);
				if (!(q_new >= 0.0) || !std::isfinite(q_new)) {
					throw std::runtime_error("Moist reference vapor fixed point became invalid");
				}
				converged = std::abs(q_new - q) <= Q_TOL * std::max(1.0, q_new);
				q = q_new;
			}
			if (!converged) {
				throw std::runtime_error("Moist reference vapor fixed point did not converge");
			}
			const double p_lower = p_upper + gravity * md * (1.0 + q);
			const double p_center = std::sqrt(p_lower * p_upper);
			col.qv[static_cast<size_t>(k)] = q;
			col.pressure_center[static_cast<size_t>(k)] = p_center;
			col.theta[static_cast<size_t>(k)] = temperature_k * std::pow(
				VoronoiDryHydrostatic::P0_PA / p_center,
				VoronoiDryHydrostatic::KAPPA);
			p_upper = p_lower;
		}
		col.surface_pressure_pa = p_upper;

		// Pressure was reconstructed top->bottom. Integrate hydrostatic thickness
		// bottom->top using exactly the same layer Tv convention as the active
		// moist hydrostatic operator.
		double phi = phi_surface;
		double p_lower = col.surface_pressure_pa;
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			const double md = col.dry_mass[static_cast<size_t>(k)];
			const double q = col.qv[static_cast<size_t>(k)];
			const double p_upper_layer = p_lower - gravity * md * (1.0 + q);
			if (!(p_lower > p_upper_layer) || !(p_upper_layer > 0.0)
					|| !std::isfinite(p_upper_layer)) {
				throw std::runtime_error("Moist reference produced invalid pressure interfaces");
			}
			const double tv = VoronoiMoistThermodynamics::virtual_temperature_k(
				temperature_k, q, 0.0, 0.0);
			phi += VoronoiDryHydrostatic::RD * tv
				* std::log(p_lower / p_upper_layer);
			p_lower = p_upper_layer;
		}
		col.top_geopotential_m2_s2 = phi;
		if (relative_error(p_lower, top_pressure) > 2.0e-12) {
			throw std::runtime_error("Moist reference pressure reconstruction did not close at model top");
		}
		return col;
	};

	auto bisect_monotone = [&](auto &&function, double target,
			double low, double high, const char *failure) -> double {
		double f_low = function(low) - target;
		double f_high = function(high) - target;
		int expand = 0;
		while (f_high < 0.0 && expand++ < 40) {
			high *= 2.0;
			f_high = function(high) - target;
		}
		if (!(f_low <= 0.0) || !(f_high >= 0.0)
				|| !std::isfinite(f_low) || !std::isfinite(f_high)) {
			throw std::runtime_error(failure);
		}
		for (int iter = 0; iter < ROOT_ITERS; ++iter) {
			const double mid = 0.5 * (low + high);
			const double f_mid = function(mid) - target;
			if (!std::isfinite(f_mid)) throw std::runtime_error(failure);
			if (f_mid >= 0.0) high = mid;
			else low = mid;
			if ((high - low) <= ROOT_REL_TOL * std::max(1.0, mid)) break;
		}
		return 0.5 * (low + high);
	};

	const double dry_guess = (reference_surface_pressure_pa - top_pressure) / gravity;
	const double reference_mass = bisect_monotone(
		[&](double m) { return build_column(m, 0.0).surface_pressure_pa; },
		reference_surface_pressure_pa,
		std::max(1.0e-8, dry_guess * 1.0e-4), dry_guess * 2.0,
		"Moist reference could not bracket reference surface pressure");
	const ColumnBuild reference_column = build_column(reference_mass, 0.0);
	const double target_top_phi = reference_column.top_geopotential_m2_s2;

	State state = core_->make_isothermal_reference(reference_surface_pressure_pa, temperature_k);
	VoronoiMoistThermodynamics moist(transport, indices_);
	moist.ensure_water_tracers(state);
	for (auto &tracer : state.tracer_mass_kg_m2) {
		std::fill(tracer.begin(), tracer.end(), 0.0);
	}
	std::fill(state.edge_normal_mps.begin(), state.edge_normal_mps.end(), 0.0);

	for (int c = 0; c < grid.cell_count(); ++c) {
		const double phi_s = surface_phi[static_cast<size_t>(c)];
		const double low_mass = std::max(1.0e-8, reference_mass * 1.0e-5);
		if (build_column(low_mass, phi_s).top_geopotential_m2_s2 > target_top_phi) {
			throw std::runtime_error("Terrain reaches above the configured moist model top");
		}
		const double column_mass = bisect_monotone(
			[&](double m) { return build_column(m, phi_s).top_geopotential_m2_s2; },
			target_top_phi, low_mass, reference_mass * 2.0,
			"Moist terrain column could not bracket common top geopotential");
		const ColumnBuild col = build_column(column_mass, phi_s);
		for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
			const size_t i = static_cast<size_t>(k * grid.cell_count() + c);
			const double md = col.dry_mass[static_cast<size_t>(k)];
			state.layer_mass_kg_m2[i] = md;
			state.theta_mass_kg_k_m2[i] = md * col.theta[static_cast<size_t>(k)];
			state.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)][i]
				= md * col.qv[static_cast<size_t>(k)];
		}
	}

	if (diagnostics) {
		VoronoiMoistHydrostatic hydrostatic(transport, indices_);
		const auto hydro = hydrostatic.diagnose(state);
		const auto accel = hydrostatic.pressure_gradient_acceleration(state, hydro);
		Diagnostics d;
		d.max_coordinate_mass_fraction_error = core_->max_coordinate_mass_fraction_error(state);
		d.min_surface_pressure_pa = std::numeric_limits<double>::infinity();
		d.max_surface_pressure_pa = -std::numeric_limits<double>::infinity();
		for (double ps : hydro.surface_pressure_pa) {
			d.min_surface_pressure_pa = std::min(d.min_surface_pressure_pa, ps);
			d.max_surface_pressure_pa = std::max(d.max_surface_pressure_pa, ps);
		}
		for (double a : accel) {
			d.max_pressure_acceleration_mps2 = std::max(
				d.max_pressure_acceleration_mps2, std::abs(a));
		}
		const auto thermo = moist.diagnose_thermodynamics(state);
		const auto &vapor = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
		for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
			const double qv = vapor[i] / state.layer_mass_kg_m2[i];
			const double qsat = VoronoiMoistThermodynamics::saturation_mixing_ratio(
				thermo.layer_pressure_pa[i], thermo.temperature_k[i]);
			d.max_relative_humidity_error = std::max(
				d.max_relative_humidity_error,
				std::abs(qv / qsat - relative_humidity));
		}
		*diagnostics = d;
	}
	return state;
}

} // namespace asterra::weather
