#pragma once

#include <orbit/lighting/Visibility.hpp>
#include <orbit/time/SimulationTime.hpp>

namespace orbit::lighting
{
class AnalyticBodyVisibilityProvider final
    : public VisibilityProvider
{
public:
    AnalyticBodyVisibilityProvider(
        const universe::BodyRegistry& bodies,
        const frames::FrameGraph& frames,
        time::SimulationTime atTime = {});

    void SetTime(
        time::SimulationTime atTime) noexcept;

    [[nodiscard]] const VisibilityProviderDesc&
    Description() const noexcept override;

    [[nodiscard]] bool SupportsPurpose(
        VisibilityPurpose purpose) const noexcept override;

    [[nodiscard]] VisibilityResult Trace(
        const VisibilityQuery& query) override;

private:
    const universe::BodyRegistry* bodies_{nullptr};
    const frames::FrameGraph* frames_{nullptr};
    time::SimulationTime atTime_{};
    VisibilityProviderDesc desc_{};
};
} // namespace orbit::lighting
