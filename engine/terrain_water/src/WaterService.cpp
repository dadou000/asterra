#include <orbit/terrain_water/WaterService.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::terrain_water
{
namespace
{
[[nodiscard]] bool FiniteNonNegative(const f64 value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] f64 Length(const math::Double2 value) noexcept
{
    return std::sqrt(value.x * value.x + value.y * value.y);
}

[[nodiscard]] math::Double2 Normalize(const math::Double2 value) noexcept
{
    const f64 length = Length(value);
    return length > 1.0e-12
        ? math::Double2{value.x / length, value.y / length}
        : math::Double2{};
}

[[nodiscard]] WaterPageSide OppositeSide(const WaterPageSide side) noexcept
{
    return static_cast<WaterPageSide>((static_cast<u8>(side) + 2U) % 4U);
}
} // namespace

bool FluidDefinition::IsValid() const noexcept
{
    const bool ordinaryRegimeValid =
        solverRegime != FluidSolverRegime::WaterLikeShallowWater ||
        dynamicViscosityPascalSeconds <= 0.05;
    return id.IsValid() && !name.empty() &&
        std::isfinite(densityKgPerCubicMeter) && densityKgPerCubicMeter > 0.0 &&
        FiniteNonNegative(dynamicViscosityPascalSeconds) &&
        FiniteNonNegative(surfaceTensionNewtonsPerMeter) && ordinaryRegimeValid;
}

bool OceanDefinition::IsValid() const noexcept
{
    return std::isfinite(datumHeightMeters) && (!enabled || fluid.IsValid());
}

bool WaterPageDefinition::IsValid() const noexcept
{
    return id.IsValid() && resolution >= 2U &&
        std::isfinite(spacingMeters) && spacingMeters > 0.0 && fluid.IsValid();
}

WaterCell& DynamicWaterPage::At(const u32 x, const u32 y)
{
    if (x >= definition.resolution || y >= definition.resolution)
    {
        throw std::out_of_range("Water page coordinate is outside the page.");
    }
    return cells[static_cast<std::size_t>(y) * definition.resolution + x];
}

const WaterCell& DynamicWaterPage::At(const u32 x, const u32 y) const
{
    if (x >= definition.resolution || y >= definition.resolution)
    {
        throw std::out_of_range("Water page coordinate is outside the page.");
    }
    return cells[static_cast<std::size_t>(y) * definition.resolution + x];
}

f64 DynamicWaterPage::VolumeCubicMeters() const noexcept
{
    f64 depth = 0.0;
    for (const auto& cell : cells)
    {
        depth += cell.depthMeters;
    }
    return depth * definition.spacingMeters * definition.spacingMeters;
}

bool WaterSource::IsValid() const noexcept
{
    return id.IsValid() && page.IsValid() &&
        FiniteNonNegative(volumeRateCubicMetersPerSecond) &&
        std::isfinite(velocityMetersPerSecond.x) &&
        std::isfinite(velocityMetersPerSecond.y);
}

bool WaterBarrier::IsValid() const noexcept
{
    return id.IsValid() && page.IsValid() &&
        std::isfinite(crestHeightMeters) &&
        std::isfinite(gateOpening) && gateOpening >= 0.0 && gateOpening <= 1.0;
}

bool WaterExclusionVolume::IsValid() const noexcept
{
    return owner.IsValid() && hullMask.IsValid() &&
        FiniteNonNegative(displacedVolumeCubicMeters);
}

bool InteriorWaterDomain::IsValid() const noexcept
{
    return id.IsValid() && FiniteNonNegative(capacityCubicMeters) &&
        FiniteNonNegative(waterVolumeCubicMeters) &&
        waterVolumeCubicMeters <= capacityCubicMeters;
}

bool FluidForceEmitter::IsValid() const noexcept
{
    return id.IsValid() && page.IsValid() && targetFluid.IsValid() &&
        std::isfinite(direction.x) && std::isfinite(direction.y) &&
        Length(direction) > 1.0e-12 &&
        std::isfinite(radiusMeters) && radiusMeters > 0.0 &&
        std::isfinite(linearImpulseNewtonSeconds) &&
        std::isfinite(angularImpulseNewtonMeterSeconds) &&
        std::isfinite(volumeCubicMeters) &&
        std::isfinite(pressureImpulse) &&
        FiniteNonNegative(turbulence);
}

WaterService::WaterService(const universe::BodyId body)
    : body_(body)
{
    if (!body_.IsValid())
    {
        throw std::invalid_argument("WaterService requires a valid body identity.");
    }
}

universe::BodyId WaterService::Body() const noexcept
{
    return body_;
}

void WaterService::RegisterFluid(FluidDefinition fluid)
{
    if (!fluid.IsValid())
    {
        throw std::invalid_argument("WaterService fluid definition is invalid.");
    }
    fluids_.insert_or_assign(fluid.id, std::move(fluid));
}

const FluidDefinition* WaterService::FindFluid(const FluidId id) const noexcept
{
    const auto found = fluids_.find(id);
    return found == fluids_.end() ? nullptr : &found->second;
}

void WaterService::ConfigureOcean(const OceanDefinition ocean)
{
    if (!ocean.IsValid() || (ocean.enabled && FindFluid(ocean.fluid) == nullptr))
    {
        throw std::invalid_argument("WaterService ocean definition is invalid.");
    }
    ocean_ = ocean;
}

const OceanDefinition& WaterService::Ocean() const noexcept
{
    return ocean_;
}

bool WaterService::IsOceanConnected(const f64 terrainHeightMeters) const noexcept
{
    return ocean_.enabled && std::isfinite(terrainHeightMeters) &&
        terrainHeightMeters <= ocean_.datumHeightMeters;
}

f64 WaterService::OceanReservoirVolumeCubicMeters() const noexcept
{
    return oceanReservoirVolumeCubicMeters_;
}

void WaterService::CreatePage(
    WaterPageDefinition definition,
    const std::span<const f64> bedElevations)
{
    const std::size_t count = static_cast<std::size_t>(definition.resolution) *
        definition.resolution;
    if (!definition.IsValid() || FindFluid(definition.fluid) == nullptr ||
        bedElevations.size() != count)
    {
        throw std::invalid_argument("WaterService page definition is invalid.");
    }

    DynamicWaterPage page{
        .definition = definition,
        .cells = std::vector<WaterCell>(count)
    };
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!std::isfinite(bedElevations[index]))
        {
            throw std::invalid_argument("WaterService bed elevation is invalid.");
        }
        page.cells[index].bedElevationMeters = bedElevations[index];
        if (definition.fluid == ocean_.fluid && IsOceanConnected(bedElevations[index]))
        {
            page.cells[index].depthMeters = ocean_.datumHeightMeters - bedElevations[index];
        }
    }
    pages_.insert_or_assign(definition.id, std::move(page));
    Recount();
}

void WaterService::ConnectPages(
    const WaterPageId first,
    const WaterPageSide firstSide,
    const WaterPageId second)
{
    const auto* firstPage = FindPage(first);
    const auto* secondPage = FindPage(second);
    if (static_cast<u8>(firstSide) > static_cast<u8>(WaterPageSide::West) ||
        firstPage == nullptr || secondPage == nullptr || first == second ||
        firstPage->definition.resolution != secondPage->definition.resolution ||
        firstPage->definition.spacingMeters != secondPage->definition.spacingMeters ||
        firstPage->definition.fluid != secondPage->definition.fluid)
    {
        throw std::invalid_argument("WaterService page connection is invalid.");
    }

    const WaterPageSide secondSide = OppositeSide(firstSide);
    for (const auto& connection : connections_)
    {
        if ((connection.first == first && connection.firstSide == firstSide &&
             connection.second == second) ||
            (connection.first == second && connection.firstSide == secondSide &&
             connection.second == first))
        {
            return;
        }

        const WaterPageSide connectedSecondSide = OppositeSide(connection.firstSide);
        if ((connection.first == first && connection.firstSide == firstSide) ||
            (connection.second == first && connectedSecondSide == firstSide) ||
            (connection.first == second && connection.firstSide == secondSide) ||
            (connection.second == second && connectedSecondSide == secondSide))
        {
            throw std::invalid_argument("WaterService page side already has a connection.");
        }
    }
    connections_.push_back({
        .first = first,
        .firstSide = firstSide,
        .second = second});
}

DynamicWaterPage* WaterService::FindPage(const WaterPageId id) noexcept
{
    const auto found = pages_.find(id);
    return found == pages_.end() ? nullptr : &found->second;
}

const DynamicWaterPage* WaterService::FindPage(const WaterPageId id) const noexcept
{
    const auto found = pages_.find(id);
    return found == pages_.end() ? nullptr : &found->second;
}

void WaterService::SetSleeping(const WaterPageId id, const bool sleeping)
{
    auto& page = RequirePage(id);
    page.sleeping = sleeping;
    page.physicallyForced = !sleeping;
    Recount();
}

void WaterService::NotifyCameraMoved(const WaterPageId) noexcept
{
    // Residency/view changes are deliberately not physical wake sources.
}

void WaterService::AddSource(WaterSource source)
{
    if (!source.IsValid())
    {
        throw std::invalid_argument("WaterService source is invalid.");
    }
    sources_.insert_or_assign(source.id, std::move(source));
}

void WaterService::AddBarrier(WaterBarrier barrier)
{
    if (!barrier.IsValid())
    {
        throw std::invalid_argument("WaterService barrier is invalid.");
    }
    barriers_.insert_or_assign(barrier.id, std::move(barrier));
}

void WaterService::AddExclusion(WaterExclusionVolume exclusion)
{
    if (!exclusion.IsValid())
    {
        throw std::invalid_argument("WaterService exclusion volume is invalid.");
    }
    exclusions_.insert_or_assign(exclusion.hullMask, std::move(exclusion));
    Recount();
}

void WaterService::AddInteriorDomain(InteriorWaterDomain domain)
{
    if (!domain.IsValid())
    {
        throw std::invalid_argument("WaterService interior domain is invalid.");
    }
    interiors_.insert_or_assign(domain.id, std::move(domain));
    Recount();
}

void WaterService::SetBreach(const WaterDomainId domain, const bool breached)
{
    RequireDomain(domain).breached = breached;
}

void WaterService::InjectVolume(
    const WaterPageId pageId,
    const u32 x,
    const u32 y,
    const f64 volumeCubicMeters)
{
    InjectVolumeIntoPage(RequirePage(pageId), x, y, volumeCubicMeters);
    Recount();
}

void WaterService::InjectVolumeIntoPage(
    DynamicWaterPage& page,
    const u32 x,
    const u32 y,
    const f64 volumeCubicMeters)
{
    if (!std::isfinite(volumeCubicMeters))
    {
        throw std::invalid_argument("WaterService volume injection is invalid.");
    }
    auto& cell = page.At(x, y);
    const f64 area = page.definition.spacingMeters * page.definition.spacingMeters;
    if (cell.depthMeters + volumeCubicMeters / area < 0.0)
    {
        throw std::invalid_argument("WaterService volume extraction exceeds local water.");
    }
    cell.depthMeters += volumeCubicMeters / area;
    page.sleeping = false;
    page.physicallyForced = true;
}

void WaterService::InjectMomentum(
    const WaterPageId pageId,
    const u32 x,
    const u32 y,
    const math::Double2 impulseNewtonSeconds)
{
    InjectMomentumIntoPage(RequirePage(pageId), x, y, impulseNewtonSeconds);
    Recount();
}

void WaterService::InjectMomentumIntoPage(
    DynamicWaterPage& page,
    const u32 x,
    const u32 y,
    const math::Double2 impulseNewtonSeconds)
{
    const auto* fluid = FindFluid(page.definition.fluid);
    if (fluid == nullptr || !std::isfinite(impulseNewtonSeconds.x) ||
        !std::isfinite(impulseNewtonSeconds.y))
    {
        throw std::invalid_argument("WaterService momentum injection is invalid.");
    }
    auto& cell = page.At(x, y);
    const f64 scale = fluid->densityKgPerCubicMeter *
        page.definition.spacingMeters * page.definition.spacingMeters;
    cell.momentumXSquareMetersPerSecond += impulseNewtonSeconds.x / scale;
    cell.momentumYSquareMetersPerSecond += impulseNewtonSeconds.y / scale;
    page.sleeping = false;
    page.physicallyForced = true;
}

void WaterService::InjectAngularMomentum(
    const WaterPageId page,
    const u32 x,
    const u32 y,
    const f64 impulse)
{
    InjectMomentum(page, x, y, {-impulse * 0.5, impulse * 0.5});
}

void WaterService::InjectPressure(
    const WaterPageId page,
    const u32 x,
    const u32 y,
    const f64 pressureImpulse)
{
    if (!std::isfinite(pressureImpulse))
    {
        throw std::invalid_argument("WaterService pressure injection is invalid.");
    }
    InjectVolume(page, x, y, std::max(0.0, pressureImpulse) * 1.0e-4);
}

void WaterService::InjectVorticity(
    const WaterPageId page,
    const u32 x,
    const u32 y,
    const f64 circulation)
{
    if (!std::isfinite(circulation))
    {
        throw std::invalid_argument("WaterService vorticity injection is invalid.");
    }
    InjectMomentum(page, x, y, {-circulation, circulation});
}

void WaterService::EmitPhysicalWave(
    const WaterPageId page,
    const u32 x,
    const u32 y,
    const f64 amplitudeMeters,
    const math::Double2 direction)
{
    auto& target = RequirePage(page);
    if (!FiniteNonNegative(amplitudeMeters) ||
        !std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(Length(direction)) || Length(direction) <= 1.0e-12 ||
        FindFluid(target.definition.fluid) == nullptr)
    {
        throw std::invalid_argument("WaterService physical wave is invalid.");
    }
    const f64 area = target.definition.spacingMeters * target.definition.spacingMeters;
    InjectVolume(page, x, y, amplitudeMeters * area);
    const auto unit = Normalize(direction);
    const auto* fluid = FindFluid(target.definition.fluid);
    const f64 impulse = fluid->densityKgPerCubicMeter * amplitudeMeters * area *
        std::sqrt(9.81 * std::max(amplitudeMeters, 0.001));
    InjectMomentum(page, x, y, {unit.x * impulse, unit.y * impulse});
}

f64 WaterService::VisualWaveHeight(
    const WaterPageId page,
    const u32 x,
    const u32 y,
    const f64 timeSeconds) const noexcept
{
    const auto* target = FindPage(page);
    if (target == nullptr || x >= target->definition.resolution ||
        y >= target->definition.resolution || !std::isfinite(timeSeconds))
    {
        return 0.0;
    }
    return 0.1 * std::sin(timeSeconds + static_cast<f64>(x + y));
}

PropellerResult WaterService::ApplyPropeller(const FluidForceEmitter& emitter)
{
    if (!emitter.IsValid())
    {
        throw std::invalid_argument("WaterService propeller emitter is invalid.");
    }
    const auto* page = FindPage(emitter.page);
    if (page == nullptr || page->definition.fluid != emitter.targetFluid)
    {
        throw std::invalid_argument("WaterService propeller target is invalid.");
    }
    const auto direction = Normalize(emitter.direction);
    const math::Double2 body{
        direction.x * emitter.linearImpulseNewtonSeconds,
        direction.y * emitter.linearImpulseNewtonSeconds};
    const math::Double2 water{-body.x, -body.y};
    InjectMomentum(emitter.page, emitter.x, emitter.y, water);
    InjectAngularMomentum(
        emitter.page, emitter.x, emitter.y,
        -emitter.angularImpulseNewtonMeterSeconds);
    if (emitter.volumeCubicMeters != 0.0)
    {
        InjectVolume(emitter.page, emitter.x, emitter.y, emitter.volumeCubicMeters);
    }
    return {
        .bodyImpulseNewtonSeconds = body,
        .waterImpulseNewtonSeconds = water,
        .waterAngularImpulseNewtonMeterSeconds =
            -emitter.angularImpulseNewtonMeterSeconds,
        .cavitationIndicator = std::clamp(
            std::abs(emitter.pressureImpulse) * 1.0e-4 + emitter.turbulence,
            0.0, 1.0)};
}

void WaterService::DisplaceHull(
    const WaterPageId page,
    const u32 x,
    const u32 y,
    const f64 displacedVolumeCubicMeters,
    const math::Double2 hullImpulse)
{
    InjectVolume(page, x, y, displacedVolumeCubicMeters);
    InjectMomentum(page, x, y, {-hullImpulse.x, -hullImpulse.y});
}

void WaterService::Step(const f64 seconds, const u32 substeps)
{
    if (!std::isfinite(seconds) || seconds <= 0.0 || substeps == 0U)
    {
        throw std::invalid_argument("WaterService step is invalid.");
    }
    const auto started = std::chrono::steady_clock::now();
    const f64 dt = seconds / static_cast<f64>(substeps);

    for (u32 step = 0; step < substeps; ++step)
    {
        for (const auto& [id, source] : sources_)
        {
            static_cast<void>(id);
            if (!source.enabled || source.volumeRateCubicMetersPerSecond == 0.0)
            {
                continue;
            }
            auto& page = RequirePage(source.page);
            InjectVolumeIntoPage(page, source.x, source.y,
                source.volumeRateCubicMetersPerSecond * dt);
            const auto* fluid = FindFluid(page.definition.fluid);
            const f64 mass = source.volumeRateCubicMetersPerSecond * dt *
                fluid->densityKgPerCubicMeter;
            InjectMomentumIntoPage(page, source.x, source.y,
                {source.velocityMetersPerSecond.x * mass,
                 source.velocityMetersPerSecond.y * mass});
        }

        for (auto& [pageId, page] : pages_)
        {
            if (page.sleeping)
            {
                continue;
            }
            const u32 n = page.definition.resolution;
            const f64 spacing = page.definition.spacingMeters;
            const f64 area = spacing * spacing;
            depthDeltas_.assign(page.cells.size(), 0.0);
            auto& delta = depthDeltas_;
            bool hasBarrier = false;
            f64 permeability = 1.0;
            for (const auto& [barrierId, barrier] : barriers_)
            {
                static_cast<void>(barrierId);
                if (barrier.page == pageId)
                {
                    hasBarrier = true;
                    permeability = std::min(
                        permeability,
                        barrier.closed ? 0.0 : barrier.gateOpening);
                }
            }
            if (!hasBarrier)
            {
                permeability = 1.0;
            }
            const f64 flowScale = 0.20 * dt * permeability;
            auto exchange = [&](const u32 ax, const u32 ay, const u32 bx, const u32 by)
            {
                const std::size_t ai = static_cast<std::size_t>(ay) * n + ax;
                const std::size_t bi = static_cast<std::size_t>(by) * n + bx;
                const auto& a = page.cells[ai];
                const auto& b = page.cells[bi];
                const f64 difference =
                    (a.bedElevationMeters + a.depthMeters) -
                    (b.bedElevationMeters + b.depthMeters);
                f64 transferDepth = difference * flowScale;
                transferDepth = std::clamp(
                    transferDepth,
                    -(b.depthMeters + delta[bi]),
                    a.depthMeters + delta[ai]);
                delta[ai] -= transferDepth;
                delta[bi] += transferDepth;
            };
            for (u32 y = 0; y < n; ++y)
            {
                for (u32 x = 0; x < n; ++x)
                {
                    if (x + 1U < n)
                    {
                        exchange(x, y, x + 1U, y);
                    }
                    if (y + 1U < n)
                    {
                        exchange(x, y, x, y + 1U);
                    }
                }
            }
            for (std::size_t index = 0; index < page.cells.size(); ++index)
            {
                page.cells[index].depthMeters =
                    std::max(0.0, page.cells[index].depthMeters + delta[index]);
                page.cells[index].momentumXSquareMetersPerSecond *= 0.995;
                page.cells[index].momentumYSquareMetersPerSecond *= 0.995;
            }

            if (ocean_.enabled && page.definition.fluid == ocean_.fluid)
            {
                std::array<bool, 4U> reservoirSides{};
                for (std::size_t side = 0; side < reservoirSides.size(); ++side)
                {
                    const auto boundary = page.definition.boundaries[side];
                    reservoirSides[side] = boundary == WaterBoundaryType::Open ||
                        boundary == WaterBoundaryType::Reservoir;
                }
                // An explicit neighbor owns this edge's flux. Applying an
                // ocean reservoir as well would create a second authority.
                for (const auto& connection : connections_)
                {
                    if (connection.first == pageId)
                    {
                        reservoirSides[static_cast<u8>(connection.firstSide)] = false;
                    }
                    if (connection.second == pageId)
                    {
                        reservoirSides[static_cast<u8>(OppositeSide(connection.firstSide))] = false;
                    }
                }
                for (u32 y = 0; y < n; ++y)
                {
                    for (u32 x = 0; x < n; ++x)
                    {
                        const bool edge = (y == 0U && reservoirSides[0]) ||
                            (x + 1U == n && reservoirSides[1]) ||
                            (y + 1U == n && reservoirSides[2]) ||
                            (x == 0U && reservoirSides[3]);
                        if (!edge)
                        {
                            continue;
                        }
                        auto& cell = page.At(x, y);
                        const f64 target = std::max(0.0,
                            ocean_.datumHeightMeters - cell.bedElevationMeters);
                        const f64 exchangeVolume = (cell.depthMeters - target) * area * 0.1;
                        cell.depthMeters -= exchangeVolume / area;
                        oceanReservoirVolumeCubicMeters_ += exchangeVolume;
                    }
                }
            }
            page.elapsedSeconds += dt;
            page.physicallyForced = false;
        }

        const auto haloStarted = std::chrono::steady_clock::now();
        for (const auto& connection : connections_)
        {
            auto& first = RequirePage(connection.first);
            auto& second = RequirePage(connection.second);
            if (first.sleeping && second.sleeping)
            {
                continue;
            }
            const u32 n = first.definition.resolution;
            for (u32 index = 0; index < n; ++index)
            {
                u32 ax = 0U;
                u32 ay = 0U;
                u32 bx = 0U;
                u32 by = 0U;
                switch (connection.firstSide)
                {
                case WaterPageSide::North:
                    ax = index; ay = 0U; bx = index; by = n - 1U; break;
                case WaterPageSide::East:
                    ax = n - 1U; ay = index; bx = 0U; by = index; break;
                case WaterPageSide::South:
                    ax = index; ay = n - 1U; bx = index; by = 0U; break;
                case WaterPageSide::West:
                    ax = 0U; ay = index; bx = n - 1U; by = index; break;
                }
                auto& a = first.At(ax, ay);
                auto& b = second.At(bx, by);
                const f64 difference =
                    (a.bedElevationMeters + a.depthMeters) -
                    (b.bedElevationMeters + b.depthMeters);
                const f64 transfer = std::clamp(
                    difference * 0.10 * dt,
                    -b.depthMeters,
                    a.depthMeters);
                a.depthMeters -= transfer;
                b.depthMeters += transfer;
                if (std::abs(transfer) > 1.0e-12)
                {
                    first.sleeping = false;
                    second.sleeping = false;
                }
            }
        }
        const auto haloEnded = std::chrono::steady_clock::now();
        counters_.lastHaloExchangeMilliseconds =
            std::chrono::duration<f64, std::milli>(haloEnded - haloStarted).count();
    }
    const auto ended = std::chrono::steady_clock::now();
    counters_.lastStepMilliseconds =
        std::chrono::duration<f64, std::milli>(ended - started).count();
    Recount();
}

void WaterService::ExchangeInterior(
    const WaterDomainId domainId,
    const WaterPageId exteriorPage,
    const u32 x,
    const u32 y,
    const f64 maximumVolumeCubicMeters)
{
    auto& domain = RequireDomain(domainId);
    auto& page = RequirePage(exteriorPage);
    if (!domain.breached || !FiniteNonNegative(maximumVolumeCubicMeters))
    {
        return;
    }
    auto& cell = page.At(x, y);
    const f64 area = page.definition.spacingMeters * page.definition.spacingMeters;
    const f64 available = cell.depthMeters * area;
    const f64 capacity = domain.capacityCubicMeters - domain.waterVolumeCubicMeters;
    const f64 moved = std::min({available, capacity, maximumVolumeCubicMeters});
    cell.depthMeters -= moved / area;
    domain.waterVolumeCubicMeters += moved;
    page.sleeping = false;
    Recount();
}

bool WaterService::MasksRenderedWater(const HullMaskId mask) const noexcept
{
    const auto found = exclusions_.find(mask);
    return found != exclusions_.end() && found->second.sealedInterior;
}

WaterServiceSnapshot WaterService::CaptureAuthoredSnapshot() const
{
    WaterServiceSnapshot snapshot{.ocean = ocean_};
    for (const auto& [id, value] : fluids_)
    {
        static_cast<void>(id);
        snapshot.fluids.push_back(value);
    }
    for (const auto& [id, value] : sources_)
    {
        static_cast<void>(id);
        snapshot.sources.push_back(value);
    }
    for (const auto& [id, value] : barriers_)
    {
        static_cast<void>(id);
        snapshot.barriers.push_back(value);
    }
    for (const auto& [id, value] : exclusions_)
    {
        static_cast<void>(id);
        snapshot.exclusions.push_back(value);
    }
    for (const auto& [id, value] : interiors_)
    {
        static_cast<void>(id);
        snapshot.interiors.push_back(value);
    }
    return snapshot;
}

void WaterService::RestoreAuthoredSnapshot(const WaterServiceSnapshot& snapshot)
{
    fluids_.clear();
    sources_.clear();
    barriers_.clear();
    exclusions_.clear();
    interiors_.clear();
    for (const auto& fluid : snapshot.fluids)
    {
        RegisterFluid(fluid);
    }
    ConfigureOcean(snapshot.ocean);
    for (const auto& source : snapshot.sources)
    {
        AddSource(source);
    }
    for (const auto& barrier : snapshot.barriers)
    {
        AddBarrier(barrier);
    }
    for (const auto& exclusion : snapshot.exclusions)
    {
        AddExclusion(exclusion);
    }
    for (const auto& interior : snapshot.interiors)
    {
        AddInteriorDomain(interior);
    }
}

WaterServiceCounters WaterService::Counters() const noexcept
{
    return counters_;
}

TerrainWaterForcing WaterService::SampleForTerrain(
    const WaterPageId pageId,
    const u32 x,
    const u32 y) const
{
    const auto* page = FindPage(pageId);
    if (page == nullptr)
    {
        throw std::invalid_argument("WaterService terrain sample page is unknown.");
    }
    const auto& cell = page->At(x, y);
    return {
        .depthMeters = cell.depthMeters,
        .momentumSquareMetersPerSecond = {
            cell.momentumXSquareMetersPerSecond,
            cell.momentumYSquareMetersPerSecond},
        .suspendedSedimentKg = cell.suspendedSedimentKg};
}

bool WaterService::IsValid() const noexcept
{
    if (!body_.IsValid() || !ocean_.IsValid())
    {
        return false;
    }
    for (const auto& [id, fluid] : fluids_)
    {
        if (id != fluid.id || !fluid.IsValid())
        {
            return false;
        }
    }
    return !ocean_.enabled || FindFluid(ocean_.fluid) != nullptr;
}

DynamicWaterPage& WaterService::RequirePage(const WaterPageId id)
{
    auto* page = FindPage(id);
    if (page == nullptr)
    {
        throw std::invalid_argument("WaterService page identity is unknown.");
    }
    return *page;
}

InteriorWaterDomain& WaterService::RequireDomain(const WaterDomainId id)
{
    const auto found = interiors_.find(id);
    if (found == interiors_.end())
    {
        throw std::invalid_argument("WaterService domain identity is unknown.");
    }
    return found->second;
}

void WaterService::Recount() noexcept
{
    counters_.activeWaterPages = 0U;
    counters_.sleepingWaterPages = 0U;
    counters_.wetCellCount = 0U;
    for (const auto& [id, page] : pages_)
    {
        static_cast<void>(id);
        if (page.sleeping)
        {
            ++counters_.sleepingWaterPages;
        }
        else
        {
            ++counters_.activeWaterPages;
        }
        counters_.wetCellCount += static_cast<u32>(std::count_if(
            page.cells.begin(), page.cells.end(),
            [](const WaterCell& cell) { return cell.depthMeters > 1.0e-6; }));
    }
    counters_.fluidForceEmitterCount = 0U;
    counters_.hullMaskBytes = exclusions_.size() * sizeof(WaterExclusionVolume);
    counters_.interiorDomainBytes = interiors_.size() * sizeof(InteriorWaterDomain);
}
} // namespace orbit::terrain_water
