+++
path = "/rendering/clouds/reference-eve-v5-medium"
title = "EVE V5 physical medium, cloud noise, ray geometry and LOD"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Implementable volume-density, Hermite cloud-type, curved-shell intersection, conservative raymarch and orbital-handoff design."
keywords = ["medium", "extinction", "cloud shell", "SDF", "noise", "raymarch", "Lod", "orbit", "grazing"]
related = ["/rendering/clouds/reference-eve-v5"]
depends_on = ["/rendering/clouds/reference-eve-v5-config"]
+++

# Separation of facts and implementation

**EXACT-ARCHIVE:** each V5 cloud type has its own min/max altitude, density (often approximately 0.001–0.012 in Kerbin configs), baseNoiseTiling, coverageCurve, densityCurve, noiseEdgeHardness, optional per-type curl/multiple scattering/droplet properties. Each layer has a noise definition (V5 erosionDepth, optional Perlin/Worley, persistence, integer lacunarity, spherical cellular parameter); optional SDF and flow maps; raymarch baseStepSize, adaptiveStepSizeFactor, maxStepSize, lightMarchSteps, lightMarchDistance and continuousAccumulationDistance. Reference release notes claim anti-tiling, continuous distance-dependent temporal accumulation, 2D scaled transition, multi-layer overlap and SDF acceleration.

**UNRESOLVED:** compiled V5 shader source, exact noise/mipmap math, anti-tiling kernel, sample weighting, curve clamp modes, SDF units/bake, continuous-accumulation formula and per-ray jitter law are NOT in the ZIP. The code below is a **PROPOSED-PORT** testable substitute, not a literal transcript. Mark any later source-backed correction with verified shader/version/permission provenance.

# Physical coordinate and medium model

The reference renderer consumes `CloudSceneSnapshot` in planet-body fixed space. A sample position is `double3 positionBodyMeters`. CPU uses doubles to compute ray intersections and local origin, then subtracts a rounded render anchor before packing float ray coefficients to GPU. Stable cloud noise coordinates are body fixed plus atmospheric advection over simulation time, independent of camera motion. Planet radius is an actual reference shell; ellipsoid and terrain are separate physical blockers, not alterations to cloud noise geometry.

```cpp
struct ReferenceMediumSample {
    float sigmaTPerMeter = 0.f;
    float sigmaSPerMeter = 0.f;
    float phaseAsymmetry = 0.f;
    float liquidWaterWeight = 0.f;
    float dropletProbability = 0.f;
    float precipitationProbability = 0.f;
    // Never store a camera-exposed or tonemapped RGB here.
};
struct ReferenceSampleContext {
    math::Double3 positionBodyMeters;
    math::Double3 cameraPositionBodyMeters;
    math::Double3 rayDirectionBody; // normalized
    double simulationSeconds;
    double footprintMeters;
    bool secondaryLightQuery;
    u32 layerMask;
};
ReferenceMediumSample EvaluateReferenceMedium(
    const CloudSceneSnapshot& scene,
    const ReferenceSampleContext& sample) noexcept;
```

For overlapping types or source regions at P, define total extinction and scattering as sums of physical contributions. For a procedural region that a simulated storm **replaces**, use an authored blending mask m(P), and **replace**, not add, the corresponding convective medium:

```hlsl
MediumCoefficients BlendWeatherAndStorm(
    MediumCoefficients independentOther,
    MediumCoefficients proceduralConvective,
    MediumCoefficients simulatedConvective, float mask)
{
    MediumCoefficients outValue;
    outValue.sigmaT = independentOther.sigmaT +
        lerp(proceduralConvective.sigmaT, simulatedConvective.sigmaT, mask);
    outValue.sigmaS = independentOther.sigmaS +
        lerp(proceduralConvective.sigmaS, simulatedConvective.sigmaS, mask);
    outValue.phaseMoment = independentOther.phaseMoment +
        lerp(proceduralConvective.phaseMoment, simulatedConvective.phaseMoment, mask);
    return outValue;
}
```

Compute a scatter-weighted phase asymmetry moment only if `sigmaS > 0`. Clamp `sigmaT>=sigmaS>=0`, unless a separately specified emissive medium is present. Never allow thresholded coverage to be used as scene alpha directly: opacity comes from integrated extinction and path length.

# Type and density interpretation

**PROPOSED-PORT**, with gaps explicitly measurable in golden captures:

1. Evaluate coverage and type maps on the same planet-fixed direction; apply flow advection in a continuous chart or divergence-free tangent mapping. Alpha swizzles are imported, not hardcoded.
2. Map type scalar to adjacent source-ordered ReferenceCloudType indices and interpolation t. Evaluate each type's Hermite coverageCurve and densityCurve; interpolate typed optical parameters, NOT just top height.
3. Evaluate layer altitude in metres. Top/bottom may be negative for gas giants (Jool). Allow `interpolateCloudHeights` to be false for sharply separated cloud types.
4. Build the primary density shape from one V5-style base noise definition: 3D Worley/Perlin-Worley with octaves, lacunarity and persistence, modified by spherical Worley shape and erosionDepth. The exact V5 erosion implementation remains unresolved; independently define one consistent policy and compare.
5. Apply `noiseEdgeHardness` at the iso-surface, coverageCurve modulation and densityCurve over normalized cloud height. Multiply by authored density in inverse metres and scattering albedo; DO NOT uniformly multiply optical thickness by the cloud layer's mean coverage.
6. Add `upwardsCloudSpeed`, curl warp and type-dependent optional effects *only* to the sampling position; preserve physical storm centre location and maintain cloud advection motion vectors.
7. Return zero when sample lies outside all active material intervals, even if 2D layer macro appearance is present.

Example original HLSL **PROPOSED-PORT** for a single material:

```hlsl
float InverseCellF1(float3 p, uint seed);       // independent tileable 3D implementation
float PerlinGradient(float3 p, uint seed);
float CurveLut(Texture2D curves, uint index, float coordinate);
float ReferenceTypeSigma(
    float weatherCoverage, float normalizedType, float altitudeMeters,
    float3 worldMeters, float timeSeconds,
    ReferenceTypeGpu typeParams, float sampleFootprint)
{
    if (altitudeMeters < typeParams.baseMeters ||
        altitudeMeters >= typeParams.topMeters) return 0.0;

    const float h = saturate((altitudeMeters - typeParams.baseMeters) /
        max(typeParams.topMeters - typeParams.baseMeters, 0.1));
    const float3 animated = worldMeters/typeParams.baseNoiseMeters +
        typeParams.windNormalized * timeSeconds;
    float f = 0., weight = 1., sumWeight = 0.;
    float frequency = 1.;
    [loop] for (uint octave=0; octave<typeParams.octaves; ++octave) {
        // Stop octaves when projected feature size is below the sample footprint.
        const float feature = typeParams.baseNoiseMeters/frequency;
        if (feature < 2.0*sampleFootprint) break;
        const float cellular = InverseCellF1(animated*frequency, typeParams.seed+octave);
        const float gradient = PerlinGradient(animated*frequency, typeParams.seed+octave+17u);
        f += weight*lerp(cellular,gradient,typeParams.perlinWeight);
        sumWeight += weight;
        frequency *= typeParams.lacunarity;
        weight *= typeParams.persistence;
    }
    const float smoothShape = f/max(sumWeight,1e-6);
    const float typeCoverage = CurveLut(gCurves,typeParams.coverageCurve,weatherCoverage);
    const float verticalWeight = CurveLut(gCurves,typeParams.densityCurve,h);
    const float iso = saturate(1.-typeCoverage);
    const float densityShape = smoothstep(iso-typeParams.edgeWidth,
                                         iso+typeParams.edgeWidth,smoothShape);
    return typeParams.sigmaTPerMeter * verticalWeight * densityShape;
}
```

The actual V5's spherical cellular and deep erosion features are configurable, but this example's math is an **independent Orbit choice**. Matching a V5 look requires maps and image-based calibration, not declaring this formula exact.

# Correct cloud ray intervals: CPU double reference

For each sphere with center at planet origin, solve \`|o + td|^2 = r^2\` using doubles and the nonnegative discriminant. A bounded atmosphere cloud shell may produce **two disjoint occupied intervals** on grazing rays. The planet must occlude its far side, but it must NOT be used as an empty-space shortcut on a gas giant without the appropriate cut/ray boundary.

```cpp
struct Ray { math::Double3 o, d; };
struct Interval { double a, b; };
std::optional<Interval> SphereInterval(const Ray& ray, double radius) {
    const double b = Dot(ray.o,ray.d);
    const double c = Dot(ray.o,ray.o)-radius*radius;
    const double q = b*b-c;
    if (q < 0) return std::nullopt;
    const double h = std::sqrt(std::max(0.,q));
    return Interval{-b-h,-b+h};
}
std::vector<Interval> CloudShellIntervals(
    const Ray& ray, double bottom, double top,
    std::optional<double> planetStopT) {
    std::vector<Interval> out;
    auto ext = SphereInterval(ray,top);
    if (!ext) return out;
    const double a = std::max(0.,ext->a);
    const double b = planetStopT ? std::min(ext->b,*planetStopT) : ext->b;
    if (b<=a) return out;
    if (bottom<=0) { out.push_back({a,b}); return out; }
    auto clear = SphereInterval(ray,bottom);
    if (!clear || clear->b<=a || clear->a>=b) {
        out.push_back({a,b}); return out;
    }
    if (clear->a>a) out.push_back({a,std::min(b,clear->a)});
    if (clear->b<b) out.push_back({std::max(a,clear->b),b});
    return out;
}
```

For multiple altitude bands, union occupied shell intervals in ascending t; do not just use minimum base and maximum top, or secondary light rays waste steps in vertical voids. Keep all opaque terrain/vehicle/ship depth bounds. Surface depth reconstructed from Orbit RHI must be in the same camera/body frame; reverse-Z must be handled explicitly. Primary cloud march and secondary cloud shadows should share the interval calculation.

A correct OpenGL/KSP-like near/far camera split is not a reason to introduce camera-dependent cloud positions in Vulkan. Orbit owns one camera path and double world space.

# Adaptive raymarch with guaranteed completeness

**EXACT-ARCHIVE** per-layer settings expose baseStepSize, adaptiveStepSizeFactor, maxStepSize and optional SDF. Exact V5 formula unknown. **PROPOSED-PORT**:

```cpp
struct MarchPolicy {
    double minimumStepMeters;
    double maximumStepMeters;
    double distanceFactor;
    u32 maximumSamples;
};
double DesiredStep(const MarchPolicy& m, double distanceFromCamera) {
    return std::clamp(
        m.minimumStepMeters + m.distanceFactor*distanceFromCamera,
        m.minimumStepMeters,m.maximumStepMeters);
}
struct SampleMarchState { double T=1; math::Float3 L{}; u32 samples=0; };
SampleMarchState IntegrateRay(
    std::span<const Interval> intervals,
    const Ray& ray, MarchPolicy policy,
    const ReferenceFrameContext& frame)
{
    SampleMarchState out;
    for (const Interval& v : intervals) {
        double t = v.a;
        while (t < v.b && out.T > 0.002) {
            if (out.samples == policy.maximumSamples) {
                // Report budget exhaustion! Never mark truncated ray complete.
                frame.diagnostics.MarkRayBudgetExhausted();
                return out;
            }
            double dt = std::min(DesiredStep(policy,t),v.b-t);
            double jitter = frame.BlueNoiseJitter(out.samples,ray);
            auto medium = EvaluateReferenceMedium(frame.scene,{
                .positionBodyMeters=ray.o+ray.d*(t+jitter*dt),
                .cameraPositionBodyMeters=ray.o,
                .rayDirectionBody=ray.d,
                .simulationSeconds=frame.timeSeconds,
                .footprintMeters=frame.PixelFootprint(t),
                .secondaryLightQuery=false,
            });
            const double tau = std::max(0.f,medium.sigmaTPerMeter)*dt;
            const double alpha = -std::expm1(-tau);
            const auto source = frame.LightTransport(
                ray.o+ray.d*(t+jitter*dt),medium,ray.d);
            out.L += out.T*static_cast<float>(alpha)*source;
            out.T *= std::exp(-tau);
            t += dt;
            ++out.samples;
        }
    }
    return out;
}
```

This reference code shows required contracts; compiler hookup needs definitions in other chapters. In GPU shader use an explicit overflow flag/buffer and a conservative coarse remainder policy instead of silently returning incomplete radiance. Empty-space SDF steps must have proven conservative lower bounds, account for evolving weather and never jump across a narrow cloud feature.

# Noise statistics and anti-tiling

B's current 32-cubed cellular resource repeats visibly in close views. V5 release claims higher-frequency detail from one base noise and improved anti-tiling. For independent port, use a configurable 3D tileable noise atlas with separately tracked mip/footprint, curl warp and nonperiodic region transforms. For far pixels, **preserve average transmittance**, not average thresholded shape:
`T_pixel = mean(exp(-integrated sigmaT * pathLength))`.
Orbit already has `CloudFarField.hpp` with `BuildFarFieldLut` and `EvaluateFarFieldTransmittance`; reuse the math contract while keeping EVE reference GPU caches independent from A. Evaluate noise volume histograms over full represented cells and compare sky coverage and optical depth at every LOD transition.

A synthetic 3D volume without the omitted coverage/type textures is not a reference for V5 morphology, even if its single cloud looks attractive.

# Near-to-orbit continuity

**EXACT-ARCHIVE** provides per-layer scaledFadeStartAltitude / scaledFadeEndAltitude, macro layer2D, painter/exportable 2D cloud maps. **EXISTING-ORBIT** `engine/optical_transport/include/orbit/optical_transport/CloudRepresentation.hpp` has `CloudForm::{FullVolume,CoarseVolume,DepthImpostor,CurvedSheet,PlanetaryMap}` and `ChooseForm` with error tolerances. Reuse one physical region identity, optical-depth statistics and world anchor for all forms.

Define w=smoothstep(fadeStart,fadeEnd,cameraAltitude). When both representations overlap, combine *transmittance and radiance*, not two independent alpha-over layers pretending to be the same physical medium. Transition cloud direct shadows and motion vectors in the same interval. Under thresholded orbit view, a 2D cloud map must represent filtered macro optical depth; do not simultaneously raymarch the same cloud column at full opacity. Maintain the same flow-map advection in the global map and nearby volume; import normals/bump/material reflectance separately from cloud extinction.

# Special planetary cases

- Rocky planets with atmosphere: surface terrain occludes view and Sun; reference cloud shell based on atmosphere bottom radius but actual ellipsoid/terrain geometry for blockers.
- Deep gas giants: negative cloud altitudes permitted (Jool reference -80000 m); no synthetic solid floor unless world design requires it. Inside deeper atmosphere, terminate at renderer-defined absorption cutoff.
- Far orbit: use representation error bound in pixels/radiance/limb instead of altitude alone; low-res cloud texture creates UV seam alias if cube face handling differs.
- Storm domain: parent+child optical sigma has actual world placement. Preview density textures must not define storm physics; a camera-local simulation never outranks global weather authority.
- Lightning and droplets: sampled extinction drives probability, but visual/audible event state comes from authoritative storm/electrification event stream.

# Acceptance and numerical tests

1. Sphere ray: outside/tangent/inside, inner-shell double crossing, hit solid planet, camera below base/inside layer, gas giant negative altitudes; CPU-vs-GPU intervals within bounded epsilon.
2. Ray from 3000 m altitude grazing beneath 1500 m cloud base: two disjoint intervals both integrated, with no camera translation-dependent movement.
3. CloudType 0/1 boundaries and Hermite curve anchors; layer ordering/overlap and fxOnly behavior.
4. Same mean transmittance at orbital 1/4, 1/16 and near full-resolution; no coverage jumps across threshold.
5. Cube and equirectangular map continuity at poles and face edges, with flow-map circulation.
6. Empty-space skipping cannot skip narrow cloud sheets or storm tops; numerical high-budget baseline compares to bounded reference.
7. Timewarp and storm replay update noise motion smoothly with absolute time and deterministic sampled cloud state.
