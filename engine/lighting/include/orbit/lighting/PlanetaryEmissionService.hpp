#pragma once

#include <orbit/lighting/PlanetaryEmissionField.hpp>
#include <orbit/lighting/RuntimeEmissiveSurface.hpp>

#include <map>
#include <optional>
#include <vector>

namespace orbit::lighting
{
struct PlanetaryEmissionServiceStats
{
    u32 bodyCount{0U};
    u32 sourceCount{0U};
    u64 rebuildCount{0U};
};

class PlanetaryEmissionService
{
public:
    explicit PlanetaryEmissionService(
        PlanetaryEmissionFieldConfig config = {});

    // Adds or replaces one canonical emissive surface authority. Equal
    // stableId/body/frame + equal content revision is a no-op.
    [[nodiscard]] bool Upsert(
        RuntimeEmissiveSurface surface);

    [[nodiscard]] bool Remove(
        universe::BodyId body,
        u64 stableId);

    void Clear() noexcept;

    [[nodiscard]] const PlanetaryEmissionField*
    Resolve(
        frames::FrameId frame,
        universe::BodyId body,
        const math::Double3& bodyCenterInFrameMeters);

    [[nodiscard]] PlanetaryEmissionServiceStats
    Stats() const noexcept;

private:
    struct Source
    {
        RuntimeEmissiveSurface surface;
    };

    struct BodyState
    {
        frames::FrameId frame{};
        universe::BodyId body{};
        std::map<u64, Source> sources;
        u64 sourceRevision{0U};
        bool dirty{true};
        std::optional<PlanetaryEmissionField> field;
        math::Double3 lastBodyCenter{};
        bool hasCenter{false};
    };

    [[nodiscard]] static u64 MixRevision(
        u64 seed,
        u64 value) noexcept;

    [[nodiscard]] u64 ComputeBodyRevision(
        const BodyState& state) const noexcept;

    PlanetaryEmissionFieldConfig config_{};
    std::map<
        universe::BodyId,
        BodyState>
        bodies_;
    u64 rebuildCount_{0U};
};
} // namespace orbit::lighting
