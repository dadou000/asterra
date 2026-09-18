#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::terrain_material_column
{
namespace
{
constexpr f32 kLayerEpsilonMeters = 1.0e-6F;

[[nodiscard]] bool FiniteNonNegative(const f32 value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}

[[nodiscard]] bool FinitePositive(const f64 value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] const terrain_geology::GeologicalMaterial&
RequireRock(
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const terrain_geology::RockTypeId id)
{
    const auto* material = geology.Find(id);
    if (material == nullptr)
    {
        throw std::invalid_argument(
            "M08 material column references an unknown RockTypeId.");
    }
    return *material;
}

[[nodiscard]] f64 RemoveLayer(
    f32& layerMeters,
    const f64 remainingMeters) noexcept
{
    if (remainingMeters <= 0.0 ||
        layerMeters <= 0.0F)
    {
        return 0.0;
    }

    const f64 removed =
        std::min(
            static_cast<f64>(layerMeters),
            remainingMeters);

    layerMeters =
        static_cast<f32>(
            std::max(
                0.0,
                static_cast<f64>(layerMeters) - removed));

    return removed;
}

[[nodiscard]] f64 LayerMass(
    const f64 depthMeters,
    const f64 areaSquareMeters,
    const f64 densityKgPerCubicMeter) noexcept
{
    return std::max(depthMeters, 0.0) *
        areaSquareMeters *
        densityKgPerCubicMeter;
}
} // namespace

bool LooseMaterialDensities::IsValid() const noexcept
{
    return
        std::isfinite(regolithKgPerCubicMeter) &&
        regolithKgPerCubicMeter > 0.0F &&
        std::isfinite(soilKgPerCubicMeter) &&
        soilKgPerCubicMeter > 0.0F &&
        std::isfinite(sandKgPerCubicMeter) &&
        sandKgPerCubicMeter > 0.0F &&
        std::isfinite(debrisKgPerCubicMeter) &&
        debrisKgPerCubicMeter > 0.0F;
}

f32 LooseMaterialDensities::Density(
    const LooseMaterialKind kind) const noexcept
{
    switch (kind)
    {
    case LooseMaterialKind::Regolith:
        return regolithKgPerCubicMeter;
    case LooseMaterialKind::Soil:
        return soilKgPerCubicMeter;
    case LooseMaterialKind::Sand:
        return sandKgPerCubicMeter;
    case LooseMaterialKind::Debris:
        return debrisKgPerCubicMeter;
    }

    return regolithKgPerCubicMeter;
}

bool MaterialColumnCell::IsValid() const noexcept
{
    return
        std::isfinite(bedrockHeightMeters) &&
        std::isfinite(referenceBedrockHeightMeters) &&
        bedrockMaterial.IsValid() &&
        FiniteNonNegative(regolithMeters) &&
        FiniteNonNegative(soilMeters) &&
        FiniteNonNegative(sandMeters) &&
        FiniteNonNegative(debrisMeters) &&
        std::isfinite(moisture) &&
        moisture >= 0.0F &&
        moisture <= 1.0F &&
        std::isfinite(temporaryScalar);
}

f32 MaterialColumnCell::LooseDepthMeters() const noexcept
{
    return
        regolithMeters +
        soilMeters +
        sandMeters +
        debrisMeters;
}

f32 MaterialColumnCell::SurfaceHeightMeters() const noexcept
{
    return
        bedrockHeightMeters +
        LooseDepthMeters();
}

ExposedSurfaceKind MaterialColumnCell::ExposedSurface() const noexcept
{
    if (debrisMeters > kLayerEpsilonMeters)
    {
        return ExposedSurfaceKind::Debris;
    }

    if (sandMeters > kLayerEpsilonMeters)
    {
        return ExposedSurfaceKind::Sand;
    }

    if (soilMeters > kLayerEpsilonMeters)
    {
        return ExposedSurfaceKind::Soil;
    }

    if (regolithMeters > kLayerEpsilonMeters)
    {
        return ExposedSurfaceKind::Regolith;
    }

    return ExposedSurfaceKind::Bedrock;
}

f64 MaterialMassSummary::LooseMassKg() const noexcept
{
    return
        regolithKg +
        soilKg +
        sandKg +
        debrisKg;
}

f64 MaterialMassSummary::AccountedMassKg() const noexcept
{
    return
        LooseMassKg() +
        excavatedBedrockKg;
}

MaterialColumnPage::MaterialColumnPage(
    const u32 resolution,
    const f64 spacingMeters,
    const LooseMaterialDensities densities)
    : resolution_(resolution),
      spacingMeters_(spacingMeters),
      densities_(densities)
{
    if (resolution_ == 0 ||
        !FinitePositive(spacingMeters_) ||
        !densities_.IsValid())
    {
        throw std::invalid_argument(
            "M08 MaterialColumnPage requires a nonzero resolution, "
            "positive spacing, and valid densities.");
    }

    const std::size_t count =
        static_cast<std::size_t>(resolution_) *
        static_cast<std::size_t>(resolution_);

    if (count / resolution_ != resolution_)
    {
        throw std::overflow_error(
            "M08 MaterialColumnPage resolution overflow.");
    }

    cells_.resize(count);
}

u32 MaterialColumnPage::Resolution() const noexcept
{
    return resolution_;
}

f64 MaterialColumnPage::SpacingMeters() const noexcept
{
    return spacingMeters_;
}

f64 MaterialColumnPage::CellAreaSquareMeters() const noexcept
{
    return spacingMeters_ * spacingMeters_;
}

std::size_t MaterialColumnPage::Index(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution_ || y >= resolution_)
    {
        throw std::out_of_range(
            "M08 material-column coordinate is outside the page.");
    }

    return
        static_cast<std::size_t>(y) *
            resolution_ +
        x;
}

MaterialColumnCell& MaterialColumnPage::At(
    const u32 x,
    const u32 y)
{
    return cells_[Index(x, y)];
}

const MaterialColumnCell& MaterialColumnPage::At(
    const u32 x,
    const u32 y) const
{
    return cells_[Index(x, y)];
}

void MaterialColumnPage::SetCell(
    const u32 x,
    const u32 y,
    MaterialColumnCell cell,
    const bool establishReference)
{
    if (establishReference)
    {
        cell.referenceBedrockHeightMeters =
            cell.bedrockHeightMeters;
    }

    if (!cell.IsValid())
    {
        throw std::invalid_argument(
            "M08 cannot install an invalid material-column cell.");
    }

    At(x, y) = cell;
}

void MaterialColumnPage::DisplaceBedrock(
    const u32 x,
    const u32 y,
    const f64 deltaMeters,
    const bool shiftReference)
{
    if (!std::isfinite(deltaMeters))
    {
        throw std::invalid_argument(
            "M08 bedrock displacement must be finite.");
    }

    MaterialColumnCell& cell = At(x, y);

    if (!cell.IsValid())
    {
        throw std::logic_error(
            "M08 cannot displace an uninitialized material-column cell.");
    }

    const f64 nextBedrock =
        static_cast<f64>(
            cell.bedrockHeightMeters) +
        deltaMeters;

    const f64 nextReference =
        static_cast<f64>(
            cell.referenceBedrockHeightMeters) +
        (shiftReference ? deltaMeters : 0.0);

    if (!std::isfinite(nextBedrock) ||
        !std::isfinite(nextReference) ||
        std::abs(nextBedrock) >
            std::numeric_limits<f32>::max() ||
        std::abs(nextReference) >
            std::numeric_limits<f32>::max())
    {
        throw std::overflow_error(
            "M08 bedrock displacement exceeds representable range.");
    }

    cell.bedrockHeightMeters =
        static_cast<f32>(nextBedrock);

    if (shiftReference)
    {
        cell.referenceBedrockHeightMeters =
            static_cast<f32>(nextReference);
    }
}

MaterialRemoval MaterialColumnPage::Erode(
    const u32 x,
    const u32 y,
    const f64 depthMeters,
    const terrain_geology::GeologicalMaterialLibrary& geology)
{
    if (!std::isfinite(depthMeters) ||
        depthMeters < 0.0)
    {
        throw std::invalid_argument(
            "M08 erosion depth must be finite and nonnegative.");
    }

    MaterialColumnCell& cell = At(x, y);

    if (!cell.IsValid())
    {
        throw std::logic_error(
            "M08 cannot erode an uninitialized material-column cell.");
    }

    MaterialRemoval removal{
        .requestedDepthMeters = depthMeters
    };

    f64 remaining = depthMeters;

    // Canonical top-down stack: debris -> sand -> soil -> regolith -> bedrock.
    removal.debrisMeters =
        RemoveLayer(cell.debrisMeters, remaining);
    remaining -= removal.debrisMeters;

    removal.sandMeters =
        RemoveLayer(cell.sandMeters, remaining);
    remaining -= removal.sandMeters;

    removal.soilMeters =
        RemoveLayer(cell.soilMeters, remaining);
    remaining -= removal.soilMeters;

    removal.regolithMeters =
        RemoveLayer(cell.regolithMeters, remaining);
    remaining -= removal.regolithMeters;

    if (remaining > 0.0)
    {
        removal.bedrockMeters = remaining;
        cell.bedrockHeightMeters =
            static_cast<f32>(
                static_cast<f64>(
                    cell.bedrockHeightMeters) -
                removal.bedrockMeters);
        remaining = 0.0;
    }

    removal.removedDepthMeters =
        removal.debrisMeters +
        removal.sandMeters +
        removal.soilMeters +
        removal.regolithMeters +
        removal.bedrockMeters;

    const f64 area = CellAreaSquareMeters();
    const auto& rock =
        RequireRock(geology, cell.bedrockMaterial);

    removal.removedMassKg =
        LayerMass(
            removal.debrisMeters,
            area,
            densities_.debrisKgPerCubicMeter) +
        LayerMass(
            removal.sandMeters,
            area,
            densities_.sandKgPerCubicMeter) +
        LayerMass(
            removal.soilMeters,
            area,
            densities_.soilKgPerCubicMeter) +
        LayerMass(
            removal.regolithMeters,
            area,
            densities_.regolithKgPerCubicMeter) +
        LayerMass(
            removal.bedrockMeters,
            area,
            rock.density);

    return removal;
}

f64 MaterialColumnPage::Deposit(
    const u32 x,
    const u32 y,
    const LooseMaterialKind kind,
    const f64 depthMeters)
{
    if (!std::isfinite(depthMeters) ||
        depthMeters < 0.0)
    {
        throw std::invalid_argument(
            "M08 deposition depth must be finite and nonnegative.");
    }

    MaterialColumnCell& cell = At(x, y);

    if (!cell.IsValid())
    {
        throw std::logic_error(
            "M08 cannot deposit onto an uninitialized material-column cell.");
    }

    f32* layer = nullptr;

    switch (kind)
    {
    case LooseMaterialKind::Regolith:
        layer = &cell.regolithMeters;
        break;
    case LooseMaterialKind::Soil:
        layer = &cell.soilMeters;
        break;
    case LooseMaterialKind::Sand:
        layer = &cell.sandMeters;
        break;
    case LooseMaterialKind::Debris:
        layer = &cell.debrisMeters;
        break;
    }

    const f64 next =
        static_cast<f64>(*layer) +
        depthMeters;

    if (!std::isfinite(next) ||
        next > std::numeric_limits<f32>::max())
    {
        throw std::overflow_error(
            "M08 deposition exceeds representable layer depth.");
    }

    *layer = static_cast<f32>(next);

    return LayerMass(
        depthMeters,
        CellAreaSquareMeters(),
        densities_.Density(kind));
}

MaterialRemoval MaterialColumnPage::ApplyImpact(
    const u32 x,
    const u32 y,
    const terrain_impacts::CraterProcessSample& impact,
    const terrain_geology::GeologicalMaterialLibrary& geology)
{
    if (!std::isfinite(impact.excavationDepthMeters) ||
        impact.excavationDepthMeters < 0.0 ||
        !std::isfinite(impact.ejectaThicknessMeters) ||
        impact.ejectaThicknessMeters < 0.0 ||
        !std::isfinite(impact.rayField))
    {
        throw std::invalid_argument(
            "M08 received invalid M07 crater process channels.");
    }

    MaterialRemoval removal =
        Erode(
            x,
            y,
            impact.excavationDepthMeters,
            geology);

    if (impact.ejectaThicknessMeters > 0.0)
    {
        static_cast<void>(
            Deposit(
                x,
                y,
                LooseMaterialKind::Debris,
                impact.ejectaThicknessMeters));
    }

    MaterialColumnCell& cell = At(x, y);
    cell.temporaryScalar =
        static_cast<f32>(
            std::clamp(
                impact.rayField,
                0.0,
                1.0));

    return removal;
}

MaterialMassSummary MaterialColumnPage::QueryMass(
    const terrain_geology::GeologicalMaterialLibrary& geology) const
{
    MaterialMassSummary result{};
    const f64 area = CellAreaSquareMeters();

    for (const MaterialColumnCell& cell : cells_)
    {
        if (!cell.IsValid())
        {
            throw std::logic_error(
                "M08 mass accounting encountered an uninitialized cell.");
        }

        result.regolithKg +=
            LayerMass(
                cell.regolithMeters,
                area,
                densities_.regolithKgPerCubicMeter);
        result.soilKg +=
            LayerMass(
                cell.soilMeters,
                area,
                densities_.soilKgPerCubicMeter);
        result.sandKg +=
            LayerMass(
                cell.sandMeters,
                area,
                densities_.sandKgPerCubicMeter);
        result.debrisKg +=
            LayerMass(
                cell.debrisMeters,
                area,
                densities_.debrisKgPerCubicMeter);

        const f64 excavated =
            std::max(
                static_cast<f64>(
                    cell.referenceBedrockHeightMeters) -
                static_cast<f64>(
                    cell.bedrockHeightMeters),
                0.0);

        if (excavated > 0.0)
        {
            const auto& rock =
                RequireRock(
                    geology,
                    cell.bedrockMaterial);

            result.excavatedBedrockKg +=
                LayerMass(
                    excavated,
                    area,
                    rock.density);
        }
    }

    return result;
}

std::span<const MaterialColumnCell>
MaterialColumnPage::Cells() const noexcept
{
    return cells_;
}

const LooseMaterialDensities&
MaterialColumnPage::Densities() const noexcept
{
    return densities_;
}

u16 FloatToHalfBits(const f32 value) noexcept
{
    const u32 bits = std::bit_cast<u32>(value);
    const u32 sign = (bits >> 16U) & 0x8000U;
    const u32 exponent = (bits >> 23U) & 0xFFU;
    u32 mantissa = bits & 0x007FFFFFU;

    if (exponent == 0xFFU)
    {
        if (mantissa == 0U)
        {
            return static_cast<u16>(
                sign | 0x7C00U);
        }

        mantissa >>= 13U;
        return static_cast<u16>(
            sign | 0x7C00U |
            mantissa |
            (mantissa == 0U ? 1U : 0U));
    }

    const i32 unbiased =
        static_cast<i32>(exponent) - 127;
    i32 halfExponent = unbiased + 15;

    if (halfExponent >= 31)
    {
        return static_cast<u16>(
            sign | 0x7C00U);
    }

    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
        {
            return static_cast<u16>(sign);
        }

        mantissa |= 0x00800000U;
        const u32 shift =
            static_cast<u32>(14 - halfExponent);
        u32 halfMantissa = mantissa >> shift;

        const u32 remainderMask =
            (1U << shift) - 1U;
        const u32 remainder =
            mantissa & remainderMask;
        const u32 halfway =
            1U << (shift - 1U);

        if (remainder > halfway ||
            (remainder == halfway &&
             (halfMantissa & 1U) != 0U))
        {
            ++halfMantissa;
        }

        return static_cast<u16>(
            sign | halfMantissa);
    }

    u32 halfMantissa = mantissa >> 13U;
    const u32 remainder = mantissa & 0x1FFFU;

    if (remainder > 0x1000U ||
        (remainder == 0x1000U &&
         (halfMantissa & 1U) != 0U))
    {
        ++halfMantissa;

        if (halfMantissa == 0x400U)
        {
            halfMantissa = 0U;
            ++halfExponent;

            if (halfExponent >= 31)
            {
                return static_cast<u16>(
                    sign | 0x7C00U);
            }
        }
    }

    return static_cast<u16>(
        sign |
        (static_cast<u32>(halfExponent) << 10U) |
        (halfMantissa & 0x3FFU));
}

f32 HalfBitsToFloat(const u16 bits) noexcept
{
    const u32 sign =
        (static_cast<u32>(bits) & 0x8000U)
        << 16U;
    const u32 exponent =
        (static_cast<u32>(bits) >> 10U) &
        0x1FU;
    u32 mantissa =
        static_cast<u32>(bits) & 0x03FFU;

    u32 resultBits = 0U;

    if (exponent == 0U)
    {
        if (mantissa == 0U)
        {
            resultBits = sign;
        }
        else
        {
            i32 exponent32 = 127 - 14;

            while ((mantissa & 0x0400U) == 0U)
            {
                mantissa <<= 1U;
                --exponent32;
            }

            mantissa &= 0x03FFU;

            resultBits =
                sign |
                (static_cast<u32>(exponent32) << 23U) |
                (mantissa << 13U);
        }
    }
    else if (exponent == 0x1FU)
    {
        resultBits =
            sign |
            0x7F800000U |
            (mantissa << 13U);
    }
    else
    {
        const u32 exponent32 =
            exponent + (127U - 15U);

        resultBits =
            sign |
            (exponent32 << 23U) |
            (mantissa << 13U);
    }

    return std::bit_cast<f32>(resultBits);
}

std::size_t GpuMaterialColumnPage::TexelCount() const noexcept
{
    return
        static_cast<std::size_t>(resolution) *
        static_cast<std::size_t>(resolution);
}

std::size_t GpuMaterialColumnPage::PackedByteSize() const noexcept
{
    return
        bedrockHeightR32F.size() *
            sizeof(f32) +
        looseRgba16F.size() *
            sizeof(GpuLooseMaterialTexel) +
        moistureProcessRg16F.size() *
            sizeof(GpuMoistureProcessTexel) +
        geologicalMaterialR16Uint.size() *
            sizeof(u16);
}

GpuMaterialColumnPage PackGpuPage(
    const MaterialColumnPage& page,
    const terrain_geology::GeologicalMaterialGpuTable& geologyTable)
{
    GpuMaterialColumnPage packed{
        .resolution = page.Resolution()
    };

    const std::span<const MaterialColumnCell> cells =
        page.Cells();

    packed.bedrockHeightR32F.reserve(cells.size());
    packed.looseRgba16F.reserve(cells.size());
    packed.moistureProcessRg16F.reserve(cells.size());
    packed.geologicalMaterialR16Uint.reserve(cells.size());

    for (const MaterialColumnCell& cell : cells)
    {
        if (!cell.IsValid())
        {
            throw std::logic_error(
                "M08 cannot pack an invalid/uninitialized material cell.");
        }

        const auto materialIndex =
            geologyTable.IndexOf(
                cell.bedrockMaterial);

        if (!materialIndex.has_value() ||
            *materialIndex >
                std::numeric_limits<u16>::max())
        {
            throw std::invalid_argument(
                "M08 GPU page cannot encode the cell RockTypeId in R16_UINT.");
        }

        packed.bedrockHeightR32F.push_back(
            cell.bedrockHeightMeters);

        packed.looseRgba16F.push_back({
            .regolith =
                FloatToHalfBits(
                    cell.regolithMeters),
            .soil =
                FloatToHalfBits(
                    cell.soilMeters),
            .sand =
                FloatToHalfBits(
                    cell.sandMeters),
            .debris =
                FloatToHalfBits(
                    cell.debrisMeters)
        });

        packed.moistureProcessRg16F.push_back({
            .moisture =
                FloatToHalfBits(
                    cell.moisture),
            .temporaryScalar =
                FloatToHalfBits(
                    cell.temporaryScalar)
        });

        packed.geologicalMaterialR16Uint.push_back(
            static_cast<u16>(
                *materialIndex));
    }

    return packed;
}

} // namespace orbit::terrain_material_column
