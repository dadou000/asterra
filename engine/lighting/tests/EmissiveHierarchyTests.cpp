#include <orbit/lighting/EmissiveHierarchy.hpp>

#include <array>
#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    const std::array<math::Float3, 16> pixels{{
        {8.0F, 0.0F, 0.0F}, {8.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F},
        {8.0F, 0.0F, 0.0F}, {8.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 12.0F}, {0.0F, 0.0F, 12.0F},
        {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 12.0F}, {0.0F, 0.0F, 12.0F}
    }};

    const auto hierarchy =
        BuildEmissiveHierarchy({
            .width = 4U,
            .height = 4U,
            .texelAreaSquareMeters =
                0.01F,
            .giRadiance = pixels
        });

    if (hierarchy.root >=
            hierarchy.nodes.size() ||
        EmissiveHierarchyLeafCount(
            hierarchy) != 16U)
    {
        return 1;
    }

    const auto& root =
        hierarchy.nodes[
            hierarchy.root];

    if (root.integratedEnergy.x <= 0.0F ||
        root.integratedEnergy.z <= 0.0F ||
        root.childCount != 4U)
    {
        return 2;
    }

    const auto far =
        SelectEmissiveHierarchyNodes(
            hierarchy,
            8.0F,
            {
                .refineAboveProjectedPixels =
                    24.0F,
                .maximumNodes = 64U
            });

    if (far.size() != 1U ||
        far.front().nodeIndex !=
            hierarchy.root)
    {
        return 3;
    }

    const auto near =
        SelectEmissiveHierarchyNodes(
            hierarchy,
            1024.0F,
            {
                .refineAboveProjectedPixels =
                    24.0F,
                .maximumNodes = 64U
            });

    if (near.size() <= 1U ||
        near.size() > 16U)
    {
        return 4;
    }

    bool sawRed = false;
    bool sawBlue = false;

    for (const auto& selected : near)
    {
        if (selected.integratedEnergy.x >
            selected.integratedEnergy.z *
                2.0F)
        {
            sawRed = true;
        }

        if (selected.integratedEnergy.z >
            selected.integratedEnergy.x *
                2.0F)
        {
            sawBlue = true;
        }
    }

    if (!sawRed || !sawBlue)
    {
        return 5;
    }

    const auto capped =
        SelectEmissiveHierarchyNodes(
            hierarchy,
            4096.0F,
            {
                .refineAboveProjectedPixels =
                    1.0F,
                .maximumNodes = 4U
            });

    if (capped.empty() ||
        capped.size() > 4U)
    {
        return 6;
    }

    return 0;
}
