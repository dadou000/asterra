#pragma once

#include "geodesic_voronoi_grid.h"

#include <array>
#include <vector>

namespace asterra::weather {

// Dry hydrostatic thermodynamic column on the geodesic Voronoi mesh.
//
// This deliberately contains no time integration yet. It establishes the
// pressure-coordinate geometry and the identities the future primitive-
// equation core must preserve before advection and vertical motion are added.
class VoronoiDryHydrostatic {
public:
	static constexpr int LEVELS = 30;
	static constexpr int INTERFACES = LEVELS + 1;
	static constexpr double P0_PA = 100000.0;
	static constexpr double RD = 287.05;
	static constexpr double CP = 1004.0;
	static constexpr double KAPPA = RD / CP;

	// Shared with the runtime 30-level layout. Interfaces are generated from
	// geometric midpoints of these nominal centre heights with z=0 at the
	// surface and one half-spacing above the top centre.
	static constexpr std::array<double, LEVELS> NOMINAL_HEIGHT_M = {
		100.0, 250.0, 450.0, 700.0, 1000.0, 1350.0, 1750.0, 2200.0,
		2700.0, 3250.0, 3850.0, 4500.0, 5200.0, 5950.0, 6750.0, 7600.0,
		8500.0, 9400.0, 10300.0, 11200.0, 12100.0, 13000.0, 13900.0, 14800.0,
		15700.0, 16600.0, 17500.0, 18400.0, 19300.0, 20200.0
	};

	struct State {
		std::vector<double> surface_pressure_pa;     // cell centred
		std::vector<double> potential_temperature_k; // [level][cell]
		std::vector<double> edge_normal_mps;         // [level][edge]
	};

	struct Diagnostics {
		std::vector<double> interface_pressure_pa; // [interface][cell], 0=surface
		std::vector<double> layer_pressure_pa;     // log-mean pressure [level][cell]
		std::vector<double> temperature_k;         // [level][cell]
		std::vector<double> layer_mass_kg_m2;       // dp/g [level][cell]
		std::vector<double> interface_geopotential; // [interface][cell], m2/s2
		std::vector<double> layer_geopotential;     // [level][cell], m2/s2
	};

	VoronoiDryHydrostatic(const GeodesicVoronoiGrid &grid,
		double gravity_mps2 = 9.80665,
		double scale_height_m = 8000.0);

	State make_isothermal_reference(double surface_pressure_pa,
		double temperature_k) const;
	Diagnostics diagnose(const State &state) const;

	// Horizontal pressure-gradient acceleration at each normal-velocity edge.
	// Primitive-equation pressure force at constant sigma:
	//   -grad_sigma(Phi) - Rd*T*grad_sigma(ln p)
	// with edge temperature arithmetic-averaged from the adjacent cells.
	std::vector<double> pressure_gradient_acceleration(
		const State &state, const Diagnostics &diag) const;

	double total_dry_air_mass_kg(const State &state) const;
	double top_sigma() const { return sigma_interface_[LEVELS]; }
	const std::array<double, INTERFACES> &sigma_interfaces() const { return sigma_interface_; }
	const GeodesicVoronoiGrid &grid() const { return *grid_; }

private:
	const GeodesicVoronoiGrid *grid_ = nullptr;
	double gravity_mps2_ = 9.80665;
	double scale_height_m_ = 8000.0;
	std::array<double, INTERFACES> interface_height_m_{};
	std::array<double, INTERFACES> sigma_interface_{};

	int scalar_index(int level, int cell) const { return level * grid_->cell_count() + cell; }
	int interface_index(int interface_level, int cell) const {
		return interface_level * grid_->cell_count() + cell;
	}
	int edge_index(int level, int edge) const { return level * grid_->edge_count() + edge; }
	void validate(const State &state) const;
};

} // namespace asterra::weather
