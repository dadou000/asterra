#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <array>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
class StudioViewportRenderer;

enum class V007ValidationScenario : u8
{
    LedRoom,
    CloudGlare,
    DarkInteriorToDaylight,
    HeadlightBrakeLight,
    CityNightFlight,
    GroundToOrbit,
    RtAb,
    SmokeObstacleAdvection,
    SurfaceDustWind,
    EmissiveFireGi,
    RoamingDomainContinuity,
    LiveToBakedEquivalence,
    NearToFarVolumeLod
};

struct V007ValidationScenarioDescriptor
{
    V007ValidationScenario id{};
    std::string_view name;
    std::string_view group;
    std::string_view invariant;
    bool gpuVisualToleranceRequired{false};
};

inline constexpr std::array<V007ValidationScenarioDescriptor, 13>
    kV007ValidationScenarios{{
        {V007ValidationScenario::LedRoom,"LED Room","Lighting","Spatially varying emissive radiance contributes to GI without proxy lights.",true},
        {V007ValidationScenario::CloudGlare,"Cloud Glare","Presentation","Extreme highlights exceed the photopic ceiling without driving exposure without bound.",true},
        {V007ValidationScenario::DarkInteriorToDaylight,"Dark Interior -> Daylight","Presentation","Photopic recovery, dark adaptation and overload remain separate time-domain states.",true},
        {V007ValidationScenario::HeadlightBrakeLight,"Headlight / Brake Light","Lighting","Small bright moving emitters retain energy and stable sampling without authored proxy lights.",true},
        {V007ValidationScenario::CityNightFlight,"City Night Flight","Lighting","Local emissive authority aggregates to regional/planetary representations continuously.",true},
        {V007ValidationScenario::GroundToOrbit,"Ground -> Orbit","Lighting","Representation changes preserve common lighting/emission authority.",true},
        {V007ValidationScenario::RtAb,"RT A/B","Lighting","Ray-query capability changes backend quality, not the lighting model or implicit budget.",true},
        {V007ValidationScenario::SmokeObstacleAdvection,"Smoke Obstacle / Advection","Volumes","Local 3D smoke advects around production obstacle/effectors deterministically at the reference level.",true},
        {V007ValidationScenario::SurfaceDustWind,"Surface Dust / Wind","Volumes","Surface-aligned transport responds to wind while retaining bounded field authority.",true},
        {V007ValidationScenario::EmissiveFireGi,"Emissive Fire GI","Volumes","Volume emission remains physical radiance and feeds the shared emissive GI authority.",true},
        {V007ValidationScenario::RoamingDomainContinuity,"Roaming Domain Continuity","Volumes","Follow-target movement changes runtime placement without changing stable authored identity.",true},
        {V007ValidationScenario::LiveToBakedEquivalence,"Live -> Baked Playback","Volumes","A validated native cache preserves bounded density/emission authority within declared tolerance.",true},
        {V007ValidationScenario::NearToFarVolumeLod,"Near -> Far Volume LOD","Volumes","Live/coarse/passive/baked transitions keep normalized representation weights and stable identity.",true}
    }};

[[nodiscard]] std::string PrepareV007ValidationScenario(
    V007ValidationScenario scenario,
    studio_session::StudioSession& session,
    StudioViewportRenderer& renderer);
} // namespace orbit::studio_ui
