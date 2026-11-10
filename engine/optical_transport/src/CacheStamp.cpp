#include "orbit/optical_transport/CacheStamp.hpp"

namespace orbit::optical_transport
{

CacheValidity ClassifyCache(const CacheStamp& cached, const CacheStamp& required)
{
    if (cached.planetId != required.planetId || cached.regionId != required.regionId)
    {
        return CacheValidity::WrongRegion;
    }
    if (cached.mediumVersion < required.mediumVersion)
    {
        return CacheValidity::StaleMedium;
    }
    if (cached.sourceVersion < required.sourceVersion)
    {
        return CacheValidity::StaleSource;
    }
    return CacheValidity::Current;
}

InvalidationSet InvalidateForMediumChange()
{
    InvalidationSet set;
    set.opticalHierarchy = true;
    set.stellarVisibility = true;
    set.directionalRadiance = true;
    set.diffuseScattering = true;
    set.surfaceTransport = true;
    return set;
}

InvalidationSet InvalidateForSourceChange(bool transientOnly)
{
    InvalidationSet set;
    set.transientAtlas = true;
    if (!transientOnly)
    {
        set.directionalRadiance = true;
        set.diffuseScattering = true;
        set.surfaceTransport = true;
    }
    return set;
}

InvalidationSet InvalidateForCameraChange()
{
    return {};
}

} // namespace orbit::optical_transport
