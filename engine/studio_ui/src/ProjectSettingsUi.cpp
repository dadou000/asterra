#include <orbit/studio_ui/ProjectSettingsUi.hpp>

#include <filesystem>
#include <format>
#include <stdexcept>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::string WorldLabel(
    const editor_session::WorldDocumentItem& item)
{
    std::string label =
        item.descriptor.displayName.empty()
            ? item.descriptor.relativePath.stem().string()
            : item.descriptor.displayName;

    if (item.descriptor.startup)
    {
        label += " [Startup]";
    }

    if (!item.valid)
    {
        label += " [Invalid]";
    }

    label += "##project-settings-world:";
    label += item.descriptor.relativePath.generic_string();
    return label;
}
} // namespace

ProjectSettingsUi::ProjectSettingsUi(
    documents::ProjectDocument& project,
    studio_session::StudioSession& session)
    : project_(&project),
      session_(&session)
{
    SynchronizeAuthority();
}

void ProjectSettingsUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Project Settings",
        .defaultOpen = false,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}


studio_session::StudioTerrainRoundTripReport
ProjectSettingsUi::RunTerrainRoundTripValidation()
{
    if (project_ == nullptr ||
        session_ == nullptr)
    {
        throw std::logic_error(
            "Project settings terrain validation has no active project/session.");
    }

    terrainRoundTripReport_ =
        studio_session::
            VerifyStudioTerrainRoundTrip(
                *project_,
                *session_,
                "studio.primary");

    const auto& report =
        *terrainRoundTripReport_;

    status_ =
        report.success
            ? "Terrain round trip verified: authored state and regenerated physical result are equivalent."
            : std::format(
                  "Terrain round trip failed at {}: {}",
                  report.failureStage.empty()
                      ? std::string("unknown")
                      : report.failureStage,
                  report.diagnostic);

    return report;
}

studio_session::StudioTerrainValidationScenarioReport
ProjectSettingsUi::RunTerrainValidationScenario()
{
    const auto validationRoot =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-m15-" +
         documents::ProjectId::Random().
             ToString());

    terrainValidationScenarioReport_ =
        studio_session::
            RunStudioTerrainValidationScenario(
                validationRoot,
                "studio.primary");

    const auto& report =
        *terrainValidationScenarioReport_;

    status_ =
        report.success
            ? "M15 terrain validation scenario passed."
            : std::format(
                  "M15 terrain validation failed at {}: {}",
                  report.failureStage.empty()
                      ? std::string("unknown")
                      : report.failureStage,
                  report.diagnostic);

    return report;
}

void ProjectSettingsUi::Draw(
    editor_ui::PanelContext& context)
{
    if (project_ == nullptr ||
        session_ == nullptr)
    {
        context.Text("Project settings are unavailable.");
        return;
    }

    SynchronizeAuthority();

    const auto& manifest =
        project_->Manifest();

    context.Text(
        std::format(
            "Project ID: {}",
            manifest.projectId.ToString()));
    context.Text(
        std::format(
            "Root: {}",
            project_->RootDirectory().
                generic_string()));
    context.Text(
        std::format(
            "Manifest: {}",
            project_->ManifestPath().
                generic_string()));
    context.Text(
        std::format(
            "Engine compatibility: {}",
            manifest.engineCompatibilityVersion));

    static_cast<void>(
        context.InputText(
            "Display Name##project-display-name",
            displayName_));

    if (context.Button("Save Project Name"))
    {
        try
        {
            project_->SetDisplayName(
                displayName_);
            observedDisplayName_ =
                project_->Manifest().
                    displayName;
            displayName_ =
                observedDisplayName_;
            status_ =
                "Project name saved.";
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
        }
    }

    context.Separator();
    context.Text(
        std::format(
            "Startup world: {}",
            manifest.startupWorld.
                generic_string()));

    for (const auto& world :
         session_->Worlds())
    {
        context.Text(
            WorldLabel(world));

        if (!world.valid)
        {
            context.Text(
                std::format(
                    "  Validation: {}",
                    world.diagnostic));
            continue;
        }

        if (!world.descriptor.startup)
        {
            const std::string button =
                "Set Startup##project-settings:" +
                world.descriptor.id.ToString();

            if (context.Button(button))
            {
                try
                {
                    static_cast<void>(
                        session_->SetStartupWorld(
                            world.descriptor.
                                relativePath));
                    status_ =
                        "Startup world updated.";
                }
                catch (const std::exception&
                           exception)
                {
                    status_ =
                        exception.what();
                }
            }
        }
    }

    context.Separator();
    context.Text("Terrain Validation");
    context.Text(
        "Checkpoint the active project, reopen it through a fresh StudioWorkspace, "
        "verify authored identity and fresh derived residency, then regenerate "
        "the same physical page.");

    if (context.Button(
            "Save, Reopen & Verify Terrain"))
    {
        try
        {
            static_cast<void>(
                RunTerrainRoundTripValidation());
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            terrainRoundTripReport_.reset();
        }
    }

    if (terrainRoundTripReport_.has_value())
    {
        const auto& report =
            *terrainRoundTripReport_;

        context.Text(
            std::format(
                "Result: {}",
                report.success
                    ? "PASS"
                    : "FAIL"));

        context.Text(
            std::format(
                "Semantic fingerprint: {} -> {}",
                report.semanticFingerprintBefore,
                report.semanticFingerprintAfter));

        context.Text(
            std::format(
                "Terrain source revision: {} -> {}",
                report.terrainSourceRevisionBefore,
                report.terrainSourceRevisionAfter));

        context.Text(
            std::format(
                "Physical/M29 fingerprint: {} -> {}",
                report.physicalFingerprintBefore,
                report.physicalFingerprintAfter));

        context.Text(
            std::format(
                "Derived reset: M26 {} | M29 {} | page {}",
                report.derivedCacheFreshAfterReopen
                    ? "fresh"
                    : "stale",
                report.debugResidencyFreshAfterReopen
                    ? "fresh"
                    : "stale",
                report.comparisonPagePreserved
                    ? "preserved"
                    : "changed"));

        if (!report.diagnostic.empty())
        {
            context.Text(report.diagnostic);
        }
    }

    context.Separator();
    context.Text("M15 End-to-End Terrain Scenario");
    context.Text(
        "Runs the deterministic production-terrain acceptance sequence in an "
        "isolated scratch Studio project. The currently open project is not modified.");

    if (context.Button(
            "Run M15 Terrain Validation Scenario"))
    {
        try
        {
            static_cast<void>(
                RunTerrainValidationScenario());
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            terrainValidationScenarioReport_.reset();
        }
    }

    if (terrainValidationScenarioReport_.has_value())
    {
        const auto& report =
            *terrainValidationScenarioReport_;

        context.Text(
            std::format(
                "M15 Result: {} | Steps: {}",
                report.success
                    ? "PASS"
                    : "FAIL",
                report.steps.size()));

        context.Text(
            std::format(
                "Scratch project: {}",
                report.projectRoot.generic_string()));

        context.Text(
            std::format(
                "M29 fields: {} | physical LOD {} | biome weights {}",
                report.debugFieldsAvailable,
                report.debugPhysicalLodAvailable
                    ? "yes"
                    : "no",
                report.debugBiomeWeightsAvailable
                    ? "yes"
                    : "no"));

        context.Text(
            std::format(
                "M26 cache: {} pages | {} bytes | hits {} | misses {}",
                report.cacheStats.residentPages,
                report.cacheStats.residentBytes,
                report.cacheStats.hits,
                report.cacheStats.misses));

        for (const auto& step :
             report.steps)
        {
            context.Text(
                std::format(
                    "{} {}{}{}",
                    step.passed
                        ? "[PASS]"
                        : "[FAIL]",
                    step.name,
                    step.diagnostic.empty()
                        ? ""
                        : " | ",
                    step.diagnostic));
        }
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}

void ProjectSettingsUi::SynchronizeAuthority()
{
    if (project_ == nullptr)
    {
        return;
    }

    const std::string& authoritative =
        project_->Manifest().displayName;

    if (authoritative !=
        observedDisplayName_)
    {
        observedDisplayName_ =
            authoritative;
        displayName_ =
            authoritative;
    }
}
} // namespace orbit::studio_ui
