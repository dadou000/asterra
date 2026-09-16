#include <orbit/build/BuildService.hpp>

#include <orbit/core/BuildInfo.hpp>
#include <orbit/documents/ProjectDocument.hpp>

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace orbit::build
{
namespace
{
[[nodiscard]] bool HasErrors(
    const std::vector<BuildIssue>& issues) noexcept
{
    return std::ranges::any_of(
        issues,
        [](const BuildIssue& issue)
        {
            return issue.severity ==
                IssueSeverity::Error;
        });
}

void AddIssue(
    std::vector<BuildIssue>& issues,
    const IssueSeverity severity,
    std::string code,
    std::string message,
    std::filesystem::path path = {})
{
    issues.push_back({
        .severity = severity,
        .code = std::move(code),
        .message = std::move(message),
        .path = std::move(path)
    });
}

[[nodiscard]] std::filesystem::path
AbsoluteNormalized(
    const std::filesystem::path& path)
{
    return std::filesystem::absolute(
        path).lexically_normal();
}

[[nodiscard]] bool IsInside(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    const auto relative =
        AbsoluteNormalized(candidate).
            lexically_relative(
                AbsoluteNormalized(root));

    if (relative.empty())
    {
        return false;
    }

    const auto first =
        relative.begin();

    return first == relative.end() ||
        *first != "..";
}

[[nodiscard]] bool IsProtectedOutput(
    const std::filesystem::path& output,
    const std::filesystem::path& projectRoot)
{
    const auto normalizedOutput =
        AbsoluteNormalized(output);
    const auto normalizedRoot =
        AbsoluteNormalized(projectRoot);

    if (normalizedOutput ==
        normalizedRoot)
    {
        return true;
    }

    constexpr std::string_view
        kProtectedDirectories[]{
            "Content",
            "Worlds",
            "Scripts",
            "Plugins",
            "Config",
            ".orbit"
        };

    for (const std::string_view directory :
         kProtectedDirectories)
    {
        if (IsInside(
                normalizedOutput,
                normalizedRoot /
                    std::filesystem::path(
                        directory)))
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]] std::string
SanitizeProfileName(
    const std::string_view name)
{
    std::string result;
    result.reserve(name.size());

    for (const unsigned char value : name)
    {
        const char character =
            static_cast<char>(value);

        if (std::isalnum(value) != 0 ||
            character == '-' ||
            character == '_')
        {
            result.push_back(character);
        }
        else
        {
            result.push_back('_');
        }
    }

    if (result.empty())
    {
        result = "Build";
    }

    return result;
}

[[nodiscard]] const documents::BuildProfile*
FindProfile(
    const documents::ProjectManifest& manifest,
    const std::string_view requested)
{
    if (requested.empty())
    {
        return manifest.buildProfiles.empty()
            ? nullptr
            : &manifest.buildProfiles.front();
    }

    const auto found =
        std::ranges::find(
            manifest.buildProfiles,
            requested,
            &documents::BuildProfile::name);

    return found == manifest.buildProfiles.end()
        ? nullptr
        : &*found;
}

[[nodiscard]] bool IsConfigurationSupported(
    const std::string_view configuration) noexcept
{
    return configuration == "Debug" ||
        configuration == "Development" ||
        configuration == "Shipping";
}

[[nodiscard]] std::filesystem::path
ResolveProjectFile(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& relative)
{
    if (relative.empty() ||
        relative.is_absolute())
    {
        throw std::invalid_argument(
            "Project file paths must be non-empty and relative.");
    }

    const auto absolute =
        std::filesystem::weakly_canonical(
            projectRoot /
            relative);

    const auto projectRelative =
        absolute.lexically_relative(
            std::filesystem::weakly_canonical(
                projectRoot));

    if (projectRelative.empty() ||
        *projectRelative.begin() == "..")
    {
        throw std::invalid_argument(
            "Project file path escapes the project root: " +
            relative.generic_string());
    }

    if (!std::filesystem::is_regular_file(
            absolute))
    {
        throw std::runtime_error(
            "Project file does not exist: " +
            relative.generic_string());
    }

    return absolute;
}

[[nodiscard]] std::string ReadText(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);

    if (!input)
    {
        throw std::runtime_error(
            "Unable to open build input: " +
            path.string());
    }

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void WriteBytes(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes)
{
    std::filesystem::create_directories(
        path.parent_path());

    std::ofstream output(
        path,
        std::ios::binary |
            std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Unable to create build product: " +
            path.string());
    }

    output.write(
        reinterpret_cast<const char*>(
            bytes.data()),
        static_cast<std::streamsize>(
            bytes.size()));

    if (!output)
    {
        throw std::runtime_error(
            "Unable to write build product: " +
            path.string());
    }
}

void WriteText(
    const std::filesystem::path& path,
    const std::string_view text)
{
    const auto bytes =
        std::as_bytes(
            std::span(
                text.data(),
                text.size()));

    WriteBytes(
        path,
        bytes);
}

[[nodiscard]] bool IsZeroHash(
    const content::ContentHash& hash) noexcept
{
    return std::ranges::all_of(
        hash.Bytes(),
        [](const std::byte value)
        {
            return value == std::byte{0};
        });
}

[[nodiscard]] bool IsSemanticRuntimeAsset(
    const content::AssetKind kind) noexcept
{
    return kind ==
            content::AssetKind::Component ||
        kind ==
            content::AssetKind::Decal ||
        kind ==
            content::AssetKind::PathProfile;
}

[[nodiscard]] std::filesystem::path
SemanticOutputPath(
    const std::filesystem::path& sourcePath)
{
    const auto relative =
        sourcePath.lexically_relative(
            "Content");

    if (relative.empty() ||
        *relative.begin() == "..")
    {
        return
            std::filesystem::path(
                "Content") /
            sourcePath.filename();
    }

    return
        std::filesystem::path(
            "Content") /
        relative;
}

[[nodiscard]] std::string
CompileScript(
    const std::string_view source,
    const std::filesystem::path& sourcePath,
    const std::string_view configuration)
{
    Luau::CompileOptions options{};

    if (configuration == "Shipping")
    {
        options.optimizationLevel = 2;
        options.debugLevel = 0;
    }
    else if (configuration ==
             "Development")
    {
        options.optimizationLevel = 1;
        options.debugLevel = 1;
    }
    else
    {
        options.optimizationLevel = 0;
        options.debugLevel = 1;
    }

    const std::string bytecode =
        Luau::compile(
            source,
            options);

    lua_State* state =
        luaL_newstate();

    if (state == nullptr)
    {
        throw std::runtime_error(
            "Luau validation VM allocation failed.");
    }

    const int loadResult =
        luau_load(
            state,
            sourcePath.
                generic_string().
                c_str(),
            bytecode.data(),
            bytecode.size(),
            0);

    std::string error;

    if (loadResult != 0)
    {
        const char* message =
            lua_tostring(
                state,
                -1);

        error =
            message != nullptr
                ? message
                : "unknown Luau compiler error";
    }

    lua_close(state);

    if (loadResult != 0)
    {
        throw std::runtime_error(
            "Luau compile failed for " +
            sourcePath.generic_string() +
            ": " +
            error);
    }

    return bytecode;
}

[[nodiscard]] std::filesystem::path
ScriptOutputPath(
    const std::filesystem::path& sourcePath)
{
    std::filesystem::path relative =
        sourcePath.lexically_relative(
            "Scripts");

    if (relative.empty() ||
        *relative.begin() == "..")
    {
        relative =
            sourcePath.filename();
    }

    relative.replace_extension(
        ".luauc");

    return
        std::filesystem::path(
            "Scripts") /
        relative;
}

[[nodiscard]] std::string
SerializeManifest(
    const BuildManifest& manifest)
{
    toml::table root;

    root.insert(
        "format_version",
        static_cast<i64>(1));

    toml::table project;
    project.insert(
        "id",
        manifest.projectId.ToString());
    project.insert(
        "name",
        manifest.projectName);
    project.insert(
        "engine_compatibility",
        manifest.
            engineCompatibilityVersion);
    project.insert(
        "startup_world",
        manifest.startupWorld.
            generic_string());
    root.insert(
        "project",
        std::move(project));

    toml::table build;
    build.insert(
        "profile",
        manifest.profile.name);
    build.insert(
        "configuration",
        manifest.profile.configuration);
    build.insert(
        "platform",
        manifest.profile.platform);
    build.insert(
        "storefront",
        manifest.profile.storefront);
    root.insert(
        "build",
        std::move(build));

    toml::array assets;

    for (const CookedAsset& asset :
         manifest.assets)
    {
        toml::table item;
        item.insert(
            "id",
            asset.id.ToString());
        item.insert(
            "kind",
            std::string(
                content::AssetKindName(
                    asset.kind)));
        item.insert(
            "source",
            asset.sourcePath.
                generic_string());
        item.insert(
            "source_sha256",
            asset.sourceHash.ToHex());

        if (!IsZeroHash(
                asset.derivedKey))
        {
            item.insert(
                "derived_key",
                asset.derivedKey.ToHex());
            item.insert(
                "cache_hit",
                asset.cacheHit);
        }

        toml::array artifacts;

        for (const auto& path :
             asset.artifacts)
        {
            artifacts.push_back(
                path.generic_string());
        }

        item.insert(
            "artifacts",
            std::move(artifacts));

        assets.push_back(
            std::move(item));
    }

    root.insert(
        "assets",
        std::move(assets));

    toml::array scripts;

    for (const CookedScript& script :
         manifest.scripts)
    {
        toml::table item;
        item.insert(
            "source",
            script.sourcePath.
                generic_string());
        item.insert(
            "source_sha256",
            script.sourceHash.ToHex());
        item.insert(
            "bytecode_sha256",
            script.bytecodeHash.ToHex());
        item.insert(
            "path",
            script.bytecodePath.
                generic_string());

        scripts.push_back(
            std::move(item));
    }

    root.insert(
        "scripts",
        std::move(scripts));

    std::ostringstream stream;
    stream << root;
    return stream.str();
}

void FinalizeStaging(
    const std::filesystem::path& staging,
    const std::filesystem::path& output,
    const bool cleanOutput)
{
    if (std::filesystem::exists(
            output))
    {
        if (!cleanOutput)
        {
            throw std::runtime_error(
                "Build output already exists and cleanOutput is disabled.");
        }

        std::filesystem::remove_all(
            output);
    }

    std::filesystem::rename(
        staging,
        output);
}
} // namespace

bool BuildValidation::Succeeded() const noexcept
{
    return !HasErrors(issues);
}

bool BuildResult::Succeeded() const noexcept
{
    return !HasErrors(issues);
}

BuildValidation BuildService::Validate(
    const BuildRequest& request) const
{
    BuildValidation result{};

    if (request.manifestPath.empty())
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.manifest.missing",
            "Build request does not specify Project.orbit.toml.");
        return result;
    }

    documents::ProjectDocument project;

    try
    {
        project =
            documents::ProjectDocument::Open(
                request.manifestPath);
    }
    catch (const std::exception& exception)
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.project.open",
            exception.what(),
            request.manifestPath);
        return result;
    }

    result.projectRoot =
        project.RootDirectory();

    const auto& manifest =
        project.Manifest();

    const documents::BuildProfile* profile =
        FindProfile(
            manifest,
            request.profileName);

    if (profile == nullptr)
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.profile.missing",
            request.profileName.empty()
                ? "Project has no build profiles."
                : "Build profile does not exist: " +
                    request.profileName,
            project.ManifestPath());
        return result;
    }

    result.profile = *profile;

    result.outputDirectory =
        request.outputDirectory.empty()
            ? result.projectRoot /
                "Build" /
                SanitizeProfileName(
                    profile->name)
            : AbsoluteNormalized(
                request.outputDirectory);

    if (IsProtectedOutput(
            result.outputDirectory,
            result.projectRoot))
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.output.protected",
            "Build output cannot replace project authority directories.",
            result.outputDirectory);
    }

    if (!IsConfigurationSupported(
            profile->configuration))
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.configuration.unsupported",
            "Unsupported build configuration: " +
                profile->configuration,
            project.ManifestPath());
    }

    if (profile->platform !=
        "Windows")
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.platform.unsupported",
            "V0.0.3 currently supports Windows build profiles only.",
            project.ManifestPath());
    }

    if (profile->storefront !=
        "standalone")
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.storefront.unavailable",
            "Only standalone packaging is available before M22 platform services.",
            project.ManifestPath());
    }

    if (manifest.engineCompatibilityVersion !=
        Version)
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.engine.version",
            "Project requires Orbit " +
                manifest.
                    engineCompatibilityVersion +
                ", current engine is " +
                std::string(Version) +
                ".",
            project.ManifestPath());
    }

    for (const auto& plugin :
         manifest.plugins)
    {
        const auto pluginManifest =
            result.projectRoot /
            "Plugins" /
            plugin.id /
            "plugin.toml";

        if (!std::filesystem::is_regular_file(
                pluginManifest))
        {
            AddIssue(
                result.issues,
                IssueSeverity::Error,
                "build.plugin.missing",
                "Enabled plugin package is missing: " +
                    plugin.id,
                pluginManifest);
        }
    }

    for (const auto& script :
         manifest.scriptEntryPoints)
    {
        try
        {
            const auto absolute =
                ResolveProjectFile(
                    result.projectRoot,
                    script);

            const auto extension =
                absolute.extension().
                    string();

            if (extension != ".luau" &&
                extension != ".lua")
            {
                AddIssue(
                    result.issues,
                    IssueSeverity::Error,
                    "build.script.extension",
                    "Script entry point must use .luau or .lua.",
                    script);
            }
        }
        catch (const std::exception& exception)
        {
            AddIssue(
                result.issues,
                IssueSeverity::Error,
                "build.script.invalid",
                exception.what(),
                script);
        }
    }

    try
    {
        content::ContentService content(
            result.projectRoot);
        content.Scan();

        for (const auto& diagnostic :
             content.Diagnostics())
        {
            AddIssue(
                result.issues,
                IssueSeverity::Error,
                "build.content.invalid",
                diagnostic.message,
                diagnostic.sourcePath);
        }
    }
    catch (const std::exception& exception)
    {
        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.content.scan",
            exception.what(),
            result.projectRoot /
                "Content");
    }

    return result;
}

BuildResult BuildService::Cook(
    const BuildRequest& request) const
{
    const BuildValidation validation =
        Validate(request);

    BuildResult result{
        .outputDirectory =
            validation.outputDirectory,
        .issues =
            validation.issues
    };

    if (!validation.Succeeded())
    {
        return result;
    }

    std::filesystem::path staging =
        result.outputDirectory;
    staging += ".staging";

    try
    {
        std::filesystem::remove_all(
            staging);
        std::filesystem::create_directories(
            staging);

        documents::ProjectDocument project =
            documents::ProjectDocument::Open(
                request.manifestPath);

        result.manifest.projectId =
            project.Manifest().projectId;
        result.manifest.projectName =
            project.Manifest().displayName;
        result.manifest.
            engineCompatibilityVersion =
                project.Manifest().
                    engineCompatibilityVersion;
        result.manifest.profile =
            validation.profile;

        const auto worldSource =
            project.StartupWorldPath();

        const auto worldRelative =
            std::filesystem::path(
                "Worlds") /
            worldSource.filename();

        std::filesystem::create_directories(
            (staging /
             worldRelative).
                parent_path());

        std::filesystem::copy_file(
            worldSource,
            staging /
                worldRelative,
            std::filesystem::
                copy_options::
                    overwrite_existing);

        result.manifest.startupWorld =
            worldRelative;

        content::ContentService content(
            validation.projectRoot);
        content.Scan();

        auto assets =
            content.All();

        std::ranges::sort(
            assets,
            {},
            &content::AssetRecord::
                sourcePath);

        for (const content::AssetRecord& asset :
             assets)
        {
            CookedAsset cooked{
                .id = asset.id,
                .kind = asset.kind,
                .sourcePath =
                    asset.sourcePath,
                .sourceHash =
                    asset.sourceHash
            };

            const auto source =
                validation.projectRoot /
                asset.sourcePath;

            if (content.Importers().
                    FindFor(source) !=
                nullptr)
            {
                const content::ImportResult
                    imported =
                        content.CookDerived(
                            asset.id,
                            validation.profile.
                                platform);

                cooked.derivedKey =
                    imported.key;
                cooked.cacheHit =
                    imported.cacheHit;

                const auto assetRoot =
                    std::filesystem::path(
                        "Content") /
                    ".cooked" /
                    asset.id.ToString();

                for (const auto& artifact :
                     imported.artifacts)
                {
                    const auto relative =
                        assetRoot /
                        artifact.name;

                    std::filesystem::
                        create_directories(
                            (staging /
                             relative).
                                parent_path());

                    std::filesystem::copy_file(
                        artifact.path,
                        staging /
                            relative,
                        std::filesystem::
                            copy_options::
                                overwrite_existing);

                    cooked.artifacts.push_back(
                        relative);
                }
            }
            else if (IsSemanticRuntimeAsset(
                         asset.kind))
            {
                const auto relative =
                    SemanticOutputPath(
                        asset.sourcePath);

                std::filesystem::
                    create_directories(
                        (staging /
                         relative).
                            parent_path());

                std::filesystem::copy_file(
                    source,
                    staging /
                        relative,
                    std::filesystem::
                        copy_options::
                            overwrite_existing);

                cooked.artifacts.push_back(
                    relative);
            }
            else
            {
                AddIssue(
                    result.issues,
                    IssueSeverity::Error,
                    "build.asset.importer",
                    "No target-platform importer is registered for " +
                        std::string(
                            content::AssetKindName(
                                asset.kind)) +
                        " asset.",
                    asset.sourcePath);
                continue;
            }

            result.manifest.assets.push_back(
                std::move(cooked));
        }

        if (HasErrors(
                result.issues))
        {
            std::filesystem::remove_all(
                staging);
            return result;
        }

        for (const auto& script :
             project.Manifest().
                 scriptEntryPoints)
        {
            const auto sourcePath =
                ResolveProjectFile(
                    validation.projectRoot,
                    script);

            const std::string source =
                ReadText(
                    sourcePath);

            const std::string bytecode =
                CompileScript(
                    source,
                    script,
                    validation.profile.
                        configuration);

            const auto byteSpan =
                std::as_bytes(
                    std::span(
                        bytecode.data(),
                        bytecode.size()));

            const auto relativeOutput =
                ScriptOutputPath(
                    script);

            WriteBytes(
                staging /
                    relativeOutput,
                byteSpan);

            result.manifest.scripts.
                push_back({
                    .sourcePath = script,
                    .sourceHash =
                        content::HashFile(
                            sourcePath),
                    .bytecodeHash =
                        content::HashBytes(
                            byteSpan),
                    .bytecodePath =
                        relativeOutput
                });
        }

        const std::string manifestText =
            SerializeManifest(
                result.manifest);

        WriteText(
            staging /
                "OrbitBuildManifest.toml",
            manifestText);

        FinalizeStaging(
            staging,
            result.outputDirectory,
            request.cleanOutput);

        result.manifestPath =
            result.outputDirectory /
            "OrbitBuildManifest.toml";
    }
    catch (const std::exception& exception)
    {
        std::error_code ignored;
        std::filesystem::remove_all(
            staging,
            ignored);

        AddIssue(
            result.issues,
            IssueSeverity::Error,
            "build.cook.failed",
            exception.what(),
            result.outputDirectory);
    }

    return result;
}
} // namespace orbit::build
