#include <orbit/lighting/CityLightTree.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
namespace
{
// Share of the sphere a spot's outer cone leaves dark, as a cosine-lobe directionality in [0, 1].
double Directionality(const LocalLight& light)
{
    if (light.type != LocalLightType::Spot) return 0.0;
    const double outer = std::clamp(static_cast<double>(light.outerConeRadians), 0.0, 3.1241);
    return 1.0 - 0.5 * (1.0 - std::cos(outer));
}
} // namespace

void CityLightTree::Build(std::span<const LocalLight> lights, std::size_t leafSize)
{
    source_.assign(lights.begin(), lights.end());
    totalFlux_ = 0.0;
    std::vector<optical_transport::Emitter> emitters;
    emitters.reserve(source_.size());
    for (std::size_t i = 0; i < source_.size(); ++i)
    {
        const LocalLight& l = source_[i];
        optical_transport::Emitter e;
        e.id = static_cast<std::uint32_t>(i);
        e.position[0] = l.positionInFrameMeters.x; e.position[1] = l.positionInFrameMeters.y; e.position[2] = l.positionInFrameMeters.z;
        e.power = std::max(static_cast<double>(l.luminousFluxLumens), 0.0);
        e.axis[0] = l.direction.x; e.axis[1] = l.direction.y; e.axis[2] = l.direction.z;
        e.directionality = Directionality(l);
        emitters.push_back(e);
        totalFlux_ += e.power;
    }
    tree_.Build(std::move(emitters), leafSize);

    const auto& nodes = tree_.Nodes();
    nodeColour_.assign(nodes.size(), math::Float3{1.0F, 1.0F, 1.0F});
    nodeReach_.assign(nodes.size(), 0.0F);
    for (std::size_t n = 0; n < nodes.size(); ++n)
    {
        const auto& node = nodes[n];
        double r = 0.0, g = 0.0, b = 0.0, weight = 0.0, reach = 0.0;
        for (std::uint32_t k = node.first; k < node.first + node.count; ++k)
        {
            const auto& e = tree_.EmitterAt(k);
            const LocalLight& l = source_[e.id];
            const double w = e.power;
            r += w * l.colorLinear.x; g += w * l.colorLinear.y; b += w * l.colorLinear.z;
            weight += w;
            const double dx = e.position[0] - node.center[0], dy = e.position[1] - node.center[1], dz = e.position[2] - node.center[2];
            reach = std::max(reach, std::sqrt(dx * dx + dy * dy + dz * dz) + static_cast<double>(l.rangeMeters));
        }
        if (weight > 0.0) nodeColour_[n] = {static_cast<float>(r / weight), static_cast<float>(g / weight), static_cast<float>(b / weight)};
        nodeReach_[n] = static_cast<float>(reach);
    }
}

CityLightTree::CutResult CityLightTree::Cut(const math::Double3& receiver, double maxAngleRadians, std::size_t maxLights) const
{
    CutResult out;
    const double v[3] = {receiver.x, receiver.y, receiver.z};
    // Keep a quarter of the budget in reserve: the tree refines by angle, which can use every node, and the leaves nearest
    // the receiver (the ones that matter most) are only expanded to their original lights if there is room left.
    const std::size_t reserve = maxLights / 4;
    const auto selected = tree_.SelectCut(v, maxAngleRadians, std::max<std::size_t>(1, maxLights - reserve));
    const auto& nodes = tree_.Nodes();
    // Selected leaves that still subtend more than the threshold hold a few lights each: expand them to the original
    // lights, nearest first, while the budget allows (the cut is then exact where the viewer is closest).
    std::vector<std::uint32_t> expandable;
    std::size_t outputSize = selected.size();
    for (const std::uint32_t index : selected)
        if (nodes[index].left < 0 && nodes[index].count > 1 && tree_.AngleAt(index, v) > maxAngleRadians) expandable.push_back(index);
    std::sort(expandable.begin(), expandable.end(),
              [&](std::uint32_t a, std::uint32_t b) { return tree_.AngleAt(a, v) > tree_.AngleAt(b, v); });
    std::vector<unsigned char> expanded(nodes.size(), 0);
    for (const std::uint32_t index : expandable)
    {
        const std::size_t extra = nodes[index].count - 1;
        if (outputSize + extra > maxLights) continue;
        expanded[index] = 1;
        outputSize += extra;
    }
    out.lights.reserve(outputSize);
    for (const std::uint32_t index : selected)
    {
        const auto& node = nodes[index];
        if (expanded[index])
        {
            for (std::uint32_t k = node.first; k < node.first + node.count; ++k)
            {
                out.lights.push_back(source_[tree_.EmitterAt(k).id]);
                out.totalFlux += out.lights.back().luminousFluxLumens;
            }
            continue;
        }
        if (node.count == 1)
        {
            out.lights.push_back(source_[tree_.EmitterAt(node.first).id]);   // an original light, untouched
            out.totalFlux += out.lights.back().luminousFluxLumens;
            continue;
        }
        LocalLight merged;
        merged.positionInFrameMeters = {node.center[0], node.center[1], node.center[2]};
        merged.colorLinear = nodeColour_[index];
        merged.luminousFluxLumens = static_cast<float>(node.power);
        merged.rangeMeters = nodeReach_[index];
        merged.stableId = kProxyLightIdBit | index;
        if (node.directionality > 0.5)
        {
            merged.type = LocalLightType::Spot;
            merged.direction = {static_cast<float>(node.axis[0]), static_cast<float>(node.axis[1]), static_cast<float>(node.axis[2])};
            // Choose the cone whose dark fraction reproduces the group's directionality D = 1 - (1 - cos(outer)) / 2, so a
            // district of identical lamps merges to a spot with the lamps' own cone instead of a different lobe.
            const double outer = std::acos(std::clamp(2.0 * node.directionality - 1.0, -1.0, 1.0));
            merged.outerConeRadians = static_cast<float>(outer);
            merged.innerConeRadians = static_cast<float>(0.5 * outer);
        }
        ++out.mergedNodes;
        out.totalFlux += node.power;
        out.lights.push_back(merged);
    }
    return out;
}
} // namespace orbit::lighting
