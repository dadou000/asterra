#include <orbit/weather_lab/FastStormSolver.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <thread>

namespace orbit::weather_lab
{
namespace
{
using Clock = std::chrono::steady_clock;
using Complex = std::complex<double>;

constexpr double kPi = 3.14159265358979323846;

double MillisecondsSince(const Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
}

bool IsPowerOfTwo(const std::uint32_t v)
{
    return v >= 4U && (v & (v - 1U)) == 0U;
}

// ---------------------------------------------------------------- parallel

void ParallelFor(
    const std::uint32_t threads,
    const std::uint32_t count,
    const std::function<void(std::uint32_t, std::uint32_t)>& body)
{
    const std::uint32_t workers = std::min(std::max(threads, 1U), count);
    if (workers <= 1U)
    {
        body(0U, count);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(workers - 1U);
    for (std::uint32_t w = 1U; w < workers; ++w)
    {
        const std::uint32_t begin = count * w / workers;
        const std::uint32_t end = count * (w + 1U) / workers;
        pool.emplace_back([&body, begin, end] { body(begin, end); });
    }
    body(0U, count / workers);
    for (auto& t : pool)
    {
        t.join();
    }
}

// ----------------------------------------------------------------- sampling

struct FieldView
{
    const float* data;
    std::int32_t nx;
    std::int32_t ny;
    std::int32_t nz; // number of levels held by this field

    [[nodiscard]] const float* Row(
        const std::int32_t j, const std::int32_t k) const
    {
        return data + (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny)
            + static_cast<std::size_t>(j & (ny - 1))) * static_cast<std::size_t>(nx);
    }
};

inline std::int32_t ClampLevel(const std::int32_t k, const std::int32_t n)
{
    return k < 0 ? 0 : (k >= n ? n - 1 : k);
}

float Trilinear(
    const FieldView f, const float ix, const float iy, const float iz)
{
    const float fx = std::floor(ix);
    const float fy = std::floor(iy);
    const float fz = std::floor(iz);
    const auto i0 = static_cast<std::int32_t>(fx);
    const auto j0 = static_cast<std::int32_t>(fy);
    const auto k0 = static_cast<std::int32_t>(fz);
    const float tx = ix - fx;
    const float ty = iy - fy;
    const float tz = iz - fz;
    const std::int32_t ia = i0 & (f.nx - 1);
    const std::int32_t ib = (i0 + 1) & (f.nx - 1);
    float plane[2];
    for (std::int32_t dk = 0; dk < 2; ++dk)
    {
        const std::int32_t k = ClampLevel(k0 + dk, f.nz);
        const float* r0 = f.Row(j0, k);
        const float* r1 = f.Row(j0 + 1, k);
        const float a = r0[ia] + (r0[ib] - r0[ia]) * tx;
        const float b = r1[ia] + (r1[ib] - r1[ia]) * tx;
        plane[dk] = a + (b - a) * ty;
    }
    return plane[0] + (plane[1] - plane[0]) * tz;
}

inline float CatmullRom(
    const float p0, const float p1, const float p2, const float p3,
    const float t)
{
    return p1 + 0.5F * t * (p2 - p0 + t * (2.0F * p0 - 5.0F * p1 + 4.0F * p2
        - p3 + t * (3.0F * (p1 - p2) + p3 - p0)));
}

// Catmull-Rom in all three axes, clamped to the range of the eight enclosing
// samples so advection never creates new extrema.
float MonotoneCubic(
    const FieldView f, const float ix, const float iy, const float iz)
{
    const float fx = std::floor(ix);
    const float fy = std::floor(iy);
    const float fz = std::floor(iz);
    const auto i0 = static_cast<std::int32_t>(fx);
    const auto j0 = static_cast<std::int32_t>(fy);
    const auto k0 = static_cast<std::int32_t>(fz);
    const float tx = ix - fx;
    const float ty = iy - fy;
    const float tz = iz - fz;
    std::int32_t xi[4];
    for (std::int32_t d = 0; d < 4; ++d)
    {
        xi[d] = (i0 + d - 1) & (f.nx - 1);
    }
    float lo = 3.4e38F;
    float hi = -3.4e38F;
    float alongZ[4];
    for (std::int32_t dk = 0; dk < 4; ++dk)
    {
        const std::int32_t k = ClampLevel(k0 + dk - 1, f.nz);
        float alongY[4];
        for (std::int32_t dj = 0; dj < 4; ++dj)
        {
            const float* r = f.Row(j0 + dj - 1, k);
            const float a = r[xi[0]];
            const float b = r[xi[1]];
            const float c = r[xi[2]];
            const float d = r[xi[3]];
            alongY[dj] = CatmullRom(a, b, c, d, tx);
            if (dk >= 1 && dk <= 2 && dj >= 1 && dj <= 2)
            {
                lo = std::min(lo, std::min(b, c));
                hi = std::max(hi, std::max(b, c));
            }
        }
        alongZ[dk] = CatmullRom(
            alongY[0], alongY[1], alongY[2], alongY[3], ty);
    }
    const float value = CatmullRom(
        alongZ[0], alongZ[1], alongZ[2], alongZ[3], tz);
    return std::min(std::max(value, lo), hi);
}

// ----------------------------------------------------------------------- FFT

struct Fft
{
    std::uint32_t n = 0;
    std::vector<std::uint32_t> reverse;
    std::vector<Complex> twiddle;

    void Prepare(const std::uint32_t size)
    {
        n = size;
        reverse.resize(n);
        std::uint32_t bits = 0;
        while ((1U << bits) < n)
        {
            ++bits;
        }
        for (std::uint32_t i = 0; i < n; ++i)
        {
            std::uint32_t r = 0;
            for (std::uint32_t b = 0; b < bits; ++b)
            {
                r |= ((i >> b) & 1U) << (bits - 1U - b);
            }
            reverse[i] = r;
        }
        twiddle.resize(n / 2U);
        for (std::uint32_t i = 0; i < n / 2U; ++i)
        {
            const double a = -2.0 * kPi * static_cast<double>(i)
                / static_cast<double>(n);
            twiddle[i] = Complex(std::cos(a), std::sin(a));
        }
    }

    void Transform(Complex* data, const bool inverse) const
    {
        for (std::uint32_t i = 0; i < n; ++i)
        {
            if (reverse[i] > i)
            {
                std::swap(data[i], data[reverse[i]]);
            }
        }
        for (std::uint32_t len = 2U; len <= n; len <<= 1U)
        {
            const std::uint32_t half = len / 2U;
            const std::uint32_t stride = n / len;
            for (std::uint32_t start = 0; start < n; start += len)
            {
                for (std::uint32_t k = 0; k < half; ++k)
                {
                    Complex w = twiddle[k * stride];
                    if (inverse)
                    {
                        w = std::conj(w);
                    }
                    const Complex a = data[start + k];
                    const Complex b = data[start + k + half] * w;
                    data[start + k] = a + b;
                    data[start + k + half] = a - b;
                }
            }
        }
    }
};
} // namespace

// ------------------------------------------------------------------- solver

struct FastStormSolver::Impl
{
    FastStormConfig cfg;
    BaseState base;
    std::uint32_t threads = 1;
    std::size_t cells = 0;
    std::size_t faceCells = 0;

    // Prognostic state (u,v at x/y faces, w at z faces, rest at centres).
    std::vector<float> u, v, w, theta, qv, qc, qr;
    // Double buffers for semi-Lagrangian advection.
    std::vector<float> u2, v2, w2, theta2, qv2, qc2, qr2;
    std::vector<float> phi;
    std::vector<float> divergence;
    std::vector<float> rainAccumulation; // kg/m^2 per column
    std::vector<float> scratchCentre;

    // Pressure solver data.
    Fft fftX, fftY;
    std::vector<Complex> spectrum;
    std::vector<double> lowerCoeff, upperCoeff;
    std::vector<double> lambdaX, lambdaY;

    FastStormDiagnostics diag;

    explicit Impl(const FastStormConfig& config);

    [[nodiscard]] std::size_t Index(
        const std::uint32_t i, const std::uint32_t j, const std::uint32_t k) const
    {
        return (static_cast<std::size_t>(k) * cfg.ny + j) * cfg.nx + i;
    }

    void Reset();
    float Step(float dt);
    float CourantLimitedStep(float dt) const;
    void Advect(float dt);
    void FixMass(const std::vector<float>& before, std::vector<float>& after);
    double ColumnMass(const std::vector<float>& q) const;
    void Microphysics(float dt);
    void Forcing(float dt);
    void Mix(std::vector<float>& field, const std::vector<float>& baseByLevel,
        std::uint32_t levels, std::uint32_t firstLevel, float dt);
    void Project(float dt);
    void UpdateDiagnostics();
    [[nodiscard]] double TotalWater() const;
    [[nodiscard]] std::size_t Bytes() const;
};

FastStormSolver::Impl::Impl(const FastStormConfig& config)
    : cfg(config)
{
    threads = cfg.threads != 0U
        ? cfg.threads
        : std::max(1U, std::thread::hardware_concurrency());
    base = BuildSupercellBaseState(cfg.sounding, cfg.nz, cfg.dz);
    if (!cfg.moisture)
    {
        std::fill(base.vapor.begin(), base.vapor.end(), 0.0F);
    }
    cells = static_cast<std::size_t>(cfg.nx) * cfg.ny * cfg.nz;
    faceCells = static_cast<std::size_t>(cfg.nx) * cfg.ny * (cfg.nz + 1U);
    for (auto* f : {&u, &v, &theta, &qv, &qc, &qr, &u2, &v2, &theta2, &qv2,
             &qc2, &qr2, &phi, &divergence, &scratchCentre})
    {
        f->assign(cells, 0.0F);
    }
    w.assign(faceCells, 0.0F);
    w2.assign(faceCells, 0.0F);
    rainAccumulation.assign(static_cast<std::size_t>(cfg.nx) * cfg.ny, 0.0F);

    fftX.Prepare(cfg.nx);
    fftY.Prepare(cfg.ny);
    spectrum.assign(cells, Complex(0.0, 0.0));
    lambdaX.resize(cfg.nx);
    lambdaY.resize(cfg.ny);
    for (std::uint32_t m = 0; m < cfg.nx; ++m)
    {
        lambdaX[m] = (2.0 - 2.0 * std::cos(2.0 * kPi * m / cfg.nx))
            / (static_cast<double>(cfg.dx) * cfg.dx);
    }
    for (std::uint32_t m = 0; m < cfg.ny; ++m)
    {
        lambdaY[m] = (2.0 - 2.0 * std::cos(2.0 * kPi * m / cfg.ny))
            / (static_cast<double>(cfg.dy) * cfg.dy);
    }
    lowerCoeff.resize(cfg.nz);
    upperCoeff.resize(cfg.nz);
    const double dz2 = static_cast<double>(cfg.dz) * cfg.dz;
    for (std::uint32_t k = 0; k < cfg.nz; ++k)
    {
        const double rc = base.density[k];
        lowerCoeff[k] = k == 0U ? 0.0 : base.faceDensity[k] / (rc * dz2);
        upperCoeff[k] = k + 1U == cfg.nz
            ? 0.0
            : base.faceDensity[k + 1U] / (rc * dz2);
    }
    Reset();
}

void FastStormSolver::Impl::Reset()
{
    diag = {};
    std::fill(w.begin(), w.end(), 0.0F);
    std::fill(rainAccumulation.begin(), rainAccumulation.end(), 0.0F);
    std::fill(qc.begin(), qc.end(), 0.0F);
    std::fill(qr.begin(), qr.end(), 0.0F);
    const WarmBubble& b = cfg.bubble;
    const double cx = 0.5 * cfg.nx * cfg.dx;
    const double cy = 0.5 * cfg.ny * cfg.dy;
    for (std::uint32_t k = 0; k < cfg.nz; ++k)
    {
        for (std::uint32_t j = 0; j < cfg.ny; ++j)
        {
            for (std::uint32_t i = 0; i < cfg.nx; ++i)
            {
                const std::size_t idx = Index(i, j, k);
                const double x = (i + 0.5) * cfg.dx;
                const double y = (j + 0.5) * cfg.dy;
                const double z = base.centreHeight[k];
                const double beta = std::sqrt(
                    std::pow((x - cx) / b.horizontalRadius, 2.0)
                    + std::pow((y - cy) / b.horizontalRadius, 2.0)
                    + std::pow((z - b.centreHeight) / b.verticalRadius, 2.0));
                float pert = 0.0F;
                if (beta < 1.0)
                {
                    const double c = std::cos(0.5 * kPi * beta);
                    pert = static_cast<float>(b.amplitude * c * c);
                }
                theta[idx] = base.theta[k] + pert;
                qv[idx] = cfg.moisture ? base.vapor[k] : 0.0F;
                u[idx] = base.windU[k];
                v[idx] = base.windV[k];
            }
        }
    }
    diag.initialTotalWater = TotalWater();
    diag.totalWater = diag.initialTotalWater;
}

float FastStormSolver::Impl::CourantLimitedStep(const float dt) const
{
    float rate = 1.0e-6F;
    const std::size_t n = cells;
    for (std::size_t c = 0; c < n; ++c)
    {
        const std::size_t cw = c + static_cast<std::size_t>(cfg.nx) * cfg.ny;
        const float r = std::fabs(u[c] - cfg.frameU) / cfg.dx
            + std::fabs(v[c] - cfg.frameV) / cfg.dy
            + std::max(std::fabs(w[c]), std::fabs(w[cw])) / cfg.dz;
        rate = std::max(rate, r);
    }
    return std::min(dt, cfg.maxCourant / rate);
}

void FastStormSolver::Impl::Advect(const float dt)
{
    const auto nx = static_cast<std::int32_t>(cfg.nx);
    const auto ny = static_cast<std::int32_t>(cfg.ny);
    const auto nz = static_cast<std::int32_t>(cfg.nz);
    const FieldView uf{u.data(), nx, ny, nz};
    const FieldView vf{v.data(), nx, ny, nz};
    const FieldView wf{w.data(), nx, ny, nz + 1};
    const float invDx = 1.0F / cfg.dx;
    const float invDy = 1.0F / cfg.dy;
    const float invDz = 1.0F / cfg.dz;
    const float zTop = static_cast<float>(nz) * cfg.dz;
    const bool cubic = cfg.advection == AdvectionScheme::MonotoneCubic;

    auto velocity = [&](const float x, const float y, const float z,
                        float& ox, float& oy, float& oz)
    {
        ox = Trilinear(uf, x * invDx, y * invDy - 0.5F, z * invDz - 0.5F)
            - cfg.frameU;
        oy = Trilinear(vf, x * invDx - 0.5F, y * invDy, z * invDz - 0.5F)
            - cfg.frameV;
        oz = Trilinear(wf, x * invDx - 0.5F, y * invDy - 0.5F, z * invDz);
    };

    struct Target
    {
        const FieldView source;
        float* destination;
    };

    // One pass per staggering class: trace once, sample every field of it.
    auto run = [&](const float sx, const float sy, const float sz,
                   const std::int32_t levels,
                   const std::vector<Target>& targets)
    {
        ParallelFor(threads, static_cast<std::uint32_t>(levels),
            [&](const std::uint32_t kBegin, const std::uint32_t kEnd)
        {
            for (std::uint32_t k = kBegin; k < kEnd; ++k)
            {
                for (std::int32_t j = 0; j < ny; ++j)
                {
                    for (std::int32_t i = 0; i < nx; ++i)
                    {
                        const float x = (static_cast<float>(i) + sx) * cfg.dx;
                        const float y = (static_cast<float>(j) + sy) * cfg.dy;
                        const float z = (static_cast<float>(k) + sz) * cfg.dz;
                        float vx, vy, vz;
                        velocity(x, y, z, vx, vy, vz);
                        float mx = x - 0.5F * dt * vx;
                        float my = y - 0.5F * dt * vy;
                        float mz = std::min(std::max(z - 0.5F * dt * vz, 0.0F), zTop);
                        velocity(mx, my, mz, vx, vy, vz);
                        const float dxp = x - dt * vx;
                        const float dyp = y - dt * vy;
                        const float dzp = std::min(std::max(z - dt * vz, 0.0F), zTop);
                        const float ix = dxp * invDx - sx;
                        const float iy = dyp * invDy - sy;
                        const float iz = dzp * invDz - sz;
                        const std::size_t idx = (static_cast<std::size_t>(k)
                            * static_cast<std::size_t>(ny)
                            + static_cast<std::size_t>(j))
                            * static_cast<std::size_t>(nx)
                            + static_cast<std::size_t>(i);
                        for (const Target& t : targets)
                        {
                            t.destination[idx] = cubic
                                ? MonotoneCubic(t.source, ix, iy, iz)
                                : Trilinear(t.source, ix, iy, iz);
                        }
                    }
                }
            }
        });
    };

    run(0.0F, 0.5F, 0.5F, nz, {{uf, u2.data()}});
    run(0.5F, 0.0F, 0.5F, nz, {{vf, v2.data()}});
    // w faces: interior only, boundaries stay rigid.
    {
        std::fill(w2.begin(), w2.end(), 0.0F);
        ParallelFor(threads, static_cast<std::uint32_t>(nz - 1),
            [&](const std::uint32_t kBegin, const std::uint32_t kEnd)
        {
            for (std::uint32_t kk = kBegin; kk < kEnd; ++kk)
            {
                const std::uint32_t k = kk + 1U;
                for (std::int32_t j = 0; j < ny; ++j)
                {
                    for (std::int32_t i = 0; i < nx; ++i)
                    {
                        const float x = (static_cast<float>(i) + 0.5F) * cfg.dx;
                        const float y = (static_cast<float>(j) + 0.5F) * cfg.dy;
                        const float z = static_cast<float>(k) * cfg.dz;
                        float vx, vy, vz;
                        velocity(x, y, z, vx, vy, vz);
                        const float mx = x - 0.5F * dt * vx;
                        const float my = y - 0.5F * dt * vy;
                        const float mz = std::min(std::max(z - 0.5F * dt * vz, 0.0F), zTop);
                        velocity(mx, my, mz, vx, vy, vz);
                        const float ix = (x - dt * vx) * invDx - 0.5F;
                        const float iy = (y - dt * vy) * invDy - 0.5F;
                        const float iz = std::min(std::max(z - dt * vz, 0.0F), zTop) * invDz;
                        const std::size_t idx = (static_cast<std::size_t>(k)
                            * static_cast<std::size_t>(ny)
                            + static_cast<std::size_t>(j))
                            * static_cast<std::size_t>(nx)
                            + static_cast<std::size_t>(i);
                        w2[idx] = cubic ? MonotoneCubic(wf, ix, iy, iz)
                                        : Trilinear(wf, ix, iy, iz);
                    }
                }
            }
        });
    }
    std::vector<Target> scalars{
        {{theta.data(), nx, ny, nz}, theta2.data()}};
    if (cfg.moisture)
    {
        scalars.push_back({{qv.data(), nx, ny, nz}, qv2.data()});
        scalars.push_back({{qc.data(), nx, ny, nz}, qc2.data()});
        scalars.push_back({{qr.data(), nx, ny, nz}, qr2.data()});
    }
    run(0.5F, 0.5F, 0.5F, nz, scalars);

    if (cfg.moisture && cfg.massFixer)
    {
        FixMass(qv, qv2);
        FixMass(qc, qc2);
        FixMass(qr, qr2);
    }
    u.swap(u2);
    v.swap(v2);
    w.swap(w2);
    theta.swap(theta2);
    if (cfg.moisture)
    {
        qv.swap(qv2);
        qc.swap(qc2);
        qr.swap(qr2);
    }
}

double FastStormSolver::Impl::ColumnMass(const std::vector<float>& q) const
{
    double sum = 0.0;
    for (std::uint32_t k = 0; k < cfg.nz; ++k)
    {
        double layer = 0.0;
        for (std::size_t c = Index(0, 0, k);
             c < Index(0, 0, k) + static_cast<std::size_t>(cfg.nx) * cfg.ny; ++c)
        {
            layer += q[c];
        }
        sum += layer * base.density[k];
    }
    return sum;
}

void FastStormSolver::Impl::FixMass(
    const std::vector<float>& before, std::vector<float>& after)
{
    const double m0 = ColumnMass(before);
    const double m1 = ColumnMass(after);
    if (m0 <= 0.0 || m1 <= 0.0)
    {
        return;
    }
    const auto scale = static_cast<float>(m0 / m1);
    for (float& q : after)
    {
        q *= scale;
    }
}

void FastStormSolver::Impl::Microphysics(const float dt)
{
    using namespace thermo;
    const std::uint32_t columns = cfg.nx * cfg.ny;
    ParallelFor(threads, columns,
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        std::vector<float> flux(cfg.nz + 1U);
        for (std::uint32_t col = begin; col < end; ++col)
        {
            // Warm-rain Kessler processes, column by column.
            for (std::uint32_t k = 0; k < cfg.nz; ++k)
            {
                const std::size_t idx = static_cast<std::size_t>(k) * columns + col;
                const float p = base.pressure[k];
                const float exner = base.exner[k];
                const float rho = base.density[k];
                float th = theta[idx];
                float vapor = qv[idx];
                float cloud = qc[idx];
                float rain = qr[idx];
                const float heat = kLv / (kCp * exner);

                for (int iteration = 0; iteration < 2; ++iteration)
                {
                    const float temperature = th * exner;
                    const float qvs = SaturationMixingRatio(p, temperature);
                    const float denominator = 1.0F + kLv * kLv * qvs
                        / (kCp * kRv * temperature * temperature);
                    float change = (vapor - qvs) / denominator;
                    change = change >= 0.0F
                        ? std::min(change, vapor)
                        : -std::min(-change, cloud);
                    vapor -= change;
                    cloud += change;
                    th += heat * change;
                }

                const float auto_ = std::min(
                    dt * 1.0e-3F * std::max(cloud - 1.0e-3F, 0.0F), cloud);
                const float accretion = std::min(
                    dt * 2.2F * cloud * std::pow(std::max(rain, 0.0F), 0.875F),
                    cloud - auto_);
                cloud -= auto_ + accretion;
                rain += auto_ + accretion;

                if (rain > 0.0F)
                {
                    const float temperature = th * exner;
                    const float qvs = SaturationMixingRatio(p, temperature);
                    if (vapor < qvs)
                    {
                        // Kessler/Klemp-Wilhelmson rain evaporation in SI
                        // units (same form as CM1's kessler.F).
                        const float rhoQr = rho * rain;
                        const float evap = dt
                            * (1.6F + 30.3922F * std::pow(rhoQr, 0.2046F))
                            * (1.0F - vapor / qvs) * std::pow(rhoQr, 0.525F)
                            / ((2.03e4F + 9.584e6F / (qvs * p)) * rho);
                        const float denominator = 1.0F + kLv * kLv * qvs
                            / (kCp * kRv * temperature * temperature);
                        const float taken = std::min(
                            std::min(evap, rain), (qvs - vapor) / denominator);
                        rain -= taken;
                        vapor += taken;
                        th -= heat * taken;
                    }
                }
                theta[idx] = th;
                qv[idx] = vapor;
                qc[idx] = cloud;
                qr[idx] = rain;
            }

            // Rain sedimentation: conservative upwind, subcycled to its CFL.
            float maxFall = 0.0F;
            auto fallSpeed = [&](const std::uint32_t k, const float rain)
            {
                const float r = base.density[k] * std::max(rain, 0.0F);
                return r > 0.0F
                    ? 14.34F * std::pow(r, 0.1346F)
                        * std::sqrt(base.density[0] / base.density[k])
                    : 0.0F;
            };
            for (std::uint32_t k = 0; k < cfg.nz; ++k)
            {
                maxFall = std::max(maxFall,
                    fallSpeed(k, qr[static_cast<std::size_t>(k) * columns + col]));
            }
            if (maxFall <= 0.0F)
            {
                continue;
            }
            const auto substeps = static_cast<std::uint32_t>(
                std::ceil(maxFall * dt / (0.9F * cfg.dz)));
            const float sub = dt / static_cast<float>(substeps);
            for (std::uint32_t s = 0; s < substeps; ++s)
            {
                for (std::uint32_t k = 0; k < cfg.nz; ++k)
                {
                    const float rain = qr[static_cast<std::size_t>(k) * columns + col];
                    flux[k] = base.density[k] * rain * fallSpeed(k, rain);
                }
                flux[cfg.nz] = 0.0F;
                for (std::uint32_t k = 0; k < cfg.nz; ++k)
                {
                    const std::size_t idx = static_cast<std::size_t>(k) * columns + col;
                    qr[idx] = std::max(qr[idx]
                        + sub * (flux[k + 1U] - flux[k])
                            / (base.density[k] * cfg.dz), 0.0F);
                }
                rainAccumulation[col] += sub * flux[0];
            }
        }
    });
}

void FastStormSolver::Impl::Mix(
    std::vector<float>& field,
    const std::vector<float>& baseByLevel,
    const std::uint32_t levels,
    const std::uint32_t firstLevel,
    const float dt)
{
    const float ch = cfg.horizontalMixing * dt / (cfg.dx * cfg.dx);
    const float cv = cfg.verticalMixing * dt / (cfg.dz * cfg.dz);
    if (ch <= 0.0F && cv <= 0.0F)
    {
        return;
    }
    const auto nx = static_cast<std::int32_t>(cfg.nx);
    const auto ny = static_cast<std::int32_t>(cfg.ny);
    auto at = [&](const std::int32_t i, const std::int32_t j,
                  const std::uint32_t k)
    {
        const float b = baseByLevel.empty() ? 0.0F : baseByLevel[k];
        return field[(static_cast<std::size_t>(k) * cfg.ny
                      + static_cast<std::size_t>(j & (ny - 1))) * cfg.nx
                      + static_cast<std::size_t>(i & (nx - 1))] - b;
    };
    scratchCentre.assign(field.size(), 0.0F);
    ParallelFor(threads, levels,
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        for (std::uint32_t kk = begin; kk < end; ++kk)
        {
            const std::uint32_t k = kk + firstLevel;
            const bool hasBelow = k > 0U;
            const bool hasAbove = (k + 1U) < (firstLevel == 0U ? cfg.nz : cfg.nz + 1U);
            for (std::int32_t j = 0; j < ny; ++j)
            {
                for (std::int32_t i = 0; i < nx; ++i)
                {
                    const float c = at(i, j, k);
                    float tendency = ch * (at(i + 1, j, k) + at(i - 1, j, k)
                        + at(i, j + 1, k) + at(i, j - 1, k) - 4.0F * c);
                    const float below = hasBelow ? at(i, j, k - 1U) : c;
                    const float above = hasAbove ? at(i, j, k + 1U) : c;
                    tendency += cv * (below + above - 2.0F * c);
                    scratchCentre[(static_cast<std::size_t>(k) * cfg.ny
                        + static_cast<std::size_t>(j)) * cfg.nx
                        + static_cast<std::size_t>(i)] = tendency;
                }
            }
        }
    });
    for (std::uint32_t kk = 0; kk < levels; ++kk)
    {
        const std::size_t offset = static_cast<std::size_t>(kk + firstLevel)
            * cfg.ny * cfg.nx;
        for (std::size_t c = 0; c < static_cast<std::size_t>(cfg.ny) * cfg.nx; ++c)
        {
            field[offset + c] += scratchCentre[offset + c];
        }
    }
}

void FastStormSolver::Impl::Forcing(const float dt)
{
    using namespace thermo;
    const std::size_t plane = static_cast<std::size_t>(cfg.nx) * cfg.ny;
    // Buoyancy from the virtual potential-temperature perturbation and
    // condensate loading, averaged to the w faces.
    ParallelFor(threads, cfg.nz - 1U,
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        for (std::uint32_t kk = begin; kk < end; ++kk)
        {
            const std::uint32_t k = kk + 1U;
            for (std::size_t c = 0; c < plane; ++c)
            {
                float buoyancy[2];
                for (std::uint32_t side = 0; side < 2U; ++side)
                {
                    const std::uint32_t level = k - 1U + side;
                    const std::size_t idx = static_cast<std::size_t>(level) * plane + c;
                    buoyancy[side] = kGravity
                        * ((theta[idx] - base.theta[level]) / base.theta[level]
                            + 0.608F * (qv[idx] - base.vapor[level])
                            - qc[idx] - qr[idx]);
                }
                w[static_cast<std::size_t>(k) * plane + c] +=
                    dt * 0.5F * (buoyancy[0] + buoyancy[1]);
            }
        }
    });

    // Top sponge.
    const float top = static_cast<float>(cfg.nz) * cfg.dz;
    if (cfg.spongeBase < top)
    {
        for (std::uint32_t k = 0; k <= cfg.nz; ++k)
        {
            const float z = static_cast<float>(k) * cfg.dz;
            if (z <= cfg.spongeBase)
            {
                continue;
            }
            const float s = std::sin(0.5F * static_cast<float>(kPi)
                * (z - cfg.spongeBase) / (top - cfg.spongeBase));
            const float damp = 1.0F / (1.0F + dt * cfg.spongeRate * s * s);
            for (std::size_t c = 0; c < plane; ++c)
            {
                w[static_cast<std::size_t>(k) * plane + c] *= damp;
                if (k < cfg.nz)
                {
                    const std::size_t idx = static_cast<std::size_t>(k) * plane + c;
                    theta[idx] = base.theta[k] + (theta[idx] - base.theta[k]) * damp;
                }
            }
        }
    }

    Mix(u, base.windU, cfg.nz, 0U, dt);
    Mix(v, base.windV, cfg.nz, 0U, dt);
    Mix(w, {}, cfg.nz - 1U, 1U, dt);
    Mix(theta, base.theta, cfg.nz, 0U, dt);
    if (cfg.moisture)
    {
        Mix(qv, base.vapor, cfg.nz, 0U, dt);
        Mix(qc, {}, cfg.nz, 0U, dt);
        Mix(qr, {}, cfg.nz, 0U, dt);
    }
}

void FastStormSolver::Impl::Project(const float dt)
{
    const std::uint32_t nx = cfg.nx;
    const std::uint32_t ny = cfg.ny;
    const std::uint32_t nz = cfg.nz;
    const std::size_t plane = static_cast<std::size_t>(nx) * ny;
    const double invDx = 1.0 / cfg.dx;
    const double invDy = 1.0 / cfg.dy;
    const double invDz = 1.0 / cfg.dz;
    const double invDt = 1.0 / dt;

    // Right-hand side: divergence of the density-weighted velocity / dt.
    ParallelFor(threads, nz,
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        for (std::uint32_t k = begin; k < end; ++k)
        {
            const double rc = base.density[k];
            const double rLow = base.faceDensity[k];
            const double rHigh = base.faceDensity[k + 1U];
            for (std::uint32_t j = 0; j < ny; ++j)
            {
                for (std::uint32_t i = 0; i < nx; ++i)
                {
                    const std::size_t idx = Index(i, j, k);
                    const std::uint32_t ip = (i + 1U) & (nx - 1U);
                    const std::uint32_t jp = (j + 1U) & (ny - 1U);
                    const double div =
                        (u[Index(ip, j, k)] - u[idx]) * invDx
                        + (v[Index(i, jp, k)] - v[idx]) * invDy
                        + (rHigh * w[Index(i, j, k + 1U)]
                           - rLow * w[idx]) / rc * invDz;
                    spectrum[idx] = Complex(div * invDt, 0.0);
                }
            }
        }
    });

    // Horizontal forward transforms, one level at a time.
    ParallelFor(threads, nz,
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        std::vector<Complex> line(std::max(nx, ny));
        for (std::uint32_t k = begin; k < end; ++k)
        {
            Complex* level = spectrum.data() + static_cast<std::size_t>(k) * plane;
            for (std::uint32_t j = 0; j < ny; ++j)
            {
                fftX.Transform(level + static_cast<std::size_t>(j) * nx, false);
            }
            for (std::uint32_t i = 0; i < nx; ++i)
            {
                for (std::uint32_t j = 0; j < ny; ++j)
                {
                    line[j] = level[static_cast<std::size_t>(j) * nx + i];
                }
                fftY.Transform(line.data(), false);
                for (std::uint32_t j = 0; j < ny; ++j)
                {
                    level[static_cast<std::size_t>(j) * nx + i] = line[j];
                }
            }
        }
    });

    // Vertical tridiagonal solve per horizontal wavenumber.
    ParallelFor(threads, static_cast<std::uint32_t>(plane),
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        std::vector<double> centre(nz);
        std::vector<double> cPrime(nz);
        std::vector<Complex> dPrime(nz);
        for (std::uint32_t mode = begin; mode < end; ++mode)
        {
            const double lambda = lambdaX[mode % nx] + lambdaY[mode / nx];
            const bool singular = mode == 0U;
            for (std::uint32_t k = 0; k < nz; ++k)
            {
                centre[k] = -lowerCoeff[k] - upperCoeff[k] - lambda;
            }
            auto rhs = [&](const std::uint32_t k) -> Complex&
            {
                return spectrum[static_cast<std::size_t>(k) * plane + mode];
            };
            if (singular)
            {
                // Pin the mean pressure: phi_0 = 0.
                centre[0] = 1.0;
                rhs(0) = Complex(0.0, 0.0);
            }
            double upper0 = singular ? 0.0 : upperCoeff[0];
            cPrime[0] = upper0 / centre[0];
            dPrime[0] = rhs(0) / centre[0];
            for (std::uint32_t k = 1; k < nz; ++k)
            {
                const double denom = centre[k] - lowerCoeff[k] * cPrime[k - 1U];
                cPrime[k] = upperCoeff[k] / denom;
                dPrime[k] = (rhs(k) - lowerCoeff[k] * dPrime[k - 1U]) / denom;
            }
            rhs(nz - 1U) = dPrime[nz - 1U];
            for (std::uint32_t k = nz - 1U; k-- > 0U;)
            {
                rhs(k) = dPrime[k] - cPrime[k] * rhs(k + 1U);
            }
        }
    });

    // Inverse transforms.
    ParallelFor(threads, nz,
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        std::vector<Complex> line(std::max(nx, ny));
        const double norm = 1.0 / static_cast<double>(plane);
        for (std::uint32_t k = begin; k < end; ++k)
        {
            Complex* level = spectrum.data() + static_cast<std::size_t>(k) * plane;
            for (std::uint32_t i = 0; i < nx; ++i)
            {
                for (std::uint32_t j = 0; j < ny; ++j)
                {
                    line[j] = level[static_cast<std::size_t>(j) * nx + i];
                }
                fftY.Transform(line.data(), true);
                for (std::uint32_t j = 0; j < ny; ++j)
                {
                    level[static_cast<std::size_t>(j) * nx + i] = line[j];
                }
            }
            for (std::uint32_t j = 0; j < ny; ++j)
            {
                fftX.Transform(level + static_cast<std::size_t>(j) * nx, true);
            }
            for (std::size_t c = 0; c < plane; ++c)
            {
                phi[static_cast<std::size_t>(k) * plane + c] =
                    static_cast<float>(level[c].real() * norm);
            }
        }
    });

    // Subtract the pressure gradient.
    const float gx = dt * static_cast<float>(invDx);
    const float gy = dt * static_cast<float>(invDy);
    const float gz = dt * static_cast<float>(invDz);
    ParallelFor(threads, nz,
        [&](const std::uint32_t begin, const std::uint32_t end)
    {
        for (std::uint32_t k = begin; k < end; ++k)
        {
            for (std::uint32_t j = 0; j < ny; ++j)
            {
                for (std::uint32_t i = 0; i < nx; ++i)
                {
                    const std::size_t idx = Index(i, j, k);
                    const std::uint32_t im = (i + nx - 1U) & (nx - 1U);
                    const std::uint32_t jm = (j + ny - 1U) & (ny - 1U);
                    u[idx] -= gx * (phi[idx] - phi[Index(im, j, k)]);
                    v[idx] -= gy * (phi[idx] - phi[Index(i, jm, k)]);
                    if (k > 0U)
                    {
                        w[idx] -= gz * (phi[idx] - phi[Index(i, j, k - 1U)]);
                    }
                }
            }
        }
    });
}

double FastStormSolver::Impl::TotalWater() const
{
    const std::size_t plane = static_cast<std::size_t>(cfg.nx) * cfg.ny;
    const double cellArea = static_cast<double>(cfg.dx) * cfg.dy;
    double total = 0.0;
    for (std::uint32_t k = 0; k < cfg.nz; ++k)
    {
        double layer = 0.0;
        for (std::size_t c = 0; c < plane; ++c)
        {
            const std::size_t idx = static_cast<std::size_t>(k) * plane + c;
            layer += static_cast<double>(qv[idx]) + qc[idx] + qr[idx];
        }
        total += layer * base.density[k] * cfg.dz * cellArea;
    }
    double rain = 0.0;
    for (const float r : rainAccumulation)
    {
        rain += r;
    }
    return total + rain * cellArea;
}

void FastStormSolver::Impl::UpdateDiagnostics()
{
    float maxUp = 0.0F;
    float maxDown = 0.0F;
    float maxSpeed = 0.0F;
    for (std::size_t c = 0; c < faceCells; ++c)
    {
        maxUp = std::max(maxUp, w[c]);
        maxDown = std::min(maxDown, w[c]);
    }
    for (std::size_t c = 0; c < cells; ++c)
    {
        maxSpeed = std::max(maxSpeed, std::hypot(
            u[c] - cfg.frameU, v[c] - cfg.frameV));
    }
    float maxDiv = 0.0F;
    for (std::uint32_t k = 0; k < cfg.nz; ++k)
    {
        const double rc = base.density[k];
        for (std::uint32_t j = 0; j < cfg.ny; ++j)
        {
            for (std::uint32_t i = 0; i < cfg.nx; ++i)
            {
                const std::size_t idx = Index(i, j, k);
                const double div =
                    (u[Index((i + 1U) & (cfg.nx - 1U), j, k)] - u[idx]) / cfg.dx
                    + (v[Index(i, (j + 1U) & (cfg.ny - 1U), k)] - v[idx]) / cfg.dy
                    + (base.faceDensity[k + 1U] * w[Index(i, j, k + 1U)]
                        - base.faceDensity[k] * w[idx]) / (rc * cfg.dz);
                maxDiv = std::max(maxDiv, static_cast<float>(std::fabs(div)));
            }
        }
    }
    double rain = 0.0;
    for (const float r : rainAccumulation)
    {
        rain += r;
    }
    diag.maxUpdraft = maxUp;
    diag.maxDowndraft = maxDown;
    diag.maxHorizontalSpeed = maxSpeed;
    diag.maxDivergence = maxDiv;
    diag.surfaceRain = rain * cfg.dx * cfg.dy;
    diag.totalWater = TotalWater();
}

float FastStormSolver::Impl::Step(const float requested)
{
    const auto t0 = Clock::now();
    const float dt = CourantLimitedStep(std::min(requested, cfg.maxTimeStep));
    auto t = Clock::now();
    Advect(dt);
    diag.timings.advectMs = MillisecondsSince(t);

    t = Clock::now();
    if (cfg.moisture)
    {
        Microphysics(dt);
    }
    diag.timings.microphysicsMs = MillisecondsSince(t);

    t = Clock::now();
    Forcing(dt);
    diag.timings.forcingMs = MillisecondsSince(t);

    t = Clock::now();
    Project(dt);
    diag.timings.projectionMs = MillisecondsSince(t);

    diag.time += dt;
    ++diag.steps;
    diag.lastTimeStep = dt;
    UpdateDiagnostics();
    diag.timings.totalMs = MillisecondsSince(t0);
    return dt;
}

std::size_t FastStormSolver::Impl::Bytes() const
{
    std::size_t bytes = 0;
    for (const auto* f : {&u, &v, &w, &theta, &qv, &qc, &qr, &u2, &v2, &w2,
             &theta2, &qv2, &qc2, &qr2, &phi, &divergence,
             &rainAccumulation, &scratchCentre})
    {
        bytes += f->capacity() * sizeof(float);
    }
    bytes += spectrum.capacity() * sizeof(Complex);
    return bytes;
}

// ---------------------------------------------------------------- public API

FastStormSolver::FastStormSolver(const FastStormConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
}

FastStormSolver::~FastStormSolver() = default;
FastStormSolver::FastStormSolver(FastStormSolver&&) noexcept = default;
FastStormSolver& FastStormSolver::operator=(FastStormSolver&&) noexcept = default;

std::string FastStormSolver::Validate(const FastStormConfig& c)
{
    if (!IsPowerOfTwo(c.nx) || !IsPowerOfTwo(c.ny))
    {
        return "nx and ny must be powers of two (>= 4)";
    }
    if (c.nz < 4U)
    {
        return "nz must be at least 4";
    }
    if (!(c.dx > 0.0F) || !(c.dy > 0.0F) || !(c.dz > 0.0F))
    {
        return "cell sizes must be positive";
    }
    if (!(c.maxTimeStep > 0.0F) || !(c.maxCourant > 0.0F))
    {
        return "maxTimeStep and maxCourant must be positive";
    }
    if (c.horizontalMixing < 0.0F || c.verticalMixing < 0.0F)
    {
        return "mixing coefficients must be non-negative";
    }
    return {};
}

void FastStormSolver::Reset() { impl_->Reset(); }

float FastStormSolver::Step(const float dt) { return impl_->Step(dt); }

std::uint32_t FastStormSolver::Advance(const double seconds)
{
    std::uint32_t steps = 0;
    const double target = impl_->diag.time + seconds;
    while (impl_->diag.time < target - 1.0e-4)
    {
        impl_->Step(static_cast<float>(target - impl_->diag.time));
        ++steps;
    }
    return steps;
}

const FastStormConfig& FastStormSolver::Config() const { return impl_->cfg; }
const BaseState& FastStormSolver::Base() const { return impl_->base; }
const FastStormDiagnostics& FastStormSolver::Diagnostics() const
{
    return impl_->diag;
}

const std::vector<std::string>& FastStormSolver::ExportFields()
{
    static const std::vector<std::string> fields{
        "th", "qv", "qc", "qr", "u", "v", "w", "zvort"};
    return fields;
}

void FastStormSolver::CopyField(
    const std::string& name, std::vector<float>& out) const
{
    const Impl& s = *impl_;
    const std::uint32_t nx = s.cfg.nx;
    const std::uint32_t ny = s.cfg.ny;
    const std::uint32_t nz = s.cfg.nz;
    out.assign(s.cells, 0.0F);
    if (name == "th") { out = s.theta; }
    else if (name == "qv") { out = s.qv; }
    else if (name == "qc") { out = s.qc; }
    else if (name == "qr") { out = s.qr; }
    else if (name == "u" || name == "v" || name == "w" || name == "zvort")
    {
        for (std::uint32_t k = 0; k < nz; ++k)
        {
            for (std::uint32_t j = 0; j < ny; ++j)
            {
                for (std::uint32_t i = 0; i < nx; ++i)
                {
                    const std::uint32_t ip = (i + 1U) & (nx - 1U);
                    const std::uint32_t jp = (j + 1U) & (ny - 1U);
                    float value = 0.0F;
                    if (name == "u")
                    {
                        value = 0.5F * (s.u[s.Index(i, j, k)] + s.u[s.Index(ip, j, k)]);
                    }
                    else if (name == "v")
                    {
                        value = 0.5F * (s.v[s.Index(i, j, k)] + s.v[s.Index(i, jp, k)]);
                    }
                    else if (name == "w")
                    {
                        value = 0.5F * (s.w[s.Index(i, j, k)] + s.w[s.Index(i, j, k + 1U)]);
                    }
                    else
                    {
                        // Corner vorticity averaged to the cell centre.
                        auto corner = [&](const std::uint32_t ci, const std::uint32_t cj)
                        {
                            const std::uint32_t cim = (ci + nx - 1U) & (nx - 1U);
                            const std::uint32_t cjm = (cj + ny - 1U) & (ny - 1U);
                            return (s.v[s.Index(ci, cj, k)] - s.v[s.Index(cim, cj, k)]) / s.cfg.dx
                                - (s.u[s.Index(ci, cj, k)] - s.u[s.Index(ci, cjm, k)]) / s.cfg.dy;
                        };
                        value = 0.25F * (corner(i, j) + corner(ip, j)
                            + corner(i, jp) + corner(ip, jp));
                    }
                    out[s.Index(i, j, k)] = value;
                }
            }
        }
    }
}

WxHeader FastStormSolver::MakeHeader(const std::vector<float>& times) const
{
    WxHeader header;
    header.source = "fastcore";
    header.nx = impl_->cfg.nx;
    header.ny = impl_->cfg.ny;
    header.nz = impl_->cfg.nz;
    header.dx = impl_->cfg.dx;
    header.dy = impl_->cfg.dy;
    header.moveU = impl_->cfg.frameU;
    header.moveV = impl_->cfg.frameV;
    header.centreHeight = impl_->base.centreHeight;
    header.times = times;
    header.fields = ExportFields();
    return header;
}

bool FastStormSolver::WriteFrame(WxWriter& writer) const
{
    const auto& names = ExportFields();
    std::vector<std::vector<float>> data(names.size());
    std::vector<const float*> pointers;
    for (std::size_t f = 0; f < names.size(); ++f)
    {
        CopyField(names[f], data[f]);
        pointers.push_back(data[f].data());
    }
    return writer.AppendFrame(pointers);
}

std::size_t FastStormSolver::ResidentBytes() const { return impl_->Bytes(); }
} // namespace orbit::weather_lab
