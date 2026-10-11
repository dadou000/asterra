#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orbit::storm_electrification
{

inline constexpr double kVacuumPermittivity = 8.8541878128e-12;  // F/m

// Uniform Cartesian block holding the space charge of a storm and its electric potential. Cell (i, j, k) has its
// centre at ((i + 0.5) dx, (j + 0.5) dx, (k + 0.5) dx); z = 0 is the ground, which together with every other face of
// the block is held at 0 V (callers pad the block around the storm so the side and top boundaries are far away).
//
// Charge lives on two carriers because charging separates them: ice crystals are lofted by the updraft, graupel
// falls. rho = ice + graupel is the space charge the field sees. Everything is SI.
class ChargeGrid
{
public:
    ChargeGrid(std::uint32_t nx, std::uint32_t ny, std::uint32_t nz, double spacing);

    std::uint32_t Nx() const { return nx_; }
    std::uint32_t Ny() const { return ny_; }
    std::uint32_t Nz() const { return nz_; }
    double Spacing() const { return dx_; }
    std::size_t CellCount() const { return rhoIce_.size(); }
    std::size_t Index(std::uint32_t i, std::uint32_t j, std::uint32_t k) const
    {
        return (static_cast<std::size_t>(k) * ny_ + j) * nx_ + i;
    }
    double Height(std::uint32_t k) const { return (static_cast<double>(k) + 0.5) * dx_; }

    std::vector<double>& IceCharge() { return rhoIce_; }
    std::vector<double>& GraupelCharge() { return rhoGraupel_; }
    const std::vector<double>& IceCharge() const { return rhoIce_; }
    const std::vector<double>& GraupelCharge() const { return rhoGraupel_; }
    double SpaceCharge(std::size_t cell) const { return rhoIce_[cell] + rhoGraupel_[cell]; }

    // Solves  -div(eps0 grad phi) = rho  by conjugate gradients (warm-started from the previous potential).
    // Returns the iteration count; `converged` reports whether the relative residual reached `tolerance`.
    int SolvePotential(double tolerance = 1.0e-7, int maxIterations = 600);
    bool Converged() const { return converged_; }
    const std::vector<double>& Potential() const { return phi_; }

    // Field (V/m) at a cell centre by central differences; a face neighbour outside the block is 0 V.
    void Field(std::uint32_t i, std::uint32_t j, std::uint32_t k, double out[3]) const;
    double FieldMagnitude(std::uint32_t i, std::uint32_t j, std::uint32_t k) const;

    double TotalCharge() const;      // C, both carriers
    double FieldEnergy() const;      // J, 1/2 integral rho phi dV (uses the current potential)

private:
    std::uint32_t nx_, ny_, nz_;
    double dx_;
    std::vector<double> rhoIce_, rhoGraupel_, phi_;
    bool converged_ = false;
};

} // namespace orbit::storm_electrification
