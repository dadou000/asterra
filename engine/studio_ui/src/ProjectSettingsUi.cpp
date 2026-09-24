#include <orbit/studio_ui/ProjectSettingsUi.hpp>
#include <orbit/studio_ui/LightingDisplaySettingsRuntime.hpp>
#include <orbit/lighting/LightingScheduler.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <string>

#define Register RegisterBase
#define Draw DrawBase
#include "ProjectSettingsUiBase.cpp"
#undef Draw
#undef Register

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

    context.Separator();
    context.Heading("Lighting / Display Defaults");
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

    if (!state.status.empty())
    {
        context.Text(state.status);
    }
}
} // namespace orbit::studio_ui
