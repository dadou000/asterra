#include <orbit/studio_ui/ProjectSettingsUi.hpp>
#include <orbit/studio_ui/LightingDisplaySettingsRuntime.hpp>
#include <orbit/lighting/LightingScheduler.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <string>

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

    if (item.active)
    {
        label += " [Active]";
    }

    if (item.descriptor.startup)
    {
        label += " [Startup]";
    }

    if (!item.valid)
    {
        label += " [Invalid]";
    }

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

void ProjectSettingsUi::RegisterBase(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Project Settings",
        .defaultOpen = false,
        .defaultDock = orbit::editor_ui::DockRegion::Right,
        .dockOrder = 30,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                DrawBase(context);
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

void ProjectSettingsUi::DrawBase(
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

    if (context.Section("Project", true))
    {
        static_cast<void>(
            context.InputText(
                "Name##project-display-name",
                displayName_));

        if (displayName_ != observedDisplayName_ &&
            context.PrimaryButton("Save Project Name"))
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
                status_ = std::format(
                    "Could not rename the project: {}",
                    exception.what());
            }
        }

        context.KeyValue(
            "Folder",
            project_->RootDirectory().
                generic_string());
        context.KeyValue(
            "Manifest",
            project_->ManifestPath().
                generic_string());
        context.KeyValue(
            "Project ID",
            manifest.projectId.ToString());
        context.KeyValue(
            "Engine compat.",
            std::format(
                "{}",
                manifest.engineCompatibilityVersion));
    }

    if (context.Section("Worlds", true))
    {
        context.KeyValue(
            "Startup world",
            manifest.startupWorld.
                generic_string());

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

    }

    if (context.Section("Developer Validation", false))
    {
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

namespace orbit::studio_ui
{
namespace
{
struct M40ProjectSettingsState
{
    LightingDisplaySettings settings{};
    bool loaded{false};
    std::string status;
};

[[nodiscard]] std::map<const ProjectSettingsUi*, M40ProjectSettingsState>&
M40States() noexcept
{
    static std::map<
        const ProjectSettingsUi*,
        M40ProjectSettingsState>
        states;
    return states;
}

M40ProjectSettingsState& EnsureM40State(
    const ProjectSettingsUi* owner,
    const std::filesystem::path& projectRoot)
{
    auto& state = M40States()[owner];
    if (!state.loaded)
    {
        state.settings =
            LoadLightingDisplaySettings(
                projectRoot);
        state.loaded = true;
        lighting::SetStudioLightingRuntimeConfig(
            state.settings.lighting);
        PublishStudioDisplayDefaultsRuntime(
            state.settings.display);
    }
    return state;
}
} // namespace

void ProjectSettingsUi::Register(
    editor_ui::EditorUi& ui)
{
    if (project_ != nullptr)
    {
        static_cast<void>(
            EnsureM40State(
                this,
                project_->RootDirectory()));
    }

    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Project Settings",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Right,
        .dockOrder = 30,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void ProjectSettingsUi::Draw(
    editor_ui::PanelContext& context)
{
    DrawBase(context);

    if (project_ == nullptr)
    {
        return;
    }

    auto& state =
        EnsureM40State(
            this,
            project_->RootDirectory());
    auto& lightingConfig =
        state.settings.lighting;
    auto& display =
        state.settings.display;

    if (context.Section("Lighting / Display Defaults", false))
    {
        context.MutedText(
            "Project-owned defaults. Display Diagnostics may override them for the current Studio session without rewriting this file.");

        bool hardwareRt =
            lightingConfig.hardwareRayQueryEnabled;
        f64 emissiveQuality =
            lightingConfig.emissiveGiQualityScale;

        static_cast<void>(
            context.Checkbox(
                "Hardware Ray Query Default##m40-project-hwrt",
                hardwareRt));
        static_cast<void>(
            context.InputDouble(
                "Emissive GI Quality Default##m40-project-emissive-quality",
                emissiveQuality));

        lightingConfig.hardwareRayQueryEnabled =
            hardwareRt;
        lightingConfig.emissiveGiQualityScale =
            static_cast<f32>(
                std::clamp(
                    emissiveQuality,
                    0.0,
                    4.0));

        context.Text("Advanced Lighting Budgets (ms)");

        f64 direct = lightingConfig.budget.directLightingMs;
        f64 visibility = lightingConfig.budget.visibilityMs;
        f64 gi = lightingConfig.budget.giMs;
        f64 reflections = lightingConfig.budget.reflectionMs;
        f64 emissive = lightingConfig.budget.emissiveMs;
        f64 post = lightingConfig.budget.postProcessMs;

        static_cast<void>(context.InputDouble(
            "Direct##m40-project-direct", direct));
        static_cast<void>(context.InputDouble(
            "Visibility##m40-project-visibility", visibility));
        static_cast<void>(context.InputDouble(
            "GI##m40-project-gi", gi));
        static_cast<void>(context.InputDouble(
            "Reflections##m40-project-reflections", reflections));
        static_cast<void>(context.InputDouble(
            "Emissive##m40-project-emissive", emissive));
        static_cast<void>(context.InputDouble(
            "Post Process##m40-project-post", post));

        lightingConfig.budget.directLightingMs =
            static_cast<f32>(std::max(direct, 0.0));
        lightingConfig.budget.visibilityMs =
            static_cast<f32>(std::max(visibility, 0.0));
        lightingConfig.budget.giMs =
            static_cast<f32>(std::max(gi, 0.0));
        lightingConfig.budget.reflectionMs =
            static_cast<f32>(std::max(reflections, 0.0));
        lightingConfig.budget.emissiveMs =
            static_cast<f32>(std::max(emissive, 0.0));
        lightingConfig.budget.postProcessMs =
            static_cast<f32>(std::max(post, 0.0));

        context.Text("Display Defaults");

        f64 middleGray = display.eye.exposureMiddleGray;
        f64 ceiling = display.eye.photopicCeilingLog2;
        bool bloom = display.highlights.bloomEnabled;
        f64 bloomStrength = display.highlights.bloomStrength;
        bool lut = display.colorLut.enabled;
        f64 lutStrength = display.colorLut.strength;
        f64 referenceWhite = display.output.referenceWhiteNits;
        f64 peak = display.output.requestedPeakNits;

        static_cast<void>(context.InputDouble(
            "Exposure Middle Gray##m40-project-middle-gray",
            middleGray));
        static_cast<void>(context.InputDouble(
            "Photopic Ceiling log2##m40-project-ceiling",
            ceiling));
        static_cast<void>(context.Checkbox(
            "Bloom Default##m40-project-bloom",
            bloom));
        static_cast<void>(context.InputDouble(
            "Bloom Strength##m40-project-bloom-strength",
            bloomStrength));
        static_cast<void>(context.Checkbox(
            "LUT Default##m40-project-lut",
            lut));
        static_cast<void>(context.InputDouble(
            "LUT Strength##m40-project-lut-strength",
            lutStrength));
        static_cast<void>(context.InputDouble(
            "Reference White nits##m40-project-white",
            referenceWhite));
        static_cast<void>(context.InputDouble(
            "Requested Peak nits##m40-project-peak",
            peak));

        display.eye.exposureMiddleGray =
            static_cast<f32>(
                std::max(middleGray, 1.0e-6));
        display.eye.photopicCeilingLog2 =
            static_cast<f32>(ceiling);
        display.highlights.bloomEnabled = bloom;
        display.highlights.bloomStrength =
            static_cast<f32>(
                std::max(bloomStrength, 0.0));
        display.colorLut.enabled = lut;
        display.colorLut.strength =
            static_cast<f32>(
                std::clamp(lutStrength, 0.0, 1.0));
        display.output.referenceWhiteNits =
            static_cast<f32>(
                std::max(referenceWhite, 1.0));
        display.output.requestedPeakNits =
            static_cast<f32>(
                std::max(peak, referenceWhite));

        if (context.Button(
                "Adopt Session Lighting##m40-adopt-runtime"))
        {
            if (const auto runtime =
                    lighting::StudioLightingRuntimeConfig();
                runtime.has_value())
            {
                state.settings.lighting = *runtime;
                state.status =
                    "Session lighting override copied into project defaults; save to persist.";
            }
        }

        context.SameLine();

        if (context.Button(
                "Save Lighting / Display Defaults##m40-save-defaults"))
        {
            try
            {
                SaveLightingDisplaySettings(
                    project_->RootDirectory(),
                    state.settings);
                lighting::SetStudioLightingRuntimeConfig(
                    state.settings.lighting);
                PublishStudioDisplayDefaultsRuntime(
                    state.settings.display);
                state.status =
                    "LightingDisplay.orbitcfg saved and applied.";
            }
            catch (const std::exception& exception)
            {
                state.status = exception.what();
            }
        }

        context.SameLine();

        if (context.Button(
                "Reload Project Defaults##m40-reload-defaults"))
        {
            state.settings =
                LoadLightingDisplaySettings(
                    project_->RootDirectory());
            lighting::SetStudioLightingRuntimeConfig(
                state.settings.lighting);
            PublishStudioDisplayDefaultsRuntime(
                state.settings.display);
            state.status =
                "Project lighting/display defaults reloaded and applied.";
        }

        context.MutedText(
            std::format(
                "Persisted budget {:.2f} ms | emissive quality x{:.2f} | ray query {}",
                state.settings.lighting.budget.TotalMs(),
                state.settings.lighting.emissiveGiQualityScale,
                state.settings.lighting.hardwareRayQueryEnabled
                    ? "enabled"
                    : "disabled"));

    }

    if (!state.status.empty())
    {
        context.Text(state.status);
    }
}
} // namespace orbit::studio_ui
