#include "voronoi_moist_thermodynamics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double MIN_THERMODYNAMIC_T_K = 150.0;
constexpr double MAX_THERMODYNAMIC_T_K = 400.0;
constexpr double ROOT_ENTHALPY_TOL_J_KG = 1.0e-7;
constexpr int ROOT_ITERATIONS = 96;
constexpr int RH_INITIALIZATION_ITERATIONS = 64;
constexpr double RH_INITIALIZATION_Q_TOL = 2.0e-13;

struct EquilibriumPartition {
	double vapor = 0.0;
	double liquid = 0.0;
	double ice = 0.0;
	double specific_enthalpy_j_kg = 0.0;
};

bool finite_nonnegative(double x) {
	return std::isfinite(x) && x >= 0.0;
}

double saturation_mixing_ratio_or_infinity(double pressure_pa, double temperature_k) {
	const double es = VoronoiMoistThermodynamics::saturation_vapor_pressure_pa(temperature_k);
	if (!(pressure_pa > 0.0) || !std::isfinite(pressure_pa)) {
		throw std::invalid_argument("Moist thermodynamics pressure must be finite and positive");
	}
	if (es >= pressure_pa) return std::numeric_limits<double>::infinity();
	return VoronoiMoistThermodynamics::EPSILON * es / (pressure_pa - es);
}

EquilibriumPartition equilibrium_partition(double pressure_pa, double temperature_k,
		double total_cloud_water_mixing_ratio) {
	if (!finite_nonnegative(total_cloud_water_mixing_ratio)) {
		throw std::invalid_argument("Moist cloud-water mixing ratio must be finite and non-negative");
	}
	const double qsat = saturation_mixing_ratio_or_infinity(pressure_pa, temperature_k);
	EquilibriumPartition e;
	e.vapor = std::min(total_cloud_water_mixing_ratio, qsat);
	const double condensate = total_cloud_water_mixing_ratio - e.vapor;
	const double fi = VoronoiMoistThermodynamics::ice_fraction(temperature_k);
	e.ice = condensate * fi;
	e.liquid = condensate - e.ice;
	e.specific_enthalpy_j_kg = VoronoiMoistThermodynamics::CP_DRY * temperature_k
		+ VoronoiMoistThermodynamics::LV0_J_KG * e.vapor
		- VoronoiMoistThermodynamics::LF0_J_KG * e.ice;
	return e;
}

bool same_partition(const EquilibriumPartition &equilibrium,
		double vapor, double liquid, double ice, double total_cloud_water) {
	const double tolerance = 1.0e-14 * std::max(1.0, total_cloud_water);
	return std::abs(equilibrium.vapor - vapor) <= tolerance
		&& std::abs(equilibrium.liquid - liquid) <= tolerance
		&& std::abs(equilibrium.ice - ice) <= tolerance;
}

} // namespace

VoronoiMoistThermodynamics::VoronoiMoistThermodynamics(
		const VoronoiDryTransport &transport, TracerIndices indices)
	: transport_(&transport), indices_(indices) {
	validate_indices();
	if (transport.grid().cell_count() <= 0) {
		throw std::invalid_argument("Moist thermodynamics requires a built dry transport grid");
	}
}

int VoronoiMoistThermodynamics::scalar_count() const {
	return VoronoiDryTransport::LEVELS * transport_->grid().cell_count();
}

void VoronoiMoistThermodynamics::validate_indices() const {
	const std::array<int, 5> all{
		indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice,
		indices_.rain, indices_.snow};
	for (int index : all) {
		if (index < 0) throw std::invalid_argument("Moist tracer indices must be non-negative");
	}
	for (size_t a = 0; a < all.size(); ++a) {
		for (size_t b = a + 1; b < all.size(); ++b) {
			if (all[a] == all[b]) {
				throw std::invalid_argument("Moist water tracer indices must be distinct");
			}
		}
	}
}

void VoronoiMoistThermodynamics::ensure_water_tracers(State &state) const {
	const size_t expected = static_cast<size_t>(scalar_count());
	if (state.layer_mass_kg_m2.size() != expected
			|| state.theta_mass_kg_k_m2.size() != expected) {
		throw std::invalid_argument("Moist thermodynamics dry scalar arrays have wrong size");
	}
	for (const auto &tracer : state.tracer_mass_kg_m2) {
		if (tracer.size() != expected) {
			throw std::invalid_argument("Moist thermodynamics encountered a malformed tracer field");
		}
	}
	const int highest = std::max({indices_.vapor, indices_.cloud_liquid,
		indices_.cloud_ice, indices_.rain, indices_.snow});
	while (static_cast<int>(state.tracer_mass_kg_m2.size()) <= highest) {
		state.tracer_mass_kg_m2.emplace_back(expected, 0.0);
	}
}

double VoronoiMoistThermodynamics::saturation_vapor_pressure_pa(double temperature_k) {
	if (!(temperature_k >= MIN_THERMODYNAMIC_T_K)
			|| !(temperature_k <= MAX_THERMODYNAMIC_T_K)
			|| !std::isfinite(temperature_k)) {
		throw std::invalid_argument("Moist saturation temperature is outside [150,400] K");
	}

	const double log_t = std::log(temperature_k);
	const double log_es_water = 54.842763 - 6763.22 / temperature_k
		- 4.210 * log_t + 0.000367 * temperature_k
		+ std::tanh(0.0415 * (temperature_k - 218.8))
			* (53.878 - 1331.22 / temperature_k - 9.44523 * log_t
				+ 0.014025 * temperature_k);
	const double log_es_ice = 9.550426 - 5723.265 / temperature_k
		+ 3.53068 * log_t - 0.00728332 * temperature_k;
	const double fi = ice_fraction(temperature_k);
	const double log_es = (1.0 - fi) * log_es_water + fi * log_es_ice;
	const double es = std::exp(log_es);
	if (!(es > 0.0) || !std::isfinite(es)) {
		throw std::runtime_error("Moist saturation vapor pressure became invalid");
	}
	return es;
}

double VoronoiMoistThermodynamics::saturation_mixing_ratio(
		double pressure_pa, double temperature_k) {
	if (!(pressure_pa > 0.0) || !std::isfinite(pressure_pa)) {
		throw std::invalid_argument("Moist saturation pressure must be finite and positive");
	}
	const double es = saturation_vapor_pressure_pa(temperature_k);
	if (!(pressure_pa > es)) {
		throw std::invalid_argument("Moist saturation mixing ratio requires p > saturation vapor pressure");
	}
	return EPSILON * es / (pressure_pa - es);
}

double VoronoiMoistThermodynamics::ice_fraction(double temperature_k) {
	if (!std::isfinite(temperature_k)) {
		throw std::invalid_argument("Moist phase temperature must be finite");
	}
	if (temperature_k <= T_ALL_ICE_K) return 1.0;
	if (temperature_k >= T_FREEZE_K) return 0.0;
	return (T_FREEZE_K - temperature_k) / (T_FREEZE_K - T_ALL_ICE_K);
}

void VoronoiMoistThermodynamics::initialize_uniform_relative_humidity(
		State &state, double relative_humidity) const {
	if (!(relative_humidity >= 0.0) || !std::isfinite(relative_humidity)) {
		throw std::invalid_argument("Moist relative humidity must be finite and non-negative");
	}
	ensure_water_tracers(state);
	const std::array<size_t, 5> water_indices{
		static_cast<size_t>(indices_.vapor),
		static_cast<size_t>(indices_.cloud_liquid),
		static_cast<size_t>(indices_.cloud_ice),
		static_cast<size_t>(indices_.rain),
		static_cast<size_t>(indices_.snow)};
	for (size_t tracer : water_indices) {
		std::fill(state.tracer_mass_kg_m2[tracer].begin(),
			state.tracer_mass_kg_m2[tracer].end(), 0.0);
	}
	if (relative_humidity == 0.0) return;

	const size_t vapor_index = static_cast<size_t>(indices_.vapor);
	bool converged = false;
	for (int iteration = 0; iteration < RH_INITIALIZATION_ITERATIONS; ++iteration) {
		const auto thermo = diagnose_thermodynamics(state);
		double max_q_change = 0.0;
		for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
			const double dry = state.layer_mass_kg_m2[i];
			const double qsat = saturation_mixing_ratio_or_infinity(
				thermo.layer_pressure_pa[i], thermo.temperature_k[i]);
			if (!std::isfinite(qsat)) {
				throw std::runtime_error("Uniform-RH initialization reached non-dilute saturation regime");
			}
			const double target_qv = relative_humidity * qsat;
			if (!finite_nonnegative(target_qv)) {
				throw std::runtime_error("Uniform-RH initialization produced invalid vapor mixing ratio");
			}
			const double previous_qv = state.tracer_mass_kg_m2[vapor_index][i] / dry;
			max_q_change = std::max(max_q_change, std::abs(target_qv - previous_qv));
			state.tracer_mass_kg_m2[vapor_index][i] = dry * target_qv;
		}
		if (max_q_change <= RH_INITIALIZATION_Q_TOL) {
			converged = true;
			break;
		}
	}
	if (!converged) {
		throw std::runtime_error("Uniform-RH initialization failed full-pressure fixed-point convergence");
	}

	const auto final_thermo = diagnose_thermodynamics(state);
	double max_rh_error = 0.0;
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double dry = state.layer_mass_kg_m2[i];
		const double qsat = saturation_mixing_ratio_or_infinity(
			final_thermo.layer_pressure_pa[i], final_thermo.temperature_k[i]);
		const double qv = state.tracer_mass_kg_m2[vapor_index][i] / dry;
		max_rh_error = std::max(max_rh_error,
			std::abs(qv / qsat - relative_humidity));
	}
	if (max_rh_error > 2.0e-10 * std::max(1.0, relative_humidity)) {
		throw std::runtime_error("Uniform-RH initialization did not close on full-pressure RH");
	}
}

double VoronoiMoistThermodynamics::total_water_mass_kg(const State &state) const {
	validate_water_state(state);
	const int cells = transport_->grid().cell_count();
	const std::array<const std::vector<double> *, 5> water{
		&state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.vapor)),
		&state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.cloud_liquid)),
		&state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.cloud_ice)),
		&state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.rain)),
		&state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.snow))};
	long double total = 0.0L;
	for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
		for (int c = 0; c < cells; ++c) {
			const size_t i = static_cast<size_t>(k * cells + c);
			double layer_water = 0.0;
			for (const auto *field : water) layer_water += (*field)[i];
			total += static_cast<long double>(layer_water)
				* static_cast<long double>(transport_->grid().cell(c).area_m2);
		}
	}
	return static_cast<double>(total);
}

VoronoiMoistThermodynamics::AdjustmentDiagnostics
VoronoiMoistThermodynamics::saturation_adjust(State &state) const {
	ensure_water_tracers(state);
	const auto thermo = diagnose_thermodynamics(state);
	const int cells = transport_->grid().cell_count();
	const size_t vapor_index = static_cast<size_t>(indices_.vapor);
	const size_t liquid_index = static_cast<size_t>(indices_.cloud_liquid);
	const size_t ice_index = static_cast<size_t>(indices_.cloud_ice);

	AdjustmentDiagnostics d;
	d.total_water_before_kg = total_water_mass_kg(state);
	d.min_temperature_k = std::numeric_limits<double>::infinity();
	d.max_temperature_k = -std::numeric_limits<double>::infinity();

	for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
		for (int c = 0; c < cells; ++c) {
			const size_t i = static_cast<size_t>(k * cells + c);
			const double dry = state.layer_mass_kg_m2[i];
			const double pressure = thermo.layer_pressure_pa[i];
			const double temperature_before = thermo.temperature_k[i];
			const double qv_before = state.tracer_mass_kg_m2[vapor_index][i] / dry;
			const double ql_before = state.tracer_mass_kg_m2[liquid_index][i] / dry;
			const double qi_before = state.tracer_mass_kg_m2[ice_index][i] / dry;
			const double qt = qv_before + ql_before + qi_before;
			if (!finite_nonnegative(qt)) {
				throw std::runtime_error("Moist saturation adjustment encountered invalid cloud-water total");
			}

			const double qsat_before = saturation_mixing_ratio_or_infinity(pressure, temperature_before);
			if (std::isfinite(qsat_before) && qsat_before > 0.0) {
				d.max_relative_humidity_before = std::max(
					d.max_relative_humidity_before, qv_before / qsat_before);
			}
			const double h_target = CP_DRY * temperature_before
				+ LV0_J_KG * qv_before - LF0_J_KG * qi_before;
			const EquilibriumPartition initial_equilibrium = equilibrium_partition(
				pressure, temperature_before, qt);
			const double initial_enthalpy_residual =
				initial_equilibrium.specific_enthalpy_j_kg - h_target;

			if (std::abs(initial_enthalpy_residual) <= ROOT_ENTHALPY_TOL_J_KG
					&& same_partition(initial_equilibrium,
						qv_before, ql_before, qi_before, qt)) {
				if (std::isfinite(qsat_before) && qsat_before > 0.0) {
					const double rh = qv_before / qsat_before;
					d.max_relative_humidity_after = std::max(d.max_relative_humidity_after, rh);
					if ((ql_before + qi_before) > 1.0e-14
							&& std::abs(qv_before - qsat_before)
								<= 1.0e-9 * std::max(qsat_before, 1.0e-12)) {
						++d.saturated_cell_count;
					}
				}
				d.min_temperature_k = std::min(d.min_temperature_k, temperature_before);
				d.max_temperature_k = std::max(d.max_temperature_k, temperature_before);
				continue;
			}

			auto residual = [&](double temperature) -> double {
				return equilibrium_partition(pressure, temperature, qt).specific_enthalpy_j_kg
					- h_target;
			};

			double lo = MIN_THERMODYNAMIC_T_K;
			double hi = MAX_THERMODYNAMIC_T_K;
			const double flo = residual(lo);
			const double fhi = residual(hi);
			if (!(flo <= 0.0 && fhi >= 0.0)) {
				throw std::runtime_error("Moist saturation adjustment could not bracket enthalpy root");
			}
			for (int iteration = 0; iteration < ROOT_ITERATIONS; ++iteration) {
				const double mid = 0.5 * (lo + hi);
				const double fm = residual(mid);
				if (fm > 0.0) hi = mid;
				else lo = mid;
			}
			const double temperature_after = 0.5 * (lo + hi);
			const EquilibriumPartition eq = equilibrium_partition(
				pressure, temperature_after, qt);
			const double cloud_water_after = eq.vapor + eq.liquid + eq.ice;
			const double local_water_error = std::abs(cloud_water_after - qt)
				/ std::max(std::abs(qt), 1.0e-15);
			d.max_relative_cell_water_error = std::max(
				d.max_relative_cell_water_error, local_water_error);
			d.max_specific_enthalpy_error_j_kg = std::max(
				d.max_specific_enthalpy_error_j_kg,
				std::abs(eq.specific_enthalpy_j_kg - h_target));

			const double condensate_before = ql_before + qi_before;
			const double condensate_after = eq.liquid + eq.ice;
			const double phase_delta_kg = (condensate_after - condensate_before)
				* dry * transport_->grid().cell(c).area_m2;
			if (phase_delta_kg > 0.0) d.condensed_water_kg += phase_delta_kg;
			else d.evaporated_water_kg += -phase_delta_kg;

			state.tracer_mass_kg_m2[vapor_index][i] = dry * eq.vapor;
			state.tracer_mass_kg_m2[liquid_index][i] = dry * eq.liquid;
			state.tracer_mass_kg_m2[ice_index][i] = dry * eq.ice;
			const double theta_after = temperature_after * std::pow(
				VoronoiDryHydrostatic::P0_PA / pressure,
				VoronoiDryHydrostatic::KAPPA);
			state.theta_mass_kg_k_m2[i] = dry * theta_after;

			const double qsat_after = saturation_mixing_ratio_or_infinity(pressure, temperature_after);
			if (std::isfinite(qsat_after) && qsat_after > 0.0) {
				const double rh_after = eq.vapor / qsat_after;
				d.max_relative_humidity_after = std::max(d.max_relative_humidity_after, rh_after);
				if (condensate_after > 1.0e-14
						&& std::abs(eq.vapor - qsat_after)
							<= 1.0e-9 * std::max(qsat_after, 1.0e-12)) {
					++d.saturated_cell_count;
				}
			}
			d.max_abs_temperature_change_k = std::max(
				d.max_abs_temperature_change_k,
				std::abs(temperature_after - temperature_before));
			d.min_temperature_k = std::min(d.min_temperature_k, temperature_after);
			d.max_temperature_k = std::max(d.max_temperature_k, temperature_after);
		}
	}

	d.total_water_after_kg = total_water_mass_kg(state);
	d.relative_total_water_error = std::abs(d.total_water_after_kg - d.total_water_before_kg)
		/ std::max(std::abs(d.total_water_before_kg), 1.0);
	if (!std::isfinite(d.relative_total_water_error)
			|| d.relative_total_water_error > 5.0e-12
			|| d.max_relative_cell_water_error > 5.0e-12
			|| d.max_specific_enthalpy_error_j_kg > 1.0e-4) {
		throw std::runtime_error("Moist saturation adjustment failed conservation/enthalpy gate");
	}
	return d;
}

} // namespace asterra::weather
