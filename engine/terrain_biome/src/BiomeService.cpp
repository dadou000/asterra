#include <orbit/terrain_biome/BiomeService.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace orbit::terrain_biome
{
namespace
{
[[nodiscard]] bool FiniteUnit(
    const f32 value) noexcept
{
    return
        std::isfinite(value) &&
        value >= 0.0F &&
        value <= 1.0F;
}

[[nodiscard]] bool FiniteNonNegative(
    const f32 value) noexcept
{
    return
        std::isfinite(value) &&
        value >= 0.0F;
}
} // namespace

bool BiomePlacementRules::IsValid() const noexcept
{
    return
        FiniteUnit(
            minimumResolvedWeight);
}

bool BiomeSurfaceRules::IsValid() const noexcept
{
    return
        FiniteNonNegative(
            materialInfluence);
}

bool BiomeScatterRules::IsValid() const noexcept
{
    return
        FiniteNonNegative(
            densityMultiplier);
}

bool BiomeProcessModifiers::IsValid() const noexcept
{
    return
        FiniteNonNegative(
            hydraulicErosion) &&
        FiniteNonNegative(
            thermalTransport) &&
        FiniteNonNegative(
            aeolianTransport) &&
        FiniteNonNegative(
            glacialErosion) &&
        FiniteNonNegative(
            coastalErosion) &&
        FiniteNonNegative(
            chemicalWeathering);
}

bool BiomeDefinition::IsValid() const noexcept
{
    return
        id.IsValid() &&
        !name.empty() &&
        placement.IsValid() &&
        surface.IsValid() &&
        scatter.IsValid() &&
        processModifiers.IsValid();
}

BiomeService::BiomeService(
    const universe::BodyId body)
    : body_(body)
{
    if (!body_.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M19 BiomeService requires a valid rocky-body ID.");
    }

    baseBiome_ = {
        .id = BaseBiomeId(
            body_),
        .name = "BaseBiome",
        .placement = {
            .minimumResolvedWeight = 0.0F,
            .enabled = true
        }
    };
}

universe::BodyId
BiomeService::Body() const noexcept
{
    return body_;
}

const BiomeDefinition&
BiomeService::BaseBiome() const noexcept
{
    return baseBiome_;
}

void BiomeService::ConfigureBaseBiome(
    BiomeDefinition definition)
{
    definition.id =
        baseBiome_.id;

    if (!definition.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M19 BaseBiome definition is invalid.");
    }

    baseBiome_ =
        std::move(
            definition);

    ++revision_;
}

void BiomeService::UpsertBiome(
    BiomeDefinition definition)
{
    if (!definition.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M19 optional biome definition is invalid.");
    }

    if (definition.id ==
        baseBiome_.id)
    {
        throw std::invalid_argument(
            "Orbit M19 BaseBiome cannot be inserted as an optional biome.");
    }

    biomes_.insert_or_assign(
        definition.id,
        std::move(
            definition));

    ++revision_;
}

bool BiomeService::RemoveBiome(
    const BiomeId id)
{
    if (id ==
        baseBiome_.id)
    {
        return false;
    }

    const std::size_t removed =
        biomes_.erase(
            id);

    if (removed != 0U)
    {
        ++revision_;
        return true;
    }

    return false;
}

const BiomeDefinition*
BiomeService::Find(
    const BiomeId id) const noexcept
{
    if (id ==
        baseBiome_.id)
    {
        return &baseBiome_;
    }

    const auto found =
        biomes_.find(
            id);

    return
        found != biomes_.end()
            ? &found->second
            : nullptr;
}

std::vector<BiomeDefinition>
BiomeService::Definitions() const
{
    std::vector<BiomeDefinition> result;

    result.reserve(
        biomes_.size() +
        1U);

    result.push_back(
        baseBiome_);

    std::vector<const BiomeDefinition*> ordered;
    ordered.reserve(
        biomes_.size());

    for (const auto& [id, definition] :
         biomes_)
    {
        static_cast<void>(id);
        ordered.push_back(
            &definition);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const BiomeDefinition* a,
           const BiomeDefinition* b)
        {
            return
                a->id <
                b->id;
        });

    for (const BiomeDefinition* definition :
         ordered)
    {
        result.push_back(
            *definition);
    }

    return result;
}

std::vector<ResolvedBiomeWeight>
BiomeService::Resolve(
    const std::span<
        const BiomeWeightContribution> contributions) const
{
    std::unordered_map<BiomeId, f64>
        accumulated;

    for (const BiomeWeightContribution& contribution :
         contributions)
    {
        if (!contribution.id.IsValid() ||
            !std::isfinite(
                contribution.weight) ||
            contribution.weight <= 0.0F ||
            contribution.id ==
                baseBiome_.id)
        {
            continue;
        }

        const auto found =
            biomes_.find(
                contribution.id);

        if (found ==
                biomes_.end() ||
            !found->second.
                placement.
                enabled)
        {
            continue;
        }

        accumulated[
            contribution.id] +=
                static_cast<f64>(
                    contribution.weight);
    }

    std::vector<ResolvedBiomeWeight>
        optional;

    optional.reserve(
        accumulated.size());

    f64 acceptedTotal = 0.0;

    for (const auto& [id, rawWeight] :
         accumulated)
    {
        const auto found =
            biomes_.find(id);

        if (found ==
            biomes_.end())
        {
            continue;
        }

        const f64 clamped =
            std::clamp(
                rawWeight,
                0.0,
                1.0);

        if (clamped <
            static_cast<f64>(
                found->second.
                    placement.
                    minimumResolvedWeight))
        {
            continue;
        }

        optional.push_back({
            .id = id,
            .weight =
                static_cast<f32>(
                    clamped),
            .base = false
        });

        acceptedTotal +=
            clamped;
    }

    std::sort(
        optional.begin(),
        optional.end(),
        [](const ResolvedBiomeWeight& a,
           const ResolvedBiomeWeight& b)
        {
            return
                a.id <
                b.id;
        });

    f64 baseWeight = 0.0;

    if (acceptedTotal <= 1.0)
    {
        baseWeight =
            1.0 -
            acceptedTotal;
    }
    else
    {
        const f64 inverse =
            1.0 /
            acceptedTotal;

        for (ResolvedBiomeWeight& weight :
             optional)
        {
            weight.weight =
                static_cast<f32>(
                    static_cast<f64>(
                        weight.weight) *
                    inverse);
        }

        acceptedTotal = 1.0;
    }

    std::vector<ResolvedBiomeWeight> result;
    result.reserve(
        optional.size() +
        1U);

    result.push_back({
        .id =
            baseBiome_.id,
        .weight =
            static_cast<f32>(
                baseWeight),
        .base = true
    });

    result.insert(
        result.end(),
        optional.begin(),
        optional.end());

    return result;
}

u64 BiomeService::Revision() const noexcept
{
    return revision_;
}

BiomeId BiomeService::BaseBiomeId(
    const universe::BodyId body) noexcept
{
    BiomeId id{
        .high =
            body.high ^
            0x4241534542494f4dULL,
        .low =
            body.low ^
            0x4556303030344d31ULL
    };

    if (!id.IsValid())
    {
        id.low = 1U;
    }

    return id;
}
} // namespace orbit::terrain_biome
