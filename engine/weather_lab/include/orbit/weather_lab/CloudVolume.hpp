#pragma once

#include <orbit/weather_lab/WxFormat.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Turns the storm's condensate (cloud water, rain, ice) into a density grid
// that a Studio Volume object can display through its baked-cache path. Pure
// functions, no engine dependencies, so the mapping is testable on any host.

namespace orbit::weather_lab
{
// Which axis of the target volume is "up". The simulation's vertical is
// remapped onto it; the other two axes keep x then y order.
enum class VolumeUpAxis : std::uint8_t
{
    X,
    Y,
    Z,
};

[[nodiscard]] const char* VolumeUpAxisName(VolumeUpAxis axis) noexcept;
[[nodiscard]] bool ParseVolumeUpAxis(const std::string& text, VolumeUpAxis& out);

struct CloudVolumeRequest
{
    // density = 1 - exp(-gain * condensate[g/kg]); 0.5 makes 2 g/kg about 63%
    // opaque per unit, and a mature anvil/core saturate near 1.
    float gainPerGramPerKg = 0.5F;
    VolumeUpAxis upAxis = VolumeUpAxis::Y;
};

// Density in [0, 1] on a regular grid laid out as the volume cache expects:
// index = (z * resolutionY + y) * resolutionX + x, x fastest.
struct CloudVolumeGrid
{
    bool valid = false;
    std::string error;
    double time = 0.0;          // simulated seconds
    std::uint32_t resolutionX = 0;
    std::uint32_t resolutionY = 0;
    std::uint32_t resolutionZ = 0;
    // Full extent along each cache axis, metres.
    double sizeX = 0.0;
    double sizeY = 0.0;
    double sizeZ = 0.0;
    float maxCondensateGramsPerKg = 0.0F;
    std::vector<float> density;
};

// `condensate` is kg/kg, header.nx * ny * nz, x fastest then y then z (up).
[[nodiscard]] CloudVolumeGrid BuildCloudVolumeGrid(
    const WxHeader& header,
    const std::vector<float>& condensate,
    const CloudVolumeRequest& request);
} // namespace orbit::weather_lab
