#include <orbit/runtime_project/CookedProject.hpp>

#include <orbit/core/BuildInfo.hpp>
#include <orbit/core/Log.hpp>

#include <lua.h>
#include <lualib.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>

namespace orbit::runtime_project
{
namespace
{
[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<std::string>();

    if (!value.has_value() ||
        value->empty())
    {
        throw std::runtime_error(
            "Cooked project manifest missing string field: " +
            std::string(key));
    }

    return *value;
}

[[nodiscard]] i64 RequiredInteger(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<i64>();

    if (!value.has_value())
    {
        throw std::runtime_error(
            "Cooked project manifest missing integer field: " +
            std::string(key));
    }

    return *value;
}

[[nodiscard]] std::filesystem::path
NormalizeRoot(
    const std::filesystem::path& root)
{
    if (root.empty())
    {
        throw std::invalid_argument(
            "Cooked project root must not be empty.");
    }

    return std::filesystem::weakly_canonical(
        std::filesystem::absolute(root));
}

[[nodiscard]] std::filesystem::path
ResolveInsidePackage(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    const bool requireRegularFile)
{
    if (relative.empty() ||
        relative.is_absolute())
    {
        throw std::runtime_error(
            "Cooked project paths must be non-empty package-relative paths.");
    }

    const auto normalizedRoot =
        NormalizeRoot(root);

    const auto resolved =
        std::filesystem::weakly_canonical(
            normalizedRoot /
            relative);

    const auto packageRelative =
        resolved.lexically_relative(
            normalizedRoot);

    if (packageRelative.empty() ||
        *packageRelative.begin() == "..")
    {
        throw std::runtime_error(
            "Cooked project path escapes the package root: " +
            relative.generic_string());
    }

    if (requireRegularFile &&
        !std::filesystem::is_regular_file(
            resolved))
    {
        throw std::runtime_error(
            "Cooked project file is missing: " +
            relative.generic_string());
    }

    return resolved;
}

[[nodiscard]] std::string ReadBinary(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);

    if (!input)
    {
        throw std::runtime_error(
            "Unable to open cooked runtime file: " +
            path.string());
    }

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void OpenLibrary(
    lua_State* state,
    const char* name,
    lua_CFunction function)
{
    lua_pushcfunction(
        state,
        function,
        name);

    lua_pushstring(
        state,
        name);

    lua_call(
        state,
        1,
        0);
}

void OpenSafeLibraries(
    lua_State* state)
{
    OpenLibrary(
        state,
        "",
        luaopen_base);
    OpenLibrary(
        state,
        LUA_COLIBNAME,
        luaopen_coroutine);
    OpenLibrary(
        state,
        LUA_TABLIBNAME,
        luaopen_table);
    OpenLibrary(
        state,
        LUA_STRLIBNAME,
        luaopen_string);
    OpenLibrary(
        state,
        LUA_MATHLIBNAME,
        luaopen_math);
    OpenLibrary(
        state,
        LUA_UTF8LIBNAME,
        luaopen_utf8);
    OpenLibrary(
        state,
        LUA_BITLIBNAME,
        luaopen_bit32);
    OpenLibrary(
        state,
        LUA_BUFFERLIBNAME,
        luaopen_buffer);
    OpenLibrary(
        state,
        LUA_VECLIBNAME,
        luaopen_vector);
}

[[nodiscard]] CookedProjectManifest
LoadManifest(
    const std::filesystem::path& path)
{
    const toml::table root =
        toml::parse_file(
            path.string());

    CookedProjectManifest manifest{};

    const i64 formatVersion =
        RequiredInteger(
            root,
            "format_version");

    if (formatVersion != 1)
    {
        throw std::runtime_error(
            "Unsupported Orbit build manifest format version.");
    }

    manifest.formatVersion =
        static_cast<u32>(
            formatVersion);

    const toml::table* project =
        root["project"].as_table();

    if (project == nullptr)
    {
        throw std::runtime_error(
            "Cooked project manifest is missing [project].");
    }

    const auto projectId =
        documents::ProjectId::Parse(
            RequiredString(
                *project,
                "id"));

    if (!projectId.has_value())
    {
        throw std::runtime_error(
            "Cooked project manifest contains an invalid project ID.");
    }

    manifest.projectId =
        *projectId;
    manifest.projectName =
        RequiredString(
            *project,
            "name");
    manifest.engineCompatibilityVersion =
        RequiredString(
            *project,
            "engine_compatibility");
    manifest.startupWorld =
        std::filesystem::path(
            RequiredString(
                *project,
                "startup_world"));

    if (const toml::array* entries =
            (*project)["script_entry_points"].
                as_array())
    {
        for (const toml::node& node :
             *entries)
        {
            const auto value =
                node.value<std::string>();

            if (!value.has_value() ||
                value->empty())
            {
                throw std::runtime_error(
                    "Cooked script entry points must be non-empty strings.");
            }

            manifest.scriptEntryPoints.
                emplace_back(*value);
        }
    }

    const toml::table* build =
        root["build"].as_table();

    if (build == nullptr)
    {
        throw std::runtime_error(
            "Cooked project manifest is missing [build].");
    }

    manifest.build = {
        .name =
            RequiredString(
                *build,
                "profile"),
        .configuration =
            RequiredString(
                *build,
                "configuration"),
        .platform =
            RequiredString(
                *build,
                "platform"),
        .storefront =
            RequiredString(
                *build,
                "storefront")
    };

    if (const toml::array* assets =
            root["assets"].as_array())
    {
        std::set<std::string>
            assetIds;

        for (const toml::node& node :
             *assets)
        {
            const toml::table* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "Cooked asset entries must be tables.");
            }

            CookedAssetRecord asset{
                .id =
                    RequiredString(
                        *item,
                        "id"),
                .kind =
                    RequiredString(
                        *item,
                        "kind"),
                .sourcePath =
                    std::filesystem::path(
                        RequiredString(
                            *item,
                            "source")),
                .sourceSha256 =
                    RequiredString(
                        *item,
                        "source_sha256")
            };

            if (!assetIds.insert(
                    asset.id).
                    second)
            {
                throw std::runtime_error(
                    "Cooked project manifest contains duplicate asset IDs.");
            }

            if (const auto derived =
                    (*item)["derived_key"].
                        value<std::string>();
                derived.has_value())
            {
                asset.derivedKey =
                    *derived;
            }

            if (const toml::array* artifacts =
                    (*item)["artifacts"].
                        as_array())
            {
                for (const toml::node&
                         artifact :
                     *artifacts)
                {
                    const auto value =
                        artifact.
                            value<std::string>();

                    if (!value.has_value() ||
                        value->empty())
                    {
                        throw std::runtime_error(
                            "Cooked asset artifact paths must be non-empty strings.");
                    }

                    asset.artifacts.
                        emplace_back(*value);
                }
            }

            manifest.assets.push_back(
                std::move(asset));
        }
    }

    if (const toml::array* scripts =
            root["scripts"].as_array())
    {
        std::set<std::string>
            scriptSources;

        for (const toml::node& node :
             *scripts)
        {
            const toml::table* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "Cooked script entries must be tables.");
            }

            CookedScriptRecord script{
                .sourcePath =
                    std::filesystem::path(
                        RequiredString(
                            *item,
                            "source")),
                .sourceSha256 =
                    RequiredString(
                        *item,
                        "source_sha256"),
                .bytecodeSha256 =
                    RequiredString(
                        *item,
                        "bytecode_sha256"),
                .bytecodePath =
                    std::filesystem::path(
                        RequiredString(
                            *item,
                            "path"))
            };

            if (!scriptSources.insert(
                    script.sourcePath.
                        generic_string()).
                    second)
            {
                throw std::runtime_error(
                    "Cooked project manifest contains duplicate script sources.");
            }

            manifest.scripts.push_back(
                std::move(script));
        }
    }

    return manifest;
}
} // namespace

CookedProject CookedProject::Open(
    const std::filesystem::path&
        packageOrManifest)
{
    const auto input =
        std::filesystem::absolute(
            packageOrManifest).
            lexically_normal();

    const std::filesystem::path
        manifestPath =
            std::filesystem::is_directory(
                input)
                ? input /
                    "OrbitBuildManifest.toml"
                : input;

    if (!std::filesystem::is_regular_file(
            manifestPath))
    {
        throw std::runtime_error(
            "Orbit build manifest does not exist: " +
            manifestPath.string());
    }

    CookedProject result;
    result.manifestPath_ =
        std::filesystem::weakly_canonical(
            manifestPath);
    result.rootDirectory_ =
        NormalizeRoot(
            result.manifestPath_.
                parent_path());
    result.manifest_ =
        LoadManifest(
            result.manifestPath_);

    if (result.manifest_.
            engineCompatibilityVersion !=
        build::Version)
    {
        throw std::runtime_error(
            "Cooked project requires Orbit " +
            result.manifest_.
                engineCompatibilityVersion +
            ", current runtime is " +
            std::string(
                build::Version) +
            ".");
    }

    static_cast<void>(
        result.StartupWorldPath());

    for (const auto& asset :
         result.manifest_.assets)
    {
        for (const auto& artifact :
             asset.artifacts)
        {
            static_cast<void>(
                result.ResolvePackagedFile(
                    artifact));
        }
    }

    for (const auto& script :
         result.manifest_.scripts)
    {
        static_cast<void>(
            result.ResolvePackagedFile(
                script.bytecodePath));
    }

    for (const auto& entry :
         result.manifest_.
             scriptEntryPoints)
    {
        if (result.FindScript(
                entry) == nullptr)
        {
            throw std::runtime_error(
                "Cooked script entry point has no packaged bytecode: " +
                entry.generic_string());
        }
    }

    return result;
}

const std::filesystem::path&
CookedProject::RootDirectory() const noexcept
{
    return rootDirectory_;
}

const std::filesystem::path&
CookedProject::ManifestPath() const noexcept
{
    return manifestPath_;
}

const CookedProjectManifest&
CookedProject::Manifest() const noexcept
{
    return manifest_;
}

std::filesystem::path
CookedProject::StartupWorldPath() const
{
    return ResolvePackagedFile(
        manifest_.startupWorld);
}

std::filesystem::path
CookedProject::ResolvePackagedFile(
    const std::filesystem::path&
        relative) const
{
    return ResolveInsidePackage(
        rootDirectory_,
        relative,
        true);
}

const CookedScriptRecord*
CookedProject::FindScript(
    const std::filesystem::path&
        sourcePath) const noexcept
{
    const auto found =
        std::ranges::find(
            manifest_.scripts,
            sourcePath,
            &CookedScriptRecord::
                sourcePath);

    return found ==
            manifest_.scripts.end()
        ? nullptr
        : &*found;
}

class ScriptRuntime::Impl
{
public:
    explicit Impl(
        const CookedProject& project)
        : project_(project),
          state_(luaL_newstate())
    {
        if (state_ == nullptr)
        {
            throw std::runtime_error(
                "Luau runtime state allocation failed.");
        }

        OpenSafeLibraries(
            state_);

        lua_newtable(
            state_);

        lua_pushstring(
            state_,
            project_.Manifest().
                projectName.c_str());
        lua_setfield(
            state_,
            -2,
            "project_name");

        const std::string projectId =
            project_.Manifest().
                projectId.ToString();

        lua_pushstring(
            state_,
            projectId.c_str());
        lua_setfield(
            state_,
            -2,
            "project_id");

        lua_setglobal(
            state_,
            "Orbit");

        luaL_sandbox(
            state_);
    }

    ~Impl()
    {
        if (state_ != nullptr)
        {
            lua_close(
                state_);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void ExecuteEntryPoints()
    {
        for (const auto& entry :
             project_.Manifest().
                 scriptEntryPoints)
        {
            const auto* script =
                project_.FindScript(
                    entry);

            if (script == nullptr)
            {
                throw std::runtime_error(
                    "Cooked entry point is missing from the script catalog: " +
                    entry.generic_string());
            }

            const auto bytecodePath =
                project_.
                    ResolvePackagedFile(
                        script->
                            bytecodePath);

            const std::string bytecode =
                ReadBinary(
                    bytecodePath);

            if (luau_load(
                    state_,
                    entry.generic_string().
                        c_str(),
                    bytecode.data(),
                    bytecode.size(),
                    0) != 0)
            {
                const char* message =
                    lua_tostring(
                        state_,
                        -1);

                const std::string error =
                    message != nullptr
                        ? message
                        : "unknown Luau load error";

                lua_pop(
                    state_,
                    1);

                throw std::runtime_error(
                    "Failed to load cooked Luau entry point '" +
                    entry.generic_string() +
                    "': " +
                    error);
            }

            if (lua_pcall(
                    state_,
                    0,
                    0,
                    0) != 0)
            {
                const char* message =
                    lua_tostring(
                        state_,
                        -1);

                const std::string error =
                    message != nullptr
                        ? message
                        : "unknown Luau runtime error";

                lua_pop(
                    state_,
                    1);

                throw std::runtime_error(
                    "Cooked Luau entry point failed '" +
                    entry.generic_string() +
                    "': " +
                    error);
            }

            log::Info(
                "Executed cooked Luau entry point: " +
                entry.generic_string());
        }
    }

private:
    const CookedProject& project_;
    lua_State* state_{nullptr};
};

ScriptRuntime::ScriptRuntime(
    const CookedProject& project)
    : impl_(
          std::make_unique<Impl>(
              project))
{
}

ScriptRuntime::~ScriptRuntime() = default;

ScriptRuntime::ScriptRuntime(
    ScriptRuntime&&) noexcept = default;

ScriptRuntime&
ScriptRuntime::operator=(
    ScriptRuntime&&) noexcept = default;

void ScriptRuntime::ExecuteEntryPoints()
{
    impl_->ExecuteEntryPoints();
}
} // namespace orbit::runtime_project
