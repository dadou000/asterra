#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

namespace orbit::celestial_clouds
{

// Footprint-filtered ("far field") cloud transmittance.
//
// The ray march evaluates the thresholded base shape s(noise; coverage, hardness) per sample. Once a pixel covers
// more than a few noise cells the point sample aliases, and the old remedy (fade the shape to the weather coverage
// and the extinction to a thin constant) changes how much sky a region covers with distance. The statistically
// correct pixel value is the average of exp(-K s) over the noise distribution, K = unit extinction x step length.
// This table holds that average at the four scales of optical_transport::TransmittanceStats for a grid of
// (hardness, coverage); the shader interpolates it and blends toward it by how unresolved the noise is.
inline constexpr int kFarFieldHardnessCount = 5;     // type hardness 1.0 .. 3.0 (TypeParams .w)
inline constexpr float kFarFieldHardnessMin = 1.0F;
inline constexpr float kFarFieldHardnessMax = 3.0F;
inline constexpr int kFarFieldCoverageSteps = 17;    // skin coverage 0 .. 1

struct FarFieldLut
{
    // [hardness][coverage][channel]; channel j = mean exp(-K_j s).
    std::array<std::array<std::array<float, 4>, kFarFieldCoverageSteps>, kFarFieldHardnessCount> channels{};
};

// Bakes the table from the cloud noise volume (grid^3 voxels, 4 floats each, R = base shape). Deterministic.
FarFieldLut BuildFarFieldLut(std::span<const float> noiseRgba, unsigned grid);

// CPU reference of exactly what the shader's FarFieldTransmittance does.
float EvaluateFarFieldTransmittance(const FarFieldLut& lut, float coverage, float hardness, float scale);

// HLSL block (LUT constants, gStep helper and FarFieldTransmittance) injected before the extinction function.
std::string FarFieldShaderBlock(const FarFieldLut& lut);

} // namespace orbit::celestial_clouds
