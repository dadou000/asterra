#include <orbit/terrain_erosion/SedimentExchange.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_erosion
{
namespace
{
[[nodiscard]] f64 NonNegativeFinite(
    const f64 value,
    const char* message)
{
    if (!std::isfinite(value) ||
        value < 0.0)
    {
        throw std::invalid_argument(message);
    }

    return value;
}

[[nodiscard]] SedimentMass MinMass(
    const SedimentMass& available,
    const SedimentMass& requested) noexcept
{
    return {
        .sandKg =
            std::min(
                available.sandKg,
                requested.sandKg),
        .finesKg =
            std::min(
                available.finesKg,
                requested.finesKg),
        .coarseDebrisKg =
            std::min(
                available.coarseDebrisKg,
                requested.coarseDebrisKg)
    };
}

void ClampTinyNegative(
    SedimentMass& mass) noexcept
{
    constexpr f64 epsilon = 1.0e-10;

    if (mass.sandKg < 0.0 &&
        mass.sandKg > -epsilon)
    {
        mass.sandKg = 0.0;
    }

    if (mass.finesKg < 0.0 &&
        mass.finesKg > -epsilon)
    {
        mass.finesKg = 0.0;
    }

    if (mass.coarseDebrisKg < 0.0 &&
        mass.coarseDebrisKg > -epsilon)
    {
        mass.coarseDebrisKg = 0.0;
    }
}

[[nodiscard]] SedimentMass SumPackets(
    const std::span<const SedimentTransportPacket> packets) noexcept
{
    SedimentMass total{};

    for (const auto& packet : packets)
    {
        total += packet.Total();
    }

    return total;
}

[[nodiscard]] f64 LayerMass(
    const f64 depthMeters,
    const f64 areaSquareMeters,
    const f64 densityKgPerCubicMeter) noexcept
{
    return
        depthMeters *
        areaSquareMeters *
        densityKgPerCubicMeter;
}

[[nodiscard]] GpuSedimentMediumTexel
PackMedium(
    const SedimentMass& mass,
    const f64 area)
{
    const auto toFloat =
        [area](const f64 kilograms)
        {
            const f64 value =
                kilograms /
                area;

            if (!std::isfinite(value) ||
                value >
                    static_cast<f64>(
                        std::numeric_limits<f32>::max()))
            {
                throw std::overflow_error(
                    "Orbit M14 GPU sediment packing exceeded f32 range.");
            }

            return static_cast<f32>(
                value);
        };

    return {
        .sandKgPerSquareMeter =
            toFloat(mass.sandKg),
        .finesKgPerSquareMeter =
            toFloat(mass.finesKg),
        .coarseDebrisKgPerSquareMeter =
            toFloat(
                mass.coarseDebrisKg),
        .reserved = 0.0F
    };
}
} // namespace

bool SedimentMass::IsValid() const noexcept
{
    return
        std::isfinite(sandKg) &&
        std::isfinite(finesKg) &&
        std::isfinite(coarseDebrisKg) &&
        sandKg >= 0.0 &&
        finesKg >= 0.0 &&
        coarseDebrisKg >= 0.0;
}

f64 SedimentMass::TotalKg() const noexcept
{
    return
        sandKg +
        finesKg +
        coarseDebrisKg;
}

bool SedimentMass::Empty(
    const f64 epsilonKg) const noexcept
{
    return
        TotalKg() <=
        std::max(
            epsilonKg,
            0.0);
}

SedimentMass&
SedimentMass::operator+=(
    const SedimentMass& other) noexcept
{
    sandKg +=
        other.sandKg;

    finesKg +=
        other.finesKg;

    coarseDebrisKg +=
        other.coarseDebrisKg;

    return *this;
}

SedimentMass&
SedimentMass::operator-=(
    const SedimentMass& other) noexcept
{
    sandKg -=
        other.sandKg;

    finesKg -=
        other.finesKg;

    coarseDebrisKg -=
        other.coarseDebrisKg;

    ClampTinyNegative(
        *this);

    return *this;
}

SedimentMass operator+(
    SedimentMass left,
    const SedimentMass& right) noexcept
{
    left += right;
    return left;
}

SedimentMass operator-(
    SedimentMass left,
    const SedimentMass& right) noexcept
{
    left -= right;
    return left;
}

bool SedimentConversionRules::IsValid() const noexcept
{
    return
        std::isfinite(
            hydraulicBedrockSandFraction) &&
        hydraulicBedrockSandFraction >=
            0.0 &&
        hydraulicBedrockSandFraction <=
            1.0 &&
        std::isfinite(
            aeolianBedrockSandFraction) &&
        aeolianBedrockSandFraction >=
            0.0 &&
        aeolianBedrockSandFraction <=
            1.0 &&
        std::isfinite(
            glacialBedrockSandFraction) &&
        glacialBedrockSandFraction >=
            0.0 &&
        glacialBedrockSandFraction <=
            1.0 &&
        std::isfinite(
            glacialBedrockCoarseFraction) &&
        glacialBedrockCoarseFraction >=
            0.0 &&
        glacialBedrockCoarseFraction <=
            1.0 &&
        glacialBedrockSandFraction +
                glacialBedrockCoarseFraction <=
            1.0 &&
        std::isfinite(
            coastalBedrockSandFraction) &&
        coastalBedrockSandFraction >=
            0.0 &&
        coastalBedrockSandFraction <=
            1.0;
}

SedimentMass&
MobileSedimentCell::Medium(
    const SedimentTransportMedium medium) noexcept
{
    switch (medium)
    {
    case SedimentTransportMedium::Waterborne:
        return waterborne;
    case SedimentTransportMedium::Airborne:
        return airborne;
    case SedimentTransportMedium::SurfaceMobile:
        return surfaceMobile;
    }

    return waterborne;
}

const SedimentMass&
MobileSedimentCell::Medium(
    const SedimentTransportMedium medium) const noexcept
{
    switch (medium)
    {
    case SedimentTransportMedium::Waterborne:
        return waterborne;
    case SedimentTransportMedium::Airborne:
        return airborne;
    case SedimentTransportMedium::SurfaceMobile:
        return surfaceMobile;
    }

    return waterborne;
}

SedimentMass
MobileSedimentCell::Total() const noexcept
{
    return
        waterborne +
        airborne +
        surfaceMobile;
}

SedimentMass&
SedimentTransportPacket::Medium(
    const SedimentTransportMedium medium) noexcept
{
    switch (medium)
    {
    case SedimentTransportMedium::Waterborne:
        return waterborne;
    case SedimentTransportMedium::Airborne:
        return airborne;
    case SedimentTransportMedium::SurfaceMobile:
        return surfaceMobile;
    }

    return waterborne;
}

const SedimentMass&
SedimentTransportPacket::Medium(
    const SedimentTransportMedium medium) const noexcept
{
    switch (medium)
    {
    case SedimentTransportMedium::Waterborne:
        return waterborne;
    case SedimentTransportMedium::Airborne:
        return airborne;
    case SedimentTransportMedium::SurfaceMobile:
        return surfaceMobile;
    }

    return waterborne;
}

SedimentMass
SedimentTransportPacket::Total() const noexcept
{
    return
        waterborne +
        airborne +
        surfaceMobile;
}

bool SedimentBoundaryFlux::IsComplete(
    const u32 resolution) const noexcept
{
    return
        resolution > 0U &&
        north.size() == resolution &&
        east.size() == resolution &&
        south.size() == resolution &&
        west.size() == resolution;
}

SedimentMass
SedimentBoundaryFlux::Total() const noexcept
{
    SedimentMass total{};

    total +=
        SumPackets(north);

    total +=
        SumPackets(east);

    total +=
        SumPackets(south);

    total +=
        SumPackets(west);

    for (const auto& corner :
         corners)
    {
        total +=
            corner.Total();
    }

    return total;
}

f64 SedimentDepositResult::DepositedKg() const noexcept
{
    return
        deposited.TotalKg();
}

SedimentMass
SedimentMassBalance::NetBoundary() const noexcept
{
    return
        imported -
        exported;
}

bool SedimentTransportVector::IsValid() const noexcept
{
    return
        std::isfinite(eastKg) &&
        std::isfinite(northKg);
}

SedimentTransportVector&
SedimentTransportVector::operator+=(
    const SedimentTransportVector& other) noexcept
{
    eastKg += other.eastKg;
    northKg += other.northKg;
    return *this;
}

SedimentTransportVector&
SedimentTransportCell::Medium(
    const SedimentTransportMedium medium) noexcept
{
    switch (medium)
    {
    case SedimentTransportMedium::Waterborne:
        return waterborne;
    case SedimentTransportMedium::Airborne:
        return airborne;
    case SedimentTransportMedium::SurfaceMobile:
        return surfaceMobile;
    }

    return waterborne;
}

const SedimentTransportVector&
SedimentTransportCell::Medium(
    const SedimentTransportMedium medium) const noexcept
{
    switch (medium)
    {
    case SedimentTransportMedium::Waterborne:
        return waterborne;
    case SedimentTransportMedium::Airborne:
        return airborne;
    case SedimentTransportMedium::SurfaceMobile:
        return surfaceMobile;
    }

    return waterborne;
}

SedimentTransportVector
SedimentTransportCell::Total() const noexcept
{
    SedimentTransportVector total =
        waterborne;
    total += airborne;
    total += surfaceMobile;
    return total;
}

SedimentExchangePage::SedimentExchangePage(
    const u32 resolution,
    const f64 spacingMeters,
    SedimentConversionRules conversionRules)
    : resolution_(resolution),
      spacingMeters_(spacingMeters),
      conversionRules_(
          std::move(
              conversionRules)),
      cells_(
          static_cast<std::size_t>(
              resolution) *
          resolution),
      transport_(
          static_cast<std::size_t>(
              resolution) *
          resolution),
      outgoing_{
          .north =
              std::vector<SedimentTransportPacket>(
                  resolution),
          .east =
              std::vector<SedimentTransportPacket>(
                  resolution),
          .south =
              std::vector<SedimentTransportPacket>(
                  resolution),
          .west =
              std::vector<SedimentTransportPacket>(
                  resolution)}
{
    if (resolution_ == 0U ||
        !std::isfinite(
            spacingMeters_) ||
        spacingMeters_ <= 0.0 ||
        !conversionRules_.
            IsValid())
    {
        throw std::invalid_argument(
            "Orbit M14 sediment exchange page configuration is invalid.");
    }
}

u32 SedimentExchangePage::Resolution() const noexcept
{
    return resolution_;
}

f64 SedimentExchangePage::SpacingMeters() const noexcept
{
    return spacingMeters_;
}

MobileSedimentCell&
SedimentExchangePage::At(
    const u32 x,
    const u32 y)
{
    return
        cells_[Index(x, y)];
}

const MobileSedimentCell&
SedimentExchangePage::At(
    const u32 x,
    const u32 y) const
{
    return
        cells_[Index(x, y)];
}

const SedimentConversionRules&
SedimentExchangePage::ConversionRules() const noexcept
{
    return conversionRules_;
}

const SedimentMassBalance&
SedimentExchangePage::Accounting() const noexcept
{
    return accounting_;
}

const SedimentTransportCell&
SedimentExchangePage::TransportAt(
    const u32 x,
    const u32 y) const
{
    return transport_[Index(x, y)];
}

void SedimentExchangePage::RecordTransport(
    const u32 sourceX,
    const u32 sourceY,
    const i32 targetX,
    const i32 targetY,
    const SedimentTransportMedium medium,
    const SedimentMass& mass)
{
    if (!mass.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M14 cannot record invalid transported sediment mass.");
    }

    if (sourceX >= resolution_ ||
        sourceY >= resolution_)
    {
        throw std::out_of_range(
            "Orbit M14 transport source coordinate is out of range.");
    }

    const f64 east =
        static_cast<f64>(targetX) -
        static_cast<f64>(sourceX);

    // Page-local y grows south. M14 diagnostics expose tangent-space north.
    const f64 north =
        static_cast<f64>(sourceY) -
        static_cast<f64>(targetY);

    AccumulateTransport(
        sourceX,
        sourceY,
        east,
        north,
        medium,
        mass);
}

void SedimentExchangePage::AccumulateTransport(
    const u32 cellX,
    const u32 cellY,
    const f64 eastDirection,
    const f64 northDirection,
    const SedimentTransportMedium medium,
    const SedimentMass& mass)
{
    const f64 length =
        std::hypot(
            eastDirection,
            northDirection);

    if (length <= 0.0 ||
        mass.Empty())
    {
        return;
    }

    const f64 kilograms =
        mass.TotalKg();

    auto& vector =
        transport_[Index(
            cellX,
            cellY)].
            Medium(medium);

    vector.eastKg +=
        kilograms *
        eastDirection /
        length;

    vector.northKg +=
        kilograms *
        northDirection /
        length;
}

void SedimentExchangePage::ClearTransportDiagnostics() noexcept
{
    std::fill(
        transport_.begin(),
        transport_.end(),
        SedimentTransportCell{});
}

void SedimentExchangePage::Add(
    const u32 x,
    const u32 y,
    const SedimentTransportMedium medium,
    const SedimentMass& mass)
{
    if (!mass.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M14 cannot add invalid sediment mass.");
    }

    At(x, y).
        Medium(medium) +=
            mass;
}

void SedimentExchangePage::PublishPhysicalRemoval(
    const u32 x,
    const u32 y,
    const SedimentTransportMedium medium,
    const SedimentMass& mass)
{
    Add(
        x,
        y,
        medium,
        mass);

    accounting_.
        physicalToMobile +=
            mass;
}

SedimentMass SedimentExchangePage::Take(
    const u32 x,
    const u32 y,
    const SedimentTransportMedium medium,
    const SedimentMass& requested)
{
    if (!requested.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M14 cannot take invalid sediment mass.");
    }

    SedimentMass& available =
        At(x, y).
            Medium(medium);

    const SedimentMass taken =
        MinMass(
            available,
            requested);

    available -=
        taken;

    return taken;
}

SedimentMass
SedimentExchangePage::PickupFromColumn(
    terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const u32 x,
    const u32 y,
    const f64 depthMeters,
    const SedimentSourceProcess sourceProcess,
    const SedimentTransportMedium medium)
{
    static_cast<void>(
        NonNegativeFinite(
            depthMeters,
            "Orbit M14 pickup depth must be finite and nonnegative."));

    if (material.Resolution() !=
            resolution_ ||
        std::abs(
            material.SpacingMeters() -
            spacingMeters_) >
            1.0e-9)
    {
        throw std::invalid_argument(
            "Orbit M14 material column does not match the exchange page.");
    }

    const auto& sourceCell =
        material.At(
            x,
            y);

    const auto* rock =
        geology.Find(
            sourceCell.
                bedrockMaterial);

    if (rock == nullptr)
    {
        throw std::invalid_argument(
            "Orbit M14 pickup references an unknown M02 rock.");
    }

    const auto removal =
        material.Erode(
            x,
            y,
            depthMeters,
            geology);

    const SedimentMass mass =
        ClassifyRemovedMaterial(
            removal,
            material,
            *rock,
            sourceProcess,
            conversionRules_);

    Add(
        x,
        y,
        medium,
        mass);

    accounting_.
        physicalToMobile +=
            mass;

    return mass;
}

SedimentDepositResult
SedimentExchangePage::DepositToColumn(
    terrain_material_column::MaterialColumnPage& material,
    const u32 x,
    const u32 y,
    const SedimentTransportMedium medium,
    const f64 maximumTotalMassKg)
{
    if (material.Resolution() !=
            resolution_ ||
        std::abs(
            material.SpacingMeters() -
            spacingMeters_) >
            1.0e-9 ||
        std::isnan(
            maximumTotalMassKg) ||
        maximumTotalMassKg < 0.0)
    {
        throw std::invalid_argument(
            "Orbit M14 deposition request is invalid.");
    }

    SedimentMass& available =
        At(x, y).
            Medium(medium);

    SedimentDepositResult result{
        .requested =
            available
    };

    f64 remainingBudget =
        maximumTotalMassKg;

    const f64 area =
        material.
            CellAreaSquareMeters();

    const auto deposit =
        [&](const SedimentClass kind,
            const terrain_material_column::LooseMaterialKind materialKind,
            const f64 density)
        {
            f64* source = nullptr;
            f64* destination = nullptr;

            switch (kind)
            {
            case SedimentClass::Sand:
                source =
                    &available.sandKg;
                destination =
                    &result.deposited.sandKg;
                break;

            case SedimentClass::Fines:
                source =
                    &available.finesKg;
                destination =
                    &result.deposited.finesKg;
                break;

            case SedimentClass::CoarseDebris:
                source =
                    &available.coarseDebrisKg;
                destination =
                    &result.deposited.coarseDebrisKg;
                break;
            }

            const f64 mass =
                std::min(
                    *source,
                    remainingBudget);

            if (mass <= 0.0)
            {
                return;
            }

            const f64 depth =
                mass /
                std::max(
                    area *
                        density,
                    1.0e-12);

            const f64 depositedMass =
                material.Deposit(
                    x,
                    y,
                    materialKind,
                    depth);

            *source =
                std::max(
                    *source -
                        depositedMass,
                    0.0);

            *destination +=
                depositedMass;

            remainingBudget =
                std::max(
                    remainingBudget -
                        depositedMass,
                    0.0);
        };

    // Canonical deposition priority: coarse -> sand -> fines.
    deposit(
        SedimentClass::CoarseDebris,
        terrain_material_column::
            LooseMaterialKind::
                Debris,
        material.Densities().
            debrisKgPerCubicMeter);

    deposit(
        SedimentClass::Sand,
        terrain_material_column::
            LooseMaterialKind::
                Sand,
        material.Densities().
            sandKgPerCubicMeter);

    deposit(
        SedimentClass::Fines,
        terrain_material_column::
            LooseMaterialKind::
                Soil,
        material.Densities().
            soilKgPerCubicMeter);

    result.remaining =
        available;

    accounting_.
        mobileToPhysical +=
            result.deposited;

    return result;
}

SedimentMass
SedimentExchangePage::ExportAcrossBoundary(
    const u32 sourceX,
    const u32 sourceY,
    const i32 targetX,
    const i32 targetY,
    const SedimentTransportMedium medium,
    const SedimentMass& requested)
{
    if (sourceX >= resolution_ ||
        sourceY >= resolution_ ||
        targetX <
            -1 ||
        targetY <
            -1 ||
        targetX >
            static_cast<i32>(
                resolution_) ||
        targetY >
            static_cast<i32>(
                resolution_))
    {
        throw std::invalid_argument(
            "Orbit M14 boundary export coordinates are invalid.");
    }

    const bool outsideX =
        targetX < 0 ||
        targetX >=
            static_cast<i32>(
                resolution_);

    const bool outsideY =
        targetY < 0 ||
        targetY >=
            static_cast<i32>(
                resolution_);

    if (!outsideX &&
        !outsideY)
    {
        throw std::invalid_argument(
            "Orbit M14 boundary export target must leave the physical page.");
    }

    const SedimentMass exported =
        Take(
            sourceX,
            sourceY,
            medium,
            requested);

    AccumulateBoundary(
        sourceX,
        sourceY,
        targetX,
        targetY,
        medium,
        exported);

    RecordTransport(
        sourceX,
        sourceY,
        targetX,
        targetY,
        medium,
        exported);

    accounting_.
        exported +=
            exported;

    return exported;
}

SedimentBoundaryFlux
SedimentExchangePage::TakeOutgoingBoundaryFlux(
    const u64 revision)
{
    SedimentBoundaryFlux result =
        std::move(
            outgoing_);

    result.revision =
        revision;

    outgoing_ = {
        .north =
            std::vector<SedimentTransportPacket>(
                resolution_),
        .east =
            std::vector<SedimentTransportPacket>(
                resolution_),
        .south =
            std::vector<SedimentTransportPacket>(
                resolution_),
        .west =
            std::vector<SedimentTransportPacket>(
                resolution_)};

    return result;
}

void SedimentExchangePage::ImportBoundaryFlux(
    const SedimentBoundaryFlux& incoming)
{
    if (!incoming.IsComplete(
            resolution_))
    {
        throw std::invalid_argument(
            "Orbit M14 incoming sediment boundary flux is incomplete.");
    }

    const auto importPacket =
        [&](const u32 x,
            const u32 y,
            const f64 eastDirection,
            const f64 northDirection,
            const SedimentTransportPacket& packet)
        {
            At(x, y).
                waterborne +=
                    packet.waterborne;

            At(x, y).
                airborne +=
                    packet.airborne;

            At(x, y).
                surfaceMobile +=
                    packet.surfaceMobile;

            AccumulateTransport(
                x,
                y,
                eastDirection,
                northDirection,
                SedimentTransportMedium::Waterborne,
                packet.waterborne);

            AccumulateTransport(
                x,
                y,
                eastDirection,
                northDirection,
                SedimentTransportMedium::Airborne,
                packet.airborne);

            AccumulateTransport(
                x,
                y,
                eastDirection,
                northDirection,
                SedimentTransportMedium::SurfaceMobile,
                packet.surfaceMobile);

            accounting_.
                imported +=
                    packet.Total();
        };

    for (u32 i = 0U;
         i < resolution_;
         ++i)
    {
        importPacket(
            i,
            0U,
            0.0,
            -1.0,
            incoming.north[i]);

        importPacket(
            resolution_ - 1U,
            i,
            -1.0,
            0.0,
            incoming.east[i]);

        importPacket(
            i,
            resolution_ - 1U,
            0.0,
            1.0,
            incoming.south[i]);

        importPacket(
            0U,
            i,
            1.0,
            0.0,
            incoming.west[i]);
    }

    // NW, NE, SE, SW.
    importPacket(
        0U,
        0U,
        1.0,
        -1.0,
        incoming.corners[0]);

    importPacket(
        resolution_ - 1U,
        0U,
        -1.0,
        -1.0,
        incoming.corners[1]);

    importPacket(
        resolution_ - 1U,
        resolution_ - 1U,
        -1.0,
        1.0,
        incoming.corners[2]);

    importPacket(
        0U,
        resolution_ - 1U,
        1.0,
        1.0,
        incoming.corners[3]);
}

SedimentMass
SedimentExchangePage::TotalMobileMass() const noexcept
{
    SedimentMass total{};

    for (const auto& cell :
         cells_)
    {
        total +=
            cell.Total();
    }

    return total;
}

std::size_t SedimentExchangePage::Index(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution_ ||
        y >= resolution_)
    {
        throw std::out_of_range(
            "Orbit M14 sediment cell coordinate is out of range.");
    }

    return
        static_cast<std::size_t>(y) *
            resolution_ +
        x;
}

void SedimentExchangePage::AccumulateBoundary(
    const u32 sourceX,
    const u32 sourceY,
    const i32 targetX,
    const i32 targetY,
    const SedimentTransportMedium medium,
    const SedimentMass& mass)
{
    if (mass.Empty())
    {
        return;
    }

    const i32 resolution =
        static_cast<i32>(
            resolution_);

    SedimentTransportPacket* packet =
        nullptr;

    if (targetX < 0 &&
        targetY < 0)
    {
        packet =
            &outgoing_.corners[0];
    }
    else if (
        targetX >= resolution &&
        targetY < 0)
    {
        packet =
            &outgoing_.corners[1];
    }
    else if (
        targetX >= resolution &&
        targetY >= resolution)
    {
        packet =
            &outgoing_.corners[2];
    }
    else if (
        targetX < 0 &&
        targetY >= resolution)
    {
        packet =
            &outgoing_.corners[3];
    }
    else if (targetY < 0)
    {
        packet =
            &outgoing_.north[
                sourceX];
    }
    else if (
        targetX >= resolution)
    {
        packet =
            &outgoing_.east[
                sourceY];
    }
    else if (
        targetY >= resolution)
    {
        packet =
            &outgoing_.south[
                sourceX];
    }
    else if (targetX < 0)
    {
        packet =
            &outgoing_.west[
                sourceY];
    }

    if (packet == nullptr)
    {
        throw std::logic_error(
            "Orbit M14 could not resolve boundary export target.");
    }

    packet->
        Medium(medium) +=
            mass;
}

SedimentMass ClassifyRemovedMaterial(
    const terrain_material_column::MaterialRemoval& removal,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterial& sourceRock,
    const SedimentSourceProcess sourceProcess,
    const SedimentConversionRules& rules)
{
    if (!rules.IsValid() ||
        !sourceRock.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M14 material classification received invalid conversion input.");
    }

    const f64 area =
        material.
            CellAreaSquareMeters();

    const auto& density =
        material.
            Densities();

    SedimentMass result{
        .sandKg =
            LayerMass(
                removal.sandMeters,
                area,
                density.
                    sandKgPerCubicMeter),
        .finesKg =
            LayerMass(
                removal.soilMeters,
                area,
                density.
                    soilKgPerCubicMeter) +
            LayerMass(
                removal.regolithMeters,
                area,
                density.
                    regolithKgPerCubicMeter),
        .coarseDebrisKg =
            LayerMass(
                removal.debrisMeters,
                area,
                density.
                    debrisKgPerCubicMeter)
    };

    const f64 bedrockMass =
        LayerMass(
            removal.bedrockMeters,
            area,
            sourceRock.density);

    switch (sourceProcess)
    {
    case SedimentSourceProcess::Hydraulic:
    {
        const f64 sand =
            bedrockMass *
            rules.
                hydraulicBedrockSandFraction;

        result.sandKg +=
            sand;

        result.finesKg +=
            bedrockMass -
            sand;
        break;
    }

    case SedimentSourceProcess::AeolianAbrasion:
    {
        const f64 sand =
            bedrockMass *
            rules.
                aeolianBedrockSandFraction;

        result.sandKg +=
            sand;

        result.finesKg +=
            bedrockMass -
            sand;
        break;
    }

    case SedimentSourceProcess::ThermalFracture:
        result.coarseDebrisKg +=
            bedrockMass;
        break;

    case SedimentSourceProcess::GlacialErosion:
    {
        const f64 sand =
            bedrockMass *
            rules.
                glacialBedrockSandFraction;

        const f64 coarse =
            bedrockMass *
            rules.
                glacialBedrockCoarseFraction;

        result.sandKg +=
            sand;

        result.coarseDebrisKg +=
            coarse;

        result.finesKg +=
            bedrockMass -
            sand -
            coarse;
        break;
    }

    case SedimentSourceProcess::CoastalErosion:
    {
        const f64 sand =
            bedrockMass *
            rules.
                coastalBedrockSandFraction;

        result.sandKg +=
            sand;

        result.finesKg +=
            bedrockMass -
            sand;
        break;
    }
    }

    const f64 classified =
        result.TotalKg();

    const f64 tolerance =
        std::max(
            removal.removedMassKg *
                1.0e-10,
            1.0e-7);

    if (std::abs(
            classified -
            removal.removedMassKg) >
        tolerance)
    {
        throw std::logic_error(
            "Orbit M14 sediment classification did not preserve M08 removal mass.");
    }

    return result;
}

std::size_t
GpuSedimentExchangePage::TexelCount() const noexcept
{
    return
        static_cast<std::size_t>(
            resolution) *
        resolution;
}

std::size_t
GpuSedimentExchangePage::PackedByteSize() const noexcept
{
    return
        (waterborne.size() +
         airborne.size() +
         surfaceMobile.size()) *
        sizeof(
            GpuSedimentMediumTexel);
}

GpuSedimentExchangePage
PackGpuSedimentExchangePage(
    const SedimentExchangePage& page)
{
    GpuSedimentExchangePage result{};
    result.resolution =
        page.Resolution();

    const std::size_t count =
        static_cast<std::size_t>(
            result.resolution) *
        result.resolution;

    result.waterborne.resize(
        count);

    result.airborne.resize(
        count);

    result.surfaceMobile.resize(
        count);

    const f64 area =
        page.SpacingMeters() *
        page.SpacingMeters();

    for (u32 y = 0U;
         y < result.resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < result.resolution;
             ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                    result.resolution +
                x;

            const auto& cell =
                page.At(
                    x,
                    y);

            result.waterborne[index] =
                PackMedium(
                    cell.waterborne,
                    area);

            result.airborne[index] =
                PackMedium(
                    cell.airborne,
                    area);

            result.surfaceMobile[index] =
                PackMedium(
                    cell.surfaceMobile,
                    area);
        }
    }

    return result;
}
} // namespace orbit::terrain_erosion
