#pragma once

#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/studio_ui/InspectorProviderRegistry.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace orbit::studio_ui
{
// One contextual authoring lane is appended to the production Properties
// panel. Built-in tools and plugins contribute providers to the registry rather
// than creating more permanent side panels. The existing dedicated panels stay
// available as expert workspaces; this bridge only adds progressive disclosure
// to the normal selection workflow.
[[nodiscard]] inline InspectorProviderRegistry&
StudioInspectorProviders() noexcept
{
    static InspectorProviderRegistry registry;
    return registry;
}

inline void EnsureStudioInspectorExtension()
{
    editor_ui::UpsertPanelExtension({
        .id = "orbit.studio.properties.contextual-authoring",
        .targetTitle = "Properties",
        .order = 1'000,
        .draw =
            [](editor_ui::PanelContext& context)
            {
                static_cast<void>(
                    StudioInspectorProviders().
                        DrawRelevant(context));
            }
    });
}

// Owns one registry contribution for exactly as long as its authoring UI
// instance is alive. Studio constructs these instances after EditorUi and
// destroys them before EditorUi, so callbacks never outlive their owners.
class StudioInspectorProviderRegistration
{
public:
    explicit StudioInspectorProviderRegistration(
        InspectorProviderDefinition provider)
        : id_(provider.id)
    {
        EnsureStudioInspectorExtension();
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
