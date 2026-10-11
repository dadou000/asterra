#pragma once

#include <cstdint>

namespace orbit::optical_transport
{

// Visible forms for a coherent cloud region, cheapest last. Lighting participation is independent of this choice.
enum class CloudForm : std::uint8_t
{
    FullVolume,
    CoarseVolume,
    DepthImpostor,   // 2.5D
    CurvedSheet,     // planet-anchored 2D layer
    PlanetaryMap,
};

struct CloudRegionDescriptor
{
    double distance = 0.0;          // camera to region centre, m
    double extent = 0.0;            // region horizontal size, m
    double thickness = 0.0;         // vertical size, m
    double verticalOpticalDepth = 0.0;
    double pixelAngle = 0.0;        // angular size of one pixel, rad
    double cameraAltitude = 0.0;    // above the cloud base, m
    double planetRadius = 6.371e6;
    double cloudAltitude = 0.0;     // m above the surface
};

struct RepresentationErrors
{
    double parallax = 0.0;      // pixels
    double silhouette = 0.0;    // pixels
    double radiance = 0.0;      // relative
    double limb = 0.0;          // 0..1, how close the view is to the grazing limb for a flat-layer approximation
};

struct LodTolerance
{
    double pixels = 0.5;
    double radiance = 0.02;
};

// Errors of representing the region with `form` from this camera. Pure function of the physical region and view;
// the cloud's extent, density and ability to cast shadows never change with the result.
RepresentationErrors EstimateErrors(CloudForm form, const CloudRegionDescriptor& region);

// Cheapest form whose errors are within tolerance; promotes toward full volume near the limb or when parallax or
// silhouette exceed tolerance. Hysteresis: `previous` is kept unless the new choice differs by one full tolerance margin.
CloudForm ChooseForm(const CloudRegionDescriptor& region, const LodTolerance& tolerance, CloudForm previous, bool hasPrevious);

} // namespace orbit::optical_transport
