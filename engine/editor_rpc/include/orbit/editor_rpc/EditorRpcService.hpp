#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <string>
#include <vector>

namespace orbit::editor_rpc
{
// Registers the structured authoring API onto a transport-independent
// JSON-RPC dispatcher. All persistent mutations route through CommandService
// or CommandRegistry, preserving validation, transactions and undo/redo.
class EditorRpcService
{
public:
    EditorRpcService(
        rpc::Dispatcher& dispatcher,
        const documents::ProjectDocument& project,
        commands::CommandRegistry& commandRegistry,
        commands::CommandService& commandService,
        const schema::SchemaRegistry& schemas,
        scene::ObjectStore& objects,
        selection::SelectionService& selection);

    ~EditorRpcService();

    EditorRpcService(
        const EditorRpcService&) = delete;
    EditorRpcService& operator=(
        const EditorRpcService&) = delete;

private:
    void Register(
        rpc::MethodDescriptor descriptor,
        rpc::Dispatcher::MethodHandler handler);

    rpc::Dispatcher& dispatcher_;
    std::vector<std::string>
        registeredMethods_;
};
} // namespace orbit::editor_rpc
