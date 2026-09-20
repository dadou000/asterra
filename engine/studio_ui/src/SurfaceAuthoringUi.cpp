#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_session/StudioTerrainServiceStatus.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <string_view>

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

void SurfaceAuthoringUi::Register(editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Surface Authoring",
        .defaultOpen = true,
        .draw = [this](editor_ui::PanelContext& context)
        {
            Draw(context);
        }
    });
}

void SurfaceAuthoringUi::Draw(editor_ui::PanelContext& context)
{
    if (workspace_ == nullptr || !workspace_->HasProject())
    {
        context.Text("Open a project to author planetary surfaces.");
        return;
    }

    auto& world = workspace_->Session().World();
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
        context.Text(std::format("Semantic Geology Assets: {}", counts.geologyAssets));
        context.TreePop();
    }

    const auto processTree = context.TreeItem("Terrain Processes##m28-processes", false);
    if (processTree.open)
    {
        context.Text("Implemented Process Systems");
        context.Text("- Stream power");
        context.Text("- Water / hydraulic erosion");
        context.Text("- Wind / aeolian erosion");
        context.Text("- Unified sediment exchange");
        context.Text("- Thermal / gravity");
        context.Text("- Glacial");
        context.Text("- River network / meanders");
        context.Text("- Coastal");
        context.Text(std::format("Semantic Process Assets: {}", counts.processAssets));
        context.Text(
            "Solver constants remain out of the ordinary workflow; semantic process assets remain available through Properties.");
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
                workspace_->Session(),
                selected->terrain,
                nullptr,
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

        context.Text(
            "Persistent terrain cache identity: physical page + physical LOD + authority revisions.");
        context.Text(
            "Dependency invalidation is source-domain and spatially bounded; this panel is diagnostic only.");
        context.TreePop();
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}
} // namespace orbit::studio_ui
