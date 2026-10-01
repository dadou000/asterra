#pragma once

#include <string>
#include <string_view>

namespace orbit::terrain_render
{
// Near-field standing water is a separate object: the clipmap terrain pass draws
// the true bed, and a second pass draws a flat water surface over it. These
// build the shader variants for the two passes from the shared clipmap sources.

// Terrain pixel shader for the bed. Water is not drawn or shaded here, and the
// ocean biome colour (which describes the surface seen from above) is replaced
// by a silt colour, because what lies under the water is what the terrain pass
// shows through it.
[[nodiscard]] std::string BuildClipmapBedPixelShader(
    std::string_view baseShader);

// Vertex shader of the water pass, derived from the terrain vertex shader so
// the morph, residency and clipmap holes are identical. Vertices sit on the
// water surface (the sea plane, or bed + depth where a lake stands). A triangle
// is drawn only if at least one of its corners is wet; the flat plane over its
// dry corners lies under the terrain and is rejected by the pixel stage against
// the terrain depth, which is what produces the shoreline.
[[nodiscard]] std::string BuildClipmapWaterVertexShader(
    std::string_view terrainVertexShader);

// Pixel shader of the water pass. It reads the terrain depth at graphics
// sampled-texture slot 0 and the optics buffer at buffer slot 1, rejects
// fragments behind the terrain, measures the water column from the depth
// difference, and outputs a colour and opacity for alpha blending over the lit
// scene: Fresnel sky reflection, sun glint, in-scattered body colour and the
// transmission of the terrain underneath.
[[nodiscard]] std::string BuildClipmapWaterPixelShader(
    std::string_view waterVertexShader);
} // namespace orbit::terrain_render
