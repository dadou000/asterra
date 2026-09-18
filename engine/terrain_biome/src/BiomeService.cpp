#include <orbit/terrain_biome/BiomeService.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <numbers>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

[[nodiscard]] f64 SmoothStep01(
    const f64 t) noexcept
{
    const f64 x =
        std::clamp(
            t,
            0.0,
            1.0);

    return
        x * x *
        (3.0 -
         2.0 * x);
}

[[nodiscard]] f64 RangeWeight(
    const f64 value,
    const f64 minimum,
    const f64 maximum,
    const f64 lowerFalloff,
    const f64 upperFalloff) noexcept
{
    if (value >= minimum &&
        value <= maximum)
    {
        return 1.0;
    }

    if (value < minimum)
    {
        if (lowerFalloff <= 0.0 ||
            value <=
                minimum -
                    lowerFalloff)
        {
            return 0.0;
        }

        return
            SmoothStep01(
                (value -
                 (minimum -
                  lowerFalloff)) /
                lowerFalloff);
    }

    if (upperFalloff <= 0.0 ||
        value >=
            maximum +
                upperFalloff)
    {
        return 0.0;
    }

    return
        1.0 -
        SmoothStep01(
            (value -
             maximum) /
            upperFalloff);
}

[[nodiscard]] f64 AspectWeight(
    f64 value,
    f64 minimum,
    f64 maximum,
    const f64 lowerFalloff,
    const f64 upperFalloff) noexcept
{
    constexpr f64 tau =
        2.0 *
        std::numbers::pi_v<f64>;

    auto wrap =
        [](f64 angle)
        {
            constexpr f64 localTau =
                2.0 *
                std::numbers::pi_v<f64>;

            angle =
                std::fmod(
                    angle,
                    localTau);

            if (angle < 0.0)
            {
                angle +=
                    localTau;
            }

            return angle;
        };

    value = wrap(value);
    minimum = wrap(minimum);
    maximum = wrap(maximum);

    if (minimum <= maximum)
    {
        return
            RangeWeight(
                value,
                minimum,
                maximum,
                lowerFalloff,
                upperFalloff);
    }

    // Wrapped interval such as [315°, 45°].
    if (value < minimum)
    {
        value += tau;
    }

    maximum += tau;

    return
        RangeWeight(
            value,
            minimum,
            maximum,
            lowerFalloff,
            upperFalloff);
}

[[nodiscard]] f64 SurfaceDistanceMeters(
    const math::Double3& a,
    const math::Double3& b,
    const f64 radiusMeters) noexcept
{
    const auto safeUnit =
        [](const math::Double3& v)
        {
            const f64 length =
                math::Length(v);

            return
                length > 1.0e-12
                    ? v / length
                    : math::Double3{
                        0.0,
                        1.0,
                        0.0
                    };
        };

    const math::Double3 ua =
        safeUnit(a);

    const math::Double3 ub =
        safeUnit(b);

    const f64 dot =
        std::clamp(
            math::Dot(
                ua,
                ub),
            -1.0,
            1.0);

    return
        std::acos(dot) *
        radiusMeters;
}

[[nodiscard]] f64 MaskInfluence(
    const BiomeAuthoredMask& mask,
    const BiomePlacementContext& context) noexcept
{
    if (mask.global)
    {
        return 1.0;
    }

    const f64 distance =
        SurfaceDistanceMeters(
            mask.centerUnitDirection,
            context.unitDirection,
            context.planetRadiusMeters);

    if (distance <=
        mask.innerRadiusMeters)
    {
        return 1.0;
    }

    if (mask.outerRadiusMeters <=
            mask.innerRadiusMeters ||
        distance >=
            mask.outerRadiusMeters)
    {
        return 0.0;
    }

    return
        1.0 -
        SmoothStep01(
            (distance -
             mask.innerRadiusMeters) /
            (mask.outerRadiusMeters -
             mask.innerRadiusMeters));
}

[[nodiscard]] f64 ApplyAuthoredOperation(
    const f64 current,
    const BiomeAuthoredMask& mask,
    const f64 influence) noexcept
{
    const f64 t =
        std::clamp(
            influence *
                mask.opacity,
            0.0,
            1.0);

    f64 result =
        current;

    switch (mask.operation)
    {
    case BiomeAuthoredWeightOperation::Add:
        result =
            current +
            mask.value *
                t;
        break;
    case BiomeAuthoredWeightOperation::Subtract:
        result =
            current -
            mask.value *
                t;
        break;
    case BiomeAuthoredWeightOperation::Replace:
        result =
            current +
            (mask.value -
             current) *
                t;
        break;
    case BiomeAuthoredWeightOperation::Multiply:
        result =
            current *
            (1.0 +
             (mask.value -
              1.0) *
                 t);
        break;
    case BiomeAuthoredWeightOperation::Min:
        result =
            current +
            (std::min(
                 current,
                 mask.value) -
             current) *
                t;
        break;
    case BiomeAuthoredWeightOperation::Max:
        result =
            current +
            (std::max(
                 current,
                 mask.value) -
             current) *
                t;
        break;
    }

    return
        std::clamp(
            result,
            0.0,
            1.0);
}

[[nodiscard]] const BiomeUserFieldValue*
FindUserField(
    const BiomePlacementContext& context,
    const BiomeUserFieldId id) noexcept
{
    for (const auto& field :
         context.userFields)
    {
        if (field.id == id)
        {
            return &field;
        }
    }

    return nullptr;
}

[[nodiscard]] f64 SelectorWeight(
    const BiomeAutomaticSelector& selector,
    const BiomePlacementContext& context) noexcept
{
    f64 weight = 0.0;

    if (selector.field ==
        BiomeSelectorField::GeologyMaterial)
    {
        weight =
            context.substrateRock.IsValid() &&
                    selector.material.IsValid() &&
                    context.substrateRock ==
                        selector.material
                ? 1.0
                : 0.0;
    }
    else
    {
        f64 value = 0.0;
        bool available = true;

        switch (selector.field)
        {
        case BiomeSelectorField::Temperature:
            value =
                context.temperatureC;
            break;
        case BiomeSelectorField::Moisture:
            value =
                context.moisture;
            break;
        case BiomeSelectorField::Rainfall:
            value =
                context.rainfall;
            break;
        case BiomeSelectorField::Elevation:
            value =
                context.elevationMeters;
            break;
        case BiomeSelectorField::Slope:
            value =
                context.slopeDegrees;
            break;
        case BiomeSelectorField::Aspect:
            value =
                context.aspectRadians;
            break;
        case BiomeSelectorField::Latitude:
            value =
                context.latitudeRadians;
            break;
        case BiomeSelectorField::Continentality:
            value =
                context.continentality;
            break;
        case BiomeSelectorField::DistanceToCoastWater:
            value =
                context.
                    distanceToCoastWaterMeters;
            break;
        case BiomeSelectorField::Drainage:
            value =
                context.drainage;
            break;
        case BiomeSelectorField::SoilDepth:
            value =
                context.soilDepthMeters;
            break;
        case BiomeSelectorField::SandDepth:
            value =
                context.sandDepthMeters;
            break;
        case BiomeSelectorField::SolarExposure:
            value =
                context.solarExposure;
            break;
        case BiomeSelectorField::WindExposure:
            value =
                context.windExposure;
            break;
        case BiomeSelectorField::SnowPersistence:
            value =
                context.snowPersistence;
            break;
        case BiomeSelectorField::UserField:
        {
            const auto* field =
                FindUserField(
                    context,
                    selector.userField);

            if (field == nullptr)
            {
                available = false;
            }
            else
            {
                value =
                    field->value;
            }
            break;
        }
        case BiomeSelectorField::GeologyMaterial:
            break;
        }

        if (available)
        {
            weight =
                selector.field ==
                        BiomeSelectorField::Aspect
                    ? AspectWeight(
                        value,
                        selector.minimum,
                        selector.maximum,
                        selector.lowerFalloff,
                        selector.upperFalloff)
                    : RangeWeight(
                        value,
                        selector.minimum,
                        selector.maximum,
                        selector.lowerFalloff,
                        selector.upperFalloff);
        }
    }

    if (selector.invert)
    {
        weight =
            1.0 -
            weight;
    }

    return
        std::clamp(
            weight,
            0.0,
            1.0);
}

[[nodiscard]] bool SameSelectorGroup(
    const BiomeAutomaticSelector& a,
    const BiomeAutomaticSelector& b) noexcept
{
    if (a.field != b.field)
    {
        return false;
    }

    if (a.field ==
        BiomeSelectorField::UserField)
    {
        return
            a.userField ==
            b.userField;
    }

    return true;
}
} // namespace

bool BiomeAutomaticSelector::IsValid() const noexcept
{
    if (!std::isfinite(minimum) ||
        !std::isfinite(maximum) ||
        !std::isfinite(lowerFalloff) ||
        !std::isfinite(upperFalloff) ||
        lowerFalloff < 0.0 ||
        upperFalloff < 0.0)
    {
        return false;
    }

    if (field ==
        BiomeSelectorField::GeologyMaterial)
    {
        return material.IsValid();
    }

    if (field ==
        BiomeSelectorField::UserField)
    {
        return userField.IsValid();
    }

    if (field ==
        BiomeSelectorField::Aspect)
    {
        return true;
    }

    return
        minimum <= maximum;
}

bool BiomeAuthoredMask::IsValid() const noexcept
{
    const bool directionValid =
        std::isfinite(
            centerUnitDirection.x) &&
        std::isfinite(
            centerUnitDirection.y) &&
        std::isfinite(
            centerUnitDirection.z) &&
        math::Length(
            centerUnitDirection) >
            1.0e-12;

    return
        id.IsValid() &&
        directionValid &&
        std::isfinite(
            innerRadiusMeters) &&
        std::isfinite(
            outerRadiusMeters) &&
        innerRadiusMeters >= 0.0 &&
        outerRadiusMeters >=
            innerRadiusMeters &&
        std::isfinite(value) &&
        value >= 0.0 &&
        value <= 1.0 &&
        std::isfinite(opacity) &&
        opacity >= 0.0 &&
        opacity <= 1.0 &&
        (global ||
         outerRadiusMeters > 0.0);
}

bool BiomePlacementContext::IsValid() const noexcept
{
    const auto finite =
        [](const f64 value)
        {
            return
                std::isfinite(value);
        };

    if (!finite(
            unitDirection.x) ||
        !finite(
            unitDirection.y) ||
        !finite(
            unitDirection.z) ||
        math::Length(
            unitDirection) <=
            1.0e-12 ||
        !finite(
            planetRadiusMeters) ||
        planetRadiusMeters <= 0.0 ||
        !finite(
            temperatureC) ||
        !finite(
            moisture) ||
        moisture < 0.0 ||
        moisture > 1.0 ||
        !finite(
            rainfall) ||
        rainfall < 0.0 ||
        !finite(
            elevationMeters) ||
        !finite(
            slopeDegrees) ||
        slopeDegrees < 0.0 ||
        !finite(
            aspectRadians) ||
        !finite(
            latitudeRadians) ||
        latitudeRadians <
            -std::numbers::pi_v<f64> /
                2.0 ||
        latitudeRadians >
            std::numbers::pi_v<f64> /
                2.0 ||
        !finite(
            continentality) ||
        continentality < 0.0 ||
        continentality > 1.0 ||
        !finite(
            distanceToCoastWaterMeters) ||
        distanceToCoastWaterMeters < 0.0 ||
        !finite(
            drainage) ||
        drainage < 0.0 ||
        !finite(
            soilDepthMeters) ||
        soilDepthMeters < 0.0 ||
        !finite(
            sandDepthMeters) ||
        sandDepthMeters < 0.0 ||
        !finite(
            solarExposure) ||
        solarExposure < 0.0 ||
        solarExposure > 1.0 ||
        !finite(
            windExposure) ||
        windExposure < 0.0 ||
        windExposure > 1.0 ||
        !finite(
            snowPersistence) ||
        snowPersistence < 0.0 ||
        snowPersistence > 1.0)
    {
        return false;
    }

    for (const auto& field :
         userFields)
    {
        if (!field.id.IsValid() ||
            !finite(field.value))
        {
            return false;
        }
    }

    return true;
}

BiomeUserFieldId BiomeUserFieldIdFromName(
    const std::string_view name) noexcept
{
    // Two deterministic 64-bit FNV-1a streams with distinct offsets. This is
    // an opaque stable field key, not cryptographic identity.
    u64 high =
        1469598103934665603ULL;

    u64 low =
        1099511628211ULL ^
        0x4d32305553455246ULL;

    for (const unsigned char value :
         name)
    {
        high ^=
            static_cast<u64>(value);

        high *=
            1099511628211ULL;

        low ^=
            static_cast<u64>(value) +
            0x9eU;

        low *=
            1469598103934665603ULL;
    }

    BiomeUserFieldId id{
        .high = high,
        .low = low
    };

    if (!id.IsValid())
    {
        id.low = 1U;
    }

    return id;
}

bool BiomePlacementRules::IsValid() const noexcept
{
    if (!FiniteUnit(
            minimumResolvedWeight))
    {
        return false;
    }

    for (const auto& selector :
         selectors)
    {
        if (!selector.IsValid())
        {
            return false;
        }
    }

    std::unordered_set<
        BiomeAuthoredMaskId>
        ids;

    for (const auto& mask :
         authoredMasks)
    {
        if (!mask.IsValid() ||
            !ids.insert(
                 mask.id).
                 second)
        {
            return false;
        }
    }

    return true;
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

BiomePlacementEvaluation
BiomeService::EvaluatePlacement(
    const BiomeDefinition& biome,
    const BiomePlacementContext& context) const
{
    if (!biome.IsValid() ||
        !context.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M20 biome placement input is invalid.");
    }

    struct SelectorGroup
    {
        const BiomeAutomaticSelector* exemplar{nullptr};
        f64 weight{0.0};
    };

    std::vector<SelectorGroup> groups;

    for (const auto& selector :
         biome.placement.selectors)
    {
        if (!selector.enabled)
        {
            continue;
        }

        auto found =
            std::find_if(
                groups.begin(),
                groups.end(),
                [&selector](
                    const SelectorGroup& group)
                {
                    return
                        group.exemplar !=
                            nullptr &&
                        SameSelectorGroup(
                            *group.exemplar,
                            selector);
                });

        const f64 weight =
            SelectorWeight(
                selector,
                context);

        if (found ==
            groups.end())
        {
            groups.push_back({
                .exemplar =
                    &selector,
                .weight =
                    weight
            });
        }
        else
        {
            found->weight =
                std::max(
                    found->weight,
                    weight);
        }
    }

    f64 automatic =
        groups.empty()
            ? 0.0
            : 1.0;

    for (const auto& group :
         groups)
    {
        automatic *=
            group.weight;
    }

    f64 authoredOnly = 0.0;

    for (const auto& mask :
         biome.placement.
             authoredMasks)
    {
        if (!mask.enabled)
        {
            continue;
        }

        authoredOnly =
            ApplyAuthoredOperation(
                authoredOnly,
                mask,
                MaskInfluence(
                    mask,
                    context));
    }

    f64 finalWeight = 0.0;

    switch (biome.placement.mode)
    {
    case BiomePlacementMode::Automatic:
        finalWeight =
            automatic;
        break;

    case BiomePlacementMode::Authored:
        finalWeight =
            authoredOnly;
        break;

    case BiomePlacementMode::AutomaticAndAuthored:
        finalWeight =
            automatic;

        for (const auto& mask :
             biome.placement.
                 authoredMasks)
        {
            if (!mask.enabled)
            {
                continue;
            }

            finalWeight =
                ApplyAuthoredOperation(
                    finalWeight,
                    mask,
                    MaskInfluence(
                        mask,
                        context));
        }
        break;
    }

    return {
        .automaticWeight =
            std::clamp(
                automatic,
                0.0,
                1.0),
        .authoredWeight =
            std::clamp(
                authoredOnly,
                0.0,
                1.0),
        .finalWeight =
            std::clamp(
                finalWeight,
                0.0,
                1.0)
    };
}

std::vector<ResolvedBiomeWeight>
BiomeService::ResolvePlacement(
    const BiomePlacementContext& context) const
{
    if (!context.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M20 placement context is invalid.");
    }

    std::vector<BiomeWeightContribution>
        contributions;

    contributions.reserve(
        biomes_.size());

    for (const auto& [id, biome] :
         biomes_)
    {
        if (!biome.placement.enabled)
        {
            continue;
        }

        const auto evaluated =
            EvaluatePlacement(
                biome,
                context);

        if (evaluated.finalWeight >
            0.0)
        {
            contributions.push_back({
                .id = id,
                .weight =
                    static_cast<f32>(
                        evaluated.
                            finalWeight)
            });
        }
    }

    return
        Resolve(
            contributions);
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
