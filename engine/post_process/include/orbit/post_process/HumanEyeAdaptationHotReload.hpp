#pragma once

#include <orbit/post_process/HumanEyeAdaptation.hpp>

#include <cstdint>

namespace orbit::post_process
{
inline constexpr char kHumanEyeAdaptationHotReloadModuleName[] =
    "OrbitEyeAdaptationHotReload";
inline constexpr char kHumanEyeAdaptationHotReloadInterfaceName[] =
    "orbit.post_process.human_eye_adaptation";
inline constexpr std::uint32_t
    kHumanEyeAdaptationHotReloadInterfaceVersion = 1U;

struct HumanEyeAdaptationHotReloadInterface
{
    std::uint32_t abiVersion{
        kHumanEyeAdaptationHotReloadInterfaceVersion};
    HumanEyeAdaptationUpdateOverride update{nullptr};
};
} // namespace orbit::post_process
