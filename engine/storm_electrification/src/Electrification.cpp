#include "orbit/storm_electrification/Electrification.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::storm_electrification
{

namespace
{
double SmoothRamp(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }

// First-order upwind advection of one carrier along z with per-cell vertical velocity (positive up). Flux through a
// face is taken from the upwind cell, so total charge is conserved except across the block's top and bottom faces,
// which are reported.
void AdvectVertical(const ChargeGrid& g, std::vector<double>& rho, const std::vector<float>& updraft, double fall,
                    double dt, double& top, double& ground)
{
    const std::uint32_t nx = g.Nx(), ny = g.Ny(), nz = g.Nz();
    std::vector<double> flux(static_cast<std::size_t>(nz) + 1);
    for (std::uint32_t j = 0; j < ny; ++j)
    {
        for (std::uint32_t i = 0; i < nx; ++i)
        {
            // flux[k] = charge density flux through the bottom face of cell k (positive up), face 0 is the ground.
            for (std::uint32_t k = 0; k <= nz; ++k)
            {
                const std::uint32_t lower = k == 0 ? 0 : k - 1;
                const std::uint32_t upper = k == nz ? nz - 1 : k;
                const double wl = updraft[g.Index(i, j, lower)] + fall;
                const double wu = updraft[g.Index(i, j, upper)] + fall;
                const double w = 0.5 * (wl + wu);
                double f;
                if (k == 0)
                {
                    f = std::min(w, 0.0) * rho[g.Index(i, j, 0)];      // only downward flow leaves; nothing enters
                }
                else if (k == nz)
                {
                    f = std::max(w, 0.0) * rho[g.Index(i, j, nz - 1)]; // only upward flow leaves; nothing enters
                }
                else
                {
                    f = w >= 0.0 ? w * rho[g.Index(i, j, k - 1)] : w * rho[g.Index(i, j, k)];
                }
                flux[k] = f;
            }
            for (std::uint32_t k = 0; k < nz; ++k)
            {
                rho[g.Index(i, j, k)] += dt * (flux[k] - flux[k + 1]) / g.Spacing();
            }
            ground += -flux[0] * dt * g.Spacing() * g.Spacing();   // flux[0] <= 0: charge density leaving downward
            top += flux[nz] * dt * g.Spacing() * g.Spacing();
        }
    }
}
} // namespace

ChargeBudget StepElectrification(
    ChargeGrid& grid, const StormFields& fields, const ElectrificationParams& p, double dt)
{
    ChargeBudget budget;
    const std::size_t n = grid.CellCount();

    // Charging: graupel takes sign*dq, the ice it collided with takes -sign*dq, so every exchange is neutral.
    for (std::size_t c = 0; c < n; ++c)
    {
        const double temperature = fields.temperatureK[c];
        if (temperature >= p.iceOnsetK) continue;
        const double sign = -std::tanh((temperature - p.reversalTemperatureK) / p.reversalWidthK);
        const double riming = SmoothRamp(fields.liquid[c] / p.liquidRamp);
        const double dq = p.chargingEfficiency * fields.airDensity[c] * fields.graupel[c] * fields.ice[c] * riming * dt;
        grid.GraupelCharge()[c] += sign * dq;
        grid.IceCharge()[c] -= sign * dq;
    }

    // Transport, sub-stepped so the fastest carrier moves less than one cell per sub-step.
    double fastest = 1.0e-6;
    for (std::size_t c = 0; c < n; ++c)
        fastest = std::max({fastest, std::abs(fields.updraft[c] + p.graupelFallSpeed), std::abs(fields.updraft[c] + p.iceFallSpeed)});
    const int sub = std::max(1, static_cast<int>(std::ceil(dt * fastest / (0.9 * grid.Spacing()))));
    const double h = dt / static_cast<double>(sub);
    double top = 0.0, ground = 0.0;
    for (int s = 0; s < sub; ++s)
    {
        AdvectVertical(grid, grid.IceCharge(), fields.updraft, -p.iceFallSpeed, h, top, ground);
        AdvectVertical(grid, grid.GraupelCharge(), fields.updraft, -p.graupelFallSpeed, h, top, ground);
    }
    budget.leftThroughTop = top;
    budget.groundDeposit = ground;
    return budget;
}

} // namespace orbit::storm_electrification
