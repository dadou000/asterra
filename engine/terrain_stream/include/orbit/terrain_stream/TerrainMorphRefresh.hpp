#pragma once

#include <orbit/terrain_stream/ToroidalResidency.hpp>

namespace orbit::terrain_stream
{
// Extend exposed-strip updates for samples containing frame-relative morph
// targets and pre-blended heights/biomes. Call before submitting sample jobs;
// commit the resulting patches with the matching motion and residency state.
void RefreshTerrainMorphRegions(
    const terrain_view::ClipmapLayout& layout,
    const terrain_view::ClipmapMotionUpdate& motion,
    ResidencyUpdate& residency);
} // namespace orbit::terrain_stream
