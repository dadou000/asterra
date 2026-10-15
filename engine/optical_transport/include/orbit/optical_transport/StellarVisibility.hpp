#pragma once

#include <cstddef>
#include <functional>
#include <span>

namespace orbit::optical_transport
{

// Apparent angular radius (rad) of a star of radius R at distance D.
double AngularRadius(double radius, double distance);

// Width of the penumbra cast by a blocker `separation` metres in front of the receiver.
double PenumbraWidth(double separation, double angularRadius);

// Fraction of a finite disk visible past an opaque blocker edge. `edgeOffset` is the signed angular distance
// of the blocker edge from the disk centre (positive = covering the disk centre side), radius = disk radius.
// Uniform disk (limbDarkening = 0) or linear limb darkening I(mu) = 1 - u (1 - mu).
double DiskVisibilityAcrossEdge(double edgeOffset, double angularRadius, double limbDarkening = 0.0);

// Disk-averaged transmission. `opticalDepthAt(dx, dy)` gives optical depth toward the point (dx, dy) of the disk
// (angular offsets, |offset| <= radius). Returns the intensity-weighted mean of exp(-tau), NOT exp(-mean tau).
double DiskTransmission(double angularRadius, double limbDarkening, int samplesPerAxis,
                        const std::function<double(double, double)>& opticalDepthAt);

// Stats a coarse cloud cell keeps so filtering preserves transmission: mean of exp(-tau) over sub-samples,
// and the mean tau. effectiveTau = -ln(meanTransmission) >= meanTau (Jensen).
struct TransmissionStats
{
    double meanTransmission = 1.0;
    double meanOpticalDepth = 0.0;
    double coverage = 0.0;  // fraction of sub-samples with tau > threshold
};
TransmissionStats FilterOpticalDepths(std::span<const double> opticalDepths, double coverageThreshold = 0.05);
double EffectiveOpticalDepth(const TransmissionStats& stats);

// Result of an exhausted or unresolved query is distinct from a miss.
enum class HitState
{
    Hit,
    Miss,
    Unresolved,
};

// Astronomical + terrain horizon test: a star is visible when its disk centre elevation exceeds the horizon
// elevation by the disk radius (fully visible), below minus radius (fully blocked), otherwise partial.
double HorizonVisibility(double starElevation, double horizonElevation, double angularRadius, double limbDarkening = 0.0);

} // namespace orbit::optical_transport
