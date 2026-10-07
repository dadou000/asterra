#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/mesh_render/MeshSurface.hpp>

#include <vector>

namespace orbit::mesh_render::detail
{
// One draw: a part of a model with its instance transform and material,
// addressed by `record` in the structured buffer built alongside.
struct DrawCall
{
    const MeshModel* model;
    const MeshModel::Part* part;
    u32 record;
};

// Flattens instances into draws and kMeshDrawRecordVectors float4s per
// draw: rows 0-2 = model-to-camera-relative transform, 3 = base colour
// factor, 4 = emissive rgb + metallic, 5 = roughness, normal scale, alpha
// cutoff and `metadata`. Models that are not resident are skipped.
inline void BuildDrawRecords(
    const std::span<const MeshInstance> instances,
    const f32 metadata,
    std::vector<DrawCall>& draws,
    std::vector<f32>& records)
{
    const auto push =
        [&records](const f32 a, const f32 b, const f32 c, const f32 d)
    {
        records.insert(records.end(), {a, b, c, d});
    };

    for (const MeshInstance& instance : instances)
    {
        if (instance.model == nullptr ||
            instance.model->VertexBuffer() == nullptr)
        {
            continue;
        }

        const auto& materials = instance.model->Materials();
        for (const auto& part : instance.model->Parts())
        {
            if (part.material >= materials.size())
            {
                continue;
            }
            const auto& material = materials[part.material];

            draws.push_back({
                instance.model, &part, static_cast<u32>(draws.size())});

            for (std::size_t row = 0U; row < 3U; ++row)
            {
                push(instance.rows[row * 4U],
                     instance.rows[row * 4U + 1U],
                     instance.rows[row * 4U + 2U],
                     instance.rows[row * 4U + 3U]);
            }
            push(material.baseColorFactor[0], material.baseColorFactor[1],
                 material.baseColorFactor[2], material.baseColorFactor[3]);
            push(material.emissiveFactor[0], material.emissiveFactor[1],
                 material.emissiveFactor[2], material.metallicFactor);
            push(material.roughnessFactor, material.normalScale,
                 material.alphaCutoff, metadata);
        }
    }
}
} // namespace orbit::mesh_render::detail
