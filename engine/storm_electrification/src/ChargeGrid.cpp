#include "orbit/storm_electrification/ChargeGrid.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::storm_electrification
{

ChargeGrid::ChargeGrid(std::uint32_t nx, std::uint32_t ny, std::uint32_t nz, double spacing)
    : nx_(nx), ny_(ny), nz_(nz), dx_(spacing),
      rhoIce_(static_cast<std::size_t>(nx) * ny * nz, 0.0),
      rhoGraupel_(rhoIce_.size(), 0.0),
      phi_(rhoIce_.size(), 0.0)
{
}

namespace
{
// y = A x with A = 6 x_c - sum of face neighbours (outside the block counts as 0): the Dirichlet 7-point Laplacian.
void Apply(const ChargeGrid& g, const std::vector<double>& x, std::vector<double>& y)
{
    const std::uint32_t nx = g.Nx(), ny = g.Ny(), nz = g.Nz();
    for (std::uint32_t k = 0; k < nz; ++k)
        for (std::uint32_t j = 0; j < ny; ++j)
            for (std::uint32_t i = 0; i < nx; ++i)
            {
                const std::size_t c = g.Index(i, j, k);
                double sum = 6.0 * x[c];
                if (i > 0) sum -= x[c - 1];
                if (i + 1 < nx) sum -= x[c + 1];
                if (j > 0) sum -= x[c - nx];
                if (j + 1 < ny) sum -= x[c + nx];
                if (k > 0) sum -= x[c - static_cast<std::size_t>(nx) * ny];
                if (k + 1 < nz) sum -= x[c + static_cast<std::size_t>(nx) * ny];
                y[c] = sum;
            }
}
double Dot(const std::vector<double>& a, const std::vector<double>& b)
{
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
    return s;
}
} // namespace

void ChargeGrid::MarkRefractory(const std::vector<unsigned char>& mask)
{
    if (refractory_.size() != mask.size()) { refractory_.assign(mask.size(), 0U); }
    for (std::size_t c = 0; c < mask.size(); ++c) { refractory_[c] = static_cast<unsigned char>(refractory_[c] | mask[c]); }
}

int ChargeGrid::SolvePotential(double tolerance, int maxIterations)
{
    refractory_.clear();
    const std::size_t n = CellCount();
    std::vector<double> b(n), r(n), p(n), ap(n);
    const double scale = dx_ * dx_ / kVacuumPermittivity;
    for (std::size_t c = 0; c < n; ++c) b[c] = SpaceCharge(c) * scale;
    const double bNorm = std::sqrt(Dot(b, b));
    if (bNorm == 0.0)
    {
        std::fill(phi_.begin(), phi_.end(), 0.0);
        converged_ = true;
        return 0;
    }
    Apply(*this, phi_, ap);
    for (std::size_t c = 0; c < n; ++c) { r[c] = b[c] - ap[c]; p[c] = r[c]; }
    double rr = Dot(r, r);
    int it = 0;
    converged_ = std::sqrt(rr) <= tolerance * bNorm;
    while (!converged_ && it < maxIterations)
    {
        Apply(*this, p, ap);
        const double alpha = rr / Dot(p, ap);
        for (std::size_t c = 0; c < n; ++c) { phi_[c] += alpha * p[c]; r[c] -= alpha * ap[c]; }
        const double rrNew = Dot(r, r);
        ++it;
        if (std::sqrt(rrNew) <= tolerance * bNorm) { converged_ = true; break; }
        const double beta = rrNew / rr;
        for (std::size_t c = 0; c < n; ++c) p[c] = r[c] + beta * p[c];
        rr = rrNew;
    }
    return it;
}

void ChargeGrid::Field(std::uint32_t i, std::uint32_t j, std::uint32_t k, double out[3]) const
{
    const auto at = [&](long x, long y, long z) -> double {
        if (x < 0 || y < 0 || z < 0 || x >= static_cast<long>(nx_) || y >= static_cast<long>(ny_) || z >= static_cast<long>(nz_))
            return 0.0;
        return phi_[Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z))];
    };
    const long x = i, y = j, z = k;
    const double inv = 1.0 / (2.0 * dx_);
    out[0] = -(at(x + 1, y, z) - at(x - 1, y, z)) * inv;
    out[1] = -(at(x, y + 1, z) - at(x, y - 1, z)) * inv;
    out[2] = -(at(x, y, z + 1) - at(x, y, z - 1)) * inv;
}

double ChargeGrid::FieldMagnitude(std::uint32_t i, std::uint32_t j, std::uint32_t k) const
{
    double e[3];
    Field(i, j, k, e);
    return std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
}

double ChargeGrid::TotalCharge() const
{
    double q = 0.0;
    for (std::size_t c = 0; c < CellCount(); ++c) q += SpaceCharge(c);
    return q * dx_ * dx_ * dx_;
}

double ChargeGrid::FieldEnergy() const
{
    double w = 0.0;
    for (std::size_t c = 0; c < CellCount(); ++c) w += SpaceCharge(c) * phi_[c];
    return 0.5 * w * dx_ * dx_ * dx_;
}

} // namespace orbit::storm_electrification
