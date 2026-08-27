#include "voronoi_moist_energy.h"

#include <cmath>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double CV_DRY = VoronoiDryHydrostatic::CP - VoronoiDryHydrostatic::RD;
}

VoronoiMoistEnergyDiagnostics::VoronoiMoistEnergyDiagnostics(
		const VoronoiDryCore &core, TracerIndices indices)
	: core_(&core), indices_(indices) {
	VoronoiMoistThermodynamics validate(core.transport(), indices);
	(void)validate;
}

VoronoiMoistEnergyDiagnostics::Diagnostics
VoronoiMoistEnergyDiagnostics::diagnose(const State &state) const {
	VoronoiMoistHydrostatic hydrostatic(core_->transport(), indices_);
	const auto hydro = hydrostatic.diagnose(state);
	const auto &grid = core_->grid();
	const int cells = grid.cell_count();
	const int edges = grid.edge_count();
	const auto &vapor = state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.vapor));
	const auto &cloud_ice = state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.cloud_ice));
	const auto &snow = state.tracer_mass_kg_m2.at(static_cast<size_t>(indices_.snow));

	Diagnostics d;
	long double internal = 0.0L;
	long double latent = 0.0L;
	long double potential = 0.0L;
	long double kinetic = 0.0L;

	for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
		for (int c = 0; c < cells; ++c) {
			const size_t i = static_cast<size_t>(k * cells + c);
			long double edge_ke_sum = 0.0L;
			for (int e : grid.cell(c).edges) {
				const double u = state.edge_normal_mps[
					static_cast<size_t>(k * edges + e)];
				if (!std::isfinite(u)) {
					throw std::runtime_error("Moist energy received non-finite edge wind");
				}
				edge_ke_sum += static_cast<long double>(grid.edge(e).edge_area_m2)
					* static_cast<long double>(u * u);
			}
			const double cell_ke = static_cast<double>(edge_ke_sum
				/ (4.0L * static_cast<long double>(grid.cell(c).area_m2)));
			const double dry_mass = state.layer_mass_kg_m2[i];
			const double total_mass = hydro.layer_total_mass_kg_m2[i];
			const double area = grid.cell(c).area_m2;

			internal += static_cast<long double>(area)
				* static_cast<long double>(dry_mass)
				* static_cast<long double>(CV_DRY * hydro.temperature_k[i]);
			latent += static_cast<long double>(area)
				* (static_cast<long double>(VoronoiMoistThermodynamics::LV0_J_KG)
					* static_cast<long double>(vapor[i])
					- static_cast<long double>(VoronoiMoistThermodynamics::LF0_J_KG)
					* static_cast<long double>(cloud_ice[i] + snow[i]));
			potential += static_cast<long double>(area)
				* static_cast<long double>(total_mass)
				* static_cast<long double>(hydro.layer_geopotential[i]);
			kinetic += static_cast<long double>(area)
				* static_cast<long double>(total_mass)
				* static_cast<long double>(cell_ke);
		}
	}

	d.internal_energy_j = static_cast<double>(internal);
	d.latent_reference_energy_j = static_cast<double>(latent);
	d.potential_energy_j = static_cast<double>(potential);
	d.kinetic_energy_j = static_cast<double>(kinetic);
	d.total_energy_j = d.internal_energy_j + d.latent_reference_energy_j
		+ d.potential_energy_j + d.kinetic_energy_j;
	if (!std::isfinite(d.internal_energy_j)
			|| !std::isfinite(d.latent_reference_energy_j)
			|| !std::isfinite(d.potential_energy_j)
			|| !std::isfinite(d.kinetic_energy_j)
			|| !std::isfinite(d.total_energy_j)) {
		throw std::runtime_error("Moist energy diagnostic produced a non-finite component");
	}
	return d;
}

} // namespace asterra::weather
