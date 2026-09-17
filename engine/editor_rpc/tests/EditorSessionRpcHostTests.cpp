#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_rpc/EditorSessionRpcHost.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>
#include <utility>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Editor session RPC host test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

[[nodiscard]] orbit::rpc::Value Request(
    orbit::editor_rpc::EditorSessionRpcHost& host,
    std::string id,
    std::string method,
    orbit::rpc::Value params =
        orbit::rpc::Value(
            orbit::rpc::Value::Object{}))
{
    orbit::rpc::Value request(
        orbit::rpc::Value::Object{
            {"jsonrpc", "2.0"},
            {"id", std::move(id)},
            {"method", std::move(method)},
            {"params", std::move(params)}
        });

    const auto response =
        host.Dispatch(
            orbit::rpc::Serialize(request));
    Check(response.has_value());
    return orbit::rpc::ParseValue(*response);
}

[[nodiscard]] orbit::rpc::Value Call(
    orbit::editor_rpc::EditorSessionRpcHost& host,
    std::string id,
    std::string method,
    orbit::rpc::Value params =
        orbit::rpc::Value(
            orbit::rpc::Value::Object{}))
{
    const auto response =
        Request(
            host,
            std::move(id),
            std::move(method),
            std::move(params));
    Check(response.Find("error") == nullptr);
    const auto* result = response.Find("result");
    Check(result != nullptr);
    return *result;
}

[[nodiscard]] orbit::i64 CallError(
    orbit::editor_rpc::EditorSessionRpcHost& host,
    std::string id,
    std::string method,
    orbit::rpc::Value params =
        orbit::rpc::Value(
            orbit::rpc::Value::Object{}))
{
    const auto response =
        Request(
            host,
            std::move(id),
            std::move(method),
            std::move(params));
    const auto* error = response.Find("error");
    Check(error != nullptr);
    const auto* code = error->Find("code");
    Check(code != nullptr);
    return code->AsInteger();
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-editor-session-rpc-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Session RPC Test");
        const auto secondary =
            project.CreateWorld(
                "Secondary",
                "Secondary World");

        orbit::editor_session::EditorWorldSession
            session(project);

        const auto worldRoot =
            session.Commands().CreateObject(
                orbit::editor_model::builtin::kWorldType,
                "Main World");
        session.Checkpoint();

        orbit::editor_rpc::EditorSessionRpcHost
            host(session);

        Check(host.Editor() != nullptr);

        const auto activeMain =
            Call(host, "1", "world.active");
        Check(activeMain.Find("open")->AsBool());
        Check(
            activeMain.Find("path")->AsString() ==
            "Worlds/Main.orbitworld");
        Check(
            activeMain.Find("generation")->AsInteger() ==
            1);

        const auto catalog =
            Call(host, "2", "world.list");
        Check(catalog.IsArray());
        Check(catalog.AsArray().size() == 2);

        orbit::rpc::Value batch(
            orbit::rpc::Value::Array{
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"jsonrpc", "2.0"},
                        {"id", "batch-open"},
                        {"method", "world.open"},
                        {
                            "params",
                            orbit::rpc::Value::Object{
                                {
                                    "path",
                                    secondary.generic_string()
                                }
                            }
                        }
                    }),
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"jsonrpc", "2.0"},
                        {"id", "batch-active"},
                        {"method", "world.active"},
                        {
                            "params",
                            orbit::rpc::Value::Object{}
                        }
                    })
            });

        const auto batchResponse =
            host.Dispatch(
                orbit::rpc::Serialize(batch));
        Check(batchResponse.has_value());
        const auto batchError =
            orbit::rpc::ParseValue(
                *batchResponse);
        Check(
            batchError.Find("error") != nullptr);
        Check(
            batchError.Find("error")->
                Find("code")->AsInteger() ==
            1025);
        Check(session.Generation() == 1U);
        Check(
            session.ActiveWorld().relativePath ==
            std::filesystem::path(
                "Worlds/Main.orbitworld"));

        static_cast<void>(
            Call(
                host,
                "3",
                "transaction.begin",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"label", "Block switch"}
                    })));

        Check(
            CallError(
                host,
                "4",
                "world.open",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "path",
                            secondary.generic_string()
                        }
                    })) ==
            1024);
        Check(session.Generation() == 1U);

        static_cast<void>(
            Call(
                host,
                "5",
                "transaction.rollback"));

        const auto openedSecondary =
            Call(
                host,
                "6",
                "world.open",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "path",
                            secondary.generic_string()
                        }
                    }));
        Check(openedSecondary.Find("open")->AsBool());
        Check(
            openedSecondary.Find("path")->AsString() ==
            secondary.generic_string());
        Check(session.Generation() == 2U);
        Check(host.Editor() != nullptr);

        const auto secondaryRoots =
            Call(host, "7", "object.roots");
        Check(secondaryRoots.IsArray());
        Check(secondaryRoots.AsArray().empty());

        const auto closed =
            Call(host, "8", "world.close");
        Check(!closed.Find("open")->AsBool());
        Check(session.Generation() == 3U);
        Check(!session.HasWorld());
        Check(host.Editor() == nullptr);

        const auto closedCatalog =
            Call(host, "9", "world.list");
        Check(closedCatalog.IsArray());
        Check(closedCatalog.AsArray().size() == 2);

        const auto projectInfo =
            Call(host, "10", "project.info");
        Check(
            projectInfo.Find("name")->AsString() ==
            "Session RPC Test");

        Check(
            CallError(
                host,
                "11",
                "object.roots") ==
            -32601);

        static_cast<void>(
            Call(
                host,
                "12",
                "world.set_startup",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "path",
                            secondary.generic_string()
                        }
                    })));
        Check(
            project.Manifest().startupWorld ==
            secondary);

        const auto reopenedMain =
            Call(
                host,
                "13",
                "world.open",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {
                            "path",
                            "Worlds/Main.orbitworld"
                        }
                    }));
        Check(reopenedMain.Find("open")->AsBool());
        Check(session.Generation() == 4U);
        Check(host.Editor() != nullptr);

        const auto mainRoots =
            Call(host, "14", "object.roots");
        Check(mainRoots.IsArray());
        Check(mainRoots.AsArray().size() == 1);
        Check(
            mainRoots.AsArray().front().
                Find("id")->AsString() ==
            worldRoot.ToString());

        const auto activeAgain =
            Call(host, "15", "world.active");
        Check(activeAgain.Find("open")->AsBool());
        Check(
            activeAgain.Find("path")->AsString() ==
            "Worlds/Main.orbitworld");
    }

    std::filesystem::remove_all(root);
    return 0;
}
