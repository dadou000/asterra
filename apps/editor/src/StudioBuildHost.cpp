#include "StudioBuildHost.hpp"

#include "EditorAppSupport.hpp"

#include <orbit/build/BuildService.hpp>
#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/content_wic/WicTextureImporter.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/core/ThreadName.hpp>
#include <orbit/dev_server/DevServer.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_model/OutputLog.hpp>
#include <orbit/editor_model/ShortcutRegistry.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/editor_rpc/ProfilerRpc.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/lighting/LightingScheduler.hpp>
#include <orbit/lighting/MaterialEmission.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_geometry/PathSource.hpp>
#include <orbit/path_routing/RouteDomains.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/platform/FileBrowser.hpp>
#include <orbit/platform/FileDialog.hpp>
#include <orbit/platform/Paths.hpp>
#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/profiler/Profiler.hpp>
#include <orbit/render_graph/GpuPassTimer.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/Capture.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/runtime/RuntimeSession.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/shading/MaterialThumbnailCache.hpp>
#include <orbit/shading/ShaderPreviewRenderer.hpp>
#include <orbit/shading/ShadingCapture.hpp>
#include <orbit/shading/ShadingRpc.hpp>
#include <orbit/shading/ShadingWorkspace.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
#include <orbit/studio_session/StudioTerrainValidationScenario.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/studio_ui/CelestialAuthoringUi.hpp>
#include <orbit/studio_ui/DebugViewUi.hpp>
#include <orbit/studio_ui/DisplayDiagnosticsUi.hpp>
#include <orbit/studio_ui/DisplayEyeRpc.hpp>
#include <orbit/studio_ui/ProfilerUi.hpp>
#include <orbit/studio_ui/ProjectAuthoringUi.hpp>
#include <orbit/studio_ui/ProjectSettingsUi.hpp>
#include <orbit/studio_ui/ReportThumbnailCache.hpp>
#include <orbit/studio_ui/ReportsUi.hpp>
#include <orbit/studio_ui/ShadingUi.hpp>
#include <orbit/studio_ui/SimulationControlsUi.hpp>
#include <orbit/studio_ui/StudioFlatMapRpc.hpp>
#include <orbit/studio_ui/StudioRenderViewRpc.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioTextDiagnosticsHud.hpp>
#include <orbit/studio_ui/StudioViewContinuity.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/studio_ui/SurfaceAuthoringUi.hpp>
#include <orbit/studio_ui/SystemViewUi.hpp>
#include <orbit/studio_ui/ViewportCaptureService.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/studio_ui/WorldDocumentsUi.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/universe/ReferenceSurface.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::editor_app
{
using namespace support;

StudioBuildHost::StudioBuildHost(StudioPanelEnvironment& environment)
    : StudioPanelBase(environment),
      rpcHost(environment.studioSession.Rpc())
{
}

rpc::Value StudioBuildHost::buildIssuesToRpc(const std::vector<build::BuildIssue>& issues)
{
    orbit::rpc::Value::Array
        result;

    result.reserve(
        issues.size());

    for (const auto& issue :
         issues)
    {
        result.emplace_back(
            orbit::rpc::Value::Object{
                {
                    "severity",
                    issue.severity ==
                            orbit::build::
                                IssueSeverity::Error
                        ? "error"
                        : "warning"
                },
                {"code", issue.code},
                {
                    "message",
                    issue.message
                },
                {
                    "path",
                    issue.path.
                        generic_string()
                }
            });
    }

    return orbit::rpc::Value(
        std::move(result));
}

void StudioBuildHost::ValidateProjectBuild()
{
    try
    {
        const auto result =
            buildService.Validate({
                .manifestPath =
                    project.ManifestPath(),
                .profileName =
                    selectedBuildProfile
            });

        buildIssues =
            result.issues;

        if (result.Succeeded())
        {
            buildStatus =
                std::format(
                    "Validated {} / {} / {}",
                    result.profile.platform,
                    result.profile.configuration,
                    result.profile.storefront);

            orbit::log::Info(
                std::format(
                    "Build validation succeeded for profile '{}'.",
                    result.profile.name));
        }
        else
        {
            buildStatus =
                "Validation failed";
            orbit::log::Error(
                "Project build validation failed.");
        }
    }
    catch (const std::exception&
               exception)
    {
        buildIssues = {
            {
                .severity =
                    orbit::build::
                        IssueSeverity::Error,
                .code =
                    "studio.build.exception",
                .message =
                    exception.what()
            }
        };
        buildStatus =
            "Validation failed";
        orbit::log::Error(
            exception.what());
    }
}

void StudioBuildHost::CookProjectBuild()
{
    try
    {
        // Studio builds always package the currently authored
        // semantic state, never a stale pre-edit checkpoint.
        project.Save();
        if (worldSession.HasWorld())
        {
            world().Checkpoint();
        }

        const auto result =
            buildService.Cook({
                .manifestPath =
                    project.ManifestPath(),
                .profileName =
                    selectedBuildProfile
            });

        buildIssues =
            result.issues;

        if (result.Succeeded())
        {
            lastBuildManifest =
                result.manifestPath;
            buildStatus =
                std::format(
                    "Cooked {} asset{} and {} script{}",
                    result.manifest.
                        assets.size(),
                    result.manifest.
                        assets.size() == 1U
                        ? ""
                        : "s",
                    result.manifest.
                        scripts.size(),
                    result.manifest.
                        scripts.size() == 1U
                        ? ""
                        : "s");

            orbit::log::Info(
                std::format(
                    "Build cook succeeded: {}",
                    result.manifestPath.
                        string()));
        }
        else
        {
            buildStatus =
                "Cook failed";
            orbit::log::Error(
                "Project build cook failed.");
        }
    }
    catch (const std::exception&
               exception)
    {
        buildIssues = {
            {
                .severity =
                    orbit::build::
                        IssueSeverity::Error,
                .code =
                    "studio.build.exception",
                .message =
                    exception.what()
            }
        };
        buildStatus =
            "Cook failed";
        orbit::log::Error(
            exception.what());
    }
}

build::PackageResult StudioBuildHost::PackageProjectBuild(const std::string& requestedProfile)
{
    project.Save();
    if (worldSession.HasWorld())
    {
        world().Checkpoint();
    }

    const auto result =
        buildService.Package(
            {
                .manifestPath =
                    project.ManifestPath(),
                .profileName =
                    requestedProfile
            },
            {
                .playerExecutable =
                    FindPlayerExecutable()
            });

    buildIssues =
        result.issues;

    if (result.Succeeded())
    {
        selectedBuildProfile =
            result.manifest.
                profile.name;
        lastBuildManifest =
            result.manifestPath;
        lastPackageExecutable =
            result.executablePath;
        buildStatus =
            std::format(
                "Packaged {}",
                result.executablePath.
                    filename().
                    string());

        orbit::log::Info(
            std::format(
                "Project package succeeded: {}",
                result.outputDirectory.
                    string()));
    }
    else
    {
        buildStatus =
            "Package failed";
        orbit::log::Error(
            "Project package failed.");
    }

    return result;
}

void StudioBuildHost::AttachRpc()
{
    rpcHost.AttachBuild({
        .profiles =
            [this]
            {
                orbit::rpc::Value::Array
                    profiles;

                for (const auto& profile :
                     project.Manifest().
                         buildProfiles)
                {
                    profiles.emplace_back(
                        orbit::rpc::Value::Object{
                            {
                                "name",
                                profile.name
                            },
                            {
                                "configuration",
                                profile.
                                    configuration
                            },
                            {
                                "platform",
                                profile.platform
                            },
                            {
                                "storefront",
                                profile.storefront
                            }
                        });
                }

                return orbit::rpc::Value(
                    std::move(
                        profiles));
            },
        .validate =
            [this](
                std::optional<
                    std::string>
                    profile)
            {
                const std::string
                    requestedProfile =
                        profile.
                            value_or(
                                std::string{});

                const auto result =
                    buildService.Validate({
                        .manifestPath =
                            project.
                                ManifestPath(),
                        .profileName =
                            requestedProfile
                    });

                buildIssues =
                    result.issues;

                if (result.Succeeded())
                {
                    selectedBuildProfile =
                        result.profile.name;
                    buildStatus =
                        std::format(
                            "Validated {} / {} / {}",
                            result.profile.platform,
                            result.profile.
                                configuration,
                            result.profile.
                                storefront);
                }
                else
                {
                    buildStatus =
                        "Validation failed";
                }

                return orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "ok",
                            result.Succeeded()
                        },
                        {
                            "profile",
                            result.profile.name
                        },
                        {
                            "configuration",
                            result.profile.
                                configuration
                        },
                        {
                            "platform",
                            result.profile.
                                platform
                        },
                        {
                            "storefront",
                            result.profile.
                                storefront
                        },
                        {
                            "output",
                            result.
                                outputDirectory.
                                generic_string()
                        },
                        {
                            "issues",
                            buildIssuesToRpc(
                                result.issues)
                        }
                    });
            },
        .cook =
            [this](
                std::optional<
                    std::string>
                    profile)
            {
                try
                {
                    project.Save();
                    if (worldSession.HasWorld())
                {
                    world().Checkpoint();
                }

                    const std::string
                        requestedProfile =
                            profile.
                                value_or(
                                    std::string{});

                    const auto result =
                        buildService.Cook({
                            .manifestPath =
                                project.
                                    ManifestPath(),
                            .profileName =
                                requestedProfile
                        });

                    buildIssues =
                        result.issues;

                    if (result.Succeeded())
                    {
                        selectedBuildProfile =
                            result.manifest.
                                profile.name;
                        lastBuildManifest =
                            result.manifestPath;
                        buildStatus =
                            std::format(
                                "Cooked {} asset{} and {} script{}",
                                result.manifest.
                                    assets.size(),
                                result.manifest.
                                    assets.size() == 1U
                                    ? ""
                                    : "s",
                                result.manifest.
                                    scripts.size(),
                                result.manifest.
                                    scripts.size() == 1U
                                    ? ""
                                    : "s");
                    }
                    else
                    {
                        buildStatus =
                            "Cook failed";
                    }

                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "ok",
                                result.
                                    Succeeded()
                            },
                            {
                                "profile",
                                result.manifest.
                                    profile.name
                            },
                            {
                                "output",
                                result.
                                    outputDirectory.
                                    generic_string()
                            },
                            {
                                "manifest",
                                result.
                                    manifestPath.
                                    generic_string()
                            },
                            {
                                "assets",
                                static_cast<
                                    orbit::i64>(
                                        result.
                                            manifest.
                                            assets.
                                            size())
                            },
                            {
                                "scripts",
                                static_cast<
                                    orbit::i64>(
                                        result.
                                            manifest.
                                            scripts.
                                            size())
                            },
                            {
                                "issues",
                                buildIssuesToRpc(
                                    result.
                                        issues)
                            }
                        });
                }
                catch (const std::exception&
                           exception)
                {
                    throw orbit::rpc::Error(
                        1100,
                        exception.what());
                }
            },
        .package =
            [this](
                std::optional<
                    std::string>
                    profile)
            {
                try
                {
                    const auto result =
                        PackageProjectBuild(
                            profile.
                                value_or(
                                    std::string{}));

                    return orbit::rpc::Value(
                        orbit::rpc::Value::Object{
                            {
                                "ok",
                                result.
                                    Succeeded()
                            },
                            {
                                "profile",
                                result.manifest.
                                    profile.name
                            },
                            {
                                "output",
                                result.
                                    outputDirectory.
                                    generic_string()
                            },
                            {
                                "executable",
                                result.
                                    executablePath.
                                    generic_string()
                            },
                            {
                                "build_manifest",
                                result.
                                    manifestPath.
                                    generic_string()
                            },
                            {
                                "package_manifest",
                                result.
                                    packageManifestPath.
                                    generic_string()
                            },
                            {
                                "issues",
                                buildIssuesToRpc(
                                    result.issues)
                            }
                        });
                }
                catch (const std::exception&
                           exception)
                {
                    throw orbit::rpc::Error(
                        1101,
                        exception.what());
                }
            }
    });
}

void StudioBuildHost::Register()
{
    ui.RegisterPanel({
        .id = kBuildPanel,
        .title = "Build",
        .defaultOpen = true,
        .defaultDock = orbit::editor_ui::DockRegion::Bottom,
        .dockOrder = 10,
        .draw =
            [this](
                orbit::editor_ui::
                    PanelContext& context)
            {
                context.Text(
                    project.Manifest().
                        displayName);
                context.Separator();

                const auto& profiles =
                    project.Manifest().
                        buildProfiles;

                if (profiles.empty())
                {
                    context.Text(
                        "No build profiles are defined.");
                }
                else
                {
                    context.Text(
                        "Build profile");

                    for (std::size_t index = 0;
                         index < profiles.size();
                         ++index)
                    {
                        const auto& profile =
                            profiles[index];

                        const bool selected =
                            profile.name ==
                            selectedBuildProfile;

                        const std::string button =
                            std::format(
                                "{}##build-profile-{}",
                                selected
                                    ? "Selected"
                                    : "Use",
                                index);

                        if (context.Button(
                                button))
                        {
                            selectedBuildProfile =
                                profile.name;
                        }

                        context.SameLine();
                        context.Text(
                            std::format(
                                "{} — {} / {} / {}",
                                profile.name,
                                profile.platform,
                                profile.configuration,
                                profile.storefront));
                    }
                }

                context.Separator();

                if (context.Button(
                        "Validate Project"))
                {
                    ValidateProjectBuild();
                }

                context.SameLine();

                if (context.Button(
                        "Cook Project"))
                {
                    CookProjectBuild();
                }

                context.SameLine();

                if (context.Button(
                        "Package Project"))
                {
                    static_cast<void>(
                        PackageProjectBuild(
                            selectedBuildProfile));
                }

                context.Separator();
                context.Text(
                    "Status: " +
                    buildStatus);

                if (!lastBuildManifest.empty())
                {
                    context.Text(
                        "Manifest: " +
                        lastBuildManifest.
                            generic_string());
                }

                if (!lastPackageExecutable.empty())
                {
                    context.Text(
                        "Executable: " +
                        lastPackageExecutable.
                            generic_string());
                }

                for (const auto& issue :
                     buildIssues)
                {
                    context.Text(
                        std::format(
                            "{} [{}] {}{}",
                            issue.severity ==
                                    orbit::build::
                                        IssueSeverity::Error
                                ? "ERROR"
                                : "WARN",
                            issue.code,
                            issue.message,
                            issue.path.empty()
                                ? std::string{}
                                : std::format(
                                      " ({})",
                                      issue.path.
                                          generic_string())));
                }
            }
    });
}
} // namespace orbit::editor_app
