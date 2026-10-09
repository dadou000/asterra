#pragma once

#include <orbit/weather_lab/WxFormat.hpp>

#include <string>
#include <vector>

// Storm diagnostics computed identically from CM1 exports and fast-core
// output, so the two models are compared on the same definitions.

namespace orbit::weather_lab
{
struct StormMetrics
{
    float time = 0.0F;            // s
    float maxUpdraft = 0.0F;      // m/s
    float maxUpdraftHeight = 0.0F; // m
    float maxDowndraft = 0.0F;    // m/s (negative)
    float maxRainMixing = 0.0F;   // g/kg
    float cloudTop = 0.0F;        // m, highest cell with >0.1 g/kg condensate
    float maxLowVorticity = 0.0F; // s^-1, below 1 km
    float maxUpdraftHelicity = 0.0F; // m^2/s^2, 2-5 km integral
    float coldPoolDeficit = 0.0F; // K, lowest level theta below frame 0 mean
    float rainArea = 0.0F;        // km^2 with >0.01 g/kg rain at the surface
};

// Computes metrics for every frame. `referenceFrame` supplies the base theta
// (horizontal mean of the first frame) for the cold-pool measure. Optional
// species (qc qi qs qg) are used when present.
[[nodiscard]] bool ComputeStormMetrics(
    WxReader& reader,
    std::vector<StormMetrics>& out,
    std::string* error = nullptr);

[[nodiscard]] std::string FormatMetricsTable(
    const std::vector<StormMetrics>& metrics);
} // namespace orbit::weather_lab
