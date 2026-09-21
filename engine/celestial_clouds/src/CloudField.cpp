#include <orbit/celestial_clouds/CloudField.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_clouds
{
namespace
{
[[nodiscard]] math::Double3 FaceDirection(
    const u32 face,
    const f64 u,
    const f64 v)
{
    math::Double3 p{};
    switch (face)
    {
    case 0: p = { 1.0, v, -u}; break;
    case 1: p = {-1.0, v,  u}; break;
    case 2: p = { u, 1.0, -v}; break;
    case 3: p = { u,-1.0,  v}; break;
    case 4: p = { u, v, 1.0}; break;
    default:p = {-u, v,-1.0}; break;
    }
    return math::Normalize(p);
}

[[nodiscard]] f64 SmoothStep(
    const f64 a,
    const f64 b,
    const f64 x) noexcept
{
    if (b <= a)
    {
        return x >= b ? 1.0 : 0.0;
    }
    const f64 t =
        std::clamp(
            (x - a) / (b - a),
            0.0,
            1.0);
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] math::Double3 RotateByAngularVelocity(
    math::Double3 direction,
    const math::Double3 omega,
    const f64 seconds) noexcept
{
    const f64 speed =
        math::Length(omega);

    if (speed <= 1.0e-20 ||
        std::abs(seconds) <= 1.0e-20)
    {
        return direction;
    }

    const math::Double3 axis =
        omega / speed;
    const f64 angle =
        speed * seconds;
    const f64 c =
        std::cos(angle);
    const f64 s =
        std::sin(angle);

    return math::Normalize(
        direction * c +
        math::Cross(axis, direction) * s +
        axis *
            math::Dot(axis, direction) *
            (1.0 - c));
}

[[nodiscard]] f64 FractalPattern(
    const math::Double3 p,
    const f64 weatherScale,
    const f64 detailScale,
    const u64 seed) noexcept
{
    const f64 phase =
        static_cast<f64>(
            terrain::StableMix64(seed) &
            0xffffULL) /
        65535.0 *
        2.0 *
        std::numbers::pi_v<f64>;

    const auto wave =
        [&](const f64 scale,
            const f64 offset) noexcept
        {
            return
                0.5 +
                0.5 *
                std::sin(
                    p.x * scale * 1.17 +
                    p.y * scale * 1.73 +
                    p.z * scale * 2.11 +
                    phase +
                    offset);
        };

    const f64 broad =
        0.58 * wave(weatherScale, 0.0) +
        0.27 * wave(weatherScale * 1.91, 1.7) +
        0.15 * wave(weatherScale * 3.43, 4.1);

    const f64 detail =
        0.55 * wave(detailScale, 2.3) +
        0.30 * wave(detailScale * 1.83, 5.2) +
        0.15 * wave(detailScale * 3.17, 0.8);

    return std::clamp(
        broad * 0.78 +
        detail * 0.22,
        0.0,
        1.0);
}

[[nodiscard]] std::size_t TexelIndex(
    const u32 face,
    const u32 x,
    const u32 y,
    const u32 resolution)
{
    return
        static_cast<std::size_t>(face) *
            resolution * resolution +
        static_cast<std::size_t>(y) *
            resolution +
        x;
}

struct CubeLookup
{
    u32 face{0};
    u32 x{0};
    u32 y{0};
};

[[nodiscard]] CubeLookup DirectionToCube(
    math::Double3 d,
    const u32 resolution)
{
    d = math::Normalize(d);

    const f64 ax = std::abs(d.x);
    const f64 ay = std::abs(d.y);
    const f64 az = std::abs(d.z);

    u32 face = 0U;
    f64 u = 0.0;
    f64 v = 0.0;

    if (ax >= ay && ax >= az)
    {
        if (d.x >= 0.0)
        {
            face = 0U;
            u = -d.z / ax;
            v = d.y / ax;
        }
        else
        {
            face = 1U;
            u = d.z / ax;
            v = d.y / ax;
        }
    }
    else if (ay >= ax && ay >= az)
    {
        if (d.y >= 0.0)
        {
            face = 2U;
            u = d.x / ay;
            v = -d.z / ay;
        }
        else
        {
            face = 3U;
            u = d.x / ay;
            v = d.z / ay;
        }
    }
    else
    {
        if (d.z >= 0.0)
        {
            face = 4U;
            u = d.x / az;
            v = d.y / az;
        }
        else
        {
            face = 5U;
            u = -d.x / az;
            v = d.y / az;
        }
    }

    const auto toIndex =
        [resolution](const f64 value)
        {
            return static_cast<u32>(
                std::clamp(
                    std::llround(
                        (value * 0.5 + 0.5) *
                        static_cast<f64>(
                            resolution - 1U)),
                    0LL,
                    static_cast<long long>(
                        resolution - 1U)));
        };

    return {
        .face = face,
        .x = toIndex(u),
        .y = toIndex(v)
    };
}

[[nodiscard]] u64 LayerFingerprint(
    const CloudLayerParameters& p)
{
    u64 value =
        0x4d3233434c4f5544ULL;
    const auto add =
        [&](const u64 item)
        {
            value =
                terrain::StableCombine64(
                    value,
                    item);
        };

    add(p.semanticIdHigh);
    add(p.semanticIdLow);
    add(static_cast<u64>(p.sourceModel));
    add(std::bit_cast<u64>(p.baseAltitudeMeters));
    add(std::bit_cast<u64>(p.topAltitudeMeters));
    add(std::bit_cast<u64>(p.coverageBias));
    add(std::bit_cast<u64>(p.peakOpticalDepth));
    add(std::bit_cast<u64>(p.singleScatteringAlbedo));
    add(std::bit_cast<u64>(p.anisotropy));
    add(std::bit_cast<u64>(p.densityExponent));
    add(std::bit_cast<u64>(p.weatherScale));
    add(std::bit_cast<u64>(p.detailScale));
    add(p.seed);
    add(std::bit_cast<u64>(p.windAngularRadiansPerSecond.x));
    add(std::bit_cast<u64>(p.windAngularRadiansPerSecond.y));
    add(std::bit_cast<u64>(p.windAngularRadiansPerSecond.z));
    add(p.shadowParticipation ? 1U : 0U);
    add(p.orbitalRepresentation ? 1U : 0U);
    return value;
}

void Validate(
    const f64 referenceRadiusMeters,
    const std::vector<CloudLayerParameters>& layers,
    const CloudFieldConfig& config)
{
    if (!std::isfinite(referenceRadiusMeters) ||
        referenceRadiusMeters <= 0.0 ||
        config.faceResolution < 4U ||
        !std::isfinite(config.footprintScale) ||
        config.footprintScale <= 0.0 ||
        config.timeQuantumMicroseconds <= 0)
    {
        throw std::invalid_argument(
            "Cloud field configuration is invalid.");
    }

    for (const auto& p : layers)
    {
        if (!std::isfinite(p.baseAltitudeMeters) ||
            !std::isfinite(p.topAltitudeMeters) ||
            p.baseAltitudeMeters < 0.0 ||
            p.topAltitudeMeters <= p.baseAltitudeMeters ||
            !std::isfinite(p.coverageBias) ||
            p.coverageBias < -1.0 ||
            p.coverageBias > 1.0 ||
            !std::isfinite(p.peakOpticalDepth) ||
            p.peakOpticalDepth < 0.0 ||
            !std::isfinite(p.singleScatteringAlbedo) ||
            p.singleScatteringAlbedo < 0.0 ||
            p.singleScatteringAlbedo > 1.0 ||
            !std::isfinite(p.anisotropy) ||
            p.anisotropy <= -1.0 ||
            p.anisotropy >= 1.0 ||
            !std::isfinite(p.densityExponent) ||
            p.densityExponent <= 0.0 ||
            !std::isfinite(p.weatherScale) ||
            p.weatherScale <= 0.0 ||
            !std::isfinite(p.detailScale) ||
            p.detailScale <= 0.0)
        {
            throw std::invalid_argument(
                "Cloud layer parameters are invalid.");
        }
    }
}
} // namespace

const CloudTexel& CloudLayerField::At(
    const u32 face,
    const u32 x,
    const u32 y,
    const u32 faceResolution) const
{
    if (face >= 6U ||
        x >= faceResolution ||
        y >= faceResolution)
    {
        throw std::out_of_range(
            "Cloud field texel is out of range.");
    }

    return texels[
        TexelIndex(
            face,
            x,
            y,
            faceResolution)];
}

CloudTexel CloudFieldProduct::Sample(
    const math::Double3& unitDirection) const
{
    if (faceResolution == 0U)
    {
        return {};
    }

    const auto lookup =
        DirectionToCube(
            unitDirection,
            faceResolution);

    f64 remaining = 1.0;
    f64 optical = 0.0;
    f64 weightedAlbedo = 0.0;
    f64 weightedG = 0.0;
    f64 weight = 0.0;

    for (const auto& layer : layers)
    {
        const auto& texel =
            layer.At(
                lookup.face,
                lookup.x,
                lookup.y,
                faceResolution);

        remaining *=
            1.0 -
            std::clamp(
                static_cast<f64>(
                    texel.coverage),
                0.0,
                1.0);

        optical +=
            texel.opticalDepth;

        const f64 w =
            texel.coverage *
            std::max(
                texel.opticalDepth,
                1.0e-6F);

        weightedAlbedo +=
            texel.singleScatteringAlbedo *
            w;
        weightedG +=
            texel.anisotropy *
            w;
        weight += w;
    }

    return {
        .coverage =
            static_cast<f32>(
                1.0 - remaining),
        .opticalDepth =
            static_cast<f32>(optical),
        .singleScatteringAlbedo =
            static_cast<f32>(
                weight > 0.0
                    ? weightedAlbedo / weight
                    : 0.999),
        .anisotropy =
            static_cast<f32>(
                weight > 0.0
                    ? weightedG / weight
                    : 0.72)
    };
}

u64 CloudFieldFingerprint(
    const terrain::TerrainSource* climateSource,
    const CloudCoverageSource* externalSource,
    const f64 referenceRadiusMeters,
    const std::vector<CloudLayerParameters>& layers,
    const time::SimulationTime atTime,
    const CloudFieldConfig& config)
{
    Validate(
        referenceRadiusMeters,
        layers,
        config);

    const i64 timeBucket =
        atTime.microsecondsFromEpoch /
        config.timeQuantumMicroseconds;

    u64 value =
        0x4d3233434c44464cULL;

    value =
        terrain::StableCombine64(
            value,
            std::bit_cast<u64>(
                referenceRadiusMeters));
    value =
        terrain::StableCombine64(
            value,
            config.faceResolution);
    value =
        terrain::StableCombine64(
            value,
            std::bit_cast<u64>(
                config.footprintScale));
    value =
        terrain::StableCombine64(
            value,
            static_cast<u64>(
                timeBucket));

    if (climateSource != nullptr)
    {
        value =
            terrain::StableCombine64(
                value,
                climateSource->
                    GenerationRevisions().
                    climate);
    }

    if (externalSource != nullptr)
    {
        value =
            terrain::StableCombine64(
                value,
                externalSource->Revision());
    }

    for (const auto& layer : layers)
    {
        value =
            terrain::StableCombine64(
                value,
                LayerFingerprint(layer));
    }

    return value;
}

CloudFieldProduct BuildCloudField(
    const terrain::TerrainSource* climateSource,
    const CloudCoverageSource* externalSource,
    const f64 referenceRadiusMeters,
    const std::vector<CloudLayerParameters>& layers,
    const time::SimulationTime atTime,
    const CloudFieldConfig& config)
{
    const u64 fingerprint =
        CloudFieldFingerprint(
            climateSource,
            externalSource,
            referenceRadiusMeters,
            layers,
            atTime,
            config);

    CloudFieldProduct result;
    result.faceResolution =
        config.faceResolution;
    result.timeBucket =
        atTime.microsecondsFromEpoch /
        config.timeQuantumMicroseconds;
    result.fingerprint =
        fingerprint;

    if (climateSource != nullptr)
    {
        result.climateRevision =
            climateSource->
                GenerationRevisions().
                climate;
    }

    const f64 angularCell =
        (0.5 *
         std::numbers::pi_v<f64>) /
        static_cast<f64>(
            config.faceResolution - 1U);

    result.sampleFootprintMeters =
        referenceRadiusMeters *
        angularCell *
        config.footprintScale;

    const f64 seconds =
        static_cast<f64>(
            atTime.microsecondsFromEpoch) *
        1.0e-6;

    const std::size_t texelCount =
        static_cast<std::size_t>(
            config.faceResolution) *
        config.faceResolution *
        6U;

    result.layers.reserve(
        layers.size());

    for (const auto& parameters :
         layers)
    {
        CloudLayerField layer;
        layer.parameters =
            parameters;
        layer.fingerprint =
            terrain::StableCombine64(
                fingerprint,
                LayerFingerprint(
                    parameters));
        layer.texels.resize(
            texelCount);

        for (u32 face = 0U;
             face < 6U;
             ++face)
        {
            for (u32 y = 0U;
                 y < config.faceResolution;
                 ++y)
            {
                const f64 v =
                    -1.0 +
                    2.0 *
                    static_cast<f64>(y) /
                    static_cast<f64>(
                        config.faceResolution - 1U);

                for (u32 x = 0U;
                     x < config.faceResolution;
                     ++x)
                {
                    const f64 u =
                        -1.0 +
                        2.0 *
                        static_cast<f64>(x) /
                        static_cast<f64>(
                            config.faceResolution - 1U);

                    const auto direction =
                        FaceDirection(
                            face,
                            u,
                            v);

                    const auto advected =
                        RotateByAngularVelocity(
                            direction,
                            parameters.
                                windAngularRadiansPerSecond,
                            -seconds);

                    f64 climateCoverage =
                        0.5;

                    if (parameters.sourceModel ==
                            CloudSourceModel::
                                ClimateProcedural)
                    {
                        if (climateSource ==
                            nullptr)
                        {
                            throw std::invalid_argument(
                                "Climate Procedural cloud layer requires a TerrainSource climate authority.");
                        }

                        const auto sample =
                            climateSource->Sample({
                                .unitDirection =
                                    advected,
                                .footprintMeters =
                                    result.
                                        sampleFootprintMeters
                            });

                        const f64 humidity =
                            std::clamp(
                                static_cast<f64>(
                                    sample.climate.
                                        humidity),
                                0.0,
                                1.0);

                        const f64 precipitation =
                            std::clamp(
                                static_cast<f64>(
                                    sample.climate.
                                        precipitation),
                                0.0,
                                1.0);

                        climateCoverage =
                            std::clamp(
                                0.62 * humidity +
                                0.38 * precipitation,
                                0.0,
                                1.0);
                    }
                    else if (
                        parameters.sourceModel ==
                            CloudSourceModel::
                                Authored ||
                        parameters.sourceModel ==
                            CloudSourceModel::
                                Imported)
                    {
                        if (externalSource == nullptr)
                        {
                            throw std::invalid_argument(
                                "Authored/Imported cloud source requires an external coverage adapter.");
                        }

                        climateCoverage =
                            std::clamp(
                                externalSource->
                                    SampleCoverage(
                                        advected,
                                        atTime),
                                0.0,
                                1.0);
                    }

                    const f64 noise =
                        FractalPattern(
                            advected,
                            parameters.
                                weatherScale,
                            parameters.
                                detailScale,
                            parameters.seed);

                    const f64 weather =
                        parameters.sourceModel ==
                                CloudSourceModel::
                                    Procedural
                            ? noise
                            : climateCoverage *
                                  0.72 +
                              noise * 0.28;

                    const f64 threshold =
                        std::clamp(
                            0.52 -
                            parameters.coverageBias *
                                0.35,
                            0.05,
                            0.95);

                    const f64 rawCoverage =
                        SmoothStep(
                            threshold - 0.18,
                            threshold + 0.18,
                            weather);

                    const f64 coverage =
                        std::pow(
                            std::clamp(
                                rawCoverage,
                                0.0,
                                1.0),
                            parameters.
                                densityExponent);

                    const std::size_t index =
                        TexelIndex(
                            face,
                            x,
                            y,
                            config.faceResolution);

                    layer.texels[index] = {
                        .coverage =
                            static_cast<f32>(
                                coverage),
                        .opticalDepth =
                            static_cast<f32>(
                                parameters.
                                    peakOpticalDepth *
                                coverage),
                        .singleScatteringAlbedo =
                            static_cast<f32>(
                                parameters.
                                    singleScatteringAlbedo),
                        .anisotropy =
                            static_cast<f32>(
                                parameters.
                                    anisotropy)
                    };
                }
            }
        }

        result.layers.push_back(
            std::move(layer));
    }

    return result;
}

void CompositeOrbitalCloudAppearance(
    const CloudFieldProduct& field,
    celestial_appearance::PlanetaryAppearanceProduct& appearance)
{
    if (appearance.faceResolution == 0U ||
        appearance.texels.empty() ||
        field.faceResolution == 0U)
    {
        return;
    }

    for (u32 face = 0U;
         face < 6U;
         ++face)
    {
        for (u32 y = 0U;
             y < appearance.faceResolution;
             ++y)
        {
            const f64 v =
                -1.0 +
                2.0 *
                static_cast<f64>(y) /
                static_cast<f64>(
                    appearance.faceResolution - 1U);

            for (u32 x = 0U;
                 x < appearance.faceResolution;
                 ++x)
            {
                const f64 u =
                    -1.0 +
                    2.0 *
                    static_cast<f64>(x) /
                    static_cast<f64>(
                        appearance.faceResolution - 1U);

                const auto lookup =
                    DirectionToCube(
                        FaceDirection(
                            face,
                            u,
                            v),
                        field.faceResolution);

                f64 remaining = 1.0;
                f64 optical = 0.0;
                f64 scatteringWeight = 0.0;
                f64 weightedScattering = 0.0;

                for (const auto& layer :
                     field.layers)
                {
                    if (!layer.parameters.
                            orbitalRepresentation)
                    {
                        continue;
                    }

                    const auto& sample =
                        layer.At(
                            lookup.face,
                            lookup.x,
                            lookup.y,
                            field.faceResolution);

                    const f64 coverage =
                        std::clamp(
                            static_cast<f64>(
                                sample.coverage),
                            0.0,
                            1.0);

                    remaining *=
                        1.0 - coverage;
                    optical +=
                        sample.opticalDepth;

                    const f64 weight =
                        coverage *
                        std::max(
                            static_cast<f64>(
                                sample.opticalDepth),
                            1.0e-6);

                    weightedScattering +=
                        sample.
                            singleScatteringAlbedo *
                        weight;
                    scatteringWeight += weight;
                }

                const f32 coverage =
                    static_cast<f32>(
                        1.0 - remaining);

                if (coverage <= 0.0F)
                {
                    continue;
                }

                auto& texel =
                    appearance.texels[
                        static_cast<std::size_t>(face) *
                            appearance.faceResolution *
                            appearance.faceResolution +
                        static_cast<std::size_t>(y) *
                            appearance.faceResolution +
                        x];

                const f32 opticalAlpha =
                    static_cast<f32>(
                        1.0 -
                        std::exp(
                            -0.35 *
                            std::max(
                                optical,
                                0.0)));

                const f32 blend =
                    std::clamp(
                        coverage *
                            opticalAlpha,
                        0.0F,
                        1.0F);

                const f32 scattering =
                    static_cast<f32>(
                        std::clamp(
                            scatteringWeight > 0.0
                                ? weightedScattering /
                                      scatteringWeight
                                : 0.999,
                            0.0,
                            1.0));

                const math::Float3 cloudAlbedo{
                    0.82F * scattering,
                    0.84F * scattering,
                    0.87F * scattering
                };

                texel.albedoLinear =
                    texel.albedoLinear *
                        (1.0F - blend) +
                    cloudAlbedo *
                        blend;

                texel.roughness =
                    std::clamp(
                        texel.roughness *
                            (1.0F - blend) +
                        0.96F * blend,
                        0.04F,
                        1.0F);
            }
        }
    }

    appearance.fingerprint =
        terrain::StableCombine64(
            appearance.fingerprint,
            field.fingerprint);
}

f64 CloudShadowTransmittanceAtSurface(
    const CloudFieldProduct& field,
    const f64 referenceRadiusMeters,
    const math::Double3& surfaceUnitDirection,
    const math::Double3& lightDirectionBody)
{
    if (!std::isfinite(
            referenceRadiusMeters) ||
        referenceRadiusMeters <= 0.0 ||
        field.faceResolution == 0U ||
        math::LengthSquared(
            surfaceUnitDirection) <=
            1.0e-20 ||
        math::LengthSquared(
            lightDirectionBody) <=
            1.0e-20)
    {
        return 1.0;
    }

    const auto surfaceDirection =
        math::Normalize(
            surfaceUnitDirection);
    const auto light =
        math::Normalize(
            lightDirectionBody);

    if (math::Dot(
            surfaceDirection,
            light) <= 0.0)
    {
        return 1.0;
    }

    const math::Double3 origin =
        surfaceDirection *
        referenceRadiusMeters;

    f64 transmittance = 1.0;

    for (const auto& layer :
         field.layers)
    {
        if (!layer.parameters.
                shadowParticipation)
        {
            continue;
        }

        const f64 shellRadius =
            referenceRadiusMeters +
            0.5 *
                (layer.parameters.
                     baseAltitudeMeters +
                 layer.parameters.
                     topAltitudeMeters);

        const f64 b =
            math::Dot(
                origin,
                light);
        const f64 c =
            math::Dot(origin, origin) -
            shellRadius * shellRadius;
        const f64 discriminant =
            b * b - c;

        if (discriminant < 0.0)
        {
            continue;
        }

        const f64 t =
            -b +
            std::sqrt(
                std::max(
                    discriminant,
                    0.0));

        if (t <= 0.0)
        {
            continue;
        }

        const auto shellDirection =
            math::Normalize(
                origin +
                light * t);

        const auto lookup =
            DirectionToCube(
                shellDirection,
                field.faceResolution);

        const auto& texel =
            layer.At(
                lookup.face,
                lookup.x,
                lookup.y,
                field.faceResolution);

        const f64 incidence =
            std::max(
                math::Dot(
                    shellDirection,
                    light),
                0.12);

        const f64 optical =
            static_cast<f64>(
                texel.opticalDepth) /
            incidence;

        transmittance *=
            std::exp(-optical);
    }

    return std::clamp(
        transmittance,
        0.0,
        1.0);
}

GpuCloudFieldProduct::GpuCloudFieldProduct(
    rhi::Device& device,
    const CloudFieldProduct& product)
    : faceResolution_(
          product.faceResolution),
      layerCount_(
          static_cast<u32>(
              product.layers.size())),
      fingerprint_(
          product.fingerprint)
{
    if (faceResolution_ == 0U)
    {
        throw std::invalid_argument(
            "GPU cloud field requires a valid product.");
    }

    std::vector<GpuCloudTexel> packed;
    const std::size_t perLayer =
        static_cast<std::size_t>(
            faceResolution_) *
        faceResolution_ *
        6U;
    packed.reserve(
        perLayer *
        layerCount_);

    for (const auto& layer :
         product.layers)
    {
        if (layer.texels.size() !=
            perLayer)
        {
            throw std::invalid_argument(
                "Cloud layer field topology mismatch.");
        }

        for (const auto& texel :
             layer.texels)
        {
            packed.push_back({
                .coverage =
                    texel.coverage,
                .opticalDepth =
                    texel.opticalDepth,
                .singleScatteringAlbedo =
                    texel.
                        singleScatteringAlbedo,
                .anisotropy =
                    texel.anisotropy
            });
        }
    }

    if (packed.empty())
    {
        packed.push_back({});
    }

    buffer_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    packed.size() *
                    sizeof(GpuCloudTexel)),
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    if (!buffer_)
    {
        throw std::runtime_error(
            "Failed to allocate GPU cloud field.");
    }

    std::memcpy(
        buffer_->Map(),
        packed.data(),
        packed.size() *
            sizeof(GpuCloudTexel));
    buffer_->Unmap();
}

rhi::Buffer& GpuCloudFieldProduct::Buffer() noexcept
{
    return *buffer_;
}

u32 GpuCloudFieldProduct::FaceResolution() const noexcept
{
    return faceResolution_;
}

u32 GpuCloudFieldProduct::LayerCount() const noexcept
{
    return layerCount_;
}

u64 GpuCloudFieldProduct::Fingerprint() const noexcept
{
    return fingerprint_;
}
} // namespace orbit::celestial_clouds
