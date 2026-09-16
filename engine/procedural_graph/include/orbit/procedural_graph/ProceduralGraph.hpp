#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/jobs/JobSystem.hpp>

#include <any>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace orbit::procedural_graph
{
struct NodeIdTag;
using NodeId = core::StrongId<NodeIdTag>;

enum class ExecutionBackend : u8
{
    Cpu,
    Gpu
};

enum class NodeState : u8
{
    Clean,
    Dirty,
    Building,
    Failed
};

struct BuildContext
{
    NodeId node{};
    u64 generation{0};
    u64 inputRevisionHash{0};
    u64 configurationRevision{0};
};

using BuildFunction =
    std::function<std::any(
        const BuildContext&)>;

struct NodeStatus
{
    NodeState state{NodeState::Dirty};
    u64 committedRevision{0};
    u64 requestedGeneration{0};
    u64 lastInputRevisionHash{0};
    std::string error;
};

// CPU-authoritative dependency graph. A GPU node means the node's build
// function is responsible for issuing GPU work through an injected GPU
// service; the graph itself remains a sparse CPU scheduler and revision
// authority.
class ProceduralGraph
{
public:
    explicit ProceduralGraph(
        jobs::JobSystem& jobs);

    [[nodiscard]] NodeId AddSource(
        std::string_view name,
        u64 initialRevision = 1);

    [[nodiscard]] NodeId AddDerived(
        std::string_view name,
        std::vector<NodeId> dependencies,
        ExecutionBackend backend,
        BuildFunction build);

    void SetSourceRevision(
        NodeId source,
        u64 revision);

    // Marks a node's own configuration as changed, then invalidates all
    // derived descendants. The node keeps its previous product until a
    // newer build commits.
    void Invalidate(NodeId node);

    void RequestBuild(NodeId target);

    // Finalizes completed jobs and schedules newly-ready work.
    void Poll();

    // Useful for headless build/cook paths and deterministic tests.
    // Returns false if the requested node fails.
    [[nodiscard]] bool BuildBlocking(
        NodeId target);

    [[nodiscard]] std::optional<NodeStatus>
    Status(NodeId node) const;

    template <typename T>
    [[nodiscard]] const T* Product(
        const NodeId node) const
    {
        const auto found = nodes_.find(node);

        if (found == nodes_.end() ||
            found->second.state !=
                NodeState::Clean)
        {
            return nullptr;
        }

        return std::any_cast<T>(
            &found->second.product);
    }

private:
    struct PendingBuild;

    struct NodeRecord
    {
        std::string name;
        bool source{false};
        ExecutionBackend backend{
            ExecutionBackend::Cpu};
        std::vector<NodeId> dependencies;
        std::vector<NodeId> dependents;
        BuildFunction build;
        NodeState state{NodeState::Dirty};
        u64 sourceRevision{0};
        u64 committedRevision{0};
        u64 configurationRevision{1};
        u64 requestedGeneration{1};
        u64 lastInputRevisionHash{0};
        std::any product;
        std::string error;
        std::shared_ptr<PendingBuild> pending;
        std::unique_ptr<jobs::JobGroup> jobGroup;
    };

    void InvalidateDescendants(
        NodeId node,
        std::unordered_set<NodeId>& visited);

    [[nodiscard]] bool TryScheduleRecursive(
        NodeId node,
        std::unordered_set<NodeId>& visiting);

    void FinalizeCompleted();
    void Schedule(NodeId node);

    [[nodiscard]] u64 InputRevisionHash(
        const NodeRecord& node) const noexcept;

    jobs::JobSystem& jobs_;
    std::unordered_map<NodeId, NodeRecord>
        nodes_;
    std::unordered_set<NodeId>
        requestedTargets_;
};
} // namespace orbit::procedural_graph
