#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/studio_session/StudioWorkspaceRpcHost.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio workspace RPC test failed.\n";
        std::exit(1);
    }
}

orbit::rpc::Value Call(
    orbit::studio_session::StudioWorkspaceRpcHost& rpc,
    const std::string& id,
    const std::string& method,
    orbit::rpc::Value params =
        orbit::rpc::Value(orbit::rpc::Value::Object{}))
{
    const orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", method},
            {"params", std::move(params)}
        });
    const auto text = rpc.Dispatch(orbit::rpc::Serialize(request));
    Check(text.has_value());
    const auto response = orbit::rpc::ParseValue(*text);
    Check(response.Find("error") == nullptr);
    Check(response.Find("result") != nullptr);
    return *response.Find("result");
}
} // namespace

int main()
{
    const auto base =
        std::filesystem::temp_directory_path() /
        ("orbit-workspace-rpc-" +
         orbit::documents::ProjectId::Random().ToString());
    const auto first = base / "First";
    const auto second = base / "Second";
    std::filesystem::remove_all(base);

    {
        orbit::studio_session::StudioWorkspace workspace;
        orbit::studio_session::StudioWorkspaceRpcHost rpc(workspace);

        const auto empty = Call(rpc, "1", "workspace.active");
        Check(!empty.Find("open")->AsBool());

        const auto created =
            Call(
                rpc,
                "2",
                "project.create",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"root", first.generic_string()},
                        {"name", "First Project"}
                    }));
        Check(created.Find("open")->AsBool());
        Check(created.Find("name")->AsString() == "First Project");
        Check(
            created.Find("startup_world")->AsString() ==
            "Worlds/Main.orbitworld");

        const auto renamed =
            Call(
                rpc,
                "3",
                "project.set_display_name",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"name", "First Project Renamed"}
                    }));
        Check(
            renamed.Find("name")->AsString() ==
            "First Project Renamed");
        Check(
            workspace.Project().Manifest().displayName ==
            "First Project Renamed");

        const auto root =
            workspace.Session().World().Commands().CreateObject(
                orbit::world_model::kWorldType,
                "Persisted Root");
        Check(root.IsValid());

        const auto projectInfo = Call(rpc, "4", "project.info");
        Check(
            projectInfo.Find("name")->AsString() ==
            "First Project Renamed");

        const auto roots = Call(rpc, "5", "object.roots");
        Check(roots.IsArray());
        Check(roots.AsArray().size() == 1U);
        Check(
            roots.AsArray().front().Find("id")->AsString() ==
            root.ToString());

        static_cast<void>(
            Call(
                rpc,
                "6",
                "project.create",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"root", second.generic_string()},
                        {"name", "Second Project"}
                    })));
        Check(workspace.Project().Manifest().displayName == "Second Project");
        Check(
            Call(rpc, "7", "object.roots").
                AsArray().empty());

        const auto reopened =
            Call(
                rpc,
                "8",
                "project.open",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"path", first.generic_string()}
                    }));
        Check(
            reopened.Find("name")->AsString() ==
            "First Project Renamed");
        const auto reopenedRoots = Call(rpc, "9", "object.roots");
        Check(reopenedRoots.AsArray().size() == 1U);
        Check(
            reopenedRoots.AsArray().front().Find("id")->AsString() ==
            root.ToString());

        const auto closed = Call(rpc, "10", "project.close");
        Check(!closed.Find("open")->AsBool());
        Check(!workspace.HasProject());

        const orbit::rpc::Value noProjectRequest(
            orbit::rpc::Value::Object{
                {"jsonrpc", "2.0"},
                {"id", "11"},
                {"method", "object.roots"},
                {"params", orbit::rpc::Value::Object{}}
            });
        const auto noProjectText =
            rpc.Dispatch(orbit::rpc::Serialize(noProjectRequest));
        Check(noProjectText.has_value());
        const auto noProject = orbit::rpc::ParseValue(*noProjectText);
        Check(noProject.Find("error") != nullptr);
        Check(
            noProject.Find("error")->Find("code")->AsInteger() ==
            1040);

        const orbit::rpc::Value noProjectRenameRequest(
            orbit::rpc::Value::Object{
                {"jsonrpc", "2.0"},
                {"id", "12"},
                {"method", "project.set_display_name"},
                {
                    "params",
                    orbit::rpc::Value::Object{
                        {"name", "Unavailable"}
                    }
                }
            });
        const auto noProjectRenameText =
            rpc.Dispatch(
                orbit::rpc::Serialize(noProjectRenameRequest));
        Check(noProjectRenameText.has_value());
        const auto noProjectRename =
            orbit::rpc::ParseValue(*noProjectRenameText);
        Check(noProjectRename.Find("error") != nullptr);
        Check(
            noProjectRename.Find("error")->Find("code")->AsInteger() ==
            1040);
    }

    std::filesystem::remove_all(base);
    return 0;
}
