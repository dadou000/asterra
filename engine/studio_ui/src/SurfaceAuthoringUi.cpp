#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_session/StudioTerrainServiceStatus.hpp>
#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>
#include <orbit/terrain_geology/Stratigraphy.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::string_view SelectorFieldName(
    const terrain_biome::BiomeSelectorField field) noexcept
{
    switch (field)
    {
    case terrain_biome::BiomeSelectorField::Temperature: return "Temperature";
    case terrain_biome::BiomeSelectorField::Moisture: return "Moisture";
    case terrain_biome::BiomeSelectorField::Rainfall: return "Rainfall";
    case terrain_biome::BiomeSelectorField::Elevation: return "Elevation";
    case terrain_biome::BiomeSelectorField::Slope: return "Slope";
    case terrain_biome::BiomeSelectorField::Aspect: return "Aspect";
    case terrain_biome::BiomeSelectorField::Latitude: return "Latitude";
    case terrain_biome::BiomeSelectorField::Continentality: return "Continentality";
    case terrain_biome::BiomeSelectorField::DistanceToCoastWater: return "Distance to Coast/Water";
    case terrain_biome::BiomeSelectorField::Drainage: return "Drainage";
    case terrain_biome::BiomeSelectorField::SoilDepth: return "Soil Depth";
    case terrain_biome::BiomeSelectorField::SandDepth: return "Sand Depth";
    case terrain_biome::BiomeSelectorField::GeologyMaterial: return "Geology Material";
    case terrain_biome::BiomeSelectorField::SolarExposure: return "Solar Exposure";
    case terrain_biome::BiomeSelectorField::WindExposure: return "Wind Exposure";
    case terrain_biome::BiomeSelectorField::SnowPersistence: return "Snow Persistence";
    case terrain_biome::BiomeSelectorField::UserField: return "User Field";
    }
    return "Unknown";
}

[[nodiscard]] std::string_view MaskOperationName(
    const terrain_biome::BiomeAuthoredWeightOperation operation) noexcept
{
    switch (operation)
    {
    case terrain_biome::BiomeAuthoredWeightOperation::Add: return "Add";
    case terrain_biome::BiomeAuthoredWeightOperation::Subtract: return "Subtract";
    case terrain_biome::BiomeAuthoredWeightOperation::Replace: return "Replace";
    case terrain_biome::BiomeAuthoredWeightOperation::Multiply: return "Multiply";
    case terrain_biome::BiomeAuthoredWeightOperation::Min: return "Min";
    case terrain_biome::BiomeAuthoredWeightOperation::Max: return "Max";
    }
    return "Unknown";
}

bool InputU32(
    editor_ui::PanelContext& context,
    const std::string& label,
    u32& value,
    std::string& error)
{
    i64 edited =
        static_cast<i64>(value);

    if (!context.InputInteger(
            label,
            edited))
    {
        return false;
    }

    if (edited < 1 ||
        static_cast<u64>(edited) >
            static_cast<u64>(
                std::numeric_limits<u32>::max()))
    {
        error =
            label +
            " must be between 1 and uint32 max.";
        return false;
    }

    value =
        static_cast<u32>(edited);
    return true;
}

void DrawPreferenceBand(
    editor_ui::PanelContext& context,
    const std::string_view prefix,
    editor_model::SurfaceBiomePreferenceBand& band,
    bool& changed)
{
    const std::string base(prefix);
    changed |= context.InputDouble(base + " Min##m28-" + base + "-min", band.minimum);
    changed |= context.InputDouble(base + " Max##m28-" + base + "-max", band.maximum);
    changed |= context.InputDouble(base + " Lower Falloff##m28-" + base + "-lower", band.lowerFalloff);
    changed |= context.InputDouble(base + " Upper Falloff##m28-" + base + "-upper", band.upperFalloff);
}

[[nodiscard]] std::string MakeImpactHistoryStarter(
    const world::PlanetDefinition& planet,
    const bool icy)
{
    auto definition = terrain_impacts::MakeMoonLikeImpactPreset(
        planet.id,
        terrain_impacts::ImpactFieldId::Random(),
        planet.generationSeed);
    definition.name = icy ? "Icy moon starter" : "Airless moon starter";
    definition.surfaceAgeYears = 4.5e9;
    if (icy)
    {
        definition.environment = terrain_impacts::SurfaceEnvironment::Icy;
        auto fractures = std::make_shared<terrain_impacts::IceFractureDefinition>();
        fractures->seed = planet.generationSeed ^ 0x4943594D4F4F4EULL;
        fractures->ageOrder = 1U;
        fractures->formationAgeYears = 2.0e8;
        fractures->fractureCount = 96U;
        definition.iceFractures = std::move(fractures);
    }
    return terrain_impacts::SerializeImpactFieldToml(definition);
}

[[nodiscard]] std::string MakeStratigraphyStarter(
    const terrain_geology::GeologicalMaterialLibrary& materials)
{
    const auto available = materials.Materials();
    if (available.empty())
        throw std::runtime_error("Add a geological material to this planet before creating a stratigraphy profile.");

    const std::size_t lowerIndex = available.size() > 1U ? 1U : 0U;
    terrain_geology::StratigraphyProfile profile{
        .id = terrain_geology::StratigraphyProfileId::Random(),
        .name = "Two-layer crust starter",
        .layers = {
            {.material = available.front().id, .thicknessMeters = 1'200.0, .transitionBandMeters = 120.0},
            {.material = available[lowerIndex].id, .thicknessMeters = 3'800.0, .transitionBandMeters = 250.0}
        },
        .basementMaterial = available.back().id
    };
    return terrain_geology::SerializeStratigraphyProfileToml(profile);
}

[[nodiscard]] std::string_view CubeFaceName(
    const world::CubeFace face) noexcept
{
    switch (face)
    {
    case world::CubeFace::PositiveX: return "+X";
    case world::CubeFace::NegativeX: return "-X";
    case world::CubeFace::PositiveY: return "+Y";
    case world::CubeFace::NegativeY: return "-Y";
    case world::CubeFace::PositiveZ: return "+Z";
    case world::CubeFace::NegativeZ: return "-Z";
    }
    return "?";
}
} // namespace

SurfaceAuthoringUi::SurfaceAuthoringUi(
    studio_session::StudioWorkspace& workspace)
    : workspace_(&workspace)
{
}

SurfaceAuthoringUi::SurfaceAuthoringUi(
    studio_session::StudioSession& session)
    : session_(&session)
{
}

void SurfaceAuthoringUi::Register(editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Surface Authoring",
        .defaultOpen = false,
        .defaultDock = orbit::editor_ui::DockRegion::Right,
        .dockOrder = 10,
        .draw = [this](editor_ui::PanelContext& context)
        {
            Draw(context);
        }
    });
}

void SurfaceAuthoringUi::SetAutomationCoverageMode(
    const bool enabled) noexcept
{
    automationCoverageMode_ = enabled;

    if (enabled)
    {
        advancedBiome_ = true;
        advancedProcesses_ = true;
    }
}

void SurfaceAuthoringUi::Draw(editor_ui::PanelContext& context)
{
    studio_session::StudioSession* session =
        session_;

    if (workspace_ != nullptr)
    {
        if (!workspace_->HasProject())
        {
            context.Text("Open a project to author planetary surfaces.");
            return;
        }

        session = &workspace_->Session();
    }

    if (session == nullptr)
    {
        context.Text("Open a project to author planetary surfaces.");
        return;
    }

    auto& world = session->World();
    if (!world.HasWorld())
    {
        context.Text("Open a world to author planetary surfaces.");
        return;
    }

    editor_model::SurfaceAuthoringModel model(
        world.Objects(),
        world.Commands(),
        world.Selection());

    const auto selected = model.SelectedRockyBody();
    if (!selected.has_value())
    {
        selectedBiome_.reset();
        context.Text(
            "Select a terrain-bearing Celestial Body, Terrain Surface, or one of its authored surface children.");
        return;
    }

    const auto counts = model.Counts(selected->terrain);

    context.Text(std::format("Planet: {}", selected->bodyName));

    const auto geologyTree = context.TreeItem("Geology##m28-geology", false);
    if (geologyTree.open)
    {
        const auto structureTree =
            context.TreeItem(
                "Structural Fields##m28-structural-fields",
                false);
        if (structureTree.open)
        {
            context.Text(
                "Orbit generates a deterministic spherical plate field with continental/oceanic identity, rotation-driven convergent/divergent/transform boundaries, and mantle hotspots.");
            context.Text(
                "Open Planet > Tectonics to edit the plate recipe, bake policy and structural probe. The viewport's Tectonics layer shows plate identity and boundary activity.");
            context.TreePop();
        }

        auto relief = model.Relief(selected->terrain);
        bool changed = false;

        context.Text("Base Relief");
        changed |= context.InputDouble("Macro Amplitude (m)##m28-macro-amplitude", relief.macroAmplitudeMeters);
        changed |= context.InputDouble("Macro Wavelength (m)##m28-macro-wavelength", relief.macroWavelengthMeters);
        changed |= context.InputDouble("Detail Amplitude (m)##m28-detail-amplitude", relief.detailAmplitudeMeters);
        changed |= context.InputDouble("Detail Wavelength (m)##m28-detail-wavelength", relief.detailWavelengthMeters);
        changed |= context.InputInteger("Detail Octaves##m28-detail-octaves", relief.detailOctaves);
        changed |= context.InputDouble("Maximum Elevation (m)##m28-max-elevation", relief.maximumElevationMeters);

        if (changed)
        {
            try
            {
                model.SetRelief(selected->terrain, relief);
                status_ = "Base relief authoring updated.";
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }

        context.Separator();
        context.Text("Implemented Geology Systems");
        context.Text("- Uplift / macro geology");
        context.Text("- Virtual stratigraphy");
        context.Text("- Rock material physics");
        context.Text("- Fault / fold authored constraints");
        context.Text("- Impacts / craters");
        context.Text("- Authored geological features");
        context.Separator();
        context.Text("Chronological Impacts, Ejecta, Ice Fractures and Resurfacing");
        if (impactHistoryDraftTerrain_ != selected->terrain)
        {
            if (impactHistoryDraftTerrain_)
                impactHistoryDrafts_[impactHistoryDraftTerrain_] = impactHistoryDraft_;
            impactHistoryDraftTerrain_ = selected->terrain;
            const auto savedDraft = impactHistoryDrafts_.find(selected->terrain);
            impactHistoryDraft_ = savedDraft != impactHistoryDrafts_.end()
                ? savedDraft->second
                : model.ImpactHistoryToml(selected->terrain);
        }
        context.Text("Start from a planet-aware example or edit the recipe directly. Examples and Reload Saved replace this unsaved draft; Save applies changes to terrain.");
        if (context.Button("Airless Moon Example##m28-impact-airless-example"))
        {
            try
            {
                const auto body = world.Universe().BodyForObject(selected->body);
                if (!body.has_value()) throw std::runtime_error("Selected terrain body is not active.");
                const auto planet = world.Surfaces().Registry().SphericalPlanetDefinition(*body);
                if (!planet.has_value()) throw std::runtime_error("Geological history requires a spherical planet.");
                impactHistoryDraft_ = MakeImpactHistoryStarter(*planet, false);
                status_ = "Airless moon example loaded into the draft. Review it, then save.";
            }
            catch (const std::exception& exception) { status_ = exception.what(); }
        }
        context.SameLine();
        if (context.Button("Icy Moon Example##m28-impact-icy-example"))
        {
            try
            {
                const auto body = world.Universe().BodyForObject(selected->body);
                if (!body.has_value()) throw std::runtime_error("Selected terrain body is not active.");
                const auto planet = world.Surfaces().Registry().SphericalPlanetDefinition(*body);
                if (!planet.has_value()) throw std::runtime_error("Geological history requires a spherical planet.");
                impactHistoryDraft_ = MakeImpactHistoryStarter(*planet, true);
                status_ = "Icy moon example loaded into the draft. Review it, then save.";
            }
            catch (const std::exception& exception) { status_ = exception.what(); }
        }
        if (context.Button("Reload Saved##m28-impact-reload"))
        {
            impactHistoryDraft_ = model.ImpactHistoryToml(selected->terrain);
            impactHistoryDrafts_[selected->terrain] = impactHistoryDraft_;
            status_ = "Geological history draft reloaded from the saved terrain data.";
        }
        static_cast<void>(context.InputTextMultiline(
            "##m28-impact-history", impactHistoryDraft_, {0.0F, 180.0F}));
        context.Text("TOML edits stay in this draft until Save. Validation errors appear in the status line; saving creates one undoable edit and queues only affected terrain regions where possible.");
        context.Text("Tectonic renewals can set displacement_x/y/z and displacement_m; plate_motion=true uses a closed spherical boundary to move older structures with that plate.");
        if (context.Button("Save Geological History##m28-impact-save"))
        {
            try
            {
                const auto body = world.Universe().BodyForObject(selected->body);
                if (!body.has_value()) throw std::runtime_error("Selected terrain body is not active.");
                const auto planet = world.Surfaces().Registry().SphericalPlanetDefinition(*body);
                if (!planet.has_value()) throw std::runtime_error("Geological history requires a spherical planet.");
                if (!impactHistoryDraft_.empty() &&
                    terrain_impacts::ParseImpactFieldToml(impactHistoryDraft_).planet != planet->id)
                    throw std::invalid_argument("Geological history belongs to a different planet.");
                const std::string previousHistory = model.ImpactHistoryToml(selected->terrain);
                std::vector<terrain_dependency::TerrainInvalidationRequest> invalidations;
                const auto runtime = session->TerrainRuntime().Capture("studio.primary");
                const u8 physicalPageLevel = runtime.has_value() && runtime->body == *body
                    ? runtime->physicalPageLevel
                    : 10U;
                invalidations = studio_session::BuildImpactHistoryInvalidations(
                    *planet, previousHistory, impactHistoryDraft_, physicalPageLevel);
                model.SetImpactHistoryToml(selected->terrain, impactHistoryDraft_);
                session->QueueTerrainInvalidations(invalidations);
                impactHistoryDrafts_[selected->terrain] = impactHistoryDraft_;
                status_ = std::format(
                    "Geological event history saved; {} local terrain regions queued.",
                    invalidations.size());
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }
        context.Separator();
        context.Text("Stratigraphic Layers Exposed by Excavation");
        if (stratigraphyDraftTerrain_ != selected->terrain)
        {
            if (stratigraphyDraftTerrain_)
                stratigraphyDrafts_[stratigraphyDraftTerrain_] = stratigraphyDraft_;
            stratigraphyDraftTerrain_ = selected->terrain;
            const auto savedDraft = stratigraphyDrafts_.find(selected->terrain);
            stratigraphyDraft_ = savedDraft != stratigraphyDrafts_.end()
                ? savedDraft->second
                : model.StratigraphyToml(selected->terrain);
        }
        context.Text("Layers use this planet's geological materials and are measured down from the exposed surface. Examples and Reload Saved replace this unsaved draft.");
        if (context.Button("Load 2-Layer Example##m28-stratigraphy-example"))
        {
            try
            {
                const auto body = world.Universe().BodyForObject(selected->body);
                if (!body.has_value()) throw std::runtime_error("Selected terrain body is not active.");
                const auto* services = world.Surfaces().ServicesForBody(*body);
                if (services == nullptr) throw std::runtime_error("Geology materials are not composed for this planet yet.");
                stratigraphyDraft_ = MakeStratigraphyStarter(services->Geology());
                status_ = "Two-layer example loaded into the draft. Review it, then save.";
            }
            catch (const std::exception& exception) { status_ = exception.what(); }
        }
        if (context.Button("Reload Saved##m28-stratigraphy-reload"))
        {
            stratigraphyDraft_ = model.StratigraphyToml(selected->terrain);
            stratigraphyDrafts_[selected->terrain] = stratigraphyDraft_;
            status_ = "Stratigraphy draft reloaded from the saved terrain data.";
        }
        static_cast<void>(context.InputTextMultiline(
            "##m28-stratigraphy", stratigraphyDraft_, {0.0F, 150.0F}));
        context.Text("Edit the profile in the draft, then Save to validate materials and queue the planet's material columns.");
        if (context.Button("Save Stratigraphy##m28-stratigraphy-save"))
        {
            try
            {
                if (!stratigraphyDraft_.empty())
                {
                    const auto profile = terrain_geology::ParseStratigraphyProfileToml(stratigraphyDraft_);
                    const auto body = world.Universe().BodyForObject(selected->body);
                    if (!body.has_value()) throw std::runtime_error("Selected terrain body is not active.");
                    const auto* services = world.Surfaces().ServicesForBody(*body);
                    if (services == nullptr || !terrain_geology::ReferencesKnownMaterials(profile, services->Geology()))
                        throw std::invalid_argument("Stratigraphy references a material not present on this body.");
                }
                const auto body = world.Universe().BodyForObject(selected->body);
                if (!body.has_value()) throw std::runtime_error("Selected terrain body is not active.");
                const auto planet = world.Surfaces().Registry().SphericalPlanetDefinition(*body);
                if (!planet.has_value()) throw std::runtime_error("Stratigraphy requires a spherical planet.");
                model.SetStratigraphyToml(selected->terrain, stratigraphyDraft_);
                session->QueueTerrainInvalidation({
                    .kind = terrain_dependency::TerrainChangeKind::TerrainAuthoring,
                    .scope = {.planet = planet->id, .global = true}});
                stratigraphyDrafts_[selected->terrain] = stratigraphyDraft_;
                status_ = "Stratigraphy saved; planet material columns queued for rebuild.";
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }
        context.Text(std::format("Semantic Geology Assets: {}", counts.geologyAssets));
        context.TreePop();
    }

    const auto processTree =
        context.TreeItem(
            "Terrain Processes##m28-processes",
            false);

    if (processTree.open)
    {
        auto settings =
            model.ProcessSettings(
                selected->terrain);

        bool changed = false;
        std::string inputError;

        context.Text(
            std::format(
                "Semantic Process Assets: {}",
                counts.processAssets));

        context.Text("Stream Power");
        changed |= context.Checkbox(
            "Enabled##m11-stream-enabled",
            settings.streamPowerEnabled);
        changed |= InputU32(
            context,
            "Iterations##m11-stream-iterations",
            settings.streamPower.iterations,
            inputError);

        context.Separator();
        context.Text("Hydraulic");
        changed |= context.Checkbox(
            "Enabled##m11-hydraulic-enabled",
            settings.hydraulicEnabled);
        changed |= context.InputDouble(
            "Rainfall (m/s)##m11-hydraulic-rain",
            settings.hydraulic.rainfallMetersPerSecond);

        context.Separator();
        context.Text("Thermal / Gravity");
        changed |= context.Checkbox(
            "Enabled##m11-thermal-enabled",
            settings.thermalEnabled);
        changed |= context.InputDouble(
            "Relaxation##m11-thermal-relaxation",
            settings.thermal.relaxation);

        context.Separator();
        context.Text("Aeolian");
        changed |= context.Checkbox(
            "Enabled##m11-aeolian-enabled",
            settings.aeolianEnabled);
        changed |= context.InputDouble(
            "Capacity Coefficient##m11-aeolian-capacity",
            settings.aeolian.capacityCoefficient);

        context.Separator();
        context.Text("Glacial");
        changed |= context.Checkbox(
            "Enabled##m11-glacial-enabled",
            settings.glacialEnabled);
        changed |= context.InputDouble(
            "Maximum Glacier Temperature (C)##m11-glacial-temp",
            settings.glacial.maximumGlacierTemperatureC);

        context.Separator();
        context.Text("River / Meander");
        changed |= context.Checkbox(
            "Enabled##m11-rivers-enabled",
            settings.riversEnabled);
        changed |= context.Checkbox(
            "Meanders##m11-rivers-meanders",
            settings.rivers.enableMeanders);
        changed |= context.Checkbox(
            "Cutoffs##m11-rivers-cutoffs",
            settings.rivers.enableCutoffs);

        context.Separator();
        context.Text("Coastal");
        changed |= context.Checkbox(
            "Enabled##m11-coastal-enabled",
            settings.coastal.enabled);
        changed |= InputU32(
            context,
            "Hydrodynamic Steps##m11-coastal-steps",
            settings.coastal.hydrodynamicSteps,
            inputError);

        context.Separator();
        static_cast<void>(
            context.Checkbox(
                "Advanced##m11-process-advanced",
                advancedProcesses_));

        if (advancedProcesses_)
        {
            context.Text("Exact Solver Fields");

            changed |= context.InputDouble(
                "Stream Incision (m/iteration)##m11-stream-incision",
                settings.streamPower.
                    incisionCoefficientMetersPerIteration);

            changed |= InputU32(
                context,
                "Hydraulic Iterations##m11-hydraulic-iterations",
                settings.hydraulic.iterations,
                inputError);
            changed |= context.InputDouble(
                "Hydraulic Time Step (s)##m11-hydraulic-dt",
                settings.hydraulic.timeStepSeconds);

            changed |= InputU32(
                context,
                "Thermal Iterations##m11-thermal-iterations",
                settings.thermal.maximumIterations,
                inputError);

            changed |= InputU32(
                context,
                "Aeolian Iterations##m11-aeolian-iterations",
                settings.aeolian.iterations,
                inputError);
            changed |= context.InputDouble(
                "Aeolian Time Step (s)##m11-aeolian-dt",
                settings.aeolian.timeStepSeconds);

            changed |= InputU32(
                context,
                "Glacial Iterations##m11-glacial-iterations",
                settings.glacial.iterations,
                inputError);
            changed |= context.InputDouble(
                "Glacial Time Step (yr)##m11-glacial-dt",
                settings.glacial.timeStepYears);

            changed |= InputU32(
                context,
                "Meander Iterations##m11-river-iterations",
                settings.rivers.meanderIterations,
                inputError);
            changed |= context.InputDouble(
                "Minimum River Drainage Area (m2)##m11-river-area",
                settings.rivers.minimumDrainageAreaSquareMeters);

            changed |= context.InputDouble(
                "Coastal CFL##m11-coastal-cfl",
                settings.coastal.water.cflNumber);
            changed |= context.InputDouble(
                "Coastal Maximum Time Step (s)##m11-coastal-dt",
                settings.coastal.water.maximumTimeStepSeconds);
        }

        if (!inputError.empty())
        {
            status_ =
                inputError;
        }
        else if (changed)
        {
            try
            {
                const auto body =
                    world.Surfaces().
                        BodyForTerrainObject(
                            selected->terrain);

                if (!body.has_value())
                {
                    throw std::runtime_error(
                        "Terrain process edit has no active runtime body.");
                }

                const auto planet =
                    world.Surfaces().
                        Registry().
                        SphericalPlanetDefinition(
                            *body);

                if (!planet.has_value())
                {
                    throw std::runtime_error(
                        "Terrain process edit requires a spherical planet.");
                }

                model.SetProcessSettings(
                    selected->terrain,
                    settings);

                session->
                    QueueTerrainInvalidation({
                        .kind =
                            terrain_dependency::
                                TerrainChangeKind::
                                    ProcessSettings,
                        .scope = {
                            .planet =
                                planet->id,
                            .global = true
                        }
                    });

                status_ =
                    "Terrain process settings updated; M27 process descendants queued.";
            }
            catch (const std::exception& exception)
            {
                status_ =
                    exception.what();
            }
        }

        if (const auto processObject =
                model.ProcessSettingsObject(
                    selected->terrain);
            processObject.has_value())
        {
            const std::string selectLabel =
                "Select Process Record##m11-select-process";

            if (context.Button(selectLabel))
            {
                model.SelectObject(
                    *processObject);
            }
        }

        context.TreePop();
    }

    const auto authoredTerrainTree =
        context.TreeItem(
            "Authored Terrain##m09-authored-terrain",
            false);

    if (authoredTerrainTree.open)
    {
        const auto constraints =
            model.TerrainConstraints(
                selected->terrain);

        context.Text(
            std::format(
                "Constraints: {}",
                constraints.size()));
        context.Text(
            "Create brush/spline constraints directly in a Perspective or Body Map viewport.");
        context.Text(
            "Select a constraint below to edit its numeric authority fields in Properties.");

        for (const auto& constraint :
             constraints)
        {
            std::string kind;

            switch (constraint.channel)
            {
            case editor_model::
                SurfaceTerrainConstraintChannel::
                    Height:
                kind = "Height";
                break;
            case editor_model::
                SurfaceTerrainConstraintChannel::
                    Protection:
                kind = "Protection";
                break;
            case editor_model::
                SurfaceTerrainConstraintChannel::
                    Drainage:
                kind = "Drainage";
                break;
            case editor_model::
                SurfaceTerrainConstraintChannel::
                    Material:
                kind = "Geology";
                break;
            }

            kind +=
                constraint.shape ==
                        editor_model::
                            SurfaceTerrainConstraintShape::
                                Spline
                    ? " Spline"
                    : " Brush";

            const std::string label =
                constraint.name +
                " [" + kind + "]##m09-constraint-" +
                constraint.id.ToString();

            if (context.Selectable(
                    label,
                    world.Selection().
                        Contains(
                            constraint.id)))
            {
                model.SelectObject(
                    constraint.id);
            }

            if (constraint.shape ==
                editor_model::
                    SurfaceTerrainConstraintShape::
                        Spline)
            {
                context.Text(
                    std::format(
                        "  {} control points | half-width {:.3g} m | falloff {:.3g} m",
                        constraint.
                            controlUnitDirections.
                            size(),
                        constraint.
                            halfWidthMeters,
                        constraint.
                            falloffMeters));
            }
        }

        context.TreePop();
    }

    const auto biomeTree = context.TreeItem("Biomes##m28-biomes", false);
    if (biomeTree.open)
    {
        auto biomes = model.Biomes(selected->terrain);

        if (automationCoverageMode_ &&
            !selectedBiome_.has_value() &&
            !biomes.empty())
        {
            selectedBiome_ =
                biomes.front().id;
        }

        if (selectedBiome_.has_value())
        {
            const auto stillPresent = std::find_if(
                biomes.begin(),
                biomes.end(),
                [this](const auto& biome)
                {
                    return biome.id == *selectedBiome_;
                });
            if (stillPresent == biomes.end())
            {
                selectedBiome_.reset();
            }
        }

        context.Text("Base Biome");
        context.Text("Non-removable residual fallback for uncovered terrain.");

        for (const auto& biome : biomes)
        {
            const std::string label =
                biome.name + "##m28-biome-" + biome.id.ToString();

            if (context.Selectable(label, selectedBiome_ == biome.id))
            {
                selectedBiome_ = biome.id;
                model.SelectObject(biome.id);
            }
        }

        context.Separator();
        static_cast<void>(context.InputText("New Biome Name##m28-new-biome", newBiomeName_));

        if (context.Button("+ Add Biome##m28-add-biome"))
        {
            try
            {
                const auto biome = model.AddBiome(selected->terrain, newBiomeName_);
                selectedBiome_ = biome;
                model.SelectObject(biome);
                status_ =
                    "Biome created with temperature, moisture and elevation preference selectors.";
                biomes = model.Biomes(selected->terrain);
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
            }
        }

        if (selectedBiome_.has_value())
        {
            const auto selectedBiome = std::find_if(
                biomes.begin(),
                biomes.end(),
                [this](const auto& biome)
                {
                    return biome.id == *selectedBiome_;
                });

            if (selectedBiome != biomes.end())
            {
                context.Separator();
                context.Text(std::format("Editing: {}", selectedBiome->name));

                auto preferences = model.CommonPreferences(selectedBiome->id);
                bool preferencesChanged = false;

                context.Text("Automatic Preference");
                DrawPreferenceBand(context, "Temperature", preferences.temperature, preferencesChanged);
                DrawPreferenceBand(context, "Moisture", preferences.moisture, preferencesChanged);
                DrawPreferenceBand(context, "Elevation", preferences.elevation, preferencesChanged);

                if (preferencesChanged)
                {
                    try
                    {
                        model.SetCommonPreferences(selectedBiome->id, preferences);
                        status_ = "Biome automatic preference updated.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }

                context.Separator();
                context.Text("Viewport Biome Paint");
                context.Text(
                    "Use Viewport > Biome Paint for normal Add/Subtract/Replace painting with production-terrain picking.");

                if (advancedBiome_)
                {
                    context.Text("Exact Authored Override (advanced)");

                    static_cast<void>(context.InputDouble3(
                        "Center Unit Direction##m28-mask-direction",
                        localOverrideDirection_));
                    static_cast<void>(context.InputDouble(
                        "Inner Radius (m)##m28-mask-inner",
                        localOverrideInnerRadiusMeters_));
                    static_cast<void>(context.InputDouble(
                        "Outer Radius (m)##m28-mask-outer",
                        localOverrideOuterRadiusMeters_));
                    static_cast<void>(context.InputDouble(
                        "Weight##m28-mask-weight",
                        localOverrideWeight_));
                    static_cast<void>(context.InputDouble(
                        "Opacity##m28-mask-opacity",
                        localOverrideOpacity_));

                    if (context.Button("Create Exact Replace Mask##m28-paint-mask"))
                    {
                        try
                        {
                            const auto mask = model.PaintLocalOverride(
                                selectedBiome->id,
                                localOverrideDirection_,
                                localOverrideInnerRadiusMeters_,
                                localOverrideOuterRadiusMeters_,
                                localOverrideWeight_,
                                localOverrideOpacity_);
                            model.SelectObject(mask);
                            status_ =
                                "Exact authored biome mask created and selected.";
                        }
                        catch (const std::exception& exception)
                        {
                            status_ = exception.what();
                        }
                    }
                }

                context.Separator();
                auto settings = model.BiomeSettings(selectedBiome->id);
                bool basicSettingsChanged = false;

                basicSettingsChanged |= context.InputDouble(
                    "Surface Material Influence##m28-material-influence",
                    settings.materialInfluence);
                basicSettingsChanged |= context.InputDouble(
                    "Scatter Density Multiplier##m28-scatter-density",
                    settings.scatterDensityMultiplier);

                if (basicSettingsChanged)
                {
                    try
                    {
                        model.SetBiomeSettings(selectedBiome->id, settings);
                        status_ = "Biome material/scatter settings updated.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }

                context.Text(std::format(
                    "Surface Layers: {} | Scatter Rules: {}",
                    selectedBiome->surfaceLayers,
                    selectedBiome->scatterRules));

                if (context.Button("+ Moss Surface##m28-add-moss"))
                {
                    try
                    {
                        const auto layer = model.AddSurfaceLayer(
                            selectedBiome->id,
                            terrain_biome::BiomeSurfaceLayerKind::Moss);
                        model.SelectObject(layer);
                        status_ =
                            "Moss surface layer created; exact fields are visible in Properties.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }

                context.SameLine();

                if (context.Button("+ Tree Scatter##m28-add-tree-scatter"))
                {
                    try
                    {
                        const auto rule = model.AddScatterRule(
                            selectedBiome->id,
                            terrain_biome::BiomeScatterKind::Tree);
                        model.SelectObject(rule);
                        status_ =
                            "Tree scatter rule created; exact fields are visible in Properties.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ = exception.what();
                    }
                }

                context.Separator();
                static_cast<void>(context.Checkbox(
                    "Advanced Fields##m28-advanced-biome",
                    advancedBiome_));

                if (advancedBiome_)
                {
                    bool advancedChanged = false;

                    advancedChanged |= context.InputDouble(
                        "Minimum Resolved Weight##m28-min-weight",
                        settings.minimumResolvedWeight);
                    advancedChanged |= context.InputDouble(
                        "Hydraulic Multiplier##m28-hydraulic",
                        settings.hydraulicErosion);
                    advancedChanged |= context.InputDouble(
                        "Thermal Multiplier##m28-thermal",
                        settings.thermalTransport);
                    advancedChanged |= context.InputDouble(
                        "Aeolian Multiplier##m28-aeolian",
                        settings.aeolianTransport);
                    advancedChanged |= context.InputDouble(
                        "Glacial Multiplier##m28-glacial",
                        settings.glacialErosion);
                    advancedChanged |= context.InputDouble(
                        "Coastal Multiplier##m28-coastal",
                        settings.coastalErosion);
                    advancedChanged |= context.InputDouble(
                        "Chemical Weathering##m28-weathering",
                        settings.chemicalWeathering);

                    if (advancedChanged)
                    {
                        try
                        {
                            model.SetBiomeSettings(selectedBiome->id, settings);
                            status_ = "Advanced biome process fields updated.";
                        }
                        catch (const std::exception& exception)
                        {
                            status_ = exception.what();
                        }
                    }

                    context.Text("Exact Automatic Selectors");
                    for (const auto& selector : model.Selectors(selectedBiome->id))
                    {
                        context.Text(std::format(
                            "{}: [{:.4g}, {:.4g}] falloff [{:.4g}, {:.4g}]{}{}",
                            SelectorFieldName(selector.field),
                            selector.minimum,
                            selector.maximum,
                            selector.lowerFalloff,
                            selector.upperFalloff,
                            selector.invert ? " inverted" : "",
                            selector.enabled ? "" : " disabled"));
                    }

                    context.Text("Exact Authored Weights");
                    for (const auto& mask : model.Masks(selectedBiome->id))
                    {
                        context.Text(std::format(
                            "{}: value {:.4g}, opacity {:.4g}, radii {:.4g}-{:.4g} m{}",
                            MaskOperationName(mask.operation),
                            mask.value,
                            mask.opacity,
                            mask.innerRadiusMeters,
                            mask.outerRadiusMeters,
                            mask.enabled ? "" : " disabled"));
                    }
                }
            }
        }

        context.TreePop();
    }

    const auto cacheTree = context.TreeItem("Surface Cache / Debug##m28-cache", false);
    if (cacheTree.open)
    {
        const auto surfaceStats = world.SurfaceStats();
        const auto universeStats = world.UniverseStats();

        const auto terrainStatus =
            studio_session::StudioTerrainStatusInspector::Capture(
                *session,
                selected->terrain,
                nullptr,
                "studio.primary");

        const auto performance =
            session->
                TerrainPerformance().
                Capture(
                    *session,
                    "studio.primary");

        context.Text(std::format("Semantic Revision: {}", counts.semanticRevision));
        context.Text(std::format("Terrain Surfaces: {}", surfaceStats.terrainSurfaces));
        context.Text(std::format(
            "Biome Services: {} | Definitions: {}",
            surfaceStats.biomeServices,
            surfaceStats.biomeDefinitions));
        context.Text(std::format("Universe Bodies: {}", universeStats.bodies));

        if (terrainStatus.has_value())
        {
            context.Separator();
            context.Text("Live TerrainBodyServices");
            context.Text(std::format(
                "BodyId: {}",
                terrainStatus->body.ToString()));
            context.Text(std::format(
                "Terrain Object: {}",
                terrainStatus->terrainObject.ToString()));

            context.Text(std::format(
                "Authority Revisions | semantic {} | surface {} | geology {} | biome {}",
                terrainStatus->semanticRevision,
                terrainStatus->surfaceSourceRevision,
                terrainStatus->geologyRevision,
                terrainStatus->biomeRevision));

            if (terrainStatus->terrainSourceRevision.has_value())
            {
                context.Text(std::format(
                    "Terrain Source Revision: {}",
                    *terrainStatus->terrainSourceRevision));
            }

            if (terrainStatus->physicalRevisionFingerprint.has_value())
            {
                context.Text(std::format(
                    "Physical Revision Fingerprint: {}",
                    *terrainStatus->physicalRevisionFingerprint));
            }

            context.Separator();

            const std::string bedrockName =
                terrainStatus->defaultBedrockName.empty()
                    ? std::string("<unresolved reference record>")
                    : terrainStatus->defaultBedrockName;

            context.Text(std::format(
                "Default Bedrock: {} ({})",
                bedrockName,
                terrainStatus->defaultBedrock.ToString()));

            context.Text(std::format(
                "Selected-page Exposed Surface: {}",
                terrainStatus->exposedSurfaceAvailable
                    ? terrainStatus->exposedSurfaceName
                    : std::string("not published")));

            context.Text(std::format(
                "BaseBiome: {} ({}) | Optional Biomes: {}",
                terrainStatus->baseBiomeName,
                terrainStatus->baseBiome.ToString(),
                terrainStatus->optionalBiomeCount));

            context.Separator();
            context.Text("Process Service");
            context.Text(std::format(
                "Stream power {} iter | Hydraulic {} iter | Thermal {} max iter",
                terrainStatus->processes.streamPowerIterations,
                terrainStatus->processes.hydraulicIterations,
                terrainStatus->processes.thermalMaximumIterations));
            context.Text(std::format(
                "Aeolian {} iter | Glacial {} iter | Coastal {} / {} hydro steps",
                terrainStatus->processes.aeolianIterations,
                terrainStatus->processes.glacialIterations,
                terrainStatus->processes.coastalEnabled ? "enabled" : "disabled",
                terrainStatus->processes.coastalHydrodynamicSteps));
            context.Text(std::format(
                "Rivers: meanders {} ({} iter) | cutoffs {}",
                terrainStatus->processes.riverMeanders ? "on" : "off",
                terrainStatus->processes.riverMeanderIterations,
                terrainStatus->processes.riverCutoffs ? "on" : "off"));

            context.Separator();
            context.Text(std::format(
                "M26 Cache | resident {} pages / {:.2f} MiB | hits {} | misses {} | evictions {}",
                terrainStatus->cacheStats.residentPages,
                static_cast<double>(terrainStatus->cacheStats.residentBytes) /
                    (1024.0 * 1024.0),
                terrainStatus->cacheStats.hits,
                terrainStatus->cacheStats.misses,
                terrainStatus->cacheStats.evictions));

            if (terrainStatus->selectedPhysicalPage.has_value())
            {
                const auto& page =
                    *terrainStatus->selectedPhysicalPage;

                context.Text(std::format(
                    "Selected Physical Page | face {} | L{} | ({}, {}) | physical LOD {}",
                    CubeFaceName(page.tile.face),
                    page.tile.level,
                    page.tile.x,
                    page.tile.y,
                    terrainStatus->selectedPhysicalLod.value_or(0U)));

                if (!terrainStatus->selectedViewport.empty())
                {
                    context.Text(std::format(
                        "Viewport: {}",
                        terrainStatus->selectedViewport));
                }
            }
            else
            {
                context.Text("Selected Physical Page: no terrain viewport bound.");
            }

            if (terrainStatus->rebuildSchedulerAttached)
            {
                context.Text(std::format(
                    "M06 Rebuild | pages {} | dirty {} | queued {} | building {} | uploading {} | failed {}{}",
                    terrainStatus->rebuildPages,
                    terrainStatus->dirtyPages,
                    terrainStatus->queuedPages,
                    terrainStatus->buildingPages,
                    terrainStatus->uploadingPages,
                    terrainStatus->failedPages,
                    terrainStatus->regenerationPaused ? " | PAUSED" : ""));

                context.Text(std::format(
                    "Selected Rebuild State: {} | Last Regeneration: {}",
                    terrainStatus->selectedRebuildState.empty()
                        ? std::string("Clean/untracked")
                        : terrainStatus->selectedRebuildState,
                    terrainStatus->lastRegenerationReason.empty()
                        ? std::string("No completed invalidation yet")
                        : terrainStatus->lastRegenerationReason));
            }
            else
            {
                context.Text(
                    "M06 Rebuild: live physical scheduler not attached yet; M12 will connect production page generation.");
                context.Text(
                    "Last Regeneration: unavailable until the production scheduler owns resident physical pages.");
            }
        }
        else
        {
            context.Text(
                "No live TerrainBodyServices are composed for the selected Terrain Surface.");
        }

        context.Separator();
        context.Text("M16 Performance / Responsiveness");
        context.Text(std::format(
            "Build {} @ {} | {}",
            performance.engineVersion,
            performance.sourceCommit,
            performance.buildConfiguration));
        context.Text(std::format(
            "CPU: {}",
            performance.cpuName));
        context.Text(std::format(
            "GPU: {}",
            performance.gpuName.empty()
                ? std::string("no production terrain draw sampled")
                : performance.gpuName));

        context.Text(std::format(
            "Frame CPU | last {:.3f} ms | avg {:.3f} ms | max {:.3f} ms | {} samples",
            performance.lastFrameCpuMs,
            performance.averageFrameCpuMs,
            performance.maximumFrameCpuMs,
            performance.frameSamples));

        context.Text(std::format(
            "During regeneration | avg {:.3f} ms | max {:.3f} ms | {} samples",
            performance.averageRegeneratingFrameCpuMs,
            performance.maximumRegeneratingFrameCpuMs,
            performance.regeneratingFrameSamples));

        context.Text(std::format(
            "M06 pages | resident {} | outstanding {} | peak {} | queued {} | building {} | upload {} | stale {} | failed {} | rejected {}",
            performance.residentTrackedPages,
            performance.outstandingPages,
            performance.peakOutstandingPages,
            performance.queuedPages,
            performance.buildingPages,
            performance.uploadingPages,
            performance.stalePages,
            performance.failedPages,
            performance.staleRejected));

        context.Text(std::format(
            "Selected page | {} | edit->Ready {} | max {}{}",
            performance.selectedPageState.empty()
                ? std::string("untracked")
                : performance.selectedPageState,
            performance.selectedEditToReadyMs >= 0.0
                ? std::format(
                      "{:.3f} ms",
                      performance.selectedEditToReadyMs)
                : std::string("not sampled"),
            performance.maximumEditToReadyMs >= 0.0
                ? std::format(
                      "{:.3f} ms",
                      performance.maximumEditToReadyMs)
                : std::string("not sampled"),
            performance.selectedAwaitingReady
                ? " | WAITING"
                : ""));

        context.Text(std::format(
            "M26 cache | hit {:.2f}% | stationary {:.2f}% over {} frames | {:.2f} MiB resident",
            performance.cacheHitRatePercent,
            performance.stationaryCacheHitRatePercent,
            performance.stationaryFrames,
            static_cast<double>(
                performance.cacheStats.residentBytes) /
                (1024.0 * 1024.0)));

        context.Text(std::format(
            "Terrain viewport | draws {} | upload {} B/frame | generated {} samples | refreshed {} regions | pending {}",
            performance.streaming.drawCallsLastFrame,
            performance.streaming.uploadedBytesLastFrame,
            performance.streaming.generatedSamplesLastUpdate,
            performance.streaming.refreshedRegionsLastUpdate,
            performance.streaming.updatePending
                ? "yes"
                : "no"));

        context.Text(std::format(
            "Streaming totals | uploaded {:.2f} MiB | generated {} | batches {}/{} committed | superseded {} | stale revision {}",
            static_cast<double>(
                performance.streaming.cumulativeUploadedBytes) /
                (1024.0 * 1024.0),
            performance.streaming.cumulativeGeneratedSamples,
            performance.streaming.submittedBatches,
            performance.streaming.committedBatches,
            performance.streaming.supersededBatches,
            performance.streaming.staleRevisionBatches));

        const auto& m30 =
            performance.m30Reference;

        context.Text(std::format(
            "M30 reference | {} | {} | {} | {}x{} physical grid",
            m30.adapter,
            m30.buildConfiguration,
            m30.sourceCommit,
            m30.resolution,
            m30.resolution));

        context.Text(std::format(
            "M30 GPU | page {:.3f} ms | drainage {:.3f} ms | hydraulic {:.4f} ms/iter | aeolian {:.4f} ms/iter | scatter {:.4f} ms",
            m30.pageGenerationGpuMs,
            m30.drainageBuildGpuMs,
            m30.hydraulicIterationGpuMs,
            m30.aeolianIterationGpuMs,
            m30.scatterGenerationGpuMs));

        context.Text(std::format(
            "M30 memory/cache | peak transient {:.2f} MiB | persistent page {:.2f} KiB | hit {:.3f}%",
            static_cast<double>(
                m30.peakTransientBytes) /
                (1024.0 * 1024.0),
            static_cast<double>(
                m30.persistentPageBytes) /
                1024.0,
            m30.cacheHitRatePercent));

        context.Text(
            "Persistent terrain cache identity: physical page + physical LOD + authority revisions.");
        context.Text(
            "Dependency invalidation is source-domain and spatially bounded; M16 metrics are diagnostic only.");
        context.TreePop();
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}
} // namespace orbit::studio_ui
