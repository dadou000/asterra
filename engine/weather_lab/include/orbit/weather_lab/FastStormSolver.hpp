#pragma once

#include <orbit/weather_lab/Thermo.hpp>
#include <orbit/weather_lab/WxFormat.hpp>

#include <array>
#include <functional>
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
    // Single-moment ice: cloud ice (qi), snow (qs) and graupel (qg) with deposition, freezing, riming, melting, sublimation and
    // latent heat of fusion. Off = the warm-rain Kessler core.
    bool ice = false;
    bool massFixer = true;

    // Explicit second-order mixing (m^2/s) on every prognostic field.
    float horizontalMixing = 100.0F;
    float verticalMixing = 50.0F;

    // Rayleigh damping of w and theta' above spongeBase (1/s at the top).
    float spongeBase = 15000.0F;
    float spongeRate = 3.3333333e-3F;

    // Lateral sponge, the open-boundary substitute on a periodic grid: within `lateralSpongeCells` of every edge the state is relaxed
    // toward the environment (base wind, theta and vapour; no condensate, w = 0) at up to `lateralSpongeRate` (1/s), so outflow
    // that reaches an edge is absorbed instead of re-entering on the other side. 0 = off (fully periodic).
    std::uint32_t lateralSpongeCells = 0;
    float lateralSpongeRate = 1.0e-2F;

    // Storm-following frame: every `trackSeconds` the Galilean frame velocity (frameU, frameV) is adjusted to the measured motion of
    // the main updraft (w above 12 m/s at 3 km, at most 1.5 m/s per update) and to bring its centroid back to the domain centre, so a long run stays inside the
    // domain. The environment is horizontally homogeneous, so a moving grid is exactly equivalent.
    bool trackStorm = false;
    float trackSeconds = 300.0F;

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

// Raw prognostic arrays, staggered as inside the solver: u at x faces (x = i dx, y and z at centres), v at y faces, w at z faces
// (nz + 1 levels), everything else at cell centres; index (k * ny + j) * nx + i. The ice arrays are null without cfg.ice.
struct FastStormState
{
    float* u = nullptr;
    float* v = nullptr;
    float* w = nullptr;
    float* theta = nullptr;
    float* qv = nullptr;
    float* qc = nullptr;
    float* qr = nullptr;
    float* qi = nullptr;
    float* qs = nullptr;
    float* qg = nullptr;
    std::uint32_t nx = 0;
    std::uint32_t ny = 0;
    std::uint32_t nz = 0;
};

// Called inside every step after the forcing and before the pressure projection, with the time at the end of the step and its
// length; the nesting uses it to relax the lateral boundary zone toward the parent (the projection then removes the divergence
// the relaxation introduces).
using FastStormHook = std::function<void(const FastStormState&, double endTime, float dt)>;

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

    // Direct access to the prognostic arrays (for nesting and tests); valid until the solver is destroyed.
    [[nodiscard]] FastStormState MutableState();
    void SetPreProjectionHook(FastStormHook hook);
    // Changes the Galilean frame velocity (a nested child follows its parent's frame through this).
    void SetFrame(float u, float v);
    // Sets the simulated time (a nested child created mid-run starts at its parent's time).
    void SetTime(double seconds);
    // Re-baselines the water budget after the state was overwritten from outside.
    void MarkInitialState();

    [[nodiscard]] const FastStormConfig& Config() const;
    [[nodiscard]] const BaseState& Base() const;
    [[nodiscard]] const FastStormDiagnostics& Diagnostics() const;

    // Field names accepted by CopyField / exported by WriteFrame:
    //   th qv qc qr u v w zvort   (all at cell centres, x fastest); qi qs qg are also accepted (zero without ice)
    [[nodiscard]] static const std::vector<std::string>& ExportFields();
    // The fields WriteFrame writes for this run: ExportFields(), plus qi qs qg when the ice scheme is on.
    [[nodiscard]] std::vector<std::string> ActiveExportFields() const;
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
