+++
path = "/rendering/clouds/reference-eve-v5-config"
title = "EVE V5 config importer, asset validation and authoring interfaces"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Typed schema and exact parse semantics for the V5 ConfigNode release, image channels, Hermite curves, multi-layer ordering and private asset access."
keywords = ["V5", "config", "import", "schema", "curves", "texture", "asset", "layer"]
related = ["/rendering/clouds/reference-eve-v5"]
depends_on = ["/rendering/clouds/reference-eve-v5-package"]
+++

# Source and mapping policy

**EXACT-ARCHIVE:** the release uses KSP ConfigNode-like nested braces, repeated names (such as OBJECT, Item and key), decimal/scientific strings, optional empty assignments, and nested maps with value, type and alphaMask. It is NOT JSON or INI. Every field not understood by Orbit must remain in a preserved raw-node extension bag and generate a visible diagnostics entry. This prevents "unknown" or "empty" becoming zero and silently changing the appearance.

Proposed importer owns two namespaces: the private installation root (`<user-selected>/GameData`) and public, typed C++ derived data. No source asset need be copied into the repository. The importer can be enabled in the existing Orbit project browser, and the runtime receives immutable typed materials. Do not make the renderer load ZIPs or decode KSP configs in its frame loop.

# Parser

**PROPOSED-PORT** working algorithm, to be placed in `engine/celestial_clouds/src/reference/EveConfigParser.cpp` and unit tested with synthetic fixtures:

```cpp
struct ConfigNode {
    std::string tag;
    // Multiple values of the same key (e.g. Hermite "key") are valid.
    std::vector<std::pair<std::string,std::string>> values;
    std::vector<ConfigNode> children;
    std::optional<std::string> First(std::string_view key) const;
    std::vector<std::string_view> All(std::string_view key) const;
    std::vector<const ConfigNode*> Children(std::string_view tag) const;
};
struct ParsedDocument {
    std::vector<ConfigNode> roots;
    std::vector<Diagnostic> diagnostics;
};

// Tokenize UTF-8 text with CRLF normalization and track line, column.
// Treat = as first unescaped delimiter on a line; split only once.
// Ignore comments outside quotations; preserve original spelling.
// "{" must follow a node name; "}" closes exactly one node.
// Preserve duplicate nodes and list values in their original order.
// Parsing must fail on missing brackets, accidental orphan values,
// nonfinite floats and explicitly invalid number/vector arity.
ParsedDocument ParseConfig(std::string_view text, std::string_view sourcePath);
```

Do NOT implement with `std::map<string,string>`; repeated `key =` entries would be lost. Do not sort cloud types by name. The V5 cloudTypeMap is an interpolated parameter between ordered Item profiles. Keep exact list indices and shader-side Hermite tangents.

### Numeric/value handling

Accept decimal, negative, exponent (`2E-06`), comma-delimited color/vectors, booleans case-insensitively, integer/wide enum and empty declarations. Preserve absent versus explicit empty versus zero. Parse curves as 4-tuple (time, value, inTangent, outTangent). Do NOT assume the original binary clamps extrapolation a certain way; make it a per-field explicit policy and verify against golden reference. Limit t to finite values; flag duplicate t times or nonmonotonic key times until their intended V5 behavior is known.

**PROPOSED-PORT** Hermite implementation that can be unit tested:

```cpp
struct HermiteKey { double t, v, inSlope, outSlope; };
double EvaluateCurve(std::span<const HermiteKey> keys, double t) {
    if (keys.empty()) return 1.0; // caller supplies field-specific default
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    const auto right = std::upper_bound(keys.begin(), keys.end(), t,
        [](double t, const auto& k){ return t < k.t; });
    const auto& a = *(right-1);
    const auto& b = *right;
    const double dt = b.t-a.t;
    if (!(dt>0)) throw std::domain_error("Unsorted Hermite knots");
    const double u = (t-a.t)/dt, u2=u*u, u3=u2*u;
    return (2*u3-3*u2+1)*a.v +
           (u3-2*u2+u)*dt*a.outSlope +
           (-2*u3+3*u2)*b.v +
           (u3-u2)*dt*b.inSlope;
}
```

This is a conventional cubic Hermite interpretation, **not verified reverse-engineered V5 source**. Golden tests must establish how the reference handles normalized density curve inputs, clamping, duplicate keys, and interpolation between types. Never silently substitute a generic sigmoid profile for the authored curves.

# Versioned typed profile: C++ contract

**PROPOSED-PORT** types should live under `orbit::celestial_clouds::reference`:

```cpp
enum class MissingAssetMode { Reject, SyntheticLabOnly };
enum class TextureLayout { Equirectangular, Cube6, Unknown };
enum class DataChannel { R, G, B, A, None };
struct TextureBinding {
    std::string logicalPath;
    TextureLayout layout{};
    DataChannel channel{};
    std::optional<std::string> separateAlphaMap;
    std::array<std::string,6> faces{}; // only for Cube6
    bool clamped{false};
    u64 checksum{0};
};
struct CloudNoiseDefinition {
    std::optional<int> worleyOctaves;
    std::optional<int> perlinOctaves;
    std::optional<int> lacunarity;
    std::optional<double> persistence;
    std::optional<double> periods;
    std::optional<double> spherical;
    std::optional<double> erosionDepth;
    std::optional<double> curlStrength;
    std::optional<double> curlScale;
};
struct ReferenceCloudType {
    std::string name;
    std::optional<double> minimumAltitudeMeters;
    std::optional<double> maximumAltitudeMeters;
    std::optional<double> densityInverseMeters;
    std::optional<double> baseNoiseTilingMeters;
    std::optional<double> edgeHardness;
    std::optional<double> multipleScatteringBrightness;
    std::optional<double> curlStrength;
    std::optional<bool> interpolateCloudHeights;
    std::vector<HermiteKey> coverageCurve;
    std::vector<HermiteKey> densityCurve;
    std::vector<ConfigNode> opaqueExtensions;
};
struct ReferenceCloudLayer {
    std::string name;
    universe::BodyId targetBody{};
    bool fxOnly{false};
    u32 overlapOrder{0};
    double fadeStartAltitudeMeters{};
    double fadeEndAltitudeMeters{};
    double upwardNoiseSpeedMetersPerSecond{};
    CloudNoiseDefinition noise;
    TextureBinding coverageMap, typeMap, colorMap, flowMap, sdfMap;
    std::vector<ReferenceCloudType> types; // ORDERED
    std::vector<ReferencePainterTile> painter;
    ReferenceRaymarchParameters marching;
    ReferencePhaseFunctions phases;
    ReferenceLightVolumeParameters lighting;
    std::vector<ReferenceEffectBinding> effects;
    std::vector<ConfigNode> opaqueExtensions;
};
struct EveReferenceProject {
    u32 schemaVersion{5};
    std::vector<ReferenceCloudLayer> layers;
    std::vector<ReferenceLightningDefinition> lightning;
    std::vector<ReferenceParticleDefinition> particles;
    std::vector<ReferenceDropletsDefinition> droplets;
    std::vector<ReferenceWetSurfaceDefinition> wetness;
    std::vector<Diagnostic> diagnostics;
};
```

**Note:** several example structs are integration contracts and need corresponding definitions/headers created as part of the implementation ledger. No C++ compile claim is made here.

# Texture and map contract

**PUBLIC-SOURCE + EXACT-ARCHIVE:** V5 wiki/configs identify equirectangular or full six-face cubemaps. Two-texture RGB-channels `2_1` cube packing is reported unsupported for volumetrics. Each layer's coverage, type, color and flow maps must use a consistent texture layout. Coverage defaults to alpha if no separate alpha-map is requested, and type generally uses red; explicit alphaMask=ALPHAMAP_R overrides any default. Read `type` and `alphaMask` from each imported node, not generic image luminance.

Texture import pipeline:

1. Resolve within the user-selected GameData root using slash-normalized relative paths; reject `..` and absolute path escape.
2. Reconstruct actual cubemap face direction transforms with an explicit six-face orientation test. Match Orbit's `CloudFieldProduct` conventions before upload; do not guess Unity face order.
3. Decode into linear scalar coverage/type/flow maps or explicit display-encoded color as appropriate. Scalar channel values are NOT automatically sRGB-decoded unless source semantics require it. Preserve alpha precision; avoid BC/DDS decode differences that change cloud boundaries.
4. Validate dimensions, mip chain, missing face, component bit-depth, channel swizzle, alpha-mask validity, clamp/repeat and seam continuity; emit texture hash, origin and effective sampler settings.
5. Generate offline mipmaps using an optical/statistical filtering policy, **not naive averaging of opacity before nonlinear Beer attenuation**. See medium chapter.
6. Report 75 missing logical texture/audio paths from THIS ZIP; do not silently substitute whitened maps. Synthetic fallback permitted only in a labeled debug lab.
7. Update a `CloudInputRevision` when any texture is reimported or painted. Existing A and B cloud sources must remain unchanged unless user explicitly selects the imported reference.

# Temporal wind, flow and painter

A layer can specify `speed`, `speedMode`, `detailSpeed`, `upwardsCloudSpeed`, `flowMap` and timeSettings (`duration`, `repeatInterval`, `fadeTime`, `unit`). Store these in their own typed structs with unconverted source units, then convert once to metres/second and planet-body angular motion using the actual radius and layer altitude. Ship-version changelog explicitly adds alternative linear speed relative to planet radius and angular mode. Preserve full orbital longitude without precision loss at timewarp and do not let normal scrolling be confused with physical storm advection.

Each `painterTiles/Item` has a stamp name, size, coverage/type remap and optional distinct coverage/type images. There are 27 stamps in the ZIP configs but their referenced images are missing. UI should use the existing Studio Build Procedurals / cloud authoring tooling, with deterministic brush sampling in body tangent coordinates, undo/revisions and a GPU invalidation notification. Prohibit painting on the unscaled macro-globe UV texture separately from physical cloud space.

# CloudField boundary

**EXISTING-ORBIT:** `engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp` exposes `CloudLayerParameters`, `CloudFieldProduct`, `GpuCloudFieldProduct`, `CloudTexel` and `CloudCoverageSource`. This existing compact type is not enough to preserve V5's 70 profiles, Hermite curves, painter stamps, flow/SDF and effect links. Do not cram these into the existing `GpuCloudLayerDescriptor` (two float4 records) or reinterpret its existing A/B ABI.

Instead create a **separate immutable `ReferenceLayerGpuTable`** containing curve sample LUTs (with slope/derivative checks), type materials, offset/stride, map handles, flow and per-layer sampling budget. These are exclusively owned by `EveV5Reference`, with a material version and content hash. The weather/physical storm model remains a common input; the reference's authored maps are additional shading data and cannot overwrite physical climate state.

# Required importer tests

- 19 OBJECTs, 70 types, 27 painter entries when run against the private release config. Tests committed to public repo should use synthetic fixtures, not proprietary complete configs.
- All 293 curve key assignments retained across 70 coverage and 70 density curve sections.
- Scientific notation (`2E-06`) parsed identically to decimal; negative altitudes permitted (Jool).
- Empty lightVolumeSettings preserves "default requested" state instead of zeroed value.
- Sorted overlap order and stable ties; never collapse two identical-name Items.
- Path traversal/security and duplicate case-insensitive asset path collision protection.
- Mixed cube/equirectangular layouts rejected; missing maps produce exact error with layer and semantic role.
- Cloud type interpolation normalized and verified at both boundary types; no division by zero on one-type layers.
- Parser emits deterministic normalized JSON and an input hash; hot reload publishes only complete frame-safe revisions.
