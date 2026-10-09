#include "StudioWorkspaceRpc.hpp"
#include <orbit/documents/ProjectManifest.hpp>
#include <orbit/platform/Paths.hpp>
#include <orbit/studio_session/ProjectBrowserModel.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/ProjectAuthoringUi.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orbit::editor_app
{
void RegisterStudioProjectRpc(rpc::Dispatcher& dispatcher, studio_ui::ProjectAuthoringUi& projectBrowserUi)
{
            using orbit::rpc::Value;

            const auto requireString =
                [](const Value& params,
                   const std::string_view key)
            {
                const Value* value =
                    params.IsObject()
                        ? params.Find(key)
                        : nullptr;
                if (value == nullptr ||
                    !value->IsString() ||
                    value->AsString().empty())
                {
                    throw orbit::rpc::Error(
                        -32602,
                        std::string(key) +
                            " must be a non-empty string.");
                }
                return value->AsString();
            };

            const auto switchResult =
                [](const std::filesystem::path& manifestPath)
            {
                const auto manifest =
                    orbit::documents::LoadProjectManifest(
                        manifestPath);
                return Value(
                    Value::Object{
                        {"id", manifest.projectId.ToString()},
                        {"name", manifest.displayName},
                        {
                            "root",
                            manifestPath.parent_path().
                                generic_string()
                        },
                        {
                            "manifest",
                            manifestPath.generic_string()
                        },
                        {"relaunching", true}
                    });
            };

            dispatcher.Register(
                {
                    .name = "project.create",
                    .description =
                        "Creates a new Orbit project in `root` named `name` and switches Studio to it. Studio relaunches into the new project; reconnect and poll project.info until its id matches.",
                    .mutating = true
                },
                [&projectBrowserUi, requireString, switchResult](
                    const Value& params)
                {
                    const std::string name =
                        requireString(params, "name");
                    const std::filesystem::path root =
                        std::filesystem::absolute(
                            requireString(params, "root"));

                    try
                    {
                        return switchResult(
                            projectBrowserUi.CreateProject(
                                root,
                                name));
                    }
                    catch (const std::exception& exception)
                    {
                        throw orbit::rpc::Error(
                            1042,
                            exception.what());
                    }
                });

            dispatcher.Register(
                {
                    .name = "project.open",
                    .description =
                        "Opens an existing Orbit project directory or Project.orbit.toml and switches Studio to it. Studio relaunches into that project; reconnect and poll project.info until its id matches.",
                    .mutating = true
                },
                [&projectBrowserUi, requireString, switchResult](
                    const Value& params)
                {
                    try
                    {
                        return switchResult(
                            projectBrowserUi.OpenProject(
                                std::filesystem::absolute(
                                    requireString(
                                        params,
                                        "path"))));
                    }
                    catch (const orbit::rpc::Error&)
                    {
                        throw;
                    }
                    catch (const std::exception& exception)
                    {
                        throw orbit::rpc::Error(
                            1042,
                            exception.what());
                    }
                });

            dispatcher.Register(
                {
                    .name = "project.discover",
                    .description =
                        "Scans the usual folders (Documents, Desktop, Downloads, Orbit's Projects folder) for Orbit projects and lists them, newest first. Optional roots overrides the folders.",
                    .mutating = false
                },
                [](const Value& params)
                {
                    std::vector<std::filesystem::path> roots;
                    if (params.IsObject())
                    {
                        const auto found = params.AsObject().find("roots");
                        if (found != params.AsObject().end() &&
                            found->second.IsArray())
                        {
                            for (const auto& item : found->second.AsArray())
                            {
                                if (item.IsString())
                                {
                                    roots.emplace_back(item.AsString());
                                }
                            }
                        }
                    }
                    if (roots.empty())
                    {
                        roots = orbit::platform::UsualProjectFolders();
                    }

                    Value::Array items;
                    for (const auto& project :
                         orbit::studio_session::ProjectBrowserModel::
                             DiscoverProjects(roots))
                    {
                        items.push_back(
                            Value(
                                Value::Object{
                                    {"manifest",
                                     [&project]
                                     {
                                         const auto text =
                                             project.manifestPath.
                                                 generic_u8string();
                                         return std::string(
                                             text.begin(),
                                             text.end());
                                     }()},
                                    {"name", project.displayName}
                                }));
                    }
                    return Value(std::move(items));
                });

            dispatcher.Register(
                {
                    .name = "project.recent",
                    .description =
                        "Lists recently opened Orbit projects, most recent first.",
                    .mutating = false
                },
                [&projectBrowserUi](const Value&)
                {
                    Value::Array items;
                    for (const auto& item :
                         projectBrowserUi.RecentProjects())
                    {
                        items.push_back(
                            Value(
                                Value::Object{
                                    {
                                        "manifest",
                                        item.manifestPath.
                                            generic_string()
                                    },
                                    {"name", item.displayName},
                                    {
                                        "id",
                                        item.projectId.ToString()
                                    },
                                    {"available", item.available},
                                    {"error", item.error}
                                }));
                    }
                    return Value(std::move(items));
                });

}

void RegisterStudioWorkspaceRpc(rpc::Dispatcher& dispatcher, editor_ui::EditorUi& ui, studio_ui::StudioRenderViewSet& studioViews, studio_session::StudioSession& studioSession, studio_ui::StudioViewportPanels& studioViewportPanels)
{
            using orbit::rpc::Value;

            const auto panelTitle =
                [](const Value& params)
                {
                    const Value* title =
                        params.IsObject()
                            ? params.Find("title")
                            : nullptr;
                    if (title == nullptr ||
                        !title->IsString() ||
                        title->AsString().empty())
                    {
                        throw orbit::rpc::Error(
                            -32602,
                            "title must be a non-empty string.");
                    }
                    return title->AsString();
                };

            dispatcher.Register(
                {
                    .name = "studio.panel_list",
                    .description =
                        "Lists every editor panel (tab) with whether it is open and currently visible.",
                    .mutating = false
                },
                [&ui](const Value&)
                {
                    Value::Array panels;
                    for (const auto& panel : ui.Panels())
                    {
                        panels.emplace_back(
                            Value::Object{
                                {"title", panel.title},
                                {"open", panel.open},
                                {"visible", panel.visible}
                            });
                    }
                    return Value(std::move(panels));
                });

            dispatcher.Register(
                {
                    .name = "studio.panel_focus",
                    .description =
                        "Opens a panel by title (case-insensitive) and brings its tab to the front.",
                    .mutating = true
                },
                [&ui, panelTitle](const Value& params)
                {
                    const std::string title =
                        panelTitle(params);
                    if (!ui.FocusPanelByTitle(title))
                    {
                        throw orbit::rpc::Error(
                            1060,
                            "No panel is titled '" +
                                title +
                                "'. See studio.panel_list.");
                    }
                    return Value(
                        Value::Object{
                            {"title", title},
                            {"focused", true}
                        });
                });

            dispatcher.Register(
                {
                    .name = "viewport.navigate",
                    .description =
                        "Applies navigation steps to a viewport camera, exactly like the right-mouse look/WASD gesture: mouse_dx/mouse_dy in pixels (applied once, on the first step), move_right/move_forward/move_up in -1..1, delta_seconds (0..1 per step), boost. steps (default 1, up to 1200) repeats the step so one call can fly or dive a distance. id defaults to studio.primary. Uses terrain navigation when the body has terrain (ground clearance, the terrain bed as the floor) and free-fly otherwise. Returns moved, navigation, steps and, for a terrain view, distance_from_core_meters, height_above_terrain_meters and height_above_water_surface_meters (set where the ground is under water: negative when the camera is beneath the surface, positive above it; null over dry land). The readout is the live observer, current right after the last step.",
                    .mutating = true
                },
                [&studioViews](const Value& params)
                {
                    const auto number =
                        [&params](const char* key)
                        {
                            if (!params.IsObject())
                            {
                                return 0.0;
                            }
                            const auto found =
                                params.AsObject().find(key);
                            if (found == params.AsObject().end() ||
                                !found->second.IsNumber())
                            {
                                return 0.0;
                            }
                            return found->second.AsNumber();
                        };

                    const auto viewId =
                        [&params]() -> std::string
                        {
                            if (params.IsObject())
                            {
                                const auto found = params.AsObject().find("id");
                                if (found != params.AsObject().end() &&
                                    found->second.IsString() &&
                                    !found->second.AsString().empty())
                                {
                                    return found->second.AsString();
                                }
                            }
                            return "studio.primary";
                        }();
                    const f64 stepCount = params.IsObject() &&
                            params.AsObject().find("steps") != params.AsObject().end()
                        ? number("steps")
                        : 1.0;
                    if (!(stepCount >= 1.0 && stepCount <= 1200.0))
                    {
                        throw orbit::rpc::Error(
                            -32602, "steps must be within 1..1200.");
                    }

                    orbit::studio_ui::StudioTerrainNavigationInput input{
                        .deltaSeconds =
                            std::clamp(
                                number("delta_seconds"),
                                0.0,
                                1.0),
                        .mouseDeltaX = number("mouse_dx"),
                        .mouseDeltaY = number("mouse_dy"),
                        .moveRight =
                            std::clamp(number("move_right"), -1.0, 1.0),
                        .moveForward =
                            std::clamp(number("move_forward"), -1.0, 1.0),
                        .moveUp =
                            std::clamp(number("move_up"), -1.0, 1.0),
                        .boost =
                            params.IsObject() &&
                            params.AsObject().find("boost") !=
                                params.AsObject().end() &&
                            params.AsObject().at("boost").IsBool() &&
                            params.AsObject().at("boost").AsBool()
                    };

                    const bool terrainDriven =
                        studioViews.HasTerrainNavigation(viewId);
                    bool moved = false;
                    for (u32 step = 0U;
                         step < static_cast<u32>(stepCount);
                         ++step)
                    {
                        moved |= studioViews.NavigateTerrain(viewId, input);
                        // Look input is a one-shot delta, not a rate.
                        input.mouseDeltaX = 0.0;
                        input.mouseDeltaY = 0.0;
                    }
                    Value::Object result{
                        {"moved", moved},
                        {"navigation",
                         std::string(
                             terrainDriven
                                 ? "terrain"
                                 : "reference_sphere")},
                        {"steps", static_cast<i64>(stepCount)}};
                    if (terrainDriven)
                    {
                        // The live observer, not the last rendered frame.
                        if (const auto readout = studioViews.NavigationReadout(viewId);
                            readout.has_value())
                        {
                            result.emplace(
                                "distance_from_core_meters",
                                readout->distanceFromCoreMeters);
                            result.emplace(
                                "height_above_terrain_meters",
                                readout->heightAboveTerrainMeters);
                            result.emplace(
                                "height_above_water_surface_meters",
                                readout->heightAboveWaterSurfaceMeters.has_value()
                                    ? Value(*readout->heightAboveWaterSurfaceMeters)
                                    : Value());
                        }
                    }
                    return Value(std::move(result));
                });

            dispatcher.Register(
                {
                    .name = "viewport.focus_surface",
                    .description =
                        "Moves the viewport camera to a low vantage point over the terrain surface under the viewport position (u, v), each in 0..1 with (0, 0) at the top-left. The same operation as double-clicking the terrain in the viewport. Returns focused=false when that position does not hit terrain. id defaults to studio.primary.",
                    .mutating = true
                },
                [&studioViews](const Value& params)
                {
                    if (!params.IsObject())
                    {
                        throw orbit::rpc::Error(
                            -32602, "Params must be an object.");
                    }
                    const auto& object = params.AsObject();
                    const auto numberOf =
                        [&object](const char* key)
                        {
                            const auto found = object.find(key);
                            if (found == object.end() ||
                                !found->second.IsNumber())
                            {
                                throw orbit::rpc::Error(
                                    -32602,
                                    std::string(key) +
                                        " must be a number in 0..1.");
                            }
                            return found->second.AsNumber();
                        };
                    const auto idFound = object.find("id");
                    const std::string id =
                        idFound != object.end() && idFound->second.IsString()
                            ? idFound->second.AsString()
                            : std::string("studio.primary");

                    const bool focused =
                        studioViews.FocusTerrainSurfacePoint(
                            id,
                            static_cast<orbit::f32>(numberOf("u")),
                            static_cast<orbit::f32>(numberOf("v")));
                    return Value(
                        Value::Object{
                            {"focused", focused}
                        });
                });

            dispatcher.Register(
                {
                    .name = "view.mode_set",
                    .description =
                        "Sets a viewport's mode (perspective | body_map | debug | system | flat_map), like the viewport mode selector. id defaults to studio.primary.",
                    .mutating = true
                },
                [&studioSession](const Value& params)
                {
                    const auto& object = params.AsObject();
                    const auto idFound = object.find("id");
                    const std::string id =
                        idFound != object.end() && idFound->second.IsString()
                            ? idFound->second.AsString()
                            : std::string("studio.primary");
                    const auto modeFound = object.find("mode");
                    if (modeFound == object.end() ||
                        !modeFound->second.IsString())
                    {
                        throw orbit::rpc::Error(
                            -32602,
                            "mode must be perspective, body_map, debug, system or flat_map.");
                    }

                    const std::string& mode = modeFound->second.AsString();
                    orbit::studio_session::ViewportMode parsed{};
                    if (mode == "perspective")
                    {
                        parsed = orbit::studio_session::ViewportMode::Perspective;
                    }
                    else if (mode == "body_map")
                    {
                        parsed = orbit::studio_session::ViewportMode::BodyMap;
                    }
                    else if (mode == "debug")
                    {
                        parsed = orbit::studio_session::ViewportMode::Debug;
                    }
                    else if (mode == "system")
                    {
                        parsed = orbit::studio_session::ViewportMode::System;
                    }
                    else if (mode == "flat_map")
                    {
                        parsed = orbit::studio_session::ViewportMode::FlatMap;
                    }
                    else
                    {
                        throw orbit::rpc::Error(
                            -32602,
                            "mode must be perspective, body_map, debug, system or flat_map.");
                    }

                    studioSession.Viewports().SetMode(id, parsed);
                    return Value(
                        Value::Object{
                            {"id", id},
                            {"mode", mode}
                        });
                });

            dispatcher.Register(
                {
                    .name = "view.debug_field_set",
                    .description =
                        "Chooses which terrain data field a viewport shows in debug mode, by the name listed in the Debug tab (for example 'Drainage', 'Final Biome'). id defaults to studio.primary; call with no field to list names.",
                    .mutating = true
                },
                [&studioViews](const Value& params)
                {
                    const auto& object = params.AsObject();
                    const auto idFound = object.find("id");
                    const std::string id =
                        idFound != object.end() && idFound->second.IsString()
                            ? idFound->second.AsString()
                            : std::string("studio.primary");

                    Value::Array names;
                    for (const auto& entry :
                         orbit::terrain_debug::FieldCatalog())
                    {
                        names.emplace_back(std::string(entry.name));
                    }

                    const auto fieldFound = object.find("field");
                    if (fieldFound == object.end() ||
                        !fieldFound->second.IsString())
                    {
                        return Value(
                            Value::Object{{"fields", Value(std::move(names))}});
                    }

                    for (const auto& entry :
                         orbit::terrain_debug::FieldCatalog())
                    {
                        if (entry.name == fieldFound->second.AsString())
                        {
                            studioViews.SetDebugField(id, entry.field);
                            return Value(
                                Value::Object{
                                    {"id", id},
                                    {"field", std::string(entry.name)}
                                });
                        }
                    }

                    throw orbit::rpc::Error(
                        -32602,
                        "Unknown debug field. Call view.debug_field_set without field to list them.");
                });

            dispatcher.Register(
                {
                    .name = "studio.workspace_get",
                    .description =
                        "Returns the active workspace mode (Build, Planet, Universe, Simulation, Shading, Planning, Plugins).",
                    .mutating = false
                },
                [&studioViewportPanels](const Value&)
                {
                    return Value(
                        Value::Object{
                            {"mode",
                             std::string(
                                 studioViewportPanels.
                                     WorkspaceModeName())}
                        });
                });

            dispatcher.Register(
                {
                    .name = "studio.workspace_set",
                    .description =
                        "Switches the workspace mode (Build, Planet, Universe, Simulation, Shading, Planning, Plugins), exactly like the workspace tabs.",
                    .mutating = true
                },
                [&studioViewportPanels](const Value& params)
                {
                    const auto* mode =
                        params.IsObject()
                            ? params.AsObject().find("mode") !=
                                      params.AsObject().end()
                                  ? &params.AsObject().at("mode")
                                  : nullptr
                            : nullptr;
                    if (mode == nullptr || !mode->IsString() ||
                        !studioViewportPanels.SetWorkspaceMode(
                            mode->AsString()))
                    {
                        throw orbit::rpc::Error(
                            -32602,
                            "mode must be one of Build, Planet, Universe, Simulation, Shading, Planning, Plugins.");
                    }
                    return Value(
                        Value::Object{
                            {"mode", mode->AsString()}
                        });
                });

            dispatcher.Register(
                {
                    .name = "studio.bubble_open",
                    .description =
                        "Opens the parameter bubble of an object in the active toolbar (Scene: the selected object; Celestial: a body or one of its enabled capabilities).",
                    .mutating = true
                },
                [&studioViewportPanels](const Value& params)
                {
                    const auto* id =
                        params.IsObject()
                            ? params.AsObject().find("id") !=
                                      params.AsObject().end()
                                  ? &params.AsObject().at("id")
                                  : nullptr
                            : nullptr;
                    const auto parsed =
                        id != nullptr && id->IsString()
                            ? orbit::scene::ObjectId::Parse(
                                  id->AsString())
                            : std::nullopt;
                    if (!parsed.has_value())
                    {
                        throw orbit::rpc::Error(
                            -32602,
                            "id must be an object id string.");
                    }
                    studioViewportPanels.RequestElementBubble(
                        *parsed);
                    return Value(
                        Value::Object{
                            {"requested", true}
                        });
                });

            const auto snapResult = [&studioViewportPanels]()
            {
                const auto snap = studioViewportPanels.Snapping();
                return Value(Value::Object{
                    {"translation_enabled",snap.translationSnap},
                    {"surface_enabled",snap.surfaceSnap},
                    {"distance",snap.translationSnapMeters / studio_ui::SnapUnitMeters(snap.translationSnapUnit)},
                    {"unit",std::string(studio_ui::SnapUnitSymbol(snap.translationSnapUnit))},
                    {"distance_meters",snap.translationSnapMeters},
                    {"rotation_enabled",snap.rotationSnap},
                    {"rotation_degrees",snap.rotationSnapDegrees},
                    {"scale_enabled",snap.scaleSnap},
                    {"scale_percent",snap.scaleSnapStep * 100.0}
                });
            };
            dispatcher.Register({.name="viewport.snapping_get",
                .description="Read the shared gizmo snap distances, units, angles, and scale increments.",.mutating=false},
                [snapResult](const Value&) { return snapResult(); });
            dispatcher.Register({.name="viewport.snapping_set",
                .description="Update shared snapping. Distance defaults to meters; units mm/cm/m/km/in/ft. Rotation uses degrees and scale uses percent.",.mutating=true},
                [&studioViewportPanels,snapResult](const Value& params)
                {
                    if (!params.IsObject()) throw rpc::Error(-32602,"Snap settings must be an object.");
                    auto next = studioViewportPanels.Snapping();
                    const auto boolean = [&](std::string_view key, bool& target)
                    {
                        if (const auto* value=params.Find(key))
                        {
                            if (!value->IsBool()) throw rpc::Error(-32602,std::string(key)+" must be boolean.");
                            target=value->AsBool();
                        }
                    };
                    const auto number = [&](std::string_view key, f64& target)
                    {
                        if (const auto* value=params.Find(key))
                        {
                            if (!value->IsNumber()) throw rpc::Error(-32602,std::string(key)+" must be numeric.");
                            target=value->AsNumber();
                        }
                    };
                    boolean("translation_enabled",next.translationSnap);
                    boolean("surface_enabled",next.surfaceSnap);
                    boolean("rotation_enabled",next.rotationSnap);
                    boolean("scale_enabled",next.scaleSnap);
                    if (const auto* value=params.Find("unit"))
                    {
                        if (!value->IsString()) throw rpc::Error(-32602,"unit must be mm, cm, m, km, in, or ft.");
                        bool found=false;
                        for (u32 i=0;i<6;++i)
                            if (value->AsString()==studio_ui::SnapUnitSymbol(static_cast<studio_ui::SnapLengthUnit>(i)))
                            { next.translationSnapUnit=static_cast<studio_ui::SnapLengthUnit>(i); found=true; break; }
                        if (!found) throw rpc::Error(-32602,"unit must be mm, cm, m, km, in, or ft.");
                    }
                    else if (params.Find("distance")) next.translationSnapUnit=studio_ui::SnapLengthUnit::Meters;
                    if (params.Find("distance"))
                    {
                        f64 distance=0; number("distance",distance);
                        next.translationSnapMeters=distance * studio_ui::SnapUnitMeters(next.translationSnapUnit);
                    }
                    number("rotation_degrees",next.rotationSnapDegrees);
                    if (params.Find("scale_percent"))
                    {
                        f64 percent=0; number("scale_percent",percent); next.scaleSnapStep=percent/100.0;
                    }
                    try { studioViewportPanels.SetSnapping(next); }
                    catch (const std::invalid_argument& e) { throw rpc::Error(-32602,e.what()); }
                    return snapResult();
                });

            dispatcher.Register(
                {
                    .name = "viewport.frame_selected",
                    .description =
                        "Frame the selected object in the controlled perspective viewport, like the Scene toolbar and F shortcut.",
                    .mutating = true
                },
                [&studioViewportPanels](const Value&)
                {
                    return Value(Value::Object{
                        {"framed", studioViewportPanels.FrameSelectedObject()}
                    });
                });

            dispatcher.Register(
                {
                    .name = "studio.panel_close",
                    .description =
                        "Closes a panel by title (case-insensitive); it can be reopened with studio.panel_focus.",
                    .mutating = true
                },
                [&ui, panelTitle](const Value& params)
                {
                    const std::string title =
                        panelTitle(params);
                    if (!ui.ClosePanelByTitle(title))
                    {
                        throw orbit::rpc::Error(
                            1060,
                            "No panel is titled '" +
                                title +
                                "'. See studio.panel_list.");
                    }
                    return Value(
                        Value::Object{
                            {"title", title},
                            {"closed", true}
                        });
                });

}


} // namespace orbit::editor_app
