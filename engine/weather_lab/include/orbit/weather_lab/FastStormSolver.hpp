#pragma once

#include <orbit/weather_lab/Thermo.hpp>
#include <orbit/weather_lab/WxFormat.hpp>

#include <array>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

// SC-01 "fast core": the compressed storm solver.
//
// A moist anelastic model on a doubly periodic Arakawa C grid that trades
// CM1's high-order conservative advection and acoustic sub-stepping for
//   * semi-Lagrangian advection (no advective CFL limit, one pass per step),
//   * an exact FFT + tridiagonal pressure projection (no acoustic waves, no
//     iterations), and
//   * Kessler warm-rain microphysics with saturation adjustment.
// It is the CPU reference whose kernels map one-to-one to Vulkan compute.

namespace orbit::weather_lab
{
enum class AdvectionScheme : std::uint8_t
{
    Linear,
    MonotoneCubic,
};

struct WarmBubble
{
    float amplitude = 2.0F;        // K (CM1 uses 1 K at 500 m cells)
    float horizontalRadius = 10000.0F;
    float verticalRadius = 1400.0F;
    float centreHeight = 1400.0F;
};

struct FastStormConfig
{
    // nx and ny must be powers of two (FFT pressure solver).
    std::uint32_t nx = 64;
    std::uint32_t ny = 64;
    std::uint32_t nz = 40;
    float dx = 2000.0F;
    float dy = 2000.0F;
    float dz = 500.0F;

    SupercellSoundingParams sounding{};
    WarmBubble bubble{};

    // Galilean frame translation, chosen so the storm stays near the domain
    // centre (CM1 supercell uses 12.5, 3.0).
    float frameU = 12.5F;
    float frameV = 3.0F;

    float maxTimeStep = 12.0F;
    float maxCourant = 1.2F;
    AdvectionScheme advection = AdvectionScheme::MonotoneCubic;
    bool moisture = true;
    bool massFixer = true;

    // Explicit second-order mixing (m^2/s) on every prognostic field.
    float horizontalMixing = 100.0F;
    float verticalMixing = 50.0F;

    // Rayleigh damping of w and theta' above spongeBase (1/s at the top).
    float spongeBase = 15000.0F;
    float spongeRate = 3.3333333e-3F;

    // 0 = hardware concurrency.
    std::uint32_t threads = 0;
};

struct FastStormTimings
{
    double advectMs = 0.0;
    double microphysicsMs = 0.0;
    double forcingMs = 0.0;
    double projectionMs = 0.0;
    double totalMs = 0.0;
};

struct FastStormDiagnostics
{
    double time = 0.0;
    std::uint64_t steps = 0;
    float lastTimeStep = 0.0F;
    float maxUpdraft = 0.0F;
    float maxDowndraft = 0.0F;
    float maxHorizontalSpeed = 0.0F;
    float maxDivergence = 0.0F;     // after projection, s^-1
    double totalWater = 0.0;        // kg: vapour + cloud + rain + fallen rain
    double initialTotalWater = 0.0;
    double surfaceRain = 0.0;       // kg that has reached the ground
    FastStormTimings timings{};
};

class FastStormSolver
{
public:
    explicit FastStormSolver(const FastStormConfig& config);
    ~FastStormSolver();
    FastStormSolver(FastStormSolver&&) noexcept;
    FastStormSolver& operator=(FastStormSolver&&) noexcept;
    FastStormSolver(const FastStormSolver&) = delete;
    FastStormSolver& operator=(const FastStormSolver&) = delete;

    // Validates the configuration; an empty string means it is usable.
    [[nodiscard]] static std::string Validate(const FastStormConfig& config);

    void Reset();
    // Advances by `seconds` of simulated time using as many adaptive steps as
    // needed; returns the number of steps taken.
    std::uint32_t Advance(double seconds);
    // One explicit step of at most `dt` seconds (clamped by the CFL limit).
    float Step(float dt);

    [[nodiscard]] const FastStormConfig& Config() const;
    [[nodiscard]] const BaseState& Base() const;
    [[nodiscard]] const FastStormDiagnostics& Diagnostics() const;

    // Field names accepted by CopyField / exported by WriteFrame:
    //   th qv qc qr u v w zvort   (all at cell centres, x fastest)
    [[nodiscard]] static const std::vector<std::string>& ExportFields();
    void CopyField(const std::string& name, std::vector<float>& out) const;
    [[nodiscard]] WxHeader MakeHeader(
        const std::vector<float>& times) const;
    [[nodiscard]] bool WriteFrame(WxWriter& writer) const;

    // Bytes of solver state and scratch (the number to hold against the
    // 2 GB weather budget once scaled to the target domain).
    [[nodiscard]] std::size_t ResidentBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::weather_lab
