from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def patch_build_service() -> None:
    path = "engine/build/src/BuildService.cpp"
    text = read(path)

    text = replace_once(
        text,
        "#include <orbit/plugins/PluginManifest.hpp>\n",
        "#include <orbit/platform_services/PlatformConfig.hpp>\n"
        "#include <orbit/plugins/PluginManifest.hpp>\n",
        "BuildService platform include",
    )

    old_storefront = '''    if (profile->storefront !=
        "standalone")
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.storefront.unavailable",
            "Only standalone packaging is available before M22 platform services.",
            project.ManifestPath());
    }
'''

    new_storefront = '''    const bool steamStorefront =
        profile->storefront == "steam";

    if (profile->storefront !=
            "standalone" &&
        !steamStorefront)
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.storefront.unsupported",
            "Unsupported storefront: " +
                profile->storefront,
            project.ManifestPath());
    }

    const auto platformConfigurationPath =
        result.projectRoot /
        "Config" /
        "PlatformServices.toml";

    if (std::filesystem::is_regular_file(
            platformConfigurationPath))
    {
        try
        {
            const auto platformConfiguration =
                platform_services::
                    LoadPlatformConfiguration(
                        platformConfigurationPath);

            for (const auto& issue :
                 platform_services::
                     ValidatePlatformConfiguration(
                         platformConfiguration,
                         steamStorefront))
            {
                AddIssue(
                    result.issues,
                    IssueSeverity::Error,
                    "build." + issue.code,
                    issue.message,
                    platformConfigurationPath);
            }
        }
        catch (const std::exception& exception)
        {
            AddIssue(
                result.issues,
                IssueSeverity::Error,
                "build.platform_services.invalid",
                exception.what(),
                platformConfigurationPath);
        }
    }
    else if (steamStorefront)
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.platform_services.missing",
            "Steam build profiles require Config/PlatformServices.toml.",
            platformConfigurationPath);
    }
'''
    text = replace_once(
        text,
        old_storefront,
        new_storefront,
        "BuildService storefront validation",
    )

    profile_assignment = '''        result.manifest.profile =
            validation.profile;

        const auto worldSource =
'''
    profile_assignment_new = '''        result.manifest.profile =
            validation.profile;

        const auto platformConfigurationSource =
            validation.projectRoot /
            "Config" /
            "PlatformServices.toml";

        if (std::filesystem::is_regular_file(
                platformConfigurationSource))
        {
            std::filesystem::copy_file(
                platformConfigurationSource,
                staging /
                    "PlatformServices.toml",
                std::filesystem::
                    copy_options::
                        overwrite_existing);
        }

        const auto worldSource =
'''
    text = replace_once(
        text,
        profile_assignment,
        profile_assignment_new,
        "BuildService platform config cook",
    )

    package_names = '''        std::set<std::string>
            packagedRuntimeNames{
                executableName
            };

        for (const auto& source :
'''
    package_names_new = '''        std::set<std::string>
            packagedRuntimeNames{
                executableName
            };

        if (result.manifest.profile.storefront ==
            "steam")
        {
            const auto steamRuntime =
                runtime.playerExecutable.
                    parent_path() /
                "steam_api64.dll";

            if (!std::filesystem::is_regular_file(
                    steamRuntime))
            {
                throw std::runtime_error(
                    "Steam package requires steam_api64.dll beside OrbitPlayer.exe. Configure Orbit with ORBIT_STEAMWORKS_SDK_ROOT before packaging a Steam profile.");
            }

            packagedRuntimeNames.insert(
                "steam_api64.dll");

            std::filesystem::copy_file(
                steamRuntime,
                result.outputDirectory /
                    "steam_api64.dll",
                std::filesystem::
                    copy_options::
                        overwrite_existing);
        }

        for (const auto& source :
'''
    text = replace_once(
        text,
        package_names,
        package_names_new,
        "BuildService Steam runtime packaging",
    )

    write(path, text)


def patch_build_cmake() -> None:
    path = "engine/build/CMakeLists.txt"
    text = read(path)
    marker = "        Orbit::Plugins\n"
    text = replace_once(
        text,
        marker,
        marker + "        Orbit::PlatformServices\n",
        "Build CMake platform services link",
    )
    write(path, text)


def patch_build_cli() -> None:
    path = "apps/build/src/Main.cpp"
    text = read(path)

    pattern = re.compile(
        r"\n\[\[nodiscard\]\] std::vector<\n    std::filesystem::path>\nFindPackageRuntimeFiles\(.*?\n}\n\nvoid PrintIssues\(",
        re.S,
    )
    text, count = pattern.subn("\nvoid PrintIssues(", text, count=1)
    if count != 1:
        raise RuntimeError(f"OrbitBuild runtime helper: expected one match, found {count}")

    runtime_block = '''                        .playerExecutable =
                            playerExecutable,
                        .runtimeFiles =
                            FindPackageRuntimeFiles(
                                options.manifestPath,
                                playerExecutable)
'''
    runtime_new = '''                        .playerExecutable =
                            playerExecutable
'''
    text = replace_once(
        text,
        runtime_block,
        runtime_new,
        "OrbitBuild package runtime delegation",
    )

    write(path, text)


def patch_build_tests() -> None:
    path = "engine/build/tests/BuildServiceTests.cpp"
    text = read(path)
    text = replace_once(
        text,
        "#include <orbit/documents/ProjectDocument.hpp>\n",
        "#include <orbit/documents/ProjectDocument.hpp>\n"
        "#include <orbit/platform_services/PlatformConfig.hpp>\n",
        "BuildServiceTests platform include",
    )

    marker = '''        orbit::build::BuildRequest
            protectedRequest = request;
'''
    insertion = '''        orbit::platform_services::
            PlatformConfiguration steamConfiguration;
        steamConfiguration.steam.enabled = true;
        steamConfiguration.steam.appId = 480;
        steamConfiguration.steam.achievements.
            push_back({
                .id = "garage.first_repair",
                .steamApiName =
                    "ACH_FIRST_REPAIR"
            });
        steamConfiguration.steam.stats.
            push_back({
                .id = "vehicles.repaired",
                .kind = orbit::platform_services::
                    StatKind::Integer,
                .steamApiName =
                    "STAT_VEHICLES_REPAIRED"
            });
        steamConfiguration.steam.timelineEvents.
            push_back({
                .eventId = "vehicle.repaired",
                .title = "Vehicle repaired",
                .description =
                    "Completed a repair.",
                .icon = "steam_achievement",
                .priority = 10,
                .clipPriority =
                    orbit::platform_services::
                        TimelineClipPriority::Standard
            });
        steamConfiguration.eventRules.
            push_back({
                .eventId = "vehicle.repaired",
                .incrementStat =
                    "vehicles.repaired",
                .statDelta = 1.0,
                .unlockAchievement =
                    "garage.first_repair",
                .unlockAtStatValue = 1.0,
                .emitTimeline = true
            });

        orbit::platform_services::
            SavePlatformConfigurationAtomic(
                temporary.Root() /
                    "Config/PlatformServices.toml",
                steamConfiguration);

        project.Manifest().buildProfiles.
            push_back({
                .name = "Shipping Steam",
                .configuration = "Shipping",
                .platform = "Windows",
                .storefront = "steam"
            });
        project.Save();

        orbit::build::BuildRequest
            steamRequest{
                .manifestPath =
                    project.ManifestPath(),
                .profileName =
                    "Shipping Steam"
            };

        const auto steamValidation =
            service.Validate(
                steamRequest);

        ORBIT_TEST_CHECK(
            steamValidation.Succeeded());
        ORBIT_TEST_CHECK(
            steamValidation.profile.storefront ==
            "steam");

        const auto steamCook =
            service.Cook(
                steamRequest);

        ORBIT_TEST_CHECK(
            steamCook.Succeeded());
        ORBIT_TEST_CHECK(
            std::filesystem::is_regular_file(
                steamCook.outputDirectory /
                "PlatformServices.toml"));

        WriteText(
            dummyPlayer.parent_path() /
                "steam_api64.dll",
            "steam-runtime-test-binary");

        const auto steamPackage =
            service.Package(
                steamRequest,
                {
                    .playerExecutable =
                        dummyPlayer
                });

        ORBIT_TEST_CHECK(
            steamPackage.Succeeded());
        ORBIT_TEST_CHECK(
            std::filesystem::is_regular_file(
                steamPackage.outputDirectory /
                "steam_api64.dll"));
        ORBIT_TEST_CHECK(
            std::filesystem::is_regular_file(
                steamPackage.outputDirectory /
                "PlatformServices.toml"));

        orbit::build::BuildRequest
            protectedRequest = request;
'''
    text = replace_once(
        text,
        marker,
        insertion,
        "BuildServiceTests Steam profile coverage",
    )
    write(path, text)


def patch_editor_cmake() -> None:
    path = "apps/editor/CMakeLists.txt"
    text = read(path)
    marker = "        Orbit::Platform\n"
    text = replace_once(
        text,
        marker,
        marker + "        Orbit::PlatformServices\n",
        "OrbitStudio platform services link",
    )
    write(path, text)


def patch_editor() -> None:
    path = "apps/editor/src/Main.cpp"
    text = read(path)

    text = replace_once(
        text,
        "#include <orbit/platform/Paths.hpp>\n",
        "#include <orbit/platform/Paths.hpp>\n"
        "#include <orbit/platform_services/PlatformConfig.hpp>\n",
        "OrbitStudio platform services include",
    )

    build_panel = '''        constexpr orbit::editor_ui::PanelId
            kBuildPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f4255494c44ULL
            };
'''
    platform_panel = build_panel + '''
        constexpr orbit::editor_ui::PanelId
            kPlatformServicesPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f504c415446ULL
            };
'''
    text = replace_once(
        text,
        build_panel,
        platform_panel,
        "OrbitStudio Platform Services panel ID",
    )

    build_state = '''        std::string buildStatus{
            "Not run"};

        std::vector<orbit::editor_ui::PanelId>
'''
    platform_state = '''        std::string buildStatus{
            "Not run"};

        const std::filesystem::path
            platformConfigurationPath =
                project.RootDirectory() /
                "Config" /
                "PlatformServices.toml";
        orbit::platform_services::
            PlatformConfiguration
                platformConfiguration;
        std::vector<
            orbit::platform_services::
                PlatformConfigIssue>
            platformConfigurationIssues;
        std::string platformStatus{
            "Not configured"};
        orbit::i64 steamAppIdEditor = 0;
        std::string newAchievementId;
        std::string newAchievementApiName;
        std::string newStatId;
        std::string newStatApiName;
        bool newStatFloat = false;
        std::string newTimelineEventId;
        std::string newTimelineTitle;
        std::string newTimelineDescription;
        std::string newTimelineIcon{
            "steam_marker"};

        if (std::filesystem::is_regular_file(
                platformConfigurationPath))
        {
            try
            {
                platformConfiguration =
                    orbit::platform_services::
                        LoadPlatformConfiguration(
                            platformConfigurationPath);
                steamAppIdEditor =
                    static_cast<orbit::i64>(
                        platformConfiguration.
                            steam.appId);
                platformConfigurationIssues =
                    orbit::platform_services::
                        ValidatePlatformConfiguration(
                            platformConfiguration,
                            false);
                platformStatus =
                    platformConfigurationIssues.
                            empty()
                        ? "Loaded"
                        : "Loaded with validation issues";
            }
            catch (const std::exception&
                       exception)
            {
                platformStatus =
                    std::string(
                        "Load failed: ") +
                    exception.what();
            }
        }

        std::vector<orbit::editor_ui::PanelId>
'''
    text = replace_once(
        text,
        build_state,
        platform_state,
        "OrbitStudio platform config state",
    )

    build_registration = '''        ui.RegisterPanel({
            .id = kBuildPanel,
'''
    platform_registration = '''        ui.RegisterPanel({
            .id = kPlatformServicesPanel,
            .title = "Platform Services",
            .defaultOpen = false,
            .draw =
                [&project,
                 &platformConfiguration,
                 &platformConfigurationIssues,
                 &platformConfigurationPath,
                 &platformStatus,
                 &steamAppIdEditor,
                 &newAchievementId,
                 &newAchievementApiName,
                 &newStatId,
                 &newStatApiName,
                 &newStatFloat,
                 &newTimelineEventId,
                 &newTimelineTitle,
                 &newTimelineDescription,
                 &newTimelineIcon](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    context.Text(
                        "Provider-neutral IDs remain project authority; storefront mappings live here.");
                    context.Separator();

                    static_cast<void>(
                        context.Checkbox(
                            "Enable Steam",
                            platformConfiguration.
                                steam.enabled));
                    static_cast<void>(
                        context.InputInteger(
                            "Steam App ID",
                            steamAppIdEditor));

                    context.Text(
                        std::format(
                            "Achievements: {}  Stats: {}  Timeline events: {}",
                            platformConfiguration.
                                steam.achievements.
                                size(),
                            platformConfiguration.
                                steam.stats.size(),
                            platformConfiguration.
                                steam.timelineEvents.
                                size()));

                    context.Separator();
                    context.Text(
                        "Achievement mapping");
                    static_cast<void>(
                        context.InputText(
                            "Orbit Achievement ID",
                            newAchievementId));
                    static_cast<void>(
                        context.InputText(
                            "Steam Achievement API",
                            newAchievementApiName));
                    if (context.Button(
                            "Add Achievement Mapping") &&
                        !newAchievementId.empty() &&
                        !newAchievementApiName.empty())
                    {
                        platformConfiguration.
                            steam.achievements.
                            push_back({
                                .id =
                                    newAchievementId,
                                .steamApiName =
                                    newAchievementApiName
                            });
                        newAchievementId.clear();
                        newAchievementApiName.clear();
                    }

                    context.Separator();
                    context.Text("Stat mapping");
                    static_cast<void>(
                        context.InputText(
                            "Orbit Stat ID",
                            newStatId));
                    static_cast<void>(
                        context.InputText(
                            "Steam Stat API",
                            newStatApiName));
                    static_cast<void>(
                        context.Checkbox(
                            "Float Stat",
                            newStatFloat));
                    if (context.Button(
                            "Add Stat Mapping") &&
                        !newStatId.empty() &&
                        !newStatApiName.empty())
                    {
                        platformConfiguration.
                            steam.stats.push_back({
                                .id = newStatId,
                                .kind = newStatFloat
                                    ? orbit::platform_services::
                                        StatKind::Float
                                    : orbit::platform_services::
                                        StatKind::Integer,
                                .steamApiName =
                                    newStatApiName
                            });
                        newStatId.clear();
                        newStatApiName.clear();
                    }

                    context.Separator();
                    context.Text(
                        "Timeline event mapping");
                    static_cast<void>(
                        context.InputText(
                            "Semantic Event ID",
                            newTimelineEventId));
                    static_cast<void>(
                        context.InputText(
                            "Timeline Title",
                            newTimelineTitle));
                    static_cast<void>(
                        context.InputText(
                            "Timeline Description",
                            newTimelineDescription));
                    static_cast<void>(
                        context.InputText(
                            "Timeline Icon",
                            newTimelineIcon));
                    if (context.Button(
                            "Add Timeline Mapping") &&
                        !newTimelineEventId.empty())
                    {
                        platformConfiguration.
                            steam.timelineEvents.
                            push_back({
                                .eventId =
                                    newTimelineEventId,
                                .title =
                                    newTimelineTitle,
                                .description =
                                    newTimelineDescription,
                                .icon =
                                    newTimelineIcon.empty()
                                        ? "steam_marker"
                                        : newTimelineIcon
                            });
                        newTimelineEventId.clear();
                        newTimelineTitle.clear();
                        newTimelineDescription.clear();
                        newTimelineIcon =
                            "steam_marker";
                    }

                    context.Separator();

                    if (context.Button(
                            "Validate Configuration"))
                    {
                        if (steamAppIdEditor < 0 ||
                            steamAppIdEditor >
                                4294967295LL)
                        {
                            platformStatus =
                                "Steam App ID is outside the uint32 range";
                        }
                        else
                        {
                            platformConfiguration.
                                steam.appId =
                                    static_cast<
                                        orbit::u32>(
                                            steamAppIdEditor);
                            platformConfigurationIssues =
                                orbit::platform_services::
                                    ValidatePlatformConfiguration(
                                        platformConfiguration,
                                        platformConfiguration.
                                            steam.enabled);
                            platformStatus =
                                platformConfigurationIssues.
                                        empty()
                                    ? "Configuration valid"
                                    : std::format(
                                        "{} validation issue{}",
                                        platformConfigurationIssues.
                                            size(),
                                        platformConfigurationIssues.
                                                size() == 1U
                                            ? ""
                                            : "s");
                        }
                    }

                    context.SameLine();

                    if (context.Button(
                            "Save Platform Config"))
                    {
                        if (steamAppIdEditor < 0 ||
                            steamAppIdEditor >
                                4294967295LL)
                        {
                            platformStatus =
                                "Steam App ID is outside the uint32 range";
                        }
                        else
                        {
                            platformConfiguration.
                                steam.appId =
                                    static_cast<
                                        orbit::u32>(
                                            steamAppIdEditor);
                            orbit::platform_services::
                                SavePlatformConfigurationAtomic(
                                    platformConfigurationPath,
                                    platformConfiguration);
                            platformConfigurationIssues =
                                orbit::platform_services::
                                    ValidatePlatformConfiguration(
                                        platformConfiguration,
                                        platformConfiguration.
                                            steam.enabled);
                            platformStatus =
                                platformConfigurationIssues.
                                        empty()
                                    ? "Saved"
                                    : std::format(
                                        "Saved with {} validation issue{}",
                                        platformConfigurationIssues.
                                            size(),
                                        platformConfigurationIssues.
                                                size() == 1U
                                            ? ""
                                            : "s");
                        }
                    }

                    if (context.Button(
                            "Add Shipping Steam Profile"))
                    {
                        const auto found =
                            std::ranges::find_if(
                                project.Manifest().
                                    buildProfiles,
                                [](const auto& profile)
                                {
                                    return profile.storefront ==
                                        "steam";
                                });

                        if (found ==
                            project.Manifest().
                                buildProfiles.end())
                        {
                            project.Manifest().
                                buildProfiles.push_back({
                                    .name =
                                        "Shipping Steam",
                                    .configuration =
                                        "Shipping",
                                    .platform =
                                        "Windows",
                                    .storefront =
                                        "steam"
                                });
                            project.Save();
                            platformStatus =
                                "Added Shipping Steam build profile";
                        }
                        else
                        {
                            platformStatus =
                                "Steam build profile already exists";
                        }
                    }

                    context.Separator();
                    context.Text(
                        std::format(
                            "Status: {}",
                            platformStatus));

                    for (const auto& issue :
                         platformConfigurationIssues)
                    {
                        context.Text(
                            std::format(
                                "[{}] {}",
                                issue.code,
                                issue.message));
                    }
                }
        });

        ui.RegisterPanel({
            .id = kBuildPanel,
'''
    text = replace_once(
        text,
        build_registration,
        platform_registration,
        "OrbitStudio Platform Services panel registration",
    )

    text = text.replace(
        '"Package Standalone"',
        '"Package Project"',
    )
    text = text.replace(
        '"Standalone package succeeded: {}"',
        '"Project package succeeded: {}"',
    )
    text = text.replace(
        '"Standalone package failed."',
        '"Project package failed."',
    )

    write(path, text)


def patch_ci() -> None:
    path = ".github/workflows/orbit-windows.yml"
    text = read(path)
    text = replace_once(
        text,
        "OrbitBuildServiceTests OrbitRuntimeProjectTests OrbitBuildCliTests",
        "OrbitBuildServiceTests OrbitRuntimeProjectTests OrbitBuildCliTests OrbitPlatformServicesTests OrbitPlatformServicesSteamTests",
        "CI platform test targets",
    )

    marker = '''          & "build/apps/build/Release/OrbitBuildCliTests.exe" "build/apps/build/Release/OrbitBuild.exe"
          if ($LASTEXITCODE -ne 0) {
            throw "OrbitBuildCliTests failed with exit code $LASTEXITCODE"
          }
'''
    marker_new = marker + '''
          & "build/engine/platform_services/Release/OrbitPlatformServicesTests.exe"
          if ($LASTEXITCODE -ne 0) {
            throw "OrbitPlatformServicesTests failed with exit code $LASTEXITCODE"
          }

          & "build/engine/platform_services/steam/Release/OrbitPlatformServicesSteamTests.exe"
          if ($LASTEXITCODE -ne 0) {
            throw "OrbitPlatformServicesSteamTests failed with exit code $LASTEXITCODE"
          }
'''
    text = replace_once(
        text,
        marker,
        marker_new,
        "CI platform test execution",
    )

    text = text.replace(
        "Assemble a standalone project package with:",
        "Assemble a project package with:",
    )
    text = text.replace(
        "OrbitPlayer.exe is the project-driven runtime copied into standalone packages.",
        "OrbitPlayer.exe is the project-driven runtime copied into project packages.",
    )
    write(path, text)


def patch_spec() -> None:
    path = "docs/V0.0.3_SPEC.md"
    text = read(path)
    text = replace_once(
        text,
        "Status: **active implementation — M21 in progress**",
        "Status: **active implementation — M22 implemented; integration proof in progress**",
        "spec headline status",
    )
    old_m21 = "| M21 — Headless BuildService | **Partial** | Shared `Orbit::Build` + `OrbitBuild.exe` now validate profiles/project boundaries, resolve plugin manifests/dependencies without editor/UI dependencies, rebuild importer-backed target DDC products with canonical settings, package every project Luau module while preserving entry points, checkpoint/cook from Studio, and expose `build.profiles` / `build.validate` / `build.cook` over JSON-RPC and MCP. Deterministic clean-DDC regression and Windows CI gating are in-tree. Shader cooking, broader runtime asset importers, a real project-driven player/runtime loader, standalone package assembly, and final clean-machine acceptance remain. |"
    new_m21 = "| M21 — Headless BuildService | **Implemented** | Shared `Orbit::Build`, `OrbitBuild.exe`, DXC shader cooking, target DDC cooking, deterministic Luau bytecode packaging, cooked-project runtime loading, `OrbitPlayer.exe`, Studio/CLI package assembly and build RPC/MCP are in-tree. Remaining importer breadth and permutation expansion are normal pipeline growth rather than scaffold blockers. |"
    text = replace_once(text, old_m21, new_m21, "spec M21 status")

    old_m22 = "| M22 — Platform services and Steam backend | Planned | Not started. |"
    new_m22 = "| M22 — Platform services and Steam backend | **Implemented** | Provider-neutral platform registry, semantic GameEvent routing, standalone stats/achievements/timeline backend, persistent platform mappings, isolated Steam provider + optional Steamworks bridge, Steam build-profile validation/package rules, OrbitPlayer provider selection, Studio Platform Services configuration panel, and dedicated CI tests are in-tree. |"
    text = replace_once(text, old_m22, new_m22, "spec M22 status")

    old_m23 = "| M23 — V0.0.3 integration proof | Planned | Final cross-system proof after preceding milestones. |"
    new_m23 = "| M23 — V0.0.3 integration proof | **Partial** | The permanent scaffold is now wired end-to-end through Studio, CLI, runtime, RPC/MCP, project documents, paths, materials/plugins and platform services. Final example-project proof and remaining visual/editor acceptance cases are tracked here rather than represented by placeholder systems. |"
    text = replace_once(text, old_m23, new_m23, "spec M23 status")

    checklist = '''- [ ] target shader/permutation cooking;
- [ ] complete runtime asset importer coverage;
- [ ] project-driven cooked-manifest runtime/player;
- [ ] standalone Windows package assembly;
- [ ] final clean-machine acceptance proof.'''
    checklist_new = '''- [x] target HLSL shader cooking through DXC to SPIR-V;
- [ ] complete runtime asset importer coverage as new asset classes are added;
- [x] project-driven cooked-manifest runtime/player;
- [x] standalone Windows package assembly;
- [x] clean-machine Windows CI rebuild path; final V0.0.3 cross-system example proof remains M23.'''
    text = replace_once(text, checklist, checklist_new, "spec M21 checkpoint")

    m22_header = '''### M22 — Platform services and Steam backend

Deliver:
'''
    m22_checkpoint = '''### M22 — Platform services and Steam backend

Current implementation checkpoint:

- [x] provider-neutral `Orbit::PlatformServices` registry and capabilities;
- [x] semantic GameEvent rules for stats, achievements and timeline events;
- [x] standalone backend with no Steam dependency;
- [x] stable Orbit ID to Steam API-name mappings persisted in `Config/PlatformServices.toml`;
- [x] isolated Steam provider plus optional native Steamworks bridge;
- [x] Steam build-profile validation and `steam_api64.dll` package enforcement;
- [x] OrbitPlayer runtime provider selection;
- [x] Orbit Studio Platform Services panel for App ID, enablement and mapping creation/validation;
- [x] provider and mapping tests integrated into Windows CI.

Deliver:
'''
    text = replace_once(text, m22_header, m22_checkpoint, "spec M22 checkpoint")
    write(path, text)


def main() -> None:
    patch_build_service()
    patch_build_cmake()
    patch_build_cli()
    patch_build_tests()
    patch_editor_cmake()
    patch_editor()
    patch_ci()
    patch_spec()


if __name__ == "__main__":
    main()
