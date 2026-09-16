#include <orbit/plugins/PluginManager.hpp>

#include <orbit/core/Log.hpp>

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <format>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace orbit::plugins
{
namespace
{
inline constexpr std::string_view kOrbitApiVersion =
    "0.0.3";

[[nodiscard]] std::string ReadTextFile(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);

    if (!input)
    {
        throw std::runtime_error(
            "Unable to open plugin file: " +
            path.string());
    }

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] u64 HashText(
    const std::string_view text,
    u64 seed) noexcept
{
    u64 hash = seed;

    for (const unsigned char value : text)
    {
        hash ^= static_cast<u64>(value);
        hash *= 1099511628211ULL;
    }

    return hash;
}

[[nodiscard]] commands::CommandId
StableCommandId(
    const std::string_view plugin,
    const std::string_view name) noexcept
{
    const std::string key =
        std::string(plugin) +
        ":command:" +
        std::string(name);

    commands::CommandId result{
        .high =
            HashText(
                key,
                1469598103934665603ULL),
        .low =
            HashText(
                key,
                1099511628211ULL ^
                    0x9e3779b97f4a7c15ULL)
    };

    if (!result)
    {
        result.low = 1;
    }

    return result;
}

[[nodiscard]] editor_ui::PanelId
StablePanelId(
    const std::string_view plugin,
    const std::string_view name) noexcept
{
    const std::string key =
        std::string(plugin) +
        ":panel:" +
        std::string(name);

    editor_ui::PanelId result{
        .high =
            HashText(
                key,
                1469598103934665603ULL ^
                    0x243f6a8885a308d3ULL),
        .low =
            HashText(
                key,
                1099511628211ULL ^
                    0x13198a2e03707344ULL)
    };

    if (!result)
    {
        result.low = 1;
    }

    return result;
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

[[nodiscard]] bool IsSupportedScope(
    const PluginScope scope) noexcept
{
    return scope == PluginScope::Editor ||
        scope == PluginScope::Both;
}

[[nodiscard]] std::optional<
    editor_model::CommandSurfaceKind>
CommandSurfaceKindFromString(
    const std::string_view value) noexcept
{
    if (value == "toolbar")
    {
        return editor_model::
            CommandSurfaceKind::Toolbar;
    }

    if (value == "context")
    {
        return editor_model::
            CommandSurfaceKind::ContextMenu;
    }

    if (value == "radial")
    {
        return editor_model::
            CommandSurfaceKind::Radial;
    }

    return std::nullopt;
}
} // namespace

class PluginManager::Impl
{
public:
    class PluginInstance
    {
    public:
        struct Panel
        {
            PluginPanelDescriptor descriptor;
            int functionRef{LUA_NOREF};
        };

        struct SurfaceContribution
        {
            std::string surface;
            editor_model::CommandSurfaceKind kind{
                editor_model::CommandSurfaceKind::Toolbar};
            commands::CommandId command{};
        };

        PluginInstance(
            Impl& owner,
            std::filesystem::path packageRoot,
            std::filesystem::path manifestPath,
            std::string expectedId,
            std::string requiredVersion)
            : owner_(owner),
              packageRoot_(
                  std::move(packageRoot)),
              manifestPath_(
                  std::move(manifestPath)),
              expectedId_(
                  std::move(expectedId)),
              requiredVersion_(
                  std::move(requiredVersion))
        {
        }

        ~PluginInstance()
        {
            Unload();
        }

        [[nodiscard]] bool Load()
        {
            Unload();

            manifest_ =
                LoadPluginManifest(
                    manifestPath_);

            if (manifest_.id != expectedId_)
            {
                throw std::runtime_error(
                    std::format(
                        "Plugin package ID '{}' does not match enabled project ID '{}'.",
                        manifest_.id,
                        expectedId_));
            }

            for (const PluginDependency& dependency :
                 manifest_.dependencies)
            {
                const auto found =
                    owner_.enabledVersions.find(
                        dependency.id);

                if (found ==
                        owner_.enabledVersions.end() ||
                    (dependency.version != "*" &&
                     found->second != dependency.version))
                {
                    throw std::runtime_error(
                        std::format(
                            "Plugin '{}' dependency '{} {}' is not enabled at the required version.",
                            manifest_.id,
                            dependency.id,
                            dependency.version));
                }
            }

            if (manifest_.orbitApiVersion !=
                kOrbitApiVersion)
            {
                throw std::runtime_error(
                    std::format(
                        "Plugin '{}' targets Orbit API '{}', expected '{}'.",
                        manifest_.id,
                        manifest_.orbitApiVersion,
                        kOrbitApiVersion));
            }

            if (!requiredVersion_.empty() &&
                requiredVersion_ != "*" &&
                requiredVersion_ !=
                    manifest_.version)
            {
                throw std::runtime_error(
                    std::format(
                        "Plugin '{}' version '{}' does not satisfy project version '{}'.",
                        manifest_.id,
                        manifest_.version,
                        requiredVersion_));
            }

            if (!IsSupportedScope(
                    manifest_.scope))
            {
                throw std::runtime_error(
                    "Runtime-only plugin cannot be loaded into Orbit Studio.");
            }

            const auto grant =
                owner_.grantedPermissions.find(
                    manifest_.id);

            const PluginPermissionSet
                granted =
                    grant ==
                        owner_.
                            grantedPermissions.
                            end()
                        ? PluginPermissionSet{}
                        : grant->second;

            effectivePermissions_ =
                manifest_.
                    requestedPermissions.
                    Intersection(granted);

            const std::filesystem::path
                entryPath =
                    packageRoot_ /
                    manifest_.entryScript;

            const auto normalizedRoot =
                std::filesystem::
                    weakly_canonical(
                        packageRoot_);

            const auto normalizedEntry =
                std::filesystem::
                    weakly_canonical(
                        entryPath);

            const auto relative =
                normalizedEntry.
                    lexically_relative(
                        normalizedRoot);

            if (relative.empty() ||
                relative.native().
                    starts_with(L".."))
            {
                throw std::runtime_error(
                    "Plugin entry script escapes its package directory.");
            }

            const std::string source =
                ReadTextFile(
                    normalizedEntry);

            state_ =
                luaL_newstate();

            if (state_ == nullptr)
            {
                throw std::runtime_error(
                    "Luau state allocation failed.");
            }

            lua_setthreaddata(
                state_,
                this);

            OpenSafeLibraries();
            InstallOrbitApi();

            // Freeze the safe library/global surface after Orbit has
            // installed its own restricted API.
            luaL_sandbox(state_);

            Luau::CompileOptions options{};
            options.optimizationLevel = 1;
            options.debugLevel = 1;

            const std::string bytecode =
                Luau::compile(
                    source,
                    options);

            if (luau_load(
                    state_,
                    normalizedEntry.
                        string().
                        c_str(),
                    bytecode.data(),
                    bytecode.size(),
                    0) != 0)
            {
                const std::string error =
                    lua_tostring(
                        state_,
                        -1);

                lua_pop(
                    state_,
                    1);

                throw std::runtime_error(
                    "Plugin compile/load failed: " +
                    error);
            }

            if (lua_pcall(
                    state_,
                    0,
                    0,
                    0) != 0)
            {
                const std::string error =
                    lua_tostring(
                        state_,
                        -1);

                lua_pop(
                    state_,
                    1);

                throw std::runtime_error(
                    "Plugin entry script failed: " +
                    error);
            }

            loaded_ = true;
            error_.clear();
            ++revision_;

            fingerprint_ =
                ObservedFingerprint();

            log::Info(
                std::format(
                    "Loaded plugin '{}' {}.",
                    manifest_.id,
                    manifest_.version));

            return true;
        }

        void SetError(
            std::string error)
        {
            loaded_ = false;
            error_ = std::move(error);
            fingerprint_ =
                ObservedFingerprint();
            ++revision_;
        }

        [[nodiscard]] bool NeedsReload() const
        {
            return ObservedFingerprint() !=
                fingerprint_;
        }

        [[nodiscard]] PluginStatus Status() const
        {
            return {
                .id = manifest_.id.empty()
                    ? packageRoot_.
                        filename().
                        string()
                    : manifest_.id,
                .version =
                    manifest_.version,
                .loaded = loaded_,
                .error = error_,
                .requestedPermissions =
                    manifest_.
                        requestedPermissions,
                .grantedPermissions =
                    effectivePermissions_,
                .revision = revision_
            };
        }

        [[nodiscard]] const std::vector<Panel>&
        Panels() const noexcept
        {
            return panels_;
        }

        [[nodiscard]] std::vector<std::string>
        Validate()
        {
            std::vector<std::string> issues;

            if (!loaded_ ||
                state_ == nullptr)
            {
                return issues;
            }

            for (const int reference :
                 validatorRefs_)
            {
                lua_getref(
                    state_,
                    reference);

                if (!lua_isfunction(
                        state_,
                        -1))
                {
                    lua_pop(
                        state_,
                        1);
                    issues.push_back(
                        "Validator callback is unavailable.");
                    continue;
                }

                if (lua_pcall(
                        state_,
                        0,
                        1,
                        0) != 0)
                {
                    const char* error =
                        lua_tostring(
                            state_,
                            -1);

                    issues.push_back(
                        std::string(
                            "Validator failed: ") +
                        (error != nullptr
                            ? error
                            : "unknown Luau error"));

                    lua_pop(
                        state_,
                        1);
                    continue;
                }

                if (lua_isnil(
                        state_,
                        -1))
                {
                    lua_pop(
                        state_,
                        1);
                    continue;
                }

                if (!lua_isstring(
                        state_,
                        -1))
                {
                    lua_pop(
                        state_,
                        1);
                    issues.push_back(
                        "Validator must return nil or a diagnostic string.");
                    continue;
                }

                issues.emplace_back(
                    lua_tostring(
                        state_,
                        -1));
                lua_pop(
                    state_,
                    1);
            }

            return issues;
        }

        [[nodiscard]] bool DrawPanel(
            const editor_ui::PanelId panel,
            editor_ui::PanelContext& context)
        {
            if (!loaded_ ||
                state_ == nullptr)
            {
                return false;
            }

            const auto found =
                std::find_if(
                    panels_.begin(),
                    panels_.end(),
                    [panel](const Panel& item)
                    {
                        return
                            item.descriptor.id ==
                            panel;
                    });

            if (found == panels_.end())
            {
                return false;
            }

            activePanelContext_ =
                &context;

            const bool result =
                InvokeRef(
                    found->functionRef,
                    "panel");

            activePanelContext_ =
                nullptr;

            return result;
        }

        [[nodiscard]] const std::string&
        Id() const noexcept
        {
            return manifest_.id.empty()
                ? expectedId_
                : manifest_.id;
        }

    private:
        [[nodiscard]] static PluginInstance*
        Self(lua_State* state)
        {
            auto* self =
                static_cast<PluginInstance*>(
                    lua_getthreaddata(
                        state));

            if (self == nullptr)
            {
                luaL_error(
                    state,
                    "Orbit plugin host is unavailable");
            }

            return self;
        }

        [[nodiscard]] bool HasPermission(
            const PluginPermission permission)
            const noexcept
        {
            return effectivePermissions_.
                Contains(permission);
        }

        void RequirePermission(
            lua_State* state,
            const PluginPermission permission)
        {
            if (!HasPermission(permission))
            {
                luaL_error(
                    state,
                    "Permission '%s' was not granted",
                    std::string(
                        PermissionName(
                            permission)).
                        c_str());
            }
        }

        static int LuaLog(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            const char* message =
                luaL_checkstring(
                    state,
                    1);

            log::Info(
                std::format(
                    "[plugin:{}] {}",
                    self->manifest_.id,
                    message));

            return 0;
        }

        static int LuaHasPermission(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            const std::string_view name(
                luaL_checkstring(
                    state,
                    1));

            const auto permission =
                PermissionFromString(name);

            lua_pushboolean(
                state,
                permission.has_value() &&
                    self->HasPermission(
                        *permission));

            return 1;
        }

        static int LuaSelectionCount(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            lua_pushinteger(
                state,
                static_cast<int>(
                    self->owner_.
                        selection.
                        Ordered().
                        size()));

            return 1;
        }

        static int LuaRenameSelected(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            self->RequirePermission(
                state,
                PluginPermission::
                    ProjectMutation);

            const char* name =
                luaL_checkstring(
                    state,
                    1);

            const auto& selected =
                self->owner_.
                    selection.
                    Ordered();

            if (selected.size() != 1)
            {
                luaL_error(
                    state,
                    "renameSelected requires exactly one selected object");
            }

            self->owner_.
                commandService.
                RenameObject(
                    selected.front(),
                    name);

            return 0;
        }

        static int LuaInvokeCommand(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            self->RequirePermission(
                state,
                PluginPermission::
                    ProjectMutation);

            const char* idText =
                luaL_checkstring(
                    state,
                    1);

            const auto id =
                commands::CommandId::
                    Parse(idText);

            if (!id.has_value())
            {
                luaL_error(
                    state,
                    "Invalid Orbit command ID");
            }

            self->owner_.
                commandRegistry.
                Invoke(*id);

            return 0;
        }

        static int LuaRegisterCommand(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            const char* name =
                luaL_checkstring(
                    state,
                    1);

            luaL_checktype(
                state,
                2,
                LUA_TFUNCTION);

            const char* category =
                luaL_optstring(
                    state,
                    3,
                    "Plugin");

            const char* description =
                luaL_optstring(
                    state,
                    4,
                    "");

            const bool mcpVisible =
                luaL_optboolean(
                    state,
                    5,
                    0) != 0;

            if (mcpVisible)
            {
                self->RequirePermission(
                    state,
                    PluginPermission::
                        McpRegistration);
            }

            const commands::CommandId id =
                StableCommandId(
                    self->manifest_.id,
                    name);

            const int functionRef =
                lua_ref(
                    state,
                    2);

            self->owner_.
                commandRegistry.
                Register({
                    .id = id,
                    .name = name,
                    .category = category,
                    .description =
                        description,
                    .automationVisible =
                        mcpVisible,
                    .invoke =
                        [self,
                         functionRef](
                            const commands::
                                CommandArguments&)
                        {
                            if (!self->
                                    InvokeRef(
                                        functionRef,
                                        "command"))
                            {
                                throw std::
                                    runtime_error(
                                        "Plugin command execution failed.");
                            }
                        }
                });

            self->commandIds_.
                push_back(id);

            lua_pushstring(
                state,
                id.ToString().
                    c_str());

            return 1;
        }

        static int LuaRegisterContextAction(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            const std::string_view idText(
                luaL_checkstring(
                    state,
                    1));
            const std::string surface =
                luaL_checkstring(
                    state,
                    2);
            const std::string_view kindText(
                luaL_checkstring(
                    state,
                    3));

            const auto id =
                commands::CommandId::Parse(
                    idText);

            if (!id.has_value() ||
                self->owner_.
                    commandRegistry.
                    Find(*id) == nullptr)
            {
                luaL_error(
                    state,
                    "registerContextAction requires a registered Orbit command ID");
            }

            const auto kind =
                CommandSurfaceKindFromString(
                    kindText);

            if (!kind.has_value())
            {
                luaL_error(
                    state,
                    "registerContextAction kind must be 'toolbar', 'context', or 'radial'");
            }

            self->owner_.
                commandSurfaces.
                Add(
                    surface,
                    *kind,
                    *id);

            const auto duplicate =
                std::find_if(
                    self->surfaceContributions_.
                        begin(),
                    self->surfaceContributions_.
                        end(),
                    [&](const SurfaceContribution&
                            contribution)
                    {
                        return
                            contribution.surface ==
                                surface &&
                            contribution.kind ==
                                *kind &&
                            contribution.command ==
                                *id;
                    });

            if (duplicate ==
                self->surfaceContributions_.end())
            {
                self->surfaceContributions_.
                    push_back({
                        .surface = surface,
                        .kind = *kind,
                        .command = *id
                    });
            }

            return 0;
        }

        static int LuaRegisterValidator(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            luaL_checktype(
                state,
                1,
                LUA_TFUNCTION);

            const int functionRef =
                lua_ref(
                    state,
                    1);

            self->validatorRefs_.
                push_back(
                    functionRef);

            return 0;
        }

        static int LuaRegisterPanel(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            const char* title =
                luaL_checkstring(
                    state,
                    1);

            luaL_checktype(
                state,
                2,
                LUA_TFUNCTION);

            const editor_ui::PanelId id =
                StablePanelId(
                    self->manifest_.id,
                    title);

            for (const Panel& panel :
                 self->panels_)
            {
                if (panel.descriptor.id ==
                    id)
                {
                    luaL_error(
                        state,
                        "Plugin panel is already registered");
                }
            }

            const int functionRef =
                lua_ref(
                    state,
                    2);

            self->panels_.push_back({
                .descriptor = {
                    .id = id,
                    .pluginId =
                        self->manifest_.id,
                    .title = title
                },
                .functionRef =
                    functionRef
            });

            lua_pushstring(
                state,
                id.ToString().
                    c_str());

            return 1;
        }

        [[nodiscard]] editor_ui::PanelContext&
        RequireUi(lua_State* state)
        {
            if (activePanelContext_ ==
                nullptr)
            {
                luaL_error(
                    state,
                    "Orbit.ui functions may only run while drawing a registered panel");
            }

            return *activePanelContext_;
        }

        static int LuaUiText(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            self->RequireUi(state).Text(
                luaL_checkstring(
                    state,
                    1));

            return 0;
        }

        static int LuaUiButton(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            const bool pressed =
                self->RequireUi(state).
                    Button(
                        luaL_checkstring(
                            state,
                            1));

            lua_pushboolean(
                state,
                pressed);

            return 1;
        }

        static int LuaUiToggle(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            bool value =
                luaL_checkboolean(
                    state,
                    2) != 0;

            static_cast<void>(
                self->RequireUi(state).
                    Checkbox(
                        luaL_checkstring(
                            state,
                            1),
                        value));

            lua_pushboolean(
                state,
                value);

            return 1;
        }

        static int LuaUiTextInput(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            std::string value =
                luaL_checkstring(
                    state,
                    2);

            const bool changed =
                self->RequireUi(state).
                    InputText(
                        luaL_checkstring(
                            state,
                            1),
                        value);

            lua_pushlstring(
                state,
                value.data(),
                value.size());

            lua_pushboolean(
                state,
                changed);

            return 2;
        }

        static int LuaUiNumberInput(
            lua_State* state)
        {
            PluginInstance* self =
                Self(state);

            f64 value =
                luaL_checknumber(
                    state,
                    2);

            const bool changed =
                self->RequireUi(state).
                    InputDouble(
                        luaL_checkstring(
                            state,
                            1),
                        value);

            lua_pushnumber(
                state,
                value);

            lua_pushboolean(
                state,
                changed);

            return 2;
        }

        void OpenSafeLibraries()
        {
            OpenLibrary(
                state_,
                "",
                luaopen_base);
            OpenLibrary(
                state_,
                LUA_COLIBNAME,
                luaopen_coroutine);
            OpenLibrary(
                state_,
                LUA_TABLIBNAME,
                luaopen_table);
            OpenLibrary(
                state_,
                LUA_STRLIBNAME,
                luaopen_string);
            OpenLibrary(
                state_,
                LUA_MATHLIBNAME,
                luaopen_math);
            OpenLibrary(
                state_,
                LUA_UTF8LIBNAME,
                luaopen_utf8);
            OpenLibrary(
                state_,
                LUA_BITLIBNAME,
                luaopen_bit32);
            OpenLibrary(
                state_,
                LUA_BUFFERLIBNAME,
                luaopen_buffer);
            OpenLibrary(
                state_,
                LUA_VECLIBNAME,
                luaopen_vector);
        }

        void InstallOrbitApi()
        {
            lua_newtable(state_);

            const auto setFunction =
                [this](
                    const char* name,
                    lua_CFunction function)
                {
                    lua_pushcfunction(
                        state_,
                        function,
                        name);

                    lua_setfield(
                        state_,
                        -2,
                        name);
                };

            setFunction(
                "log",
                &LuaLog);
            setFunction(
                "hasPermission",
                &LuaHasPermission);
            setFunction(
                "selectionCount",
                &LuaSelectionCount);
            setFunction(
                "renameSelected",
                &LuaRenameSelected);
            setFunction(
                "invokeCommand",
                &LuaInvokeCommand);
            setFunction(
                "registerCommand",
                &LuaRegisterCommand);
            setFunction(
                "registerContextAction",
                &LuaRegisterContextAction);
            setFunction(
                "registerValidator",
                &LuaRegisterValidator);
            setFunction(
                "registerPanel",
                &LuaRegisterPanel);

            lua_newtable(state_);

            lua_pushcfunction(
                state_,
                &LuaUiText,
                "text");
            lua_setfield(
                state_,
                -2,
                "text");

            lua_pushcfunction(
                state_,
                &LuaUiButton,
                "button");
            lua_setfield(
                state_,
                -2,
                "button");

            lua_pushcfunction(
                state_,
                &LuaUiToggle,
                "toggle");
            lua_setfield(
                state_,
                -2,
                "toggle");

            lua_pushcfunction(
                state_,
                &LuaUiTextInput,
                "textInput");
            lua_setfield(
                state_,
                -2,
                "textInput");

            lua_pushcfunction(
                state_,
                &LuaUiNumberInput,
                "numberInput");
            lua_setfield(
                state_,
                -2,
                "numberInput");

            lua_setfield(
                state_,
                -2,
                "ui");

            lua_setglobal(
                state_,
                "Orbit");
        }

        [[nodiscard]] bool InvokeRef(
            const int reference,
            const std::string_view kind)
        {
            if (state_ == nullptr ||
                reference <= LUA_REFNIL)
            {
                return false;
            }

            lua_getref(
                state_,
                reference);

            if (!lua_isfunction(
                    state_,
                    -1))
            {
                lua_pop(
                    state_,
                    1);
                return false;
            }

            if (lua_pcall(
                    state_,
                    0,
                    0,
                    0) != 0)
            {
                const char* error =
                    lua_tostring(
                        state_,
                        -1);

                log::Error(
                    std::format(
                        "Plugin '{}' {} failed: {}",
                        manifest_.id,
                        kind,
                        error != nullptr
                            ? error
                            : "unknown Luau error"));

                lua_pop(
                    state_,
                    1);

                return false;
            }

            return true;
        }

        void Unload()
        {
            for (const SurfaceContribution&
                     contribution :
                 surfaceContributions_)
            {
                static_cast<void>(
                    owner_.
                        commandSurfaces.
                        Remove(
                            contribution.surface,
                            contribution.kind,
                            contribution.command));
            }

            surfaceContributions_.clear();

            for (const commands::CommandId id :
                 commandIds_)
            {
                static_cast<void>(
                    owner_.
                        commandRegistry.
                        Unregister(id));
            }

            commandIds_.clear();
            validatorRefs_.clear();
            panels_.clear();
            activePanelContext_ = nullptr;

            if (state_ != nullptr)
            {
                lua_close(state_);
                state_ = nullptr;
            }

            loaded_ = false;
        }

        [[nodiscard]] u64 ObservedFingerprint() const noexcept
        {
            u64 hash =
                1469598103934665603ULL;

            try
            {
                const std::string manifestText =
                    ReadTextFile(
                        manifestPath_);

                hash =
                    HashText(
                        manifestText,
                        hash);
            }
            catch (...)
            {
                hash =
                    HashText(
                        "<missing-manifest>",
                        hash);
            }

            std::filesystem::path entry =
                manifest_.entryScript;

            if (entry.empty())
            {
                try
                {
                    entry =
                        LoadPluginManifest(
                            manifestPath_).
                            entryScript;
                }
                catch (...)
                {
                    return hash;
                }
            }

            try
            {
                hash =
                    HashText(
                        ReadTextFile(
                            packageRoot_ /
                            entry),
                        hash);
            }
            catch (...)
            {
                hash =
                    HashText(
                        "<missing-entry>",
                        hash);
            }

            return hash;
        }

        Impl& owner_;
        std::filesystem::path packageRoot_;
        std::filesystem::path manifestPath_;
        std::string expectedId_;
        std::string requiredVersion_;
        PluginManifest manifest_{};
        PluginPermissionSet effectivePermissions_{};
        lua_State* state_{nullptr};
        editor_ui::PanelContext*
            activePanelContext_{nullptr};
        std::vector<commands::CommandId>
            commandIds_;
        std::vector<SurfaceContribution>
            surfaceContributions_;
        std::vector<int> validatorRefs_;
        std::vector<Panel> panels_;
        u64 fingerprint_{0};
        u64 revision_{0};
        bool loaded_{false};
        std::string error_;
    };

    Impl(
        std::filesystem::path root,
        commands::CommandRegistry& registry,
        commands::CommandService& commands,
        editor_model::CommandSurfaceRegistry& surfaces,
        scene::ObjectStore& objectStore,
        selection::SelectionService& selectionService)
        : projectRoot(std::move(root)),
          commandRegistry(registry),
          commandService(commands),
          commandSurfaces(surfaces),
          objects(objectStore),
          selection(selectionService)
    {
    }

    std::filesystem::path projectRoot;
    commands::CommandRegistry& commandRegistry;
    commands::CommandService& commandService;
    editor_model::CommandSurfaceRegistry&
        commandSurfaces;
    scene::ObjectStore& objects;
    selection::SelectionService& selection;
    std::unordered_map<
        std::string,
        PluginPermissionSet>
        grantedPermissions;
    std::unordered_map<
        std::string,
        std::string>
        enabledVersions;
    std::vector<
        std::unique_ptr<PluginInstance>>
        instances;
    u64 panelCatalogRevision{0};
};

PluginManager::PluginManager(
    std::filesystem::path projectRoot,
    commands::CommandRegistry& commandRegistry,
    commands::CommandService& commandService,
    editor_model::CommandSurfaceRegistry& commandSurfaces,
    scene::ObjectStore& objects,
    selection::SelectionService& selection)
    : impl_(
          std::make_unique<Impl>(
              std::move(projectRoot),
              commandRegistry,
              commandService,
              commandSurfaces,
              objects,
              selection))
{
}

PluginManager::~PluginManager() = default;

void PluginManager::SetGrantedPermissions(
    std::string pluginId,
    const PluginPermissionSet permissions)
{
    impl_->grantedPermissions.
        insert_or_assign(
            std::move(pluginId),
            permissions);
}

void PluginManager::LoadEnabled(
    const documents::ProjectManifest& project)
{
    impl_->instances.clear();
    impl_->enabledVersions.clear();
    ++impl_->panelCatalogRevision;

    for (const documents::PluginRequirement&
             requirement :
         project.plugins)
    {
        impl_->enabledVersions.insert_or_assign(
            requirement.id,
            requirement.version);

        PluginPermissionSet grants{};

        for (const std::string& name :
             requirement.grantedPermissions)
        {
            const auto permission =
                PermissionFromString(name);

            if (!permission.has_value())
            {
                log::Warning(
                    std::format(
                        "Project grants unknown permission '{}' to plugin '{}'; the grant is ignored.",
                        name,
                        requirement.id));
                continue;
            }

            grants.Add(*permission);
        }

        impl_->grantedPermissions.
            insert_or_assign(
                requirement.id,
                grants);
    }

    for (const documents::PluginRequirement&
             requirement :
         project.plugins)
    {
        const std::filesystem::path
            packageRoot =
                impl_->projectRoot /
                "Plugins" /
                requirement.id;

        const std::filesystem::path
            manifestPath =
                packageRoot /
                "plugin.toml";

        auto instance =
            std::make_unique<
                Impl::PluginInstance>(
                    *impl_,
                    packageRoot,
                    manifestPath,
                    requirement.id,
                    requirement.version);

        try
        {
            static_cast<void>(
                instance->Load());
        }
        catch (const std::exception&
                   exception)
        {
            instance->SetError(
                exception.what());

            log::Error(
                std::format(
                    "Plugin '{}' failed to load: {}",
                    requirement.id,
                    exception.what()));
        }

        impl_->instances.push_back(
            std::move(instance));
    }

    ++impl_->panelCatalogRevision;
}

u32 PluginManager::PollHotReload()
{
    u32 reloaded = 0;

    for (auto& instance :
         impl_->instances)
    {
        if (!instance->NeedsReload())
        {
            continue;
        }

        try
        {
            static_cast<void>(
                instance->Load());
        }
        catch (const std::exception&
                   exception)
        {
            instance->SetError(
                exception.what());

            log::Error(
                std::format(
                    "Plugin hot reload failed: {}",
                    exception.what()));
        }

        ++reloaded;
        ++impl_->panelCatalogRevision;
    }

    return reloaded;
}

bool PluginManager::Reload(
    const std::string_view pluginId)
{
    for (auto& instance :
         impl_->instances)
    {
        if (instance->Id() !=
            pluginId)
        {
            continue;
        }

        try
        {
            const bool loaded =
                instance->Load();

            ++impl_->panelCatalogRevision;
            return loaded;
        }
        catch (const std::exception&
                   exception)
        {
            instance->SetError(
                exception.what());

            ++impl_->panelCatalogRevision;
            return false;
        }
    }

    return false;
}

std::vector<PluginStatus>
PluginManager::Statuses() const
{
    std::vector<PluginStatus> result;
    result.reserve(
        impl_->instances.size());

    for (const auto& instance :
         impl_->instances)
    {
        result.push_back(
            instance->Status());
    }

    return result;
}

std::vector<PluginValidationIssue>
PluginManager::Validate()
{
    std::vector<PluginValidationIssue>
        result;

    for (auto& instance :
         impl_->instances)
    {
        for (std::string& message :
             instance->Validate())
        {
            result.push_back({
                .pluginId = instance->Id(),
                .message =
                    std::move(message)
            });
        }
    }

    return result;
}

std::vector<PluginPanelDescriptor>
PluginManager::PanelCatalog() const
{
    std::vector<PluginPanelDescriptor>
        result;

    for (const auto& instance :
         impl_->instances)
    {
        for (const auto& panel :
             instance->Panels())
        {
            result.push_back(
                panel.descriptor);
        }
    }

    return result;
}

u64 PluginManager::PanelCatalogRevision() const noexcept
{
    return impl_->panelCatalogRevision;
}

bool PluginManager::DrawPanel(
    const editor_ui::PanelId panel,
    editor_ui::PanelContext& context)
{
    for (auto& instance :
         impl_->instances)
    {
        if (instance->DrawPanel(
                panel,
                context))
        {
            return true;
        }
    }

    return false;
}
} // namespace orbit::plugins
