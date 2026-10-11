+++
path = "/rendering/unified-transport/medium-and-clouds"
title = "Atmospheric medium, optical hierarchy and stable clouds (partly implemented)"
kind = "concept"
status = "experimental"
summary = """
Proposed design for one OpticalMedium query shared by sky, clouds, terrain, ocean and lights; humidity-driven aerosol haze; a sparse multiresolution \
optical hierarchy; and cloud LOD that changes only the visual form, never the cloud's identity, extent or lighting role."""
keywords = ["optical medium", "aerosol", "kappa kohler", "haze", "fog", "cloud lod", "cloud morphology", "optical bricks", "transmission", "cumulative optical depth"]
related = ["/rendering/clouds", "/rendering/atmosphere", "/rendering/unified-transport"]
depends_on = ["/rendering/unified-transport"]
invariants = [
  "SampleMedium(position, simulationTime, requestedAccuracy) returns spectral extinction/scattering/absorption, phase information, refractive index and a confidence; extinction = scattering + absorption and T(lambda) = exp(-integral of sigma_t ds).",
  "Humidity never multiplies an artistic fog density; haze comes from hygroscopic aerosol growth (kappa-Kohler) indexed by aerosol category, size, relative humidity and wavelength; fog begins with condensed droplets.",
  "Relative humidity is derived from absolute water content and temperature supplied by the weather solver; rain scavenges aerosols.",
  "Dynamic aerosol/fog corrections are defined against a stated reference atmosphere so baseline extinction is never counted twice; the planetary atmosphere LUT is not rebuilt for local humidity changes.",
  "Clouds and terrain use the same atmospheric transmission and in-scattering; clouds have no private aerial-perspective model.",
  "Cloud extinction at a fixed position and time is independent of camera distance; coverage-blending and near/far extinction multipliers are not allowed to change morphology.",
  "Filtering preserves transmission: mean(exp(-tau)) is not exp(-mean(tau)), so the hierarchy stores occupancy, extinction bounds, coverage and transmission statistics, not only mean density.",
  "Visual LOD (3D volume, coarse volume, 2.5D, curved 2D sheet, planetary map) is chosen per coherent region from parallax, silhouette, depth-intersection, radiance and temporal error; it never decides whether a cloud takes part in lighting.",
  "Curved 2D sheets are planet-anchored with altitude, vertical optical depth and velocity; near the limb they use spherical-shell intersection or are promoted.",
]
+++

## Implementation status

Done (CPU, on by default on GPU): the far-field bias is measured and `CloudFarField` plus `TransmittanceStats` replace the coverage blend by default
(on; `ORBIT_CLOUD_FILTERED_FAR=0` restores the old far field, see `/rendering/unified-transport/design-review`). `OpticalHierarchy` and the kappa-Kohler aerosol growth exist in `/rendering/unified-transport/core` with
no renderer consumer. Not done: the shared `SampleMedium` service, humidity-driven haze in the atmosphere passes, removal of the cloud's private aerial perspective,
the camera-independent light volume, the sample-jitter repair and any curved 2D / 2.5D cloud form.

## Context

The inspected 0.0.9 cloud shader blends thresholded cloud shape toward broad weather coverage as pixel footprint grows and switches between near and far
extinction multipliers, so distant clouds can appear to contract into smaller bodies on approach. The replacement treats morphology as persistent data
organized as planetary (fronts, decks, streets), regional (cells, towers, anvils, breaks) and local (billows, filaments, erosion) levels; approaching
reveals detail without changing identity.

## Optical hierarchy

Sparse multiresolution bricks store conservative occupancy and extinction bounds plus the summaries coarser queries need. The cloud renderer skips
empty bricks, integrates near-homogeneous regions analytically and refines at boundaries. View and solar marches query the same medium. For sunlight,
use light-aligned cumulative optical depth (tiled or hierarchical segment sums where clouds are edited) rather than every receiver marching to the star.
Across a curved atmosphere, combine sun-aligned local grids with regional hierarchical transport.

## Aerosols and baseline atmosphere

Use a small set of categories (marine salt, sulfate/pollution, organic, dust/smoke) with precomputed scattering/absorption tables. The Bruneton/Hillaire
LUT architecture remains the baseline for molecular and smooth aerosol profiles; sparse regional fields add corrections during transport.

## Diagnose

Distant clouds shrink when approached: look for distance-dependent shape or extinction terms in the cloud shader. Clouds and terrain disagree on haze
colour: check for a second aerial-perspective path.
