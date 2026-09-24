#include <orbit/studio_ui/V007ValidationScenarios.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <exception>
#include <string>

namespace orbit::studio_ui
{
namespace
{
constexpr std::string_view kValidationSurface = "explorer";
constexpr editor_model::CommandSurfaceKind kValidationSurfaceKind =
    editor_model::CommandSurfaceKind::ContextMenu;
}

V007ValidationCommandRegistration::V007ValidationCommandRegistration(
    studio_session::StudioSession* const session,
    StudioViewportRenderer* const renderer) noexcept
    : session_(session),
      renderer_(renderer)
{
    if (session_ == nullptr ||
        renderer_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    auto& world = session_->World();
    auto& registry = world.CommandRegistry();
    auto& surfaces = world.CommandSurfaces();

    try
    {
        for (std::size_t index = 0U;
             index < kV007ValidationScenarios.size();
             ++index)
        {
            const auto& scenario =
                kV007ValidationScenarios[index];
            const auto id =
                V007ValidationCommandId(
                    scenario.id);

            // Project/session rebinds may rebuild the UI object while the same
            // world command registry survives. Never double-register IDs and
            // never claim ownership of a descriptor installed by another host.
            if (registry.Find(id) != nullptr)
            {
                continue;
            }

            const auto scenarioId = scenario.id;

            registry.Register({
                .id = id,
                .name =
                    "Validate: " +
                    std::string(scenario.name),
                .category =
                    "Validation / V0.0.7",
                .description =
                    std::string(scenario.invariant) +
                    " GPU image acceptance uses the separate visual-tolerance gate.",
                .parameters = {},
                .presentationSurfaces = {
                    "explorer.context"
                },
                .automationVisible = true,
                .enablement =
                    [session = session_]
                    {
                        if (session == nullptr ||
                            !session->World().HasWorld())
                        {
                            return commands::CommandEnablement{
                                .enabled = false,
                                .reason =
                                    "Open a world before preparing validation scenarios."
                            };
                        }
                        return commands::CommandEnablement{};
                    },
                .invoke =
                    [session = session_,
                     renderer = renderer_,
                     scenarioId](const commands::CommandArguments&)
                    {
                        const std::string result =
                            PrepareV007ValidationScenario(
                                scenarioId,
                                *session,
                                *renderer);
                        log::Info(result);
                    }
            });

            surfaces.Add(
                kValidationSurface,
                kValidationSurfaceKind,
                id);
            owned_[index] = true;
        }
    }
    catch (const std::exception& exception)
    {
        log::Warning(
            std::string(
                "M43 validation command registration failed: ") +
            exception.what());
    }
}

V007ValidationCommandRegistration::~V007ValidationCommandRegistration()
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    auto& world = session_->World();
    auto& registry = world.CommandRegistry();
    auto& surfaces = world.CommandSurfaces();

    for (std::size_t index = 0U;
         index < kV007ValidationScenarios.size();
         ++index)
    {
        if (!owned_[index])
        {
            continue;
        }

        const auto id =
            V007ValidationCommandId(
                kV007ValidationScenarios[index].id);

        static_cast<void>(
            surfaces.Remove(
                kValidationSurface,
                kValidationSurfaceKind,
                id));
        static_cast<void>(
            registry.Unregister(id));
    }
}
} // namespace orbit::studio_ui
