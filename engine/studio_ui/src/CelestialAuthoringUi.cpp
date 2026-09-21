#include <orbit/studio_ui/CelestialAuthoringUi.hpp>

#include <orbit/editor_model/CelestialRecipeService.hpp>
#include <orbit/world_model/AtmospherePropertySolver.hpp>
#include <orbit/world_model/PropertyProvenanceStore.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace orbit::studio_ui
{
CelestialAuthoringUi::CelestialAuthoringUi(
    studio_session::StudioSession& session)
    : session_(session)
{
}

void CelestialAuthoringUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Celestial",
        .defaultOpen = true,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 5,
        .minSize = {
            .width = 280.0F,
            .height = 240.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void CelestialAuthoringUi::Draw(
    editor_ui::PanelContext& context)
{
    auto& world = session_.World();

    if (!world.HasWorld())
    {
        context.Text(
            "Open a world to author celestial systems.");
        return;
    }

    editor_model::CelestialAuthoringModel model(
        world.Objects(),
        world.Schemas(),
        world.Commands(),
        world.Selection());

    context.Heading("Hierarchy");

    const auto selected =
        model.PrimarySelection();

    if (selected.has_value())
    {
        context.Text(
            std::format(
                "Selected: {}",
                selected->name));
    }
    else
    {
        context.MutedText(
            "Select a system/body/reference node in Explorer.");
    }

    static_cast<void>(
        context.InputText(
            "System Name##celestial-system-name",
            newSystemName_));

    if (context.Button(
            "+ System##celestial-create-system"))
    {
        try
        {
            static_cast<void>(
                model.CreateSystem(
                    newSystemName_));
            status_ =
                "Celestial system created.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    static_cast<void>(
        context.InputText(
            "Body Name##celestial-body-name",
            newBodyName_));

    if (context.Button(
            "+ Body##celestial-create-body"))
    {
        try
        {
            static_cast<void>(
                model.CreateBody(
                    newBodyName_));
            status_ =
                "Celestial body created.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    static_cast<void>(
        context.InputText(
            "Reference Name##celestial-reference-name",
            newReferenceName_));

    if (context.Button(
            "+ Reference / Barycenter##celestial-create-reference"))
    {
        try
        {
            static_cast<void>(
                model.CreateReferenceNode(
                    newReferenceName_));
            status_ =
                "Reference/barycenter node created.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    context.Separator();
    context.Heading("Capabilities");

    const auto body =
        model.SelectedBody();

    if (!body.has_value())
    {
        context.MutedText(
            "Select a body or one of its capability children.");
    }
    else
    {
        context.Text(
            std::format(
                "Target: {}",
                body->name));

        for (const auto& capability :
             model.AvailableCapabilities())
        {
            const std::string label =
                "+ " +
                std::string(
                    capability.label) +
                "##celestial-capability-" +
                capability.type.ToString();

            if (context.Button(label))
            {
                try
                {
                    static_cast<void>(
                        model.AddCapability(
                            capability.type,
                            capability.label));
                    status_ =
                        std::string(
                            capability.label) +
                        " capability added.";
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                }
            }
        }
    }

    if (selected.has_value() &&
        model.IsCapabilityType(
            selected->type))
    {
        context.Separator();

        if (context.Button(
                "Remove Selected Capability##celestial-remove-capability"))
        {
            try
            {
                model.RemoveSelectedCapability();
                status_ =
                    "Capability removed.";
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }
    }

    std::optional<scene::ObjectRecord>
        atmosphereCapability;

    if (selected.has_value() &&
        selected->type ==
            world_model::
                kAtmosphereCapabilityType)
    {
        atmosphereCapability =
            selected;
    }
    else if (body.has_value())
    {
        for (const auto& child :
             world.Objects().
                 Children(body->id))
        {
            if (child.type ==
                world_model::
                    kAtmosphereCapabilityType)
            {
                atmosphereCapability =
                    child;
                break;
            }
        }
    }

    if (atmosphereCapability.has_value())
    {
        context.Separator();
        context.Heading("Atmosphere Solver");
        context.MutedText(
            "Presets and derived controls write the same Atmosphere capability inspected in Properties. Enable Advanced Properties for exact coefficients and provenance metadata.");

        world_model::AtmospherePropertySolver
            atmosphereSolver(
                world.Objects(),
                world.Commands());

        const auto presets =
            world_model::
                AtmospherePropertySolver::
                    Presets();
        const std::array provenanceTargets{
            world_model::kAtmosphereTopRadiusMeters,
            world_model::kAtmosphereRayleighScatteringPerMeter,
            world_model::kAtmosphereRayleighScaleHeightMeters,
            world_model::kAtmosphereMieScatteringPerMeter,
            world_model::kAtmosphereMieExtinctionPerMeter,
            world_model::kAtmosphereMieScaleHeightMeters,
            world_model::kAtmosphereMieAnisotropy,
            world_model::kAtmosphereAbsorptionExtinctionPerMeter,
            world_model::kAtmosphereAbsorptionCenterHeightMeters,
            world_model::kAtmosphereAbsorptionHalfWidthMeters
        };

        u32 derivedAuthority = 0U;
        u32 explicitAuthority = 0U;
        u32 importedAuthority = 0U;

        for (const auto property :
             provenanceTargets)
        {
            const auto provenance =
                world_model::
                    EffectivePropertyProvenance(
                        world.Objects(),
                        atmosphereCapability->id,
                        property);

            derivedAuthority +=
                provenance.sourceMode ==
                        world_model::
                            PropertySourceMode::
                                Derived
                    ? 1U
                    : 0U;
            explicitAuthority +=
                provenance.sourceMode ==
                        world_model::
                            PropertySourceMode::
                                Explicit
                    ? 1U
                    : 0U;
            importedAuthority +=
                provenance.sourceMode ==
                        world_model::
                            PropertySourceMode::
                                Imported
                    ? 1U
                    : 0U;
        }

        context.MutedText(
            std::format(
                "Derived authority: {} | Explicit: {} | Imported: {}",
                derivedAuthority,
                explicitAuthority,
                importedAuthority));


        for (std::size_t index = 0;
             index < presets.size();
             ++index)
        {
            const std::string label =
                std::string(presets[index]) +
                "##atmosphere-preset-" +
                std::to_string(index);

            if (context.Button(label))
            {
                try
                {
                    const auto report =
                        atmosphereSolver.
                            ApplyPreset(
                                atmosphereCapability->
                                    id,
                                presets[index]);

                    status_ =
                        std::format(
                            "Applied atmosphere preset '{}': {} derived field{}, {} conflict{}.",
                            presets[index],
                            report.DerivedCount(),
                            report.DerivedCount() == 1U
                                ? ""
                                : "s",
                            report.HasConflict()
                                ? 1
                                : 0,
                            report.HasConflict()
                                ? ""
                                : "s");
                }
                catch (const std::exception& exception)
                {
                    status_ =
                        exception.what();
                }
            }

            if (index + 1U <
                presets.size())
            {
                context.SameLine();
            }
        }

        if (context.Button(
                "Derived Composition##atmosphere-mode-derived"))
        {
            try
            {
                world.Commands().
                    SetProperty(
                        atmosphereCapability->id,
                        world_model::
                            kAtmosphereAuthoringMode,
                        std::string{
                            "Derived Composition"});
                status_ =
                    "Atmosphere mode: Derived Composition.";
            }
            catch (const std::exception& exception)
            {
                status_ =
                    exception.what();
            }
        }

        context.SameLine();

        if (context.Button(
                "Expert Coefficients##atmosphere-mode-expert"))
        {
            try
            {
                world.Commands().
                    SetProperty(
                        atmosphereCapability->id,
                        world_model::
                            kAtmosphereAuthoringMode,
                        std::string{
                            "Expert Coefficients"});
                status_ =
                    "Atmosphere mode: Expert Coefficients. Exact runtime fields remain under direct author authority.";
            }
            catch (const std::exception& exception)
            {
                status_ =
                    exception.what();
            }
        }

        if (context.PrimaryButton(
                "Solve Derived Coefficients##atmosphere-solve"))
        {
            try
            {
                const auto report =
                    atmosphereSolver.Solve(
                        atmosphereCapability->id);

                u32 conflicts = 0U;
                u32 invalid = 0U;

                for (const auto& event :
                     report.events)
                {
                    conflicts +=
                        event.outcome ==
                                world_model::
                                    AtmosphereSolveOutcome::
                                        Conflict
                            ? 1U
                            : 0U;
                    invalid +=
                        event.outcome ==
                                world_model::
                                    AtmosphereSolveOutcome::
                                        InvalidInput
                            ? 1U
                            : 0U;
                }

                status_ =
                    std::format(
                        "Atmosphere solve: {} derived, {} conflict{}, {} invalid input{}.",
                        report.DerivedCount(),
                        conflicts,
                        conflicts == 1U
                            ? ""
                            : "s",
                        invalid,
                        invalid == 1U
                            ? ""
                            : "s");
            }
            catch (const std::exception& exception)
            {
                status_ =
                    exception.what();
            }
        }
    }

    context.Separator();
    context.Heading("Recipes");
    context.MutedText(
        "Recipes create ordinary editable objects and capabilities. The seed controls authored physical/orbital values; generation is one undoable transaction.");

    static_cast<void>(
        context.InputInteger(
            "Seed##celestial-recipe-seed",
            recipeSeed_));

    static_cast<void>(
        context.InputInteger(
            "Rocky Planets##celestial-recipe-planets",
            recipePlanetCount_));

    static_cast<void>(
        context.Checkbox(
            "Generate Moons##celestial-recipe-moons",
            recipeGenerateMoons_));

    static_cast<void>(
        context.InputText(
            "Generated System Name##celestial-recipe-system-name",
            recipeSystemName_));

    static_cast<void>(
        context.InputText(
            "Primary Star Name##celestial-recipe-star-name",
            recipeStarName_));

    if (context.PrimaryButton(
            "Generate Seeded System##celestial-recipe-generate"))
    {
        try
        {
            const auto roots =
                world.Objects().Roots();

            const auto worldRoot =
                std::find_if(
                    roots.begin(),
                    roots.end(),
                    [](const auto& object)
                    {
                        return object.type ==
                            world_model::kWorldType;
                    });

            if (worldRoot == roots.end())
            {
                throw std::runtime_error(
                    "World root is required before running a celestial recipe.");
            }

            if (recipeSeed_ < 0)
            {
                throw std::invalid_argument(
                    "Recipe seed must be non-negative.");
            }

            if (recipePlanetCount_ <= 0 ||
                recipePlanetCount_ >
                    static_cast<i64>(
                        std::numeric_limits<u32>::max()))
            {
                throw std::invalid_argument(
                    "Rocky planet count must be between 1 and uint32 max.");
            }

            editor_model::CelestialRecipeService
                recipes(
                    world.Objects(),
                    world.Commands());

            const auto generated =
                recipes.CreateSeededSystem(
                    worldRoot->id,
                    editor_model::
                        SeededSystemRecipe{
                            .seed =
                                static_cast<u64>(
                                    recipeSeed_),
                            .systemName =
                                recipeSystemName_,
                            .starName =
                                recipeStarName_,
                            .rockyPlanetCount =
                                static_cast<u32>(
                                    recipePlanetCount_),
                            .generateMoons =
                                recipeGenerateMoons_
                        });

            const std::array selectedSystem{
                generated.system
            };

            world.Selection().Set(
                std::span(
                    selectedSystem));

            static_cast<void>(
                world.RebuildUniverse());

            status_ =
                std::format(
                    "Generated '{}' from seed {}: 1 star, {} rocky planet{}, {} moon{}.",
                    recipeSystemName_,
                    recipeSeed_,
                    generated.planets.size(),
                    generated.planets.size() == 1U
                        ? ""
                        : "s",
                    generated.moons.size(),
                    generated.moons.size() == 1U
                        ? ""
                        : "s");
        }
        catch (const std::exception& exception)
        {
            status_ =
                exception.what();
        }
    }

    context.Separator();
    context.Heading("Validation");

    if (context.Button(
            "Validate Celestial Model##celestial-validate"))
    {
        diagnostics_ =
            model.Validate();

        try
        {
            static_cast<void>(
                world.RebuildUniverse());

            if (diagnostics_.size() == 1U &&
                diagnostics_.front().severity ==
                    editor_model::
                        CelestialDiagnosticSeverity::
                            Info)
            {
                status_ =
                    "Semantic and runtime celestial validation passed.";
            }
            else
            {
                status_ =
                    "Semantic validation completed with diagnostics.";
            }
        }
        catch (const std::exception& exception)
        {
            diagnostics_.push_back({
                .severity =
                    editor_model::
                        CelestialDiagnosticSeverity::
                            Error,
                .message =
                    std::string(
                        "Runtime composition: ") +
                    exception.what(),
                .object =
                    std::nullopt
            });

            status_ =
                "Runtime celestial validation failed.";
        }
    }

    if (diagnostics_.empty())
    {
        context.MutedText(
            "Run validation to inspect hierarchy and runtime composition.");
    }
    else
    {
        for (const auto& diagnostic :
             diagnostics_)
        {
            const char* prefix = "[info]";

            if (diagnostic.severity ==
                editor_model::
                    CelestialDiagnosticSeverity::
                        Warning)
            {
                prefix = "[warning]";
            }
            else if (
                diagnostic.severity ==
                editor_model::
                    CelestialDiagnosticSeverity::
                        Error)
            {
                prefix = "[error]";
            }

            context.Text(
                std::format(
                    "{} {}{}",
                    prefix,
                    diagnostic.message,
                    diagnostic.object.
                            has_value()
                        ? " (" +
                              diagnostic.object->
                                  ToString() +
                              ")"
                        : ""));
        }
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}
} // namespace orbit::studio_ui
