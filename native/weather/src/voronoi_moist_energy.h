#pragma once

#include "voronoi_dry_core.h"
#include "voronoi_moist_hydrostatic.h"

namespace asterra::weather {

// Diagnostic global budgets for the current dilute moist dynamical model.
// This is intentionally diagnostic-only: it does not impose an acceptance gate
// on SSPRK3. Energy follows the active moist pressure/source conventions:
//   dry internal: Cv_d T * m_d
//   latent reference: +Lv m_v - Lf (m_ci + m_snow)
//   potential: Phi * (m_d + all suspended water)
//   kinetic: K * (m_d + all suspended water)
// Relative/absolute axial angular momentum also use total suspended mass.
class VoronoiMoistEnergyDiagnostics {
public:
	using State = VoronoiDryCore::State;
	using TracerIndices = VoronoiMoistThermodynamics::TracerIndices;

	struct Diagnostics {
		double internal_energy_j = 0.0;
		double latent_reference_energy_j = 0.0;
		double potential_energy_j = 0.0;
		double kinetic_energy_j = 0.0;
		double total_energy_j = 0.0;
		double relative_axial_angular_momentum_kg_m2_s = 0.0;
		double absolute_axial_angular_momentum_kg_m2_s = 0.0;
	};

	explicit VoronoiMoistEnergyDiagnostics(const VoronoiDryCore &core,
		TracerIndices indices = {});

	Diagnostics diagnose(const State &state) const;
	double total_energy_j(const State &state) const {
		return diagnose(state).total_energy_j;
	}

private:
	const VoronoiDryCore *core_ = nullptr;
	TracerIndices indices_{};
};

} // namespace asterra::weather
