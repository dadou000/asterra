#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/render_view/Capture.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace orbit::editor_rpc
{
struct ViewportAutomation
{
    render_view::RenderView* view{nullptr};
    std::function<render_view::CaptureResult(
        const std::filesystem::path&)>
        capture;
};

struct PathRoutingAutomation
{
    std::function<rpc::Value(
        scene::ObjectId)>
        status;
    std::function<rpc::Value(
        scene::ObjectId)>
        result;
    std::function<void(
        scene::ObjectId)>
        invalidate;
};

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
        selection::SelectionService& selection,
        ViewportAutomation viewport = {});

    ~EditorRpcService();

    EditorRpcService(
        const EditorRpcService&) = delete;
    EditorRpcService& operator=(
        const EditorRpcService&) = delete;

    // Adds a sequenced event to the replay journal and queues a JSON-RPC
    // notification for any currently connected automation client.
    void PublishEvent(
        std::string type,
        rpc::Value data = {});

    [[nodiscard]] std::vector<std::string>
    DrainNotifications();

    [[nodiscard]] u64 LatestEventSequence() const noexcept;

    // Attaches the live primary Studio view once graphics/runtime
    // composition exists. May be called at most once.
    void AttachViewport(
        ViewportAutomation viewport);

    void AttachPathRouting(
        PathRoutingAutomation routing);

private:
    void Register(
        rpc::MethodDescriptor descriptor,
        rpc::Dispatcher::MethodHandler handler);

    struct EventRecord
    {
        u64 sequence{0};
        std::string type;
        rpc::Value data;
    };

    rpc::Dispatcher& dispatcher_;
    std::vector<std::string>
        registeredMethods_;
    std::deque<EventRecord> events_;
    std::vector<std::string>
        pendingNotifications_;
    u64 nextEventSequence_{1};
    bool viewportRegistered_{false};
    bool pathRoutingRegistered_{false};
};
} // namespace orbit::editor_rpc
