#include "orbit/optical_transport/CloudRepresentation.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::optical_transport
{

RepresentationErrors EstimateErrors(CloudForm form, const CloudRegionDescriptor& r)
{
    RepresentationErrors e;
    if (form == CloudForm::FullVolume)
    {
        return e;
    }
    const double distance = std::max(r.distance, 1.0);
    const double pixel = std::max(r.pixelAngle, 1.0e-9);
    // Parallax of a flattened depth range: a camera shift of one thickness changes the apparent position by
    // thickness/distance radians; use the region depth as the representation error.
    const double depthFlattened = form == CloudForm::CoarseVolume ? 0.1 * r.thickness : r.thickness;
    const double depthKept = form == CloudForm::DepthImpostor ? 0.25 * r.thickness : 0.0;
    // A thin veil has no definite depth or silhouette to lose: both scale with how opaque the region is.
    const double tauClamped = std::min(1.0, std::max(r.verticalOpticalDepth, 0.0));
    const double opacity = tauClamped * tauClamped;  // visible contrast of depth structure falls faster than opacity
    e.parallax = opacity * std::max(depthFlattened - depthKept, 0.0) * (r.extent / distance) / distance / pixel;
    // Silhouette error: lost towers/overhangs relative to the footprint of a pixel.
    const double silhouetteMeters = form == CloudForm::CoarseVolume ? 0.05 * r.thickness
                                  : form == CloudForm::DepthImpostor ? 0.15 * r.thickness
                                                                      : r.thickness;
    e.silhouette = opacity * (silhouetteMeters / distance) / pixel;
    // Flat layer approximation breaks near the limb: path length through a shell vs 1/mu for a plane.
    const double R = r.planetRadius + r.cloudAltitude;
    const double horizonDistance = std::sqrt(std::max(R * R - r.planetRadius * r.planetRadius, 0.0));
    e.limb = std::clamp(r.distance / std::max(horizonDistance, 1.0), 0.0, 1.0);
    // Radiance: a thick medium collapsed to a layer loses angular structure; thin layers (tau << 1) lose nothing.
    const double tau = r.verticalOpticalDepth;
    const double thickness = tau / (1.0 + tau);
    e.radiance = form == CloudForm::CoarseVolume ? 0.01 * thickness
               : form == CloudForm::DepthImpostor ? 0.05 * thickness
               : form == CloudForm::CurvedSheet   ? 0.2 * thickness + 0.5 * e.limb * e.limb * e.limb
                                                  : 0.4 * thickness + 0.5 * e.limb * e.limb * e.limb;
    if (form == CloudForm::PlanetaryMap)
    {
        // Resolution of the planet-scale map: map texel is far larger than a pixel unless the region is subpixel.
        e.silhouette = std::max(e.silhouette, (r.extent / distance) / pixel > 2.0 ? 10.0 : 0.0);
    }
    return e;
}

CloudForm ChooseForm(const CloudRegionDescriptor& region, const LodTolerance& tol, CloudForm previous, bool hasPrevious)
{
    auto within = [&](CloudForm f, double scale) {
        const RepresentationErrors e = EstimateErrors(f, region);
        return e.parallax <= tol.pixels * scale && e.silhouette <= tol.pixels * scale && e.radiance <= tol.radiance * scale;
    };
    CloudForm chosen = CloudForm::FullVolume;
    for (int f = static_cast<int>(CloudForm::PlanetaryMap); f >= 0; --f)
    {
        if (within(static_cast<CloudForm>(f), 1.0))
        {
            chosen = static_cast<CloudForm>(f);
            break;
        }
    }
    if (hasPrevious && previous != chosen)
    {
        // Hysteresis: stay on the previous form while it is still inside a relaxed tolerance.
        if (within(previous, 1.5)) return previous;
    }
    return chosen;
}

} // namespace orbit::optical_transport
