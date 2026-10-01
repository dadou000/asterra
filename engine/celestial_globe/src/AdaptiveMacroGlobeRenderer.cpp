#include <orbit/profiler/Profiler.hpp>
#include <orbit/celestial_globe/MacroGlobe.hpp>
#include <orbit/core/ThreadName.hpp>
#include <orbit/celestial_globe/PlanetPatchHierarchy.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::celestial_globe
{
namespace
{
struct RegisteredTerrainAuthority
{
    const terrain::TerrainSource* source{nullptr};
    // Set when the caller handed over shared ownership. Background patch
    // builds hold a copy, so a world rebuild can never free the source under
    // a running job. Without it, patches are built synchronously.
    std::shared_ptr<const terrain::TerrainSource> owner;
    universe::BodyShape shape{};
    u64 sourceRevision{0U};
};

std::mutex gTerrainAuthorityMutex;
std::map<u64, RegisteredTerrainAuthority> gTerrainAuthorities;

[[nodiscard]] std::optional<RegisteredTerrainAuthority>
LookupTerrainAuthority(const u64 fingerprint)
{
    std::scoped_lock lock(gTerrainAuthorityMutex);
    const auto found = gTerrainAuthorities.find(fingerprint);
    if (found == gTerrainAuthorities.end())
    {
        return std::nullopt;
    }
    return found->second;
}

[[nodiscard]] math::Float3 Mix(
    const math::Float3 a,
    const math::Float3 b,
    const f32 t) noexcept
{
    return a * (1.0F - t) + b * t;
}

[[nodiscard]] math::Float3 BiomeAlbedo(
    const terrain::BiomeWeights& b,
    const bool drySurface) noexcept
{
    if (drySurface)
    {
        const math::Float3 lowland{0.30F, 0.105F, 0.018F};
        const math::Float3 dust{0.42F, 0.165F, 0.032F};
        const math::Float3 ochre{0.36F, 0.125F, 0.022F};
        const math::Float3 darkRock{0.20F, 0.068F, 0.016F};
        const math::Float3 basalt{0.22F, 0.080F, 0.024F};
        const math::Float3 coldDust{0.38F, 0.205F, 0.105F};
        const math::Float3 highland{0.34F, 0.145F, 0.055F};
        const math::Float3 sediment{0.37F, 0.135F, 0.022F};
        return
            lowland * b.ocean +
            dust * b.desert +
            ochre * b.grassland +
            darkRock * b.temperateForest +
            basalt * b.borealForest +
            coldDust * b.tundra +
            highland * b.alpine +
            sediment * b.wetland;
    }

    const math::Float3 ocean{0.015F, 0.055F, 0.095F};
    const math::Float3 desert{0.48F, 0.36F, 0.20F};
    const math::Float3 grass{0.13F, 0.28F, 0.10F};
    const math::Float3 temperate{0.055F, 0.18F, 0.065F};
    const math::Float3 boreal{0.045F, 0.12F, 0.065F};
    const math::Float3 tundra{0.34F, 0.36F, 0.32F};
    const math::Float3 alpine{0.31F, 0.30F, 0.28F};
    const math::Float3 wetland{0.065F, 0.16F, 0.10F};
    return
        ocean * b.ocean +
        desert * b.desert +
        grass * b.grassland +
        temperate * b.temperateForest +
        boreal * b.borealForest +
        tundra * b.tundra +
        alpine * b.alpine +
        wetland * b.wetland;
}

[[nodiscard]] f32 BiomeRoughness(
    const terrain::BiomeWeights& b) noexcept
{
    return std::clamp(
        0.58F * b.ocean +
        0.88F * b.desert +
        0.82F * b.grassland +
        0.86F * b.temperateForest +
        0.90F * b.borealForest +
        0.91F * b.tundra +
        0.94F * b.alpine +
        0.76F * b.wetland,
        0.04F,
        1.0F);
}

// Relief cue that does not depend on the sun angle. Under a high sun a crater
// has no shading, so craters vanish from orbit. A vertex that sits lower than
// its neighbours (a floor) is darkened and one that sits higher (a rim or
// ejecta blanket) is brightened, in proportion to how strongly it stands out
// at this patch's resolution. 0 is flat; positive is concave.
template <typename ElevationAt>
[[nodiscard]] f32 PatchCavity(
    const ElevationAt& elevationAt,
    const i32 x,
    const i32 y,
    const f64 footprintMeters)
{
    if (footprintMeters <= 0.0)
    {
        return 0.0F;
    }

    const f64 mean =
        0.25 * (elevationAt(x - 1, y) +
                elevationAt(x + 1, y) +
                elevationAt(x, y - 1) +
                elevationAt(x, y + 1));

    return static_cast<f32>((mean - elevationAt(x, y)) / footprintMeters);
}

// Sine of the angle between the surface normal and the local radial
// direction: 0 on flat ground, larger on crater walls and scarps.
[[nodiscard]] f32 PatchSlopeSine(const PlanetPatchVertex& vertex) noexcept
{
    const auto radial = math::Normalize(vertex.positionMeters);
    const f64 cosine = std::clamp(math::Dot(vertex.normal, radial), -1.0, 1.0);
    return static_cast<f32>(std::sqrt(std::max(0.0, 1.0 - cosine * cosine)));
}

[[nodiscard]] celestial_appearance::AppearanceTexel
BuildPatchAppearance(
    const terrain::TerrainSource& source,
    const PlanetPatchVertex& vertex,
    const f64 footprintMeters,
    const bool oceanEnabled,
    const f32 cavity,
    const f32 slopeSine)
{
    const auto direction = math::Normalize(vertex.positionMeters);
    const auto sample = source.Sample({
        .unitDirection = direction,
        .footprintMeters = footprintMeters
    });

    const f32 ocean =
        oceanEnabled && sample.standingWaterDepthMeters > 0.01
            ? 1.0F
            : 0.0F;

    f32 ice = static_cast<f32>(std::clamp(
        (-1.5 - static_cast<f64>(sample.climate.temperatureC)) / 13.5,
        0.0,
        1.0));

    if (ocean < 0.5F)
    {
        const f32 snowClimate = static_cast<f32>(std::clamp(
            (-2.0 - static_cast<f64>(sample.climate.temperatureC)) / 18.0,
            0.0,
            1.0));
        ice = std::max(
            ice,
            snowClimate * std::clamp(
                sample.biomes.tundra + sample.biomes.alpine,
                0.0F,
                1.0F));
    }

    if (!oceanEnabled)
    {
        ice = static_cast<f32>(std::clamp(
            (std::abs(direction.y) - 0.84) / 0.12,
            0.0,
            1.0));
    }

    auto albedo = BiomeAlbedo(sample.biomes, !oceanEnabled);
    if (ocean > 0.5F)
    {
        albedo = {0.012F, 0.042F, 0.075F};
    }
    albedo = Mix(albedo, {0.76F, 0.82F, 0.86F}, ice);

    if (ocean < 0.5F)
    {
        // Darker floors and steep walls, lighter rims. Clamped so noisy relief
        // never turns the surface black or white. The gains are deliberately
        // strong: from orbit under a high sun this is the only thing that
        // makes craters readable.
        const f32 relief = std::clamp(
            1.0F - 0.75F * std::tanh(cavity * 9.0F) -
                0.9F * slopeSine,
            0.30F,
            1.45F);
        albedo = {
            albedo.x * relief,
            albedo.y * relief,
            albedo.z * relief
        };
    }

    f32 roughness = BiomeRoughness(sample.biomes);
    if (ocean > 0.5F)
    {
        roughness = 0.18F;
    }
    roughness = roughness * (1.0F - ice) + 0.34F * ice;

    return {
        .albedoLinear = albedo,
        .normal = {
            static_cast<f32>(vertex.normal.x),
            static_cast<f32>(vertex.normal.y),
            static_cast<f32>(vertex.normal.z)
        },
        .roughness = roughness,
        .oceanMask = ocean,
        .waterDepthMeters = static_cast<f32>(
            std::max(sample.standingWaterDepthMeters, 0.0)),
        .iceMask = ice,
        .directLightTransmittance = 1.0F,
        .emissionLinear = {}
    };
}

[[nodiscard]] MacroGlobeMesh ToMacroMesh(
    const PlanetPatchMesh& patch)
{
    MacroGlobeMesh result;
    result.referenceRadiusMeters = patch.referenceRadiusMeters;
    result.minimumRadiusMeters = patch.minimumRadiusMeters;
    result.maximumRadiusMeters = patch.maximumRadiusMeters;
    result.sampleFootprintMeters = patch.sampleFootprintMeters;
    result.sourceRevision = patch.sourceRevision;
    result.fingerprint = patch.fingerprint;
    result.indices = patch.indices;
    result.vertices.reserve(patch.vertices.size());
    for (const auto& vertex : patch.vertices)
    {
        result.vertices.push_back({
            .positionMeters = vertex.positionMeters,
            .normal = vertex.normal,
            .elevationMeters = vertex.elevationMeters
        });
    }
    return result;
}

[[nodiscard]] std::array<PlanetPatchId, 4> Children(
    const PlanetPatchId id) noexcept
{
    const u8 next = static_cast<u8>(id.level + 1U);
    const u32 x = id.x * 2U;
    const u32 y = id.y * 2U;
    return {{
        {id.face, next, x + 0U, y + 0U},
        {id.face, next, x + 1U, y + 0U},
        {id.face, next, x + 0U, y + 1U},
        {id.face, next, x + 1U, y + 1U}
    }};
}

[[nodiscard]] PlanetPatchId Parent(
    const PlanetPatchId id) noexcept
{
    return {
        .face = id.face,
        .level = static_cast<u8>(id.level - 1U),
        .x = id.x / 2U,
        .y = id.y / 2U
    };
}
} // namespace

u64 MacroGlobeFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config)
{
    const u64 fingerprint =
        LegacyMacroGlobeFingerprint(source, shape, config);

    {
        std::scoped_lock lock(gTerrainAuthorityMutex);
        gTerrainAuthorities.insert_or_assign(
            fingerprint,
            RegisteredTerrainAuthority{
                .source = &source,
                .shape = shape,
                .sourceRevision = source.Revision()
            });
    }

    return fingerprint;
}

u64 MacroGlobeFingerprint(
    const std::shared_ptr<const terrain::TerrainSource>& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config)
{
    const u64 fingerprint =
        MacroGlobeFingerprint(*source, shape, config);

    std::scoped_lock lock(gTerrainAuthorityMutex);
    if (const auto found = gTerrainAuthorities.find(fingerprint);
        found != gTerrainAuthorities.end())
    {
        found->second.owner = source;
    }

    return fingerprint;
}

GpuMacroGlobeProduct::GpuMacroGlobeProduct(
    rhi::Device& device,
    const MacroGlobeMesh& mesh,
    const std::span<const celestial_appearance::AppearanceTexel> appearance)
    : indexCount_(static_cast<u32>(mesh.indices.size())),
      referenceRadiusMeters_(mesh.referenceRadiusMeters),
      fingerprint_(mesh.fingerprint)
{
    if (mesh.vertices.empty() ||
        mesh.indices.empty() ||
        mesh.referenceRadiusMeters <= 0.0 ||
        appearance.size() != mesh.vertices.size())
    {
        throw std::invalid_argument(
            "GPU adaptive globe patch requires matching geometry and appearance vertices.");
    }

    std::vector<GpuMacroGlobeVertex> packed;
    packed.reserve(mesh.vertices.size());
    for (std::size_t index = 0U; index < mesh.vertices.size(); ++index)
    {
        const auto& vertex = mesh.vertices[index];
        const auto& texel = appearance[index];
        packed.push_back({
            .positionNormalized = {
                static_cast<f32>(vertex.positionMeters.x / mesh.referenceRadiusMeters),
                static_cast<f32>(vertex.positionMeters.y / mesh.referenceRadiusMeters),
                static_cast<f32>(vertex.positionMeters.z / mesh.referenceRadiusMeters)
            },
            .normal = {
                static_cast<f32>(vertex.normal.x),
                static_cast<f32>(vertex.normal.y),
                static_cast<f32>(vertex.normal.z)
            },
            .albedoLinear = texel.albedoLinear,
            .appearanceNormal = texel.normal,
            .materialChannels = {
                texel.roughness,
                texel.oceanMask,
                texel.iceMask,
                texel.directLightTransmittance
            },
            .emissionLinear = texel.emissionLinear
        });
    }

    vertices_ = device.CreateBuffer({
        .sizeBytes = static_cast<u64>(packed.size() * sizeof(GpuMacroGlobeVertex)),
        .usage = rhi::BufferUsage::Vertex,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::VertexOrConstantBuffer
    });
    indices_ = device.CreateBuffer({
        .sizeBytes = static_cast<u64>(mesh.indices.size() * sizeof(u32)),
        .usage = rhi::BufferUsage::Index,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::IndexBuffer
    });

    if (!vertices_ || !indices_)
    {
        throw std::runtime_error(
            "Failed to allocate adaptive macro-globe patch buffers.");
    }

    std::memcpy(
        vertices_->Map(),
        packed.data(),
        packed.size() * sizeof(GpuMacroGlobeVertex));
    vertices_->Unmap();
    std::memcpy(
        indices_->Map(),
        mesh.indices.data(),
        mesh.indices.size() * sizeof(u32));
    indices_->Unmap();
}

class HybridMacroGlobeRuntime
{
public:
    explicit HybridMacroGlobeRuntime(rhi::Device& device)
        : device_(&device)
    {
    }

    [[nodiscard]] MacroGlobeWorkStats WorkStats() const noexcept
    {
        MacroGlobeWorkStats stats;
        for (const auto& [fingerprint, state] : states_)
        {
            static_cast<void>(fingerprint);
            stats.patchesPending += static_cast<u32>(state.pending.size());
            stats.patchesResident += static_cast<u32>(state.residents.size());
        }
        return stats;
    }

    // True while terrain patches are still being built or uploaded, so the
    // caller can keep rendering at full rate until refinement settles.
    [[nodiscard]] bool HasPendingWork() const noexcept
    {
        for (const auto& [fingerprint, state] : states_)
        {
            static_cast<void>(fingerprint);
            if (!state.pending.empty())
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool Prepare(
        GpuMacroGlobeProduct& fallback,
        const render_view::CameraState& camera,
        const u32 width,
        const u32 height,
        const bool oceanEnabled,
        const f32 lodBiasStops)
    {
        ORBIT_PROFILE_SCOPE("globe.prepare");
        const auto registered = LookupTerrainAuthority(fallback.Fingerprint());
        if (!registered.has_value() ||
            registered->source == nullptr ||
            registered->source->Revision() != registered->sourceRevision)
        {
            return false;
        }

        auto& state = states_[fallback.Fingerprint()];
        if (!state.initialized ||
            state.authority.source != registered->source ||
            state.authority.sourceRevision != registered->sourceRevision ||
            state.oceanEnabled != oceanEnabled)
        {
            state = State{};
            state.initialized = true;
            state.authority = *registered;
            state.oceanEnabled = oceanEnabled;
        }

        ++state.serial;
        state.syncBuildsThisPrepare = 0U;
        CollectCompleted(state);
        const f64 radius = std::max(fallback.ReferenceRadiusMeters(), 1.0);
        const PlanetPatchView view{
            .cameraPositionMeters = camera.localPositionMeters,
            .cameraForward = camera.forward,
            .verticalFovRadians = static_cast<f64>(camera.verticalFovRadians),
            .viewportWidthPixels = width,
            .viewportHeightPixels = height,
            .maximumDisplacementMeters = std::max(radius * 0.02, 1000.0)
        };
        // LOD bias. The pixel target alone does nothing once the selector is
        // bound by its patch budget or level cap (the usual case from orbit),
        // so the bias also scales the selection budget (x1/8 .. x2, with the
        // resident cap kept at 2.5x the selection so a refined view does not
        // thrash) and lets positive bias refine up to two levels deeper.
        const f64 bias = static_cast<f64>(
            std::clamp(lodBiasStops, -4.0F, 4.0F));
        const f64 budgetScale = std::clamp(std::exp2(bias), 0.125, 2.0);
        state.selectedBudget = std::max(
            128U,
            static_cast<u32>(
                static_cast<f64>(kMaximumSelectedPatches) * budgetScale));
        state.residentBudget =
            static_cast<std::size_t>(state.selectedBudget) * 5U / 2U;
        const u32 maximumLevel =
            kMaximumLevel +
            static_cast<u32>(std::clamp(std::lround(bias), 0L, 2L));

        const PlanetPatchSelectorConfig selectorConfig{
            .patchResolution = kPatchResolution,
            .maximumLevel = maximumLevel,
            .targetCellPixels = 2.0 * std::exp2(-bias),
            .hysteresisFraction = 0.20,
            .maximumSelectedPatches = state.selectedBudget,
            .horizonCulling = true,
            .frustumCulling = true
        };

        const auto selection = state.selector.Select(
            state.authority.shape,
            view,
            selectorConfig);

        state.desiredNodes.clear();
        state.desiredRefined.clear();
        for (const auto leaf : selection.patches)
        {
            state.desiredNodes.insert(leaf);
            auto ancestor = leaf;
            while (ancestor.level > 0U)
            {
                ancestor = Parent(ancestor);
                state.desiredNodes.insert(ancestor);
                state.desiredRefined.insert(ancestor);
            }
        }

        std::vector<PlanetPatchId> missing;
        missing.reserve(64U);

        for (u8 face = 0U; face < 6U; ++face)
        {
            const PlanetPatchId root{.face = face};
            if (state.desiredNodes.contains(root) &&
                !state.residents.contains(root))
            {
                missing.push_back(root);
            }
        }

        if (missing.empty())
        {
            for (const auto node : state.desiredRefined)
            {
                if (!state.residents.contains(node))
                {
                    continue;
                }
                for (const auto child : Children(node))
                {
                    if (state.desiredNodes.contains(child) &&
                        !state.residents.contains(child))
                    {
                        missing.push_back(child);
                    }
                }
            }
        }

        std::sort(
            missing.begin(),
            missing.end(),
            [](const PlanetPatchId a, const PlanetPatchId b)
            {
                if (a.level != b.level)
                {
                    return a.level < b.level;
                }
                return a < b;
            });
        missing.erase(
            std::unique(missing.begin(), missing.end()),
            missing.end());

        for (const auto id : missing)
        {
            if (state.pending.size() >= kPatchBuildsInFlight)
            {
                break;
            }
            LaunchBuild(state, id);
        }

        state.active.clear();
        bool allVisibleRootsResident = true;
        for (u8 face = 0U; face < 6U; ++face)
        {
            const PlanetPatchId root{.face = face};
            if (!state.desiredNodes.contains(root))
            {
                continue;
            }
            if (!state.residents.contains(root))
            {
                allVisibleRootsResident = false;
                continue;
            }
            AppendActive(state, root);
        }

        if (!allVisibleRootsResident)
        {
            return false;
        }

        for (const auto id : state.active)
        {
            if (auto found = state.residents.find(id);
                found != state.residents.end())
            {
                found->second.lastUsedSerial = state.serial;
            }
        }
        EvictColdResidents(state);
        activeState_ = &state;
        return true;
    }

    void Draw(
        LegacyMacroGlobeRenderer& renderer,
        rhi::CommandList& commands,
        rhi::Texture& target,
        const u32 width,
        const u32 height,
        const render_view::CameraState& camera,
        const f32 opacity,
        const MacroGlobeLighting& lighting)
    {
        if (activeState_ == nullptr)
        {
            return;
        }
        for (const auto id : activeState_->active)
        {
            auto found = activeState_->residents.find(id);
            if (found == activeState_->residents.end())
            {
                continue;
            }
            renderer.Draw(
                commands,
                target,
                width,
                height,
                *found->second.gpu,
                camera,
                opacity,
                lighting);
        }
    }

    void DrawSurface(
        LegacyMacroGlobeRenderer& renderer,
        rhi::CommandList& commands,
        rhi::Texture& previewColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        const u32 width,
        const u32 height,
        const render_view::CameraState& camera,
        const f32 opacity,
        const MacroGlobeLighting& lighting,
        rhi::Texture* depth)
    {
        if (activeState_ == nullptr)
        {
            return;
        }
        for (const auto id : activeState_->active)
        {
            auto found = activeState_->residents.find(id);
            if (found == activeState_->residents.end())
            {
                continue;
            }
            renderer.DrawSurface(
                commands,
                previewColor,
                surfaceBaseRoughness,
                surfaceNormalMetallic,
                surfaceEmissionClass,
                width,
                height,
                *found->second.gpu,
                camera,
                opacity,
                lighting,
                depth);
        }
    }

private:
    static constexpr u32 kPatchResolution = 33U;
    static constexpr u32 kMaximumLevel = 10U;
    static constexpr u32 kMaximumSelectedPatches = 1024U;
    // Patch builds run on worker threads. This many may be in flight at once, and
    // at most kPatchUploadsPerPrepare finished patches are uploaded to the GPU
    // per call, so the render thread never waits on terrain sampling.
    static constexpr std::size_t kPatchBuildsInFlight = 6U;
    static constexpr u32 kPatchUploadsPerPrepare = 8U;

    struct Resident
    {
        std::unique_ptr<GpuMacroGlobeProduct> gpu;
        u64 lastUsedSerial{0U};
    };

    struct BuiltPatch
    {
        PlanetPatchMesh patch;
        std::vector<celestial_appearance::AppearanceTexel> appearance;
    };

    struct State
    {
        bool initialized{false};
        RegisteredTerrainAuthority authority{};
        bool oceanEnabled{false};
        PlanetPatchSelector selector;
        std::map<PlanetPatchId, Resident> residents;
        std::set<PlanetPatchId> desiredNodes;
        std::set<PlanetPatchId> desiredRefined;
        std::vector<PlanetPatchId> active;
        u64 serial{0U};
        u32 syncBuildsThisPrepare{0U};
        // Selection and residency budgets for the current LOD bias.
        u32 selectedBudget{1024U};
        std::size_t residentBudget{2560U};
        // Destroyed first (a std::future from std::async blocks until its job
        // is done), so jobs never outlive the source/shape they read.
        std::map<PlanetPatchId, std::future<BuiltPatch>> pending;
    };

    // Samples the terrain and builds the vertex/appearance data for one patch.
    // Pure CPU work with no GPU access, so it is safe on a worker thread
    // (TerrainSource::Sample is required to be thread-safe).
    [[nodiscard]] static BuiltPatch BuildPatchData(
        const terrain::TerrainSource* source,
        const universe::BodyShape shape,
        const PlanetPatchId id,
        const bool oceanEnabled)
    {
        core::SetCurrentThreadName("Orbit.GlobePatch");
        BuiltPatch built;
        built.patch = BuildPlanetPatch(
            *source,
            shape,
            id,
            PlanetPatchMeshConfig{
                .patchResolution = kPatchResolution,
                .footprintScale = 1.5,
                .skirtDepthMeters = 0.0
            });

        built.appearance.reserve(built.patch.vertices.size());
        const i32 n = static_cast<i32>(kPatchResolution);

        // Neighbours outside the patch are sampled from the terrain source so
        // edge vertices see the same relief as interior ones; one-sided
        // neighbours would print a visible line along every patch edge.
        const auto elevationAt =
            [&](const i32 x, const i32 y) -> f64
        {
            if (x >= 0 && x < n && y >= 0 && y < n)
            {
                return built.patch.vertices[
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(n) +
                    static_cast<std::size_t>(x)].elevationMeters;
            }

            const auto direction = PlanetPatchDirection(
                id,
                static_cast<f64>(x) / static_cast<f64>(n - 1),
                static_cast<f64>(y) / static_cast<f64>(n - 1));
            return source->Sample({
                .unitDirection = direction,
                .footprintMeters = built.patch.sampleFootprintMeters
            }).elevationMeters;
        };

        for (std::size_t index = 0U; index < built.patch.vertices.size(); ++index)
        {
            const i32 x = static_cast<i32>(index % static_cast<std::size_t>(n));
            const i32 y = static_cast<i32>(index / static_cast<std::size_t>(n));
            built.appearance.push_back(BuildPatchAppearance(
                *source,
                built.patch.vertices[index],
                built.patch.sampleFootprintMeters,
                oceanEnabled,
                PatchCavity(
                    elevationAt,
                    x,
                    y,
                    built.patch.sampleFootprintMeters),
                PatchSlopeSine(built.patch.vertices[index])));
        }

        return built;
    }

    void LaunchBuild(State& state, const PlanetPatchId id)
    {
        if (state.residents.contains(id) ||
            state.pending.contains(id) ||
            state.authority.source == nullptr ||
            device_ == nullptr)
        {
            return;
        }

        if (state.authority.owner == nullptr)
        {
            // No shared ownership: the source is only guaranteed alive for
            // this call, so build on the calling thread.
            if (state.syncBuildsThisPrepare >= 2U)
            {
                return;
            }
            ++state.syncBuildsThisPrepare;

            BuiltPatch built = BuildPatchData(
                state.authority.source,
                state.authority.shape,
                id,
                state.oceanEnabled);
            Upload(state, id, built);
            return;
        }

        state.pending.emplace(
            id,
            std::async(
                std::launch::async,
                [owner = state.authority.owner,
                 shape = state.authority.shape,
                 id,
                 ocean = state.oceanEnabled]
                {
                    ORBIT_PROFILE_SCOPE("globe.build_patch");
                    return BuildPatchData(owner.get(), shape, id, ocean);
                }));
    }

    void Upload(State& state, const PlanetPatchId id, BuiltPatch& built)
    {
        ORBIT_PROFILE_SCOPE("globe.upload_patch");
        auto macroMesh = ToMacroMesh(built.patch);
        auto gpu = std::make_unique<GpuMacroGlobeProduct>(
            *device_,
            macroMesh,
            std::span<const celestial_appearance::AppearanceTexel>(
                built.appearance));

        state.residents.emplace(
            id,
            Resident{
                .gpu = std::move(gpu),
                .lastUsedSerial = state.serial
            });
    }

    // Uploads finished patches on the calling (render) thread.
    void CollectCompleted(State& state)
    {
        u32 uploaded = 0U;
        for (auto it = state.pending.begin(); it != state.pending.end();)
        {
            if (uploaded >= kPatchUploadsPerPrepare)
            {
                break;
            }

            if (it->second.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready)
            {
                ++it;
                continue;
            }

            const PlanetPatchId id = it->first;
            BuiltPatch built = it->second.get();
            it = state.pending.erase(it);

            Upload(state, id, built);
            ++uploaded;
        }
    }

    void AppendActive(State& state, const PlanetPatchId node)
    {
        if (!state.desiredNodes.contains(node))
        {
            return;
        }

        if (state.desiredRefined.contains(node))
        {
            bool childrenReady = true;
            const auto children = Children(node);
            for (const auto child : children)
            {
                if (state.desiredNodes.contains(child) &&
                    !state.residents.contains(child))
                {
                    childrenReady = false;
                    break;
                }
            }

            if (childrenReady)
            {
                for (const auto child : children)
                {
                    AppendActive(state, child);
                }
                return;
            }
        }

        state.active.push_back(node);
    }

    void EvictColdResidents(State& state)
    {
        if (state.residents.size() <= state.residentBudget)
        {
            return;
        }

        const std::set<PlanetPatchId> active(
            state.active.begin(), state.active.end());

        while (state.residents.size() > state.residentBudget)
        {
            auto victim = state.residents.end();
            for (auto it = state.residents.begin();
                 it != state.residents.end();
                 ++it)
            {
                if (it->first.level == 0U ||
                    active.contains(it->first) ||
                    state.desiredNodes.contains(it->first))
                {
                    continue;
                }
                if (victim == state.residents.end() ||
                    it->second.lastUsedSerial < victim->second.lastUsedSerial)
                {
                    victim = it;
                }
            }

            if (victim == state.residents.end())
            {
                break;
            }
            state.residents.erase(victim);
        }
    }

    rhi::Device* device_{nullptr};
    std::map<u64, State> states_;
    State* activeState_{nullptr};
};

MacroGlobeRenderer::MacroGlobeRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : legacy_(std::make_unique<LegacyMacroGlobeRenderer>(device, compiler)),
      hybrid_(std::make_unique<HybridMacroGlobeRuntime>(device))
{
}

MacroGlobeRenderer::~MacroGlobeRenderer() = default;

bool MacroGlobeRenderer::HasPendingWork() const noexcept
{
    return hybrid_ != nullptr && hybrid_->HasPendingWork();
}

MacroGlobeWorkStats MacroGlobeRenderer::WorkStats() const noexcept
{
    return hybrid_ != nullptr ? hybrid_->WorkStats() : MacroGlobeWorkStats{};
}

void MacroGlobeRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    GpuMacroGlobeProduct& globe,
    const render_view::CameraState& camera,
    const f32 opacity,
    const MacroGlobeLighting& lighting)
{
    if (hybrid_ != nullptr &&
        hybrid_->Prepare(
            globe,
            camera,
            width,
            height,
            lighting.oceanEnabled,
            lighting.lodBiasStops))
    {
        hybrid_->Draw(
            *legacy_,
            commands,
            target,
            width,
            height,
            camera,
            opacity,
            lighting);
        return;
    }

    legacy_->Draw(
        commands,
        target,
        width,
        height,
        globe,
        camera,
        opacity,
        lighting);
}

void MacroGlobeRenderer::DrawSurface(
    rhi::CommandList& commands,
    rhi::Texture& previewColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    const u32 width,
    const u32 height,
    GpuMacroGlobeProduct& globe,
    const render_view::CameraState& camera,
    const f32 opacity,
    const MacroGlobeLighting& lighting,
    rhi::Texture* depth)
{
    if (hybrid_ != nullptr &&
        hybrid_->Prepare(
            globe,
            camera,
            width,
            height,
            lighting.oceanEnabled,
            lighting.lodBiasStops))
    {
        hybrid_->DrawSurface(
            *legacy_,
            commands,
            previewColor,
            surfaceBaseRoughness,
            surfaceNormalMetallic,
            surfaceEmissionClass,
            width,
            height,
            camera,
            opacity,
            lighting,
            depth);
        return;
    }

    legacy_->DrawSurface(
        commands,
        previewColor,
        surfaceBaseRoughness,
        surfaceNormalMetallic,
        surfaceEmissionClass,
        width,
        height,
        globe,
        camera,
        opacity,
        lighting,
        depth);
}
} // namespace orbit::celestial_globe
