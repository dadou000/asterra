#include <orbit/studio_ui/VolumeAuthoringUi.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <array>
#include <exception>
#include <format>
#include <optional>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::optional<scene::ObjectId>
SelectedVolumeId(
    editor_session::EditorWorldSession& world)
{
    const auto& selected =
        world.Selection().Ordered();

    if (selected.size() != 1U)
    {
        return std::nullopt;
    }

    const auto record =
        world.Objects().Find(
            selected.front());

    if (!record.has_value())
    {
        return std::nullopt;
    }

    if (record->type ==
        world_model::kVolumeType)
    {
        return record->id;
    }

    if ((record->type ==
             world_model::kVolumeSourceType ||
         record->type ==
             world_model::kVolumeEffectorType) &&
        record->parent.has_value())
    {
        const auto parent =
            world.Objects().Find(
                *record->parent);

        if (parent.has_value() &&
            parent->type ==
                world_model::kVolumeType)
        {
            return parent->id;
        }
    }

    return std::nullopt;
}
} // namespace

VolumeAuthoringUi::VolumeAuthoringUi(
    studio_session::StudioSession& session,
    volume_fields::VolumeFieldStorageService& fields,
    StudioViewportRenderer& renderer) noexcept
    : session_(&session),
      fields_(&fields),
      renderer_(&renderer)
{
}

void VolumeAuthoringUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Volumes",
        .defaultOpen = false,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 35,
        .minSize = {
            .width = 320.0F,
            .height = 300.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void VolumeAuthoringUi::Draw(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        context.MutedText(
            "Open a world to author volumes.");
        return;
    }

    auto& world =
        session_->World();

    context.Heading("Create Volume");
    context.MutedText(
        "Presets create ordinary editable Volume domains.");

    constexpr std::array presets{
        "Empty",
        "Smoke",
        "Fire",
        "Fog",
        "Dust",
        "Snow",
        "Surface Flow"
    };

    for (std::size_t index = 0U;
         index < presets.size();
         ++index)
    {
        const std::string label =
            std::string(presets[index]) +
            "##volume-preset-" +
            std::to_string(index);

        if (context.Button(label))
        {
            try
            {
                commands::CommandArguments args;
                args.emplace(
                    "preset",
                    std::string(
                        presets[index]));

                world.CommandRegistry().Invoke(
                    editor_model::
                        authoring_commands::
                            kCreateVolume,
                    args);

                status_ =
                    std::string(presets[index]) +
                    " Volume created.";
            }
            catch (const std::exception& exception)
            {
                status_ =
                    exception.what();
            }
        }

        if (index + 1U < presets.size())
        {
            context.SameLine();
        }
    }

    const auto volumeId =
        SelectedVolumeId(world);

    if (volumeId.has_value())
    {
        const auto volume =
            world_model::ResolveVolumeDomain(
                world.Objects(),
                *volumeId);

        if (volume.has_value())
        {
            context.Separator();
            context.Heading("Selected Volume");

            context.Text(
                std::format(
                    "{} | {} | fields 0x{:X}",
                    world_model::
                        VolumeSolverPolicyName(
                            volume->solverPolicy),
                    world_model::
                        VolumeRepresentationModeName(
                            volume->representationMode),
                    volume->fieldMask));

            if (fields_ != nullptr)
            {
                fields_->RemoveMissing(
                    world.Objects());

                auto& storage =
                    fields_->Ensure(
                        *volume);

                const u32 invalidated =
                    fields_->SyncAuthoredInputs(
                        world.Objects(),
                        *volumeId);

                const auto& diagnostics =
                    storage.Diagnostics();

                context.Text(
                    std::format(
                        "GPU fields {:.2f} MiB | {}^3 cells | tiles {}x{}x{} @ {}^3",
                        static_cast<double>(
                            diagnostics.totalBytes) /
                            (1024.0 * 1024.0),
                        diagnostics.resolution,
                        diagnostics.tilesX,
                        diagnostics.tilesY,
                        diagnostics.tilesZ,
                        diagnostics.tileEdge));

                context.Text(
                    std::format(
                        "Residency {} | valid {} | pending {} | source invalidation {} tile{}",
                        diagnostics.residentTiles,
                        diagnostics.validTiles,
                        diagnostics.pendingTiles,
                        invalidated,
                        invalidated == 1U ? "" : "s"));
            }

            context.Separator();
            context.Heading("Sources");

            constexpr std::array sourceKinds{
                "Brush",
                "Texture / Mask",
                "Terrain Paint",
                "Spline",
                "Mesh / SDF",
                "Collision Proxy",
                "Particles",
                "Object Motion",
                "World Motion"
            };

            for (std::size_t index = 0U;
                 index < sourceKinds.size();
                 ++index)
            {
                const std::string label =
                    "+ " +
                    std::string(
                        sourceKinds[index]) +
                    "##volume-source-add-" +
                    std::to_string(index);

                if (context.Button(label))
                {
                    try
                    {
                        commands::CommandArguments args;
                        args.emplace(
                            "kind",
                            std::string(
                                sourceKinds[index]));

                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kAddVolumeSource,
                            args);

                        status_ =
                            std::string(
                                sourceKinds[index]) +
                            " source added.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ =
                            exception.what();
                    }
                }

                if (index + 1U < sourceKinds.size())
                {
                    context.SameLine();
                }
            }

            context.Heading("Effectors");

            constexpr std::array effectorKinds{
                "Obstacle",
                "Drag",
                "Wind",
                "Temperature",
                "Dissipation"
            };

            for (std::size_t index = 0U;
                 index < effectorKinds.size();
                 ++index)
            {
                const std::string label =
                    "+ " +
                    std::string(
                        effectorKinds[index]) +
                    "##volume-effector-add-" +
                    std::to_string(index);

                if (context.Button(label))
                {
                    try
                    {
                        commands::CommandArguments args;
                        args.emplace(
                            "kind",
                            std::string(
                                effectorKinds[index]));

                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kAddVolumeEffector,
                            args);

                        status_ =
                            std::string(
                                effectorKinds[index]) +
                            " effector added.";
                    }
                    catch (const std::exception& exception)
                    {
                        status_ =
                            exception.what();
                    }
                }

                if (index + 1U < effectorKinds.size())
                {
                    context.SameLine();
                }
            }

            const auto inputs =
                world_model::ResolveVolumeInputs(
                    world.Objects(),
                    *volumeId);

            const auto& selection =
                world.Selection().Ordered();
            const auto selectedInput =
                selection.size() == 1U
                    ? std::optional<scene::ObjectId>(
                          selection.front())
                    : std::nullopt;

            context.Separator();
            context.Heading("Evaluation Order");

            for (const auto& input :
                 inputs)
            {
                const std::string kindName =
                    input.role ==
                            world_model::
                                VolumeInputRole::Source
                        ? std::string(
                              world_model::
                                  VolumeSourceKindName(
                                      static_cast<
                                          world_model::
                                              VolumeSourceKind>(
                                          input.kind)))
                        : std::string(
                              world_model::
                                  VolumeEffectorKindName(
                                      static_cast<
                                          world_model::
                                              VolumeEffectorKind>(
                                          input.kind)));

                const std::string label =
                    std::format(
                        "{:02} {} | {} | {}##volume-input-{}",
                        input.order,
                        input.role ==
                                world_model::
                                    VolumeInputRole::Source
                            ? "Source"
                            : "Effector",
                        kindName,
                        world_model::
                            VolumeSourceShapeName(
                                input.shape),
                        input.object.ToString());

                if (context.Selectable(
                        label,
                        selectedInput.has_value() &&
                            *selectedInput ==
                                input.object))
                {
                    const scene::ObjectId selected[]{
                        input.object};
                    world.Selection().Set(
                        selected);
                }
            }

            if (selectedInput.has_value())
            {
                const auto selectedRecord =
                    world.Objects().Find(
                        *selectedInput);

                if (selectedRecord.has_value() &&
                    (selectedRecord->type ==
                         world_model::
                             kVolumeSourceType ||
                     selectedRecord->type ==
                         world_model::
                             kVolumeEffectorType))
                {
                    if (context.Button(
                            "Up##volume-input-up"))
                    {
                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kMoveVolumeInputUp);
                    }

                    context.SameLine();

                    if (context.Button(
                            "Down##volume-input-down"))
                    {
                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kMoveVolumeInputDown);
                    }

                    context.SameLine();

                    if (context.Button(
                            "Remove##volume-input-remove"))
                    {
                        world.CommandRegistry().Invoke(
                            editor_model::
                                authoring_commands::
                                    kRemoveVolumeInput);
                    }
                }
            }

            context.Separator();
            context.Heading("Terrain Source Painting");
            context.MutedText(
                "Each stroke is an authored TerrainPatch source with a stable object ID. Viewport tools/MCP may invoke the same command with picked world coordinates.");

            static_cast<void>(
                context.InputDouble3(
                    "World Position##volume-paint-position",
                    paintPosition_));
            static_cast<void>(
                context.InputDouble(
                    "Radius m##volume-paint-radius",
                    paintRadius_));
            static_cast<void>(
                context.InputDouble(
                    "Strength##volume-paint-strength",
                    paintStrength_));

            if (context.PrimaryButton(
                    "Paint Terrain Source Stroke##volume-paint-stroke"))
            {
                try
                {
                    commands::CommandArguments args;
                    args.emplace(
                        "position",
                        paintPosition_);
                    args.emplace(
                        "radius",
                        paintRadius_);
                    args.emplace(
                        "strength",
                        paintStrength_);

                    world.CommandRegistry().Invoke(
                        editor_model::
                            authoring_commands::
                                kPaintVolumeTerrainSource,
                        args);

                    status_ =
                        "Terrain source stroke authored.";
                }
                catch (const std::exception& exception)
                {
                    status_ =
                        exception.what();
                }
            }

            if (renderer_ != nullptr)
            {
                bool debug =
                    renderer_->
                        VolumeSourceDebugVisualization();

                if (context.Checkbox(
                        "Show All Source / Effector Gizmos##volume-source-debug",
                        debug))
                {
                    renderer_->
                        SetVolumeSourceDebugVisualization(
                            debug);
                }
            }

            context.MutedText(
                "Exact source geometry, field mask, asset/path, target object and values remain editable in the normal Properties Inspector.");

            const auto selectedRecord =
                world.Selection().Ordered().size() == 1U
                    ? world.Objects().Find(
                          world.Selection().Ordered().front())
                    : std::optional<scene::ObjectRecord>{};

            if (selectedRecord.has_value() &&
                selectedRecord->type ==
                    world_model::kVolumeType &&
                context.Button(
                    "Remove Selected Volume##volume-remove"))
            {
                try
                {
                    world.CommandRegistry().Invoke(
                        editor_model::
                            authoring_commands::
                                kRemoveVolume);
                    status_ =
                        "Volume removed.";
                }
                catch (const std::exception& exception)
                {
                    status_ =
                        exception.what();
                }
            }
        }
    }

    if (!status_.empty())
    {
        context.Separator();
        context.Text(status_);
    }
}
} // namespace orbit::studio_ui
