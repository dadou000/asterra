#include <orbit/terrain_render/SurfaceMaterial.hpp>

#include <stdexcept>

namespace orbit::terrain_render
{
f32 SurfaceMaterialRenderInput::TotalWeight() const noexcept
{
    f32 result = 0.0F;

    for (const f32 weight : weights)
    {
        result += weight;
    }

    return result;
}

SurfaceMaterialRenderInput MakeSurfaceMaterialRenderInput(
    const surface_model::ResolvedSurfaceMaterialBlend& blend)
{
    if (!blend.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M21 renderer requires a valid shared surface material blend.");
    }

    SurfaceMaterialRenderInput result;

    for (const auto& contribution :
         blend.contributions)
    {
        const auto index =
            static_cast<std::size_t>(
                contribution.kind);

        if (index >=
            result.weights.size())
        {
            throw std::logic_error(
                "Orbit M21 surface material kind exceeds renderer packing.");
        }

        result.weights[index] +=
            contribution.weight;

        if (contribution.kind ==
            surface_model::
                RenderedSurfaceMaterialKind::
                    Bedrock)
        {
            result.exposedBedrock =
                contribution.rock;
        }
    }

    return result;
}
} // namespace orbit::terrain_render
