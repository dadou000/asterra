#include "shallow_water_cgrid.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
using asterra::weather::CubedSphereGrid;
using asterra::weather::ShallowWaterCGrid;
using asterra::weather::Vec3d;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::normalized;

int main() {
    constexpr double PI = 3.14159265358979323846;
    constexpr double R = 3500000.0;
    constexpr double G = 9.80665;
    constexpr double ROT = 11.5 * 3600.0;
    constexpr double OMEGA = 2.0 * PI / ROT;
    bool all_ok = true;
    for (int n : {12, 24}) {
        CubedSphereGrid grid(n, R);
        const Vec3d axis = normalized(Vec3d{0.31, 0.89, -0.335});
        ShallowWaterCGrid sw(grid, G, OMEGA, axis);
        constexpr double H0 = 3000.0;
        const double w = OMEGA / 12.0;
        const double u0 = w * R;
        const double amp = (OMEGA * w + 0.5 * w * w) * R * R / G;
        auto state = sw.make_uniform_state(H0);
        for (int c = 0; c < grid.cell_count(); ++c) {
            const double mu = dot(axis, grid.cell(c).center);
            state.depth_m[c] = H0 - amp * mu * mu;
        }
        state.edge_normal_mps = sw.topology().sample_edge_normal_velocity(
            [axis, u0](const Vec3d &p) { return cross(axis, p) * u0; });
        const double mass0 = sw.total_volume_m3(state);
        const double energy0 = sw.total_energy(state);
        double elapsed = 0.0;
        int steps = 0;
        try {
            while (elapsed < 5.0 * ROT) {
                const auto d = sw.step(state, std::min(600.0, 5.0 * ROT - elapsed), 0.34);
                elapsed += d.accepted_dt_s;
                ++steps;
            }
            std::cerr << "PROBE PASS N" << n << " steps=" << steps
                      << " mass=" << std::abs(sw.total_volume_m3(state)-mass0)/mass0
                      << " energy=" << std::abs(sw.total_energy(state)-energy0)/std::abs(energy0) << "\n";
        } catch (const std::exception &e) {
            double min_h = 1e300, max_h = -1e300, max_u = 0.0;
            for (double h : state.depth_m) { min_h = std::min(min_h,h); max_h = std::max(max_h,h); }
            for (double u : state.edge_normal_mps) max_u = std::max(max_u,std::abs(u));
            std::cerr << "PROBE FAIL N" << n << " rotations=" << elapsed/ROT << " steps=" << steps
                      << " minH=" << min_h << " maxH=" << max_h << " maxU=" << max_u
                      << " mass=" << std::abs(sw.total_volume_m3(state)-mass0)/mass0
                      << " energy=" << std::abs(sw.total_energy(state)-energy0)/std::abs(energy0)
                      << " error=" << e.what() << "\n";
            all_ok = false;
        }
    }
    return all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
