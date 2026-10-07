#pragma once

// Project build operations of Orbit Studio: the validate / cook / package actions, the build.* RPC methods and the Build
// panel. They share one BuildService and one set of results (issues, status, last manifest/executable), which is why they
// live together. Moved out of Main.cpp's frame setup, bodies unchanged.

#include "StudioPanels.hpp"

#include <orbit/build/BuildService.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace orbit::editor_app
{
class StudioBuildHost : public StudioPanelBase
{
public:
    explicit StudioBuildHost(StudioPanelEnvironment& environment);

    // Registers the build.* RPC methods on the session host.
    void AttachRpc();

    // Registers the Build panel.
    void Register();

    void ValidateProjectBuild();
    void CookProjectBuild();
    [[nodiscard]] build::PackageResult PackageProjectBuild(const std::string& requestedProfile);

    // Build state shared by the panel, the menu actions and the RPC methods.
    orbit::build::BuildService
        buildService;
    std::string selectedBuildProfile =
        project.Manifest().
                buildProfiles.empty()
            ? std::string{}
            : project.Manifest().
                  buildProfiles.front().
                  name;
    std::vector<orbit::build::BuildIssue>
        buildIssues;
    std::filesystem::path
        lastBuildManifest;
    std::filesystem::path
        lastPackageExecutable;
    std::string buildStatus{
        "Not run"};

private:
    [[nodiscard]] static rpc::Value buildIssuesToRpc(const std::vector<build::BuildIssue>& issues);

    editor_rpc::EditorSessionRpcHost& rpcHost;
};
} // namespace orbit::editor_app
