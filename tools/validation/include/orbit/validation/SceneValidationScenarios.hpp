#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <array>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
class StudioViewportRenderer;

enum class SceneValidationScenario : u8
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

struct SceneValidationScenarioDescriptor
{
    SceneValidationScenario id{};
    std::string_view name;
    std::string_view group;
    std::string_view invariant;
    bool gpuVisualToleranceRequired{false};
};

inline constexpr std::array<SceneValidationScenarioDescriptor, 13>
    kSceneValidationScenarios{{
        {SceneValidationScenario::LedRoom,"LED Room","Lighting","Spatially varying emissive radiance contributes to GI without proxy lights.",true},
        {SceneValidationScenario::CloudGlare,"Cloud Glare","Presentation","Extreme highlights exceed the photopic ceiling without driving exposure without bound.",true},
        {SceneValidationScenario::DarkInteriorToDaylight,"Dark Interior -> Daylight","Presentation","Photopic recovery, dark adaptation and overload remain separate time-domain states.",true},
        {SceneValidationScenario::HeadlightBrakeLight,"Headlight / Brake Light","Lighting","Small bright moving emitters retain energy and stable sampling without authored proxy lights.",true},
        {SceneValidationScenario::CityNightFlight,"City Night Flight","Lighting","Local emissive authority aggregates to regional/planetary representations continuously.",true},
        {SceneValidationScenario::GroundToOrbit,"Ground -> Orbit","Lighting","Representation changes preserve common lighting/emission authority.",true},
        {SceneValidationScenario::RtAb,"RT A/B","Lighting","Ray-query capability changes backend quality, not the lighting model or implicit budget.",true},
        {SceneValidationScenario::SmokeObstacleAdvection,"Smoke Obstacle / Advection","Volumes","Local 3D smoke advects around production obstacle/effectors deterministically at the reference level.",true},
        {SceneValidationScenario::SurfaceDustWind,"Surface Dust / Wind","Volumes","Surface-aligned transport responds to wind while retaining bounded field authority.",true},
        {SceneValidationScenario::EmissiveFireGi,"Emissive Fire GI","Volumes","Volume emission remains physical radiance and feeds the shared emissive GI authority.",true},
        {SceneValidationScenario::RoamingDomainContinuity,"Roaming Domain Continuity","Volumes","Follow-target movement changes runtime placement without changing stable authored identity.",true},
        {SceneValidationScenario::LiveToBakedEquivalence,"Live -> Baked Playback","Volumes","A validated native cache preserves bounded density/emission authority within declared tolerance.",true},
        {SceneValidationScenario::NearToFarVolumeLod,"Near -> Far Volume LOD","Volumes","Live/coarse/passive/baked transitions keep normalized representation weights and stable identity.",true}
    }};

[[nodiscard]] constexpr commands::CommandId
SceneValidationCommandId(const SceneValidationScenario scenario) noexcept
{
    return {
        .high = 0x4f524249544d3433ULL,
        .low = 0x56414c0000000001ULL + static_cast<u64>(scenario)
    };
}

[[nodiscard]] std::string PrepareSceneValidationScenario(
    SceneValidationScenario scenario,
    studio_session::StudioSession& session,
    StudioViewportRenderer& renderer);

// RAII bridge into Studio's existing command palette/catalog. The normal
// VolumeAuthoringUi owns one registration while its active project session is
// alive; no standalone validation application or test-only editor path exists.
class SceneValidationCommandRegistration
{
public:
    SceneValidationCommandRegistration(
        studio_session::StudioSession* session,
        StudioViewportRenderer* renderer) noexcept;
    ~SceneValidationCommandRegistration();

    SceneValidationCommandRegistration(
        const SceneValidationCommandRegistration&) = delete;
    SceneValidationCommandRegistration& operator=(
        const SceneValidationCommandRegistration&) = delete;

private:
    studio_session::StudioSession* session_{nullptr};
    StudioViewportRenderer* renderer_{nullptr};
    std::array<bool, kSceneValidationScenarios.size()> owned_{};
};
} // namespace orbit::studio_ui
