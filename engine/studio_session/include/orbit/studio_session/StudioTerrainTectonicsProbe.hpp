#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TectonicStructure.hpp>

#include <optional>
#include <string_view>

namespace orbit::studio_session
{
class StudioSession;

struct StudioTectonicsProbe
{
    math::Double3 direction{};
    f64 latitudeDegrees{0.0};
    f64 longitudeDegrees{0.0};
    terrain::TectonicStructureSample structure{};
};

// Geographic convention shared with the flat map: +Y is north and longitude is
// atan2(+Z, +X).
[[nodiscard]] math::Double3 TectonicsDirectionFromLatLon(
    f64 latitudeDegrees,
    f64 longitudeDegrees) noexcept;

// Samples the planet structural layer of the viewport's current terrain body.
// With no direction the viewport observer's sub-point is used. Returns nullopt
// when the viewport has no analytic terrain runtime. Read-only.
[[nodiscard]] std::optional<StudioTectonicsProbe> ProbeTectonicStructure(
    StudioSession& session,
    std::string_view viewportId,
    const std::optional<math::Double3>& direction = std::nullopt);
} // namespace orbit::studio_session
