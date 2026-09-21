#include <orbit/celestial_globe/MacroGlobe.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace orbit::celestial_globe
{
namespace
{
[[nodiscard]] f64 ReferenceRadius(
    const universe::BodyShape& shape)
{
    return std::visit(
        [](const auto& value) -> f64
        {
            using Shape =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                return std::max({
                    value.radiiMeters.x,
                    value.radiiMeters.y,
                    value.radiiMeters.z
                });
            }
        },
        shape);
}

[[nodiscard]] f64 RadiusAlong(
    const universe::BodyShape& shape,
    const math::Double3 direction)
{
    return std::visit(
        [&direction](const auto& value) -> f64
        {
            using Shape =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                const f64 x =
                    direction.x /
                    value.radiiMeters.x;
                const f64 y =
                    direction.y /
                    value.radiiMeters.y;
                const f64 z =
                    direction.z /
                    value.radiiMeters.z;

                return 1.0 /
                    std::sqrt(
                        x*x + y*y + z*z);
            }
        },
        shape);
}

[[nodiscard]] math::Double3 FaceDirection(
    const u32 face,
    const f64 u,
    const f64 v)
{
    math::Double3 p{};

    switch (face)
    {
    case 0: p = { 1.0, v, -u}; break;
    case 1: p = {-1.0, v,  u}; break;
    case 2: p = { u, 1.0, -v}; break;
    case 3: p = { u,-1.0,  v}; break;
    case 4: p = { u, v, 1.0}; break;
    default:p = {-u, v,-1.0}; break;
    }

    return math::Normalize(p);
}

[[nodiscard]] u64 Fingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config)
{
    u64 value =
        0x4D3135474C4F4245ULL;
    value =
        terrain::StableCombine64(
            value,
            source.Revision());
    value =
        terrain::StableCombine64(
            value,
            config.faceResolution);
    value =
        terrain::StableCombine64(
            value,
            std::bit_cast<u64>(
                config.footprintScale));

    std::visit(
        [&value](const auto& bodyShape)
        {
            if constexpr (
                std::is_same_v<
                    std::decay_t<decltype(bodyShape)>,
                    universe::SphereShape>)
            {
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiusMeters));
            }
            else
            {
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiiMeters.x));
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiiMeters.y));
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiiMeters.z));
            }
        },
        shape);

    return value;
}
} // namespace

MacroGlobeMesh BuildMacroGlobe(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config)
{
    if (config.faceResolution < 3U ||
        !std::isfinite(config.footprintScale) ||
        config.footprintScale <= 0.0)
    {
        throw std::invalid_argument(
            "Macro globe config is invalid.");
    }

    const f64 referenceRadius =
        ReferenceRadius(shape);

    if (!std::isfinite(referenceRadius) ||
        referenceRadius <= 0.0)
    {
        throw std::invalid_argument(
            "Macro globe body radius is invalid.");
    }

    MacroGlobeMesh result;
    result.referenceRadiusMeters =
        referenceRadius;
    result.sourceRevision =
        source.Revision();
    result.fingerprint =
        Fingerprint(
            source,
            shape,
            config);

    const u32 n =
        config.faceResolution;
    const std::size_t verticesPerFace =
        static_cast<std::size_t>(n) * n;

    result.vertices.resize(
        verticesPerFace * 6U);

    result.indices.reserve(
        static_cast<std::size_t>(
            6U * (n - 1U) * (n - 1U) * 6U));

    const f64 angularCell =
        (0.5 * std::numbers::pi_v<f64>) /
        static_cast<f64>(n - 1U);

    result.sampleFootprintMeters =
        referenceRadius *
        angularCell *
        config.footprintScale;

    result.minimumRadiusMeters =
        std::numeric_limits<f64>::max();
    result.maximumRadiusMeters = 0.0;

    for (u32 face = 0; face < 6U; ++face)
    {
        const u32 base =
            face * n * n;

        for (u32 y = 0; y < n; ++y)
        {
            const f64 v =
                -1.0 +
                2.0 *
                static_cast<f64>(y) /
                static_cast<f64>(n - 1U);

            for (u32 x = 0; x < n; ++x)
            {
                const f64 u =
                    -1.0 +
                    2.0 *
                    static_cast<f64>(x) /
                    static_cast<f64>(n - 1U);

                const math::Double3 direction =
                    FaceDirection(
                        face,
                        u,
                        v);

                const auto sample =
                    source.Sample({
                        .unitDirection =
                            direction,
                        .footprintMeters =
                            result.
                                sampleFootprintMeters
                    });

                const f64 radius =
                    RadiusAlong(
                        shape,
                        direction) +
                    sample.elevationMeters;

                const u32 index =
                    base + y * n + x;

                result.vertices[index] = {
                    .positionMeters =
                        direction * radius,
                    .normal = {},
                    .elevationMeters =
                        sample.elevationMeters
                };

                result.minimumRadiusMeters =
                    std::min(
                        result.minimumRadiusMeters,
                        radius);
                result.maximumRadiusMeters =
                    std::max(
                        result.maximumRadiusMeters,
                        radius);
            }
        }

        for (u32 y = 0; y + 1U < n; ++y)
        {
            for (u32 x = 0; x + 1U < n; ++x)
            {
                const u32 i0 =
                    base + y * n + x;
                const u32 i1 =
                    i0 + 1U;
                const u32 i2 =
                    i0 + n;
                const u32 i3 =
                    i2 + 1U;

                result.indices.insert(
                    result.indices.end(),
                    {i0, i2, i1,
                     i1, i2, i3});
            }
        }
    }

    for (std::size_t index = 0;
         index + 2U < result.indices.size();
         index += 3U)
    {
        const u32 ia =
            result.indices[index];
        const u32 ib =
            result.indices[index + 1U];
        const u32 ic =
            result.indices[index + 2U];

        const math::Double3 a =
            result.vertices[ia].
                positionMeters;
        const math::Double3 b =
            result.vertices[ib].
                positionMeters;
        const math::Double3 c =
            result.vertices[ic].
                positionMeters;

        math::Double3 normal =
            math::Cross(
                b - a,
                c - a);

        const math::Double3 centroid =
            a + b + c;

        if (math::Dot(
                normal,
                centroid) < 0.0)
        {
            normal =
                normal * -1.0;
        }

        result.vertices[ia].normal =
            result.vertices[ia].normal +
            normal;
        result.vertices[ib].normal =
            result.vertices[ib].normal +
            normal;
        result.vertices[ic].normal =
            result.vertices[ic].normal +
            normal;
    }

    for (auto& vertex : result.vertices)
    {
        if (math::LengthSquared(
                vertex.normal) <=
            1.0e-24)
        {
            vertex.normal =
                math::Normalize(
                    vertex.positionMeters);
        }
        else
        {
            vertex.normal =
                math::Normalize(
                    vertex.normal);
        }
    }

    return result;
}

GpuMacroGlobeProduct::GpuMacroGlobeProduct(
    rhi::Device& device,
    const MacroGlobeMesh& mesh)
    : indexCount_(
          static_cast<u32>(
              mesh.indices.size())),
      referenceRadiusMeters_(
          mesh.referenceRadiusMeters),
      fingerprint_(
          mesh.fingerprint)
{
    if (mesh.vertices.empty() ||
        mesh.indices.empty() ||
        mesh.referenceRadiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "GPU macro globe requires a valid CPU mesh.");
    }

    std::vector<GpuMacroGlobeVertex>
        packed;
    packed.reserve(
        mesh.vertices.size());

    for (const auto& vertex :
         mesh.vertices)
    {
        packed.push_back({
            .positionNormalized = {
                static_cast<f32>(
                    vertex.positionMeters.x /
                    mesh.referenceRadiusMeters),
                static_cast<f32>(
                    vertex.positionMeters.y /
                    mesh.referenceRadiusMeters),
                static_cast<f32>(
                    vertex.positionMeters.z /
                    mesh.referenceRadiusMeters)
            },
            .normal = {
                static_cast<f32>(
                    vertex.normal.x),
                static_cast<f32>(
                    vertex.normal.y),
                static_cast<f32>(
                    vertex.normal.z)
            }
        });
    }

    vertices_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    packed.size() *
                    sizeof(
                        GpuMacroGlobeVertex)),
            .usage =
                rhi::BufferUsage::Vertex,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::
                    VertexOrConstantBuffer
        });

    indices_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    mesh.indices.size() *
                    sizeof(u32)),
            .usage =
                rhi::BufferUsage::Index,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::
                    IndexBuffer
        });

    if (!vertices_ || !indices_)
    {
        throw std::runtime_error(
            "Failed to allocate macro globe GPU buffers.");
    }

    std::memcpy(
        vertices_->Map(),
        packed.data(),
        packed.size() *
            sizeof(GpuMacroGlobeVertex));
    vertices_->Unmap();

    std::memcpy(
        indices_->Map(),
        mesh.indices.data(),
        mesh.indices.size() *
            sizeof(u32));
    indices_->Unmap();
}

rhi::Buffer&
GpuMacroGlobeProduct::VertexBuffer() noexcept
{
    return *vertices_;
}

rhi::Buffer&
GpuMacroGlobeProduct::IndexBuffer() noexcept
{
    return *indices_;
}

u32 GpuMacroGlobeProduct::IndexCount() const noexcept
{
    return indexCount_;
}

f64 GpuMacroGlobeProduct::
ReferenceRadiusMeters() const noexcept
{
    return referenceRadiusMeters_;
}

u64 GpuMacroGlobeProduct::Fingerprint() const noexcept
{
    return fingerprint_;
}
} // namespace orbit::celestial_globe
