#pragma once

#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace orbit::studio_ui
{
// Built-in authoring tools and hot-reloadable plugins share one registry and
// one Properties-panel extension owned by StudioExpansionShell. This prevents
// two visually identical contextual-inspector pipelines from drifting apart.
[[nodiscard]] inline InspectorProviderRegistry&
StudioInspectorProviders() noexcept
{
    return GlobalInspectorProviders();
}

// Owns one registry contribution for exactly as long as its authoring UI
// instance is alive. Headless/model workflows may populate the same registry
// without an EditorUi host; StudioExpansionShell attaches it to Properties only
// when the live unified Studio shell exists.
class StudioInspectorProviderRegistration
{
public:
    explicit StudioInspectorProviderRegistration(
        InspectorProviderDefinition provider)
        : id_(provider.id)
    {
        StudioInspectorProviders().Upsert(
            std::move(provider));
    }

    ~StudioInspectorProviderRegistration()
    {
        if (!id_.empty())
        {
            static_cast<void>(
                StudioInspectorProviders().Remove(id_));
        }
    }

    StudioInspectorProviderRegistration(
        const StudioInspectorProviderRegistration&) = delete;
    StudioInspectorProviderRegistration& operator=(
        const StudioInspectorProviderRegistration&) = delete;
    StudioInspectorProviderRegistration(
        StudioInspectorProviderRegistration&&) = delete;
    StudioInspectorProviderRegistration& operator=(
        StudioInspectorProviderRegistration&&) = delete;

    [[nodiscard]] std::string_view Id() const noexcept
    {
        return id_;
    }

private:
    std::string id_;
};
} // namespace orbit::studio_ui
