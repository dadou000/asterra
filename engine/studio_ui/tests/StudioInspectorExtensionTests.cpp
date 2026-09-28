#include <orbit/studio_ui/StudioInspectorExtension.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Studio contextual inspector gate failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

int main()
{
    using namespace orbit::studio_ui;

    auto& registry = StudioInspectorProviders();
    registry.Clear();

    // The built-in helper must be the same authority exposed to plugin-facing
    // StudioExpansionShell contributions. This is the architectural invariant
    // that keeps Properties on one contextual-extension pipeline.
    Check(&registry == &GlobalInspectorProviders());

    const auto revisionBefore = registry.Revision();

    {
        StudioInspectorProviderRegistration registration({
            .id = "orbit.test.contextual-authoring",
            .owner = "orbit.test",
            .title = "Contextual Test Tools",
            .order = 75,
            .defaultOpen = true,
            .relevant = []() { return true; },
            .draw = [](orbit::editor_ui::PanelContext&) {}
        });

        Check(registration.Id() ==
              std::string_view{"orbit.test.contextual-authoring"});
        Check(registry.Revision() > revisionBefore);

        const auto relevant = registry.Relevant();
        Check(relevant.size() == 1U);
        Check(relevant.front() != nullptr);
        Check(relevant.front()->id ==
              "orbit.test.contextual-authoring");
        Check(relevant.front()->title ==
              "Contextual Test Tools");
    }

    // RAII lifetime is what keeps plugin reloads and production authoring UI
    // destruction from leaving callbacks that target dead objects. This must
    // remain safe even when the test has no live EditorUi/ImGui host.
    Check(registry.Relevant().empty());

    registry.Clear();
    return 0;
}
