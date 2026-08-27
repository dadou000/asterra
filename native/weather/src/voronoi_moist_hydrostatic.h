#pragma once

#include "voronoi_moist_thermodynamics.h"

#include <vector>

namespace asterra::weather {

// Diagnostic moist hydrostatic column for the replacement dry-mass coordinate.
//
// This class is intentionally parallel to VoronoiDryTransport::diagnose_hydrostatic
// and is not yet used by the production momentum step. Interface mechanical
// pressure includes the weight of dry air plus suspended vapor/liquid/ice, while
// hydrostatic thickness uses the exact dry-air-basis virtual temperature. The
// zero-water limit must reproduce the dry diagnostic before this operator is
// allowed to feed dynamics.
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
		std::vector<double> potential_temperature_k;  // [level][cell]
		std::vector<double> temperature_k;            // [level][cell]
		std::vector<double> virtual_temperature_k;    // [level][cell]
		std::vector<double> layer_total_mass_kg_m2;   // dry + all water species
		std::vector<double> interface_geopotential;   // [interface][cell], m2/s2
		std::vector<double> layer_geopotential;       // [level][cell], m2/s2
	};

	explicit VoronoiMoistHydrostatic(const VoronoiDryTransport &transport,
		TracerIndices indices = {});

	Diagnostics diagnose(const State &state) const;

	// Moist primitive-equation pressure acceleration evaluated from the parallel
	// diagnostic state:
	//   -grad(Phi) - Rd*Tv*grad(ln p).
	// This is diagnostic-only until dry-mass momentum/remap coupling is migrated.
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
