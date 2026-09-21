#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::post_process
{
struct ToneMappingConfig
{
    bool enabled{true};

    // Display-referred calibration. M29 will decide the actual SDR/HDR output
    // transform; M27 only defines the linear presentation headroom available
    // before that transform.
    f32 referenceWhiteNits{203.0F};
    f32 peakNits{1000.0F};

    // Exposed scene-linear value at which the shoulder begins. A default of
    // 1.0 keeps nominal reference white in the linear region.
    f32 shoulderStart{1.0F};

    // Controls how quickly the shoulder approaches peak headroom. 1.0 uses
    // the full remaining headroom as the exponential scale.
    f32 shoulderStrength{1.0F};
};

struct ToneMappingDiagnostics
{
    f32 headroomRatio{1.0F};
    f32 shoulderStart{1.0F};
    f32 mappedReferenceWhite{1.0F};
    f32 mappedPeakProbe{1.0F};
};

[[nodiscard]] f32
DisplayHeadroomRatio(
    const ToneMappingConfig& config) noexcept;

[[nodiscard]] f32
ToneMapLuminance(
    f32 exposedLuminance,
    const ToneMappingConfig& config = {}) noexcept;

[[nodiscard]] ToneMappingDiagnostics
EvaluateToneMappingDiagnostics(
    const ToneMappingConfig& config = {}) noexcept;
} // namespace orbit::post_process
