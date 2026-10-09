#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/world/Planet.hpp>

#include <filesystem>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::terrain_impacts
{
struct ImpactFieldIdTag;
using ImpactFieldId = core::StrongId<ImpactFieldIdTag>;

struct ImpactIdTag;
using ImpactId = core::StrongId<ImpactIdTag>;

enum class CraterProfileKind : u8
{
    Auto,
    Simple,
    Complex
};

enum class SurfaceEnvironment : u8
{
    Airless,
    Wet,
    Icy,
    GeologicallyActive
};

struct IceFractureDefinition;

struct CraterSizeFrequencyDistribution
{
    u32 count{0};
    f64 minimumRadiusMeters{1'000.0};
    f64 maximumRadiusMeters{100'000.0};

    // Cumulative power-law slope: N(>R) ~ R^-b.
    f64 cumulativeExponent{2.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ImpactRecord
{
    ImpactId id{};
    math::Double3 centerUnitDirection{0.0, 1.0, 0.0};
    f64 radiusMeters{10'000.0};

    CraterProfileKind profile{CraterProfileKind::Auto};

    // Ratios are relative to crater radius.
    f64 simpleDepthRatio{0.18};
    f64 complexDepthRatio{0.075};
    f64 rimHeightRatio{0.035};
    f64 ejectaThicknessRatio{0.012};
    f64 ejectaExtentRadii{3.0};

    // Optional ejecta-ray/debris modulation.
    f64 rayStrength{0.0};
    u32 rayCount{0};
    // Zero preserves legacy rays confined to the ejecta blanket. A positive
    // extent adds a separately fading material-ray field, without stretching
    // the massive ejecta blanket. Irregularity bends rays smoothly with radius.
    f64 rayExtentRadii{0.0};
    f64 rayIrregularity{0.0};

    // 0 = pristine, 1 = fully topographically degraded.
    f64 degradation{0.0};

    // Higher values are younger. Overlapping excavation composes in this
    // chronological order so younger events can erase older relief.
    u64 ageOrder{0};

    // Optional physical/shape modifiers. Zero values preserve the traditional
    // near-vertical circular event and disable melt/breccia additions.
    f64 formationAgeYears{0.0};
    // Angle from the surface normal: 0 is vertical, 89 is nearly grazing.
    // This convention is shared by morphology and physical size scaling.
    f64 impactAngleDegrees{0.0};
    f64 impactAzimuthRadians{0.0};
    f64 shapeIrregularity{0.0};
    f64 meltFraction{0.0};
    f64 brecciaFraction{0.0};
    f64 multiringStrength{0.0};
    // Set impactor diameter, speed and density together to derive crater size
    // from the target's gravity/strength regime instead of authoring radius.
    f64 impactorDiameterMeters{0.0};
    f64 impactVelocityMetersPerSecond{0.0};
    f64 impactorDensityKgPerCubicMeter{0.0};
    f64 binarySeparationRadii{0.0};
    f64 binaryCompanionRadiusRatio{0.45};
    f64 binaryAzimuthRadians{0.0};
    u32 secondaryCount{0};
    f64 secondaryRadiusRatio{0.08};
    f64 secondaryRayAlignment{0.75};

    bool enabled{true};
    bool authored{true};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] f64 InfluenceExtentRadii() const noexcept;
};

enum class ResurfacingKind : u8
{
    LavaFlow,
    IceRenewal,
    TectonicRenewal
};

enum class GeologicalEventKind : u8
{
    Impact,
    LavaFlow,
    IceRenewal,
    TectonicRenewal
};

// Stable, compact reference into the immutable impact/resurfacing arrays.
// Consumers that compile regional batches can preserve exactly the same
// age/ID ordering as ImpactField::Sample without copying event payloads.
struct GeologicalEventReference
{
    GeologicalEventKind kind{GeologicalEventKind::Impact};
    std::size_t index{0U};
    ImpactId id{};
    u64 ageOrder{0U};
};

// Derived once when ImpactField is built and aligned by index with its
// ResolvedImpacts() array. GPU and CPU regional compilers share this geometry
// preparation rather than rebuilding frames or mass-balance coefficients.
struct PreparedImpactGeometry
{
    world::SurfaceFrame frame{};
    f64 phase{0.0};
    f64 ejectaMassBalanceScale{1.0};
    f64 azimuthCosine{1.0};
    f64 azimuthSine{0.0};
    f64 elongation{1.0};
    f64 influenceCosine{-1.0};
    f64 influenceChordRadius{2.0};
};

// A connected resurfacing or tectonic-renewal event is authored as a geodesic
// polyline on the planetary surface. Thickness tapers at the margins; later
// deposits reset exposure age, while optional tectonic slip advects older
// impact centers through the same canonical chronology.
struct ResurfacingRecord
{
    ImpactId id{};
    ResurfacingKind kind{ResurfacingKind::LavaFlow};
    std::vector<math::Double3> centerlineUnitDirections;
    f64 widthMeters{10'000.0};
    f64 thicknessMeters{100.0};
    f64 formationAgeYears{0.0};
    // TectonicRenewal can displace older structures along this tangent-like
    // planetary direction. Other resurfacing kinds ignore these fields.
    math::Double3 displacementUnitDirection{};
    f64 displacementMeters{0.0};
    // When true, the centerline is a closed spherical plate boundary and the
    // displacement is applied to older structures on its interior.
    bool regionalPlateMotion{false};
    u64 ageOrder{0};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct GeologicalInfluenceCap
{
    math::Double3 centerDirection{};
    f64 angularRadiusRadians{0.0};
};

struct ImpactFieldDefinition
{
    ImpactFieldId id{};
    world::PlanetId planet{};
    std::string name;

    // Zero derives from PlanetDefinition::generationSeed.
    u64 seed{0};

    CraterSizeFrequencyDistribution procedural{};
    f64 complexTransitionRadiusMeters{18'000.0};
    SurfaceEnvironment environment{SurfaceEnvironment::Airless};
    f64 surfaceAgeYears{0.0};
    f64 surfaceGravityMetersPerSecondSquared{1.62};
    f64 targetDensityKgPerCubicMeter{2'700.0};
    f64 targetStrengthPascals{1'000'000.0};

    std::vector<ImpactRecord> authoredImpacts;
    std::vector<ResurfacingRecord> resurfacingEvents;
    std::shared_ptr<const IceFractureDefinition> iceFractures{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ImpactScalingInput
{
    f64 impactorDiameterMeters{0.0};
    f64 impactVelocityMetersPerSecond{0.0};
    // Angle from the surface normal, as in ImpactRecord.
    f64 impactAngleDegrees{45.0};
    f64 impactorDensityKgPerCubicMeter{0.0};
    f64 targetDensityKgPerCubicMeter{0.0};
    f64 surfaceGravityMetersPerSecondSquared{0.0};
    f64 targetStrengthPascals{0.0};
};

// Engineering pi-scaling fit with both gravity and target-strength terms.
// Throws for non-physical inputs; returns final crater radius in meters.
[[nodiscard]] f64 ScaleImpactCraterRadiusMeters(
    const ImpactScalingInput& input);

// Returns the union of old and new conservative event influence bounds when
// only stable-ID authored impacts and local resurfacing records changed. A null
// optional means global history, tectonic displacement or another recipe
// dependency requires a full geology rebuild.
[[nodiscard]] std::optional<std::vector<GeologicalInfluenceCap>>
ChangedAuthoredEventInfluenceCaps(
    const world::PlanetDefinition& planet,
    const ImpactFieldDefinition& before,
    const ImpactFieldDefinition& after);

struct CraterProcessSample
{
    // Signed bedrock/terrain delta: negative excavation plus positive rim/ejecta.
    f64 heightDeltaMeters{0.0};

    // Positive process channels for M08+ material-column coupling.
    f64 excavationDepthMeters{0.0};
    f64 ejectaThicknessMeters{0.0};
    f64 debrisField{0.0};
    f64 rayField{0.0};
    f64 meltThicknessMeters{0.0};
    f64 brecciaField{0.0};
    f64 resurfacedMaterialFraction{0.0};
    f64 resurfacingThicknessMeters{0.0};
    f64 microImpactRoughnessMeters{0.0};
    f64 microImpactCoverage{0.0};
    // Fraction of pre-existing terrain excavated or reset by the youngest
    // affecting event at this sample. Used to compose history chronologically.
    f64 excavationCoverage{0.0};

    // Stable chronology coordinates for the oldest surviving structure and
    // most recent excavation/exposure at this sample. Zero means no event.
    u64 formationAgeOrder{0};
    u64 exposureAgeOrder{0};
    f64 formationAgeYears{0.0};
    f64 exposureAgeYears{0.0};

    u32 affectingImpacts{0};
};

// Caller-owned temporary storage for repeated regional sampling. Keeping this
// with the bake worker avoids a per-vertex allocation while preserving the
// immutable, thread-safe ImpactField.
struct ImpactQueryScratch
{
    std::vector<std::size_t> candidates;
    std::vector<std::size_t> impactCandidates;
    std::vector<std::size_t> traversalOverflow;
    std::vector<GeologicalEventReference> eventBatch;
    std::vector<std::size_t> fractureBatch;
};

class ImpactField
{
public:
    static constexpr u64 kAlgorithmVersion = 6U;

    ImpactField(
        world::PlanetDefinition planet,
        ImpactFieldDefinition definition);

    [[nodiscard]] CraterProcessSample Sample(
        const math::Double3& unitDirection,
        f64 footprintDiameterMeters) const;

    [[nodiscard]] CraterProcessSample Sample(
        const math::Double3& unitDirection,
        f64 footprintDiameterMeters,
        ImpactQueryScratch& scratch) const;

    // Samples against a conservative tile-local event batch. Events outside
    // the exact sample influence are rejected by the same event evaluators;
    // the caller avoids a hierarchy traversal for every texel in a tile.
    [[nodiscard]] CraterProcessSample Sample(
        const math::Double3& unitDirection,
        f64 footprintDiameterMeters,
        ImpactQueryScratch& scratch,
        std::span<const GeologicalEventReference> batch) const;

    [[nodiscard]] const ImpactFieldDefinition&
    Definition() const noexcept;

    // Includes explicit procedural impacts, authored events and deterministic
    // binary/secondary derivatives. Sub-resolution population is aggregated.
    [[nodiscard]] const std::vector<ImpactRecord>&
    ResolvedImpacts() const noexcept;

    [[nodiscard]] const std::vector<PreparedImpactGeometry>&
    PreparedImpactGeometries() const noexcept;

    // A complete chronological view used by regional compilers to build
    // per-tile event batches. Indices refer to ResolvedImpacts() or
    // Definition().resurfacingEvents according to `kind`.
    [[nodiscard]] std::vector<GeologicalEventReference>
    ChronologicalEvents() const;

    // Conservative tile-cap query for regional compilers. Events whose
    // influence bounds overlap the requested spherical cap are returned in
    // the same stable chronological order as ChronologicalEvents().
    [[nodiscard]] std::vector<GeologicalEventReference>
    EventsIntersectingCap(
        const math::Double3& centerDirection,
        f64 angularRadiusRadians) const;

    // Reusable form for tile compilers. Both traversal scratch and output
    // capacity are retained across calls so a planet with many tiles does not
    // allocate one temporary candidate array per tile.
    void CollectEventsIntersectingCap(
        const math::Double3& centerDirection,
        f64 angularRadiusRadians,
        ImpactQueryScratch& scratch,
        std::vector<GeologicalEventReference>& output) const;

    // Count of event records examined after the spherical hierarchy query.
    // Exposed for reproducible bake diagnostics and scale tests; ordinary
    // samples do not update shared counters or allocate diagnostic state.
    [[nodiscard]] std::size_t CandidateCount(
        const math::Double3& unitDirection) const;
    [[nodiscard]] u32 StatisticalMicroImpactCount() const noexcept;
    [[nodiscard]] f64 StatisticalMicroImpactMaximumRadiusMeters() const noexcept;

private:
    struct SpatialNode
    {
        math::Double3 minimum{};
        math::Double3 maximum{};
        f64 maximumChordRadius{0.0};
        std::size_t begin{0};
        std::size_t count{0};
        std::size_t left{0};
        std::size_t right{0};
        bool leaf{false};
    };

    void BuildSpatialIndex();
    void BuildResurfacingSpatialIndex();
    void QuerySpatialIndex(
        const math::Double3& point,
        ImpactQueryScratch& scratch,
        f64 queryChordRadius = 0.0) const;
    void QueryResurfacingSpatialIndex(
        const math::Double3& point,
        ImpactQueryScratch& scratch,
        f64 queryChordRadius = 0.0) const;
    [[nodiscard]] CraterProcessSample SampleCandidates(
        const math::Double3& canonicalDirection,
        f64 footprintDiameterMeters,
        ImpactQueryScratch& scratch) const;

    world::PlanetDefinition planet_{};
    ImpactFieldDefinition definition_{};
    std::vector<ImpactRecord> resolvedImpacts_;
    // Per-event geometry/profile constants are compiled once and share the
    // exact resolved-impact indexing for bake and GPU batch consumers.
    std::vector<PreparedImpactGeometry> preparedImpactGeometries_;
    std::vector<std::size_t> spatialOrder_;
    std::vector<SpatialNode> spatialNodes_;
    std::vector<math::Double3> resurfacingCenters_;
    std::vector<f64> resurfacingChordRadii_;
    std::vector<std::size_t> resurfacingOrder_;
    std::vector<SpatialNode> resurfacingSpatialNodes_;
    u32 statisticalMicroImpactCount_{0};
    f64 statisticalMicroImpactMaximumRadiusMeters_{0.0};
};

struct IceFractureDefinition
{
    u64 seed{1};
    u64 ageOrder{0};
    f64 formationAgeYears{0.0};
    bool enabled{true};
    math::Double3 tidalAxis{1.0, 0.0, 0.0};
    math::Double3 spinAxis{0.0, 1.0, 0.0};
    f64 tidalStress{1.0};
    f64 rotationalStress{0.2};
    f64 tensileStrength{0.25};
    u32 fractureCount{48};
    u32 segmentsPerFracture{12};
    f64 maximumLengthMeters{1'200'000.0};
    f64 widthMeters{1'800.0};
    f64 grooveDepthMeters{90.0};
    f64 ridgeHeightMeters{28.0};
    f64 branchProbability{0.18};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct IceFractureSample
{
    f64 heightDeltaMeters{0.0};
    f64 damage{0.0};
    f64 fractureCoverage{0.0};
    u32 nearbySegments{0};
    u64 ageOrder{0};
    f64 formationAgeYears{0.0};
};

struct IceFractureSegment
{
    math::Double3 startDirection{};
    math::Double3 endDirection{};
    math::Double3 midpointDirection{};
    f64 halfLengthMeters{0.0};
};

// Deterministic stress-guided spherical fracture curves. The field is immutable
// after construction and spatially indexed for coarse-to-fine regional queries.
class IceFractureField
{
public:
    IceFractureField(
        world::PlanetDefinition planet,
        IceFractureDefinition definition);

    [[nodiscard]] IceFractureSample Sample(
        const math::Double3& unitDirection,
        f64 footprintDiameterMeters,
        ImpactQueryScratch& scratch) const;
    [[nodiscard]] IceFractureSample Sample(
        const math::Double3& unitDirection,
        f64 footprintDiameterMeters,
        std::span<const std::size_t> segmentBatch) const;

    [[nodiscard]] const IceFractureDefinition& Definition() const noexcept;
    [[nodiscard]] std::size_t SegmentCount() const noexcept;
    [[nodiscard]] const std::vector<IceFractureSegment>& Segments() const noexcept;

    // Returns stable indices for fracture segments whose indexed influence
    // bounds overlap a tile cap. Caller-owned scratch and output vectors are
    // reusable across region queries.
    void CollectSegmentsIntersectingCap(
        const math::Double3& centerDirection,
        f64 angularRadiusRadians,
        ImpactQueryScratch& scratch,
        std::vector<std::size_t>& output) const;

private:
    struct SpatialNode
    {
        math::Double3 minimum{};
        math::Double3 maximum{};
        f64 maximumChordRadius{0.0};
        std::size_t begin{0};
        std::size_t count{0};
        std::size_t left{0};
        std::size_t right{0};
        bool leaf{false};
    };

    void BuildSpatialIndex();
    void QuerySpatialIndex(const math::Double3& point,
        ImpactQueryScratch& scratch,
        f64 queryChordRadius = 0.0) const;
    [[nodiscard]] IceFractureSample SampleSegments(
        const math::Double3& canonicalDirection,
        f64 footprintDiameterMeters,
        std::span<const std::size_t> segmentIndices) const;

    world::PlanetDefinition planet_{};
    IceFractureDefinition definition_{};
    std::vector<IceFractureSegment> segments_;
    std::vector<std::size_t> segmentOrder_;
    std::vector<SpatialNode> spatialNodes_;
};

[[nodiscard]] ImpactFieldDefinition
MakeMoonLikeImpactPreset(
    world::PlanetId planet,
    ImpactFieldId fieldId,
    u64 seed = 0);

// Project-authority codec for .orbitimpacts records.
[[nodiscard]] ImpactFieldDefinition ParseImpactFieldToml(
    std::string_view text);
[[nodiscard]] std::string SerializeImpactFieldToml(
    const ImpactFieldDefinition& definition);
[[nodiscard]] ImpactFieldDefinition LoadImpactFieldFile(
    const std::filesystem::path& path);
} // namespace orbit::terrain_impacts
