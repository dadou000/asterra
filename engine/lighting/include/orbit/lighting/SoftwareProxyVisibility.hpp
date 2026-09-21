#pragma once

#include <orbit/lighting/Visibility.hpp>
#include <orbit/math/RigidTransform.hpp>
#include <orbit/rhi/AccelerationStructure.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <span>
#include <vector>

namespace orbit::lighting
{
enum class VisibilityProxyShape : u8
{
    Sphere,
    Box
};

struct VisibilityProxy
{
    u64 stableId{0U};
    universe::BodyId body{};
    frames::FrameId frame{};

    // Transform from proxy-local coordinates into proxy.frame.
    math::RigidTransformD frameFromProxy{};

    VisibilityProxyShape shape{
        VisibilityProxyShape::Sphere};

    f64 sphereRadiusMeters{0.5};
    math::Double3 boxHalfExtentsMeters{
        0.5, 0.5, 0.5};

    u32 materialId{0U};
    u32 instanceId{0U};

    // Maximum geometric deviation of this proxy from the represented object.
    f32 nominalErrorMeters{0.25F};
    bool dynamic{false};
};

struct SoftwareProxyTraversalBudget
{
    u32 maximumNodeVisits{128U};
    u32 maximumPrimitiveTests{64U};
};

struct SoftwareProxySceneStats
{
    u32 proxyCount{0U};
    u32 nodeCount{0U};
    u32 dynamicProxyCount{0U};
    f32 maximumNominalErrorMeters{0.0F};
};

struct GpuVisibilityProxyPrimitive
{
    // centerType.xyz = center in SoftwareProxyScene::Frame()
    // centerType.w   = 0 sphere, 1 oriented box
    math::Float4 centerType{};

    // Axis vectors are normalized in scene frame. w stores radius/extent.
    math::Float4 axisXExtent{};
    math::Float4 axisYExtent{};
    math::Float4 axisZExtent{};

    u32 materialId{0U};
    u32 instanceId{0U};
    f32 nominalErrorMeters{0.0F};
    u32 reserved{0U};
};

static_assert(sizeof(GpuVisibilityProxyPrimitive) == 80U);


class SoftwareProxyScene
{
public:
    void Rebuild(
        std::span<const VisibilityProxy> proxies,
        frames::FrameId targetFrame,
        const frames::FrameGraph& frames,
        time::SimulationTime atTime = {});

    [[nodiscard]] frames::FrameId Frame() const noexcept;
    [[nodiscard]] const SoftwareProxySceneStats& Stats() const noexcept;

    [[nodiscard]] std::vector<rhi::AccelerationAabb>
    AccelerationAabbs(
        math::Double3 gpuOriginInFrameMeters = {}) const;

    [[nodiscard]] std::vector<GpuVisibilityProxyPrimitive>
    GpuPrimitives(
        math::Double3 gpuOriginInFrameMeters = {}) const;

private:
    struct ResolvedProxy
    {
        VisibilityProxy source{};
        math::RigidTransformD frameFromProxy{};
        math::Double3 boundsMinimum{};
        math::Double3 boundsMaximum{};
        math::Double3 centroid{};
    };

    struct Node
    {
        math::Double3 minimum{};
        math::Double3 maximum{};
        u32 left{~0U};
        u32 right{~0U};
        u32 first{0U};
        u32 count{0U};

        [[nodiscard]] bool IsLeaf() const noexcept
        {
            return count > 0U;
        }
    };

    u32 BuildNode(
        u32 first,
        u32 count);

    frames::FrameId frame_{};
    std::vector<ResolvedProxy> proxies_;
    std::vector<u32> indices_;
    std::vector<Node> nodes_;
    SoftwareProxySceneStats stats_{};

    friend class SoftwareProxyVisibilityProvider;
};

class SoftwareProxyVisibilityProvider final
    : public VisibilityProvider
{
public:
    SoftwareProxyVisibilityProvider(
        const SoftwareProxyScene& scene,
        SoftwareProxyTraversalBudget budget = {});

    [[nodiscard]] const VisibilityProviderDesc&
    Description() const noexcept override;

    [[nodiscard]] bool SupportsPurpose(
        VisibilityPurpose purpose) const noexcept override;

    [[nodiscard]] VisibilityResult Trace(
        const VisibilityQuery& query) override;

private:
    const SoftwareProxyScene* scene_{nullptr};
    SoftwareProxyTraversalBudget budget_{};
    VisibilityProviderDesc desc_{};
};
} // namespace orbit::lighting
