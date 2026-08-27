#pragma once

#include "voronoi_moist_thermodynamics.h"

#include <vector>

namespace asterra::weather {

// Moist hydrostatic column for the replacement dry-mass coordinate.
//
// Interface mechanical pressure includes the weight of dry air plus suspended
// vapor/liquid/ice, while hydrostatic thickness uses the exact dry-air-basis
// virtual temperature. Sensible temperature remains diagnosed from the
// prognostic dry-coordinate theta state, so adding water is not an implicit heat
// source. The zero-water limit is required to reproduce VoronoiDryTransport
// exactly. VoronoiDryDynamics can opt into this diagnosis at every SSPRK stage.
class VoronoiMoistHydrostatic {
public:
	using State = VoronoiDryTransport::State;
	using TracerIndices = VoronoiMoistThermodynamics::TracerIndices;
	static constexpr int LEVELS = VoronoiDryTransport::LEVELS;
	static constexpr int INTERFACES = LEVELS + 1;

	struct Diagnostics {
		std::vector<double> surface_pressure_pa;      // [cell], total mechanical pressure
		std::vector<double> interface_pressure_pa;    // [interface][cell]
		std::vector<double> layer_pressure_pa;        // [level][cell]
		std::vector<double> potential_temperature_k;  // [level][cell], dry coordinate
		std::vector<double> temperature_k;            // [level][cell], sensible T
		std::vector<double> virtual_temperature_k;    // [level][cell]
		std::vector<double> layer_total_mass_kg_m2;   // dry + all water species
		std::vector<double> interface_geopotential;   // [interface][cell], m2/s2
		std::vector<double> layer_geopotential;       // [level][cell], m2/s2
	};

	explicit VoronoiMoistHydrostatic(const VoronoiDryTransport &transport,
		TracerIndices indices = {});

	Diagnostics diagnose(const State &state) const;

	// Moist primitive-equation pressure acceleration on the dry-mass coordinate:
	//   -grad(Phi) - Rd*Tv*grad(ln p).
	//
	// This is the discrete reduction of the dry-mass-coordinate flux-form term
	//   -alpha grad(p)
	//   -(alpha/alpha_d) (p_eta/mu_d) grad(Phi).
	// Because p_eta/mu_d = m_total/m_d = 1 + qt and
	// alpha/alpha_d = 1/(1 + qt), the coordinate factors cancel exactly. Also
	// alpha = Rd*Tv/p, yielding the compact expression above. This cancellation
	// is why dry mass can remain the conservative coordinate while full moist
	// mechanical pressure and condensate loading feed horizontal momentum.
	std::vector<double> pressure_gradient_acceleration(
		const State &state, const Diagnostics &diagnostics) const;

	double total_moist_air_mass_kg(const State &state) const;
	const TracerIndices &tracer_indices() const { return indices_; }

private:
	const VoronoiDryTransport *transport_ = nullptr;
	TracerIndices indices_{};

	int scalar_index(int level, int cell) const {
		return level * transport_->grid().cell_count() + cell;
	}
	int interface_index(int interface_level, int cell) const {
		return interface_level * transport_->grid().cell_count() + cell;
	}
	int edge_index(int level, int edge) const {
		return level * transport_->grid().edge_count() + edge;
	}
	void validate_water_shape(const State &state) const;
};

} // namespace asterra::weather
