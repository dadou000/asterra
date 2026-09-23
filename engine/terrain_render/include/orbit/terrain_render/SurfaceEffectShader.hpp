#pragma once

#include <string>
#include <string_view>

namespace orbit::terrain_render
{
// Produces the terrain surface pixel shader variant that consumes the M38
// structured effect-stamp buffer at graphics SRV slot 1. The base shader stays
// the single source of terrain/water material truth; this function injects the
// transient runtime coating/emission stage immediately before G-buffer output.
[[nodiscard]] std::string BuildSurfaceEffectPixelShader(
    std::string_view baseShader);
} // namespace orbit::terrain_render
