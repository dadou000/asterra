#pragma once

#include <cstdint>

namespace orbit::optical_transport
{

// Identity of a cached physical product. A result is valid only for the medium and source state it
// describes; camera motion never appears here. Quality is the representation level, not a version.
struct CacheStamp
{
    std::uint64_t planetId = 0;
    std::uint64_t regionId = 0;
    double simulationTime = 0.0;
    std::uint64_t sourceVersion = 0;
    std::uint64_t mediumVersion = 0;
    std::uint32_t quality = 0;
};

// What a consumer needs: an exact planet/region match and versions no older than required.
// Mixing generations is refused rather than blended.
enum class CacheValidity
{
    Current,
    StaleMedium,
    StaleSource,
    WrongRegion,
};

CacheValidity ClassifyCache(const CacheStamp& cached, const CacheStamp& required);

// Which products a change invalidates. A light change leaves extinction alone; a medium change
// invalidates downstream visibility and scattering; a camera change invalidates neither.
struct InvalidationSet
{
    bool opticalHierarchy = false;
    bool stellarVisibility = false;
    bool directionalRadiance = false;
    bool diffuseScattering = false;
    bool surfaceTransport = false;
    bool transientAtlas = false;
};

InvalidationSet InvalidateForMediumChange();
InvalidationSet InvalidateForSourceChange(bool transientOnly);
InvalidationSet InvalidateForCameraChange();

} // namespace orbit::optical_transport
