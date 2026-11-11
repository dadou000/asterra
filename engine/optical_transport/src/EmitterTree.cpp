#include "orbit/optical_transport/EmitterTree.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <queue>

namespace orbit::optical_transport
{

namespace
{
double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double Normalize(double v[3])
{
    const double l = std::sqrt(Dot(v, v));
    if (l > 0.0) { v[0] /= l; v[1] /= l; v[2] /= l; }
    return l;
}
// Intensity (W/sr) toward unit direction `toReceiver` from a source of `power` with a cosine lobe of weight k.
double Intensity(double power, const double axis[3], double k, const double toReceiver[3])
{
    const double cosine = std::max(0.0, Dot(axis, toReceiver));
    // Normalised: (1-k)/(4 pi) + k * (cos / pi) so total power is preserved for any mix.
    return power * ((1.0 - k) / (4.0 * std::numbers::pi) + k * cosine / std::numbers::pi);
}
} // namespace

void EmitterTree::Build(std::vector<Emitter> emitters, std::size_t leafSize)
{
    emitters_ = std::move(emitters);
    nodes_.clear();
    leafSize_ = std::max<std::size_t>(leafSize, 1);
    if (!emitters_.empty())
    {
        BuildRange(0, emitters_.size());
    }
}

std::int32_t EmitterTree::BuildRange(std::size_t first, std::size_t count)
{
    Node node;
    node.first = static_cast<std::uint32_t>(first);
    node.count = static_cast<std::uint32_t>(count);
    double axis[3] = {0, 0, 0};
    double dirWeighted = 0.0;
    for (int k = 0; k < 3; ++k) { node.boundsMin[k] = 1.0e300; node.boundsMax[k] = -1.0e300; }
    for (std::size_t i = first; i < first + count; ++i)
    {
        const Emitter& e = emitters_[i];
        node.power += e.power;
        for (int k = 0; k < 3; ++k)
        {
            node.center[k] += e.power * e.position[k];
            axis[k] += e.power * e.directionality * e.axis[k];
            node.boundsMin[k] = std::min(node.boundsMin[k], e.position[k]);
            node.boundsMax[k] = std::max(node.boundsMax[k], e.position[k]);
        }
        dirWeighted += e.power * e.directionality;
    }
    if (node.power > 0.0)
    {
        for (int k = 0; k < 3; ++k) node.center[k] /= node.power;
    }
    // Coherence: |sum(p d a)| / sum(p): a mix of opposing lobes tends to isotropic.
    const double magnitude = Normalize(axis);
    if (magnitude > 0.0) { for (int k = 0; k < 3; ++k) node.axis[k] = axis[k]; }
    node.directionality = node.power > 0.0 ? magnitude / node.power : 0.0;
    (void)dirWeighted;

    const std::int32_t index = static_cast<std::int32_t>(nodes_.size());
    nodes_.push_back(node);
    if (count > leafSize_)
    {
        int split = 0;
        double extent = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            const double e = nodes_[index].boundsMax[k] - nodes_[index].boundsMin[k];
            if (e > extent) { extent = e; split = k; }
        }
        std::sort(emitters_.begin() + static_cast<std::ptrdiff_t>(first), emitters_.begin() + static_cast<std::ptrdiff_t>(first + count),
                  [split](const Emitter& a, const Emitter& b) { return a.position[split] < b.position[split]; });
        const std::size_t half = count / 2;
        const std::int32_t l = BuildRange(first, half);
        const std::int32_t r = BuildRange(first + half, count - half);
        nodes_[index].left = l;
        nodes_[index].right = r;
    }
    return index;
}

double EmitterTree::EmitterIrradiance(const Emitter& e, const double receiver[3], const double normal[3]) const
{
    double d[3] = {receiver[0] - e.position[0], receiver[1] - e.position[1], receiver[2] - e.position[2]};
    const double dist = Normalize(d);
    if (dist < 1.0e-6) return 0.0;
    const double toSource[3] = {-d[0], -d[1], -d[2]};
    const double cosReceiver = std::max(0.0, Dot(normal, toSource));
    return Intensity(e.power, e.axis, e.directionality, d) * cosReceiver / (dist * dist);
}

double EmitterTree::NodeIrradiance(const Node& node, const double receiver[3], const double normal[3]) const
{
    Emitter proxy;
    for (int k = 0; k < 3; ++k) { proxy.position[k] = node.center[k]; proxy.axis[k] = node.axis[k]; }
    proxy.power = node.power;
    proxy.directionality = node.directionality;
    return EmitterIrradiance(proxy, receiver, normal);
}

double EmitterTree::ErrorBound(const Node& node, const double receiver[3], const double normal[3]) const
{
    // Conservative: worst-case irradiance of the whole node power at the closest point of its bounds, minus the
    // best case at the farthest, with an isotropic-lobe upper bound (directional lobes peak at 1/pi vs 1/(4 pi)).
    double nearSq = 0.0, farSq = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        const double lo = node.boundsMin[k] - receiver[k];
        const double hi = node.boundsMax[k] - receiver[k];
        const double nearest = (lo > 0.0) ? lo : (hi < 0.0 ? -hi : 0.0);
        const double farthest = std::max(std::abs(lo), std::abs(hi));
        nearSq += nearest * nearest;
        farSq += farthest * farthest;
    }
    const double peakIntensity = node.power / std::numbers::pi;  // cosine lobe maximum bounds every mix
    const double upper = peakIntensity / std::max(nearSq, 1.0e-6);
    (void)normal;
    // The proxy differs from the members by the variation of 1/d^2 across the node plus the angular variation of
    // the lobe and receiver cosine, which is first order in (node diameter / distance). Lipschitz-style bound.
    double diameterSq = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        const double e = node.boundsMax[k] - node.boundsMin[k];
        diameterSq += e * e;
    }
    const double distance = std::sqrt(std::max(nearSq, 1.0e-6));
    const double distanceVariation = farSq > 0.0 ? 1.0 - nearSq / farSq : 1.0;
    const double angular = 2.0 * std::sqrt(diameterSq) / distance;
    return node.count <= 1 ? 0.0 : upper * std::min(1.0, distanceVariation + angular);
}

EmitterTree::Estimate EmitterTree::Evaluate(const double receiver[3], const double normal[3], double relativeTolerance) const
{
    Estimate out;
    if (nodes_.empty()) return out;
    out.powerAccounted = nodes_[0].power;
    // Greedy cut: refine the node with the largest error bound until the summed bound is under tolerance * estimate.
    struct Item { std::int32_t node; double bound; };
    std::vector<Item> frontier{{0, ErrorBound(nodes_[0], receiver, normal)}};
    auto total = [&] { double s = 0.0; for (const Item& i : frontier) s += NodeIrradiance(nodes_[static_cast<std::size_t>(i.node)], receiver, normal); return s; };
    for (;;)
    {
        double bound = 0.0;
        std::size_t worst = 0;
        for (std::size_t i = 0; i < frontier.size(); ++i)
        {
            bound += frontier[i].bound;
            if (frontier[i].bound > frontier[worst].bound) worst = i;
        }
        if (bound <= relativeTolerance * std::max(total(), 1.0e-30) || frontier[worst].bound <= 0.0) break;
        const Node& n = nodes_[static_cast<std::size_t>(frontier[worst].node)];
        if (n.left < 0)
        {
            // Leaf: expand to its individual emitters (bound zero).
            const std::int32_t node = frontier[worst].node;
            frontier.erase(frontier.begin() + static_cast<std::ptrdiff_t>(worst));
            for (std::uint32_t i = n.first; i < n.first + n.count; ++i)
            {
                out.irradiance += EmitterIrradiance(emitters_[i], receiver, normal);
                ++out.sourcesEvaluated;
            }
            (void)node;
            continue;
        }
        const std::int32_t l = n.left, r = n.right;
        frontier.erase(frontier.begin() + static_cast<std::ptrdiff_t>(worst));
        frontier.push_back({l, ErrorBound(nodes_[static_cast<std::size_t>(l)], receiver, normal)});
        frontier.push_back({r, ErrorBound(nodes_[static_cast<std::size_t>(r)], receiver, normal)});
    }
    for (const Item& i : frontier)
    {
        out.irradiance += NodeIrradiance(nodes_[static_cast<std::size_t>(i.node)], receiver, normal);
        out.errorBound += i.bound;
        ++out.sourcesEvaluated;
    }
    return out;
}

double EmitterTree::AngleAt(std::uint32_t index, const double viewer[3]) const
{
    const Node& n = nodes_[index];
    double extent = 0.0, nearSq = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        const double e = n.boundsMax[k] - n.boundsMin[k];
        extent += e * e;
        const double lo = n.boundsMin[k] - viewer[k], hi = n.boundsMax[k] - viewer[k];
        const double nearest = (lo > 0.0) ? lo : (hi < 0.0 ? -hi : 0.0);
        nearSq += nearest * nearest;
    }
    return nearSq <= 0.0 ? 3.14159265 : std::sqrt(extent) / std::sqrt(nearSq);
}

std::vector<std::uint32_t> EmitterTree::SelectCut(const double viewer[3], double maxAngleRadians, std::size_t maxNodes) const
{
    std::vector<std::uint32_t> cut;
    if (nodes_.empty() || maxNodes == 0) return cut;
    const auto angle = [&](std::uint32_t index) { return AngleAt(index, viewer); };
    struct Item { double angle; std::uint32_t node; };
    const auto lessAngle = [](const Item& a, const Item& b) { return a.angle < b.angle; };
    std::priority_queue<Item, std::vector<Item>, decltype(lessAngle)> open(lessAngle);
    open.push({angle(0), 0});
    std::vector<std::uint32_t> done;
    while (!open.empty())
    {
        const Item top = open.top();
        const Node& n = nodes_[top.node];
        const bool canSplit = n.left >= 0 && top.angle > maxAngleRadians && done.size() + open.size() + 1 <= maxNodes;
        open.pop();
        if (!canSplit)
        {
            done.push_back(top.node);
            // Everything still open has a smaller angle; if none can split further they all finish here too.
            if (top.angle <= maxAngleRadians || done.size() + open.size() + 1 > maxNodes)
            {
                while (!open.empty()) { done.push_back(open.top().node); open.pop(); }
            }
            continue;
        }
        open.push({angle(static_cast<std::uint32_t>(n.left)), static_cast<std::uint32_t>(n.left)});
        open.push({angle(static_cast<std::uint32_t>(n.right)), static_cast<std::uint32_t>(n.right)});
    }
    return done;
}

double EmitterTree::ExactIrradiance(const double receiver[3], const double normal[3]) const
{
    double sum = 0.0;
    for (const Emitter& e : emitters_) sum += EmitterIrradiance(e, receiver, normal);
    return sum;
}

} // namespace orbit::optical_transport
