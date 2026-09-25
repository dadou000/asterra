#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace orbit::terrain_water
{
struct FluidIdTag;
using FluidId = core::StrongId<FluidIdTag>;
struct FluidSurfaceMaterialIdTag;
using FluidSurfaceMaterialId = core::StrongId<FluidSurfaceMaterialIdTag>;
struct WaterPageIdTag;
using WaterPageId = core::StrongId<WaterPageIdTag>;
struct WaterEntityIdTag;
using WaterEntityId = core::StrongId<WaterEntityIdTag>;
struct WaterDomainIdTag;
using WaterDomainId = core::StrongId<WaterDomainIdTag>;
struct HullMaskIdTag;
using HullMaskId = core::StrongId<HullMaskIdTag>;

enum class FluidSolverRegime : u8
{
    WaterLikeShallowWater,
    ExternallyValidated
};

struct FluidDefinition
{
    FluidId id{};
    std::string name;
    f64 densityKgPerCubicMeter{997.0};
    f64 dynamicViscosityPascalSeconds{0.001};
    f64 surfaceTensionNewtonsPerMeter{0.072};
    FluidSurfaceMaterialId surfaceMaterial{};
    FluidSolverRegime solverRegime{FluidSolverRegime::WaterLikeShallowWater};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct OceanDefinition
{
    bool enabled{false};
    f64 datumHeightMeters{0.0};
    FluidId fluid{};

    [[nodiscard]] bool IsValid() const noexcept;
};

enum class WaterBoundaryType : u8
{
    Closed,
    Open,
    Reservoir,
    Source,
    Drain
};

enum class WaterPageSide : u8
{
    North,
    East,
    South,
    West
};

struct WaterCell
{
    f64 bedElevationMeters{0.0};
    f64 depthMeters{0.0};
    f64 momentumXSquareMetersPerSecond{0.0};
    f64 momentumYSquareMetersPerSecond{0.0};
    f64 suspendedSedimentKg{0.0};
};

struct WaterPageDefinition
{
    WaterPageId id{};
    u32 resolution{0};
    f64 spacingMeters{0.0};
    FluidId fluid{};
    std::array<WaterBoundaryType, 4U> boundaries{
        WaterBoundaryType::Closed,
        WaterBoundaryType::Closed,
        WaterBoundaryType::Closed,
        WaterBoundaryType::Closed};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct DynamicWaterPage
{
    WaterPageDefinition definition{};
    std::vector<WaterCell> cells;
    bool sleeping{true};
    bool physicallyForced{false};
    f64 elapsedSeconds{0.0};

    [[nodiscard]] WaterCell& At(u32 x, u32 y);
    [[nodiscard]] const WaterCell& At(u32 x, u32 y) const;
    [[nodiscard]] f64 VolumeCubicMeters() const noexcept;
};

enum class WaterSourcePlacement : u8
{
    Point,
    Spline,
    Area
};

struct WaterSource
{
    WaterEntityId id{};
    WaterPageId page{};
    WaterSourcePlacement placement{WaterSourcePlacement::Point};
    u32 x{0};
    u32 y{0};
    f64 volumeRateCubicMetersPerSecond{0.0};
    math::Double2 velocityMetersPerSecond{};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct WaterBarrier
{
    WaterEntityId id{};
    WaterPageId page{};
    bool closed{true};
    f64 crestHeightMeters{0.0};
    f64 gateOpening{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct WaterExclusionVolume
{
    WaterEntityId owner{};
    HullMaskId hullMask{};
    bool sealedInterior{true};
    f64 displacedVolumeCubicMeters{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct InteriorWaterDomain
{
    WaterDomainId id{};
    f64 capacityCubicMeters{0.0};
    f64 waterVolumeCubicMeters{0.0};
    bool breached{false};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct FluidForceEmitter
{
    WaterEntityId id{};
    WaterPageId page{};
    u32 x{0};
    u32 y{0};
    math::Double2 direction{1.0, 0.0};
    f64 radiusMeters{1.0};
    f64 linearImpulseNewtonSeconds{0.0};
    f64 angularImpulseNewtonMeterSeconds{0.0};
    f64 volumeCubicMeters{0.0};
    f64 pressureImpulse{0.0};
    f64 turbulence{0.0};
    FluidId targetFluid{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct PropellerResult
{
    math::Double2 bodyImpulseNewtonSeconds{};
    math::Double2 waterImpulseNewtonSeconds{};
    f64 waterAngularImpulseNewtonMeterSeconds{0.0};
    f64 cavitationIndicator{0.0};
};

struct WaterServiceCounters
{
    u32 activeWaterPages{0};
    u32 sleepingWaterPages{0};
    u32 wetCellCount{0};
    u32 fluidForceEmitterCount{0};
    u64 hullMaskBytes{0};
    u64 interiorDomainBytes{0};
    f64 lastStepMilliseconds{0.0};
    f64 lastHaloExchangeMilliseconds{0.0};
};

// Read-only authority handoff consumed by hydraulic erosion/sediment systems.
// Terrain processes may react to this state but cannot mutate WaterService
// geology or manufacture a second water authority.
struct TerrainWaterForcing
{
    f64 depthMeters{0.0};
    math::Double2 momentumSquareMetersPerSecond{};
    f64 suspendedSedimentKg{0.0};
};

struct WaterServiceSnapshot
{
    OceanDefinition ocean{};
    std::vector<FluidDefinition> fluids;
    std::vector<WaterSource> sources;
    std::vector<WaterBarrier> barriers;
    std::vector<WaterExclusionVolume> exclusions;
    std::vector<InteriorWaterDomain> interiors;
};

class WaterService
{
public:
    explicit WaterService(universe::BodyId body);

    [[nodiscard]] universe::BodyId Body() const noexcept;
    void RegisterFluid(FluidDefinition fluid);
    [[nodiscard]] const FluidDefinition* FindFluid(FluidId id) const noexcept;
    void ConfigureOcean(OceanDefinition ocean);
    [[nodiscard]] const OceanDefinition& Ocean() const noexcept;
    [[nodiscard]] bool IsOceanConnected(f64 terrainHeightMeters) const noexcept;
    [[nodiscard]] f64 OceanReservoirVolumeCubicMeters() const noexcept;

    void CreatePage(WaterPageDefinition definition, std::span<const f64> bedElevations);
    void ConnectPages(WaterPageId first, WaterPageSide firstSide, WaterPageId second);
    [[nodiscard]] DynamicWaterPage* FindPage(WaterPageId id) noexcept;
    [[nodiscard]] const DynamicWaterPage* FindPage(WaterPageId id) const noexcept;
    void SetSleeping(WaterPageId id, bool sleeping);
    void NotifyCameraMoved(WaterPageId) noexcept;

    void AddSource(WaterSource source);
    void AddBarrier(WaterBarrier barrier);
    void AddExclusion(WaterExclusionVolume exclusion);
    void AddInteriorDomain(InteriorWaterDomain domain);
    void SetBreach(WaterDomainId domain, bool breached);

    void InjectVolume(WaterPageId page, u32 x, u32 y, f64 volumeCubicMeters);
    void InjectMomentum(WaterPageId page, u32 x, u32 y, math::Double2 impulseNewtonSeconds);
    void InjectAngularMomentum(WaterPageId page, u32 x, u32 y, f64 impulseNewtonMeterSeconds);
    void InjectPressure(WaterPageId page, u32 x, u32 y, f64 pressureImpulse);
    void InjectVorticity(WaterPageId page, u32 x, u32 y, f64 circulationSquareMetersPerSecond);
    void EmitPhysicalWave(WaterPageId page, u32 x, u32 y, f64 amplitudeMeters, math::Double2 direction);
    [[nodiscard]] f64 VisualWaveHeight(WaterPageId page, u32 x, u32 y, f64 timeSeconds) const noexcept;
    [[nodiscard]] PropellerResult ApplyPropeller(const FluidForceEmitter& emitter);
    void DisplaceHull(WaterPageId page, u32 x, u32 y, f64 displacedVolumeCubicMeters, math::Double2 hullImpulse);

    void Step(f64 seconds, u32 substeps = 1U);
    void ExchangeInterior(WaterDomainId domain, WaterPageId exteriorPage, u32 x, u32 y, f64 maximumVolumeCubicMeters);
    [[nodiscard]] bool MasksRenderedWater(HullMaskId mask) const noexcept;
    [[nodiscard]] WaterServiceSnapshot CaptureAuthoredSnapshot() const;
    void RestoreAuthoredSnapshot(const WaterServiceSnapshot& snapshot);
    [[nodiscard]] WaterServiceCounters Counters() const noexcept;
    [[nodiscard]] TerrainWaterForcing SampleForTerrain(
        WaterPageId page, u32 x, u32 y) const;
    [[nodiscard]] bool IsValid() const noexcept;

private:
    [[nodiscard]] DynamicWaterPage& RequirePage(WaterPageId id);
    [[nodiscard]] InteriorWaterDomain& RequireDomain(WaterDomainId id);
    void InjectVolumeIntoPage(DynamicWaterPage& page, u32 x, u32 y, f64 volumeCubicMeters);
    void InjectMomentumIntoPage(DynamicWaterPage& page, u32 x, u32 y, math::Double2 impulseNewtonSeconds);
    void Recount() noexcept;

    universe::BodyId body_{};
    OceanDefinition ocean_{};
    f64 oceanReservoirVolumeCubicMeters_{0.0};
    math::Double2 oceanReservoirMomentum_{};
    std::unordered_map<FluidId, FluidDefinition> fluids_;
    std::unordered_map<WaterPageId, DynamicWaterPage> pages_;
    std::unordered_map<WaterEntityId, WaterSource> sources_;
    std::unordered_map<WaterEntityId, WaterBarrier> barriers_;
    std::unordered_map<HullMaskId, WaterExclusionVolume> exclusions_;
    std::unordered_map<WaterDomainId, InteriorWaterDomain> interiors_;
    WaterServiceCounters counters_{};
    // Shared solver scratch: pages are advanced serially, so retain one buffer.
    std::vector<f64> depthDeltas_;
    struct PageConnection
    {
        WaterPageId first{};
        WaterPageSide firstSide{WaterPageSide::East};
        WaterPageId second{};
    };
    std::vector<PageConnection> connections_;
};
} // namespace orbit::terrain_water
