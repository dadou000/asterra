#include <orbit/studio_session/StudioTerrainTectonicsProbe.hpp>

#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::studio_session
{
math::Double3 TectonicsDirectionFromLatLon(
    const f64 latitudeDegrees,
    const f64 longitudeDegrees) noexcept
{
    constexpr f64 kRadians = std::numbers::pi / 180.0;
    const f64 latitude = latitudeDegrees * kRadians;
    const f64 longitude = longitudeDegrees * kRadians;
    const f64 cosLatitude = std::cos(latitude);
    return {
        cosLatitude * std::cos(longitude),
        std::sin(latitude),
        cosLatitude * std::sin(longitude)};
}

std::optional<StudioTectonicsProbe> ProbeTectonicStructure(
    StudioSession& session,
    const std::string_view viewportId,
    const std::optional<math::Double3>& direction)
{
    const auto runtime = session.TerrainRuntime().Capture(viewportId);
    if (!runtime.has_value())
    {
        return std::nullopt;
    }

    math::Double3 unit{};
    if (direction.has_value())
    {
        unit = math::Normalize(*direction);
    }
    else
    {
        unit = math::Normalize(runtime->observer.meters);
    }
    if (!(math::LengthSquared(unit) > 0.0))
    {
        return std::nullopt;
    }

    const auto* const analytic =
        dynamic_cast<const terrain::AnalyticTerrainSource*>(
            &session.TerrainRuntime().TerrainSource(*runtime));
    if (analytic == nullptr)
    {
        return std::nullopt;
    }

    constexpr f64 kDegrees = 180.0 / std::numbers::pi;
    StudioTectonicsProbe probe;
    probe.direction = unit;
    probe.latitudeDegrees = std::asin(std::clamp(unit.y, -1.0, 1.0)) * kDegrees;
    probe.longitudeDegrees = std::atan2(unit.z, unit.x) * kDegrees;
    probe.structure = analytic->GlobalFields().SampleTectonicStructure(unit);
    return probe;
}
} // namespace orbit::studio_session
