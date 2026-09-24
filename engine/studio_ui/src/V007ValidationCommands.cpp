#include <orbit/studio_ui/V007ValidationScenarios.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <string>

namespace orbit::studio_ui
{
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

    auto& registry =
        session_->World().CommandRegistry();

    try
    {
        for (const auto& scenario :
             kV007ValidationScenarios)
        {
            const auto id =
                V007ValidationCommandId(
                    scenario.id);

            // Project/session rebinds may rebuild the UI object while the same
            // world command registry survives. Never double-register IDs.
            if (registry.Find(id) != nullptr)
            {
                continue;
            }

            const auto scenarioId =
                scenario.id;

            registry.Register({
                .id = id,
                .name =
                    "V0.0.7 Validate - " +
                    std::string(scenario.name),
                .category =
                    "Validation / V0.0.7",
                .description =
                    std::string(scenario.invariant) +
                    " GPU image acceptance uses the separate visual-tolerance gate.",
                .parameters = {},
                .presentationSurfaces = {},
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
        }

        registered_ = true;
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
    if (!registered_ ||
        session_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    auto& registry =
        session_->World().CommandRegistry();

    for (const auto& scenario :
         kV007ValidationScenarios)
    {
        static_cast<void>(
            registry.Unregister(
                V007ValidationCommandId(
                    scenario.id)));
    }
}
} // namespace orbit::studio_ui
