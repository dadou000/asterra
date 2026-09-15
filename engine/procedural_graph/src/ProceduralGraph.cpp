#include <orbit/procedural_graph/ProceduralGraph.hpp>

#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace orbit::procedural_graph
{
struct ProceduralGraph::PendingBuild
{
    std::mutex mutex;
    u64 generation{0};
    u64 inputRevisionHash{0};
    std::any product;
    std::string error;
};

namespace
{
[[nodiscard]] u64 Mix(
    u64 hash,
    const u64 value) noexcept
{
    hash ^=
        value +
        0x9e3779b97f4a7c15ULL +
        (hash << 6) +
        (hash >> 2);
    return hash;
}
} // namespace

ProceduralGraph::ProceduralGraph(
    jobs::JobSystem& jobs)
    : jobs_(jobs)
{
}

NodeId ProceduralGraph::AddSource(
    const std::string_view name,
    const u64 initialRevision)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Procedural source name must not be empty.");
    }

    NodeId id = NodeId::Random();

    while (nodes_.contains(id))
    {
        id = NodeId::Random();
    }

    nodes_.emplace(
        id,
        NodeRecord{
            .name = std::string(name),
            .source = true,
            .state = NodeState::Clean,
            .sourceRevision = initialRevision,
            .committedRevision =
                initialRevision
        });

    return id;
}

NodeId ProceduralGraph::AddDerived(
    const std::string_view name,
    std::vector<NodeId> dependencies,
    const ExecutionBackend backend,
    BuildFunction build)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Procedural node name must not be empty.");
    }

    if (!build)
    {
        throw std::invalid_argument(
            "Derived procedural node requires a build function.");
    }

    for (const NodeId dependency :
         dependencies)
    {
        if (!nodes_.contains(dependency))
        {
            throw std::invalid_argument(
                "Procedural dependency does not exist.");
        }
    }

    NodeId id = NodeId::Random();

    while (nodes_.contains(id))
    {
        id = NodeId::Random();
    }

    nodes_.emplace(
        id,
        NodeRecord{
            .name = std::string(name),
            .source = false,
            .backend = backend,
            .dependencies =
                std::move(dependencies),
            .build = std::move(build),
            .state = NodeState::Dirty
        });

    NodeRecord& record =
        nodes_.at(id);

    for (const NodeId dependency :
         record.dependencies)
    {
        nodes_.at(dependency).
            dependents.push_back(id);
    }

    return id;
}

void ProceduralGraph::SetSourceRevision(
    const NodeId source,
    const u64 revision)
{
    auto found = nodes_.find(source);

    if (found == nodes_.end() ||
        !found->second.source)
    {
        throw std::invalid_argument(
            "SetSourceRevision requires a source node.");
    }

    NodeRecord& record =
        found->second;

    if (record.sourceRevision == revision)
    {
        return;
    }

    record.sourceRevision = revision;
    record.committedRevision = revision;

    std::unordered_set<NodeId> visited;
    InvalidateDescendants(
        source,
        visited);
}

void ProceduralGraph::Invalidate(
    const NodeId node)
{
    auto found = nodes_.find(node);

    if (found == nodes_.end())
    {
        throw std::invalid_argument(
            "Cannot invalidate unknown procedural node.");
    }

    NodeRecord& record =
        found->second;

    if (record.source)
    {
        ++record.sourceRevision;
        record.committedRevision =
            record.sourceRevision;
    }
    else
    {
        ++record.configurationRevision;
        ++record.requestedGeneration;

        if (record.state !=
            NodeState::Building)
        {
            record.state =
                NodeState::Dirty;
        }

        record.error.clear();
    }

    std::unordered_set<NodeId> visited;
    InvalidateDescendants(
        node,
        visited);
}

void ProceduralGraph::InvalidateDescendants(
    const NodeId node,
    std::unordered_set<NodeId>& visited)
{
    if (!visited.insert(node).second)
    {
        return;
    }

    const auto found = nodes_.find(node);

    if (found == nodes_.end())
    {
        return;
    }

    for (const NodeId dependent :
         found->second.dependents)
    {
        NodeRecord& record =
            nodes_.at(dependent);

        ++record.requestedGeneration;

        if (record.state !=
            NodeState::Building)
        {
            record.state =
                NodeState::Dirty;
        }

        record.error.clear();

        InvalidateDescendants(
            dependent,
            visited);
    }
}

void ProceduralGraph::RequestBuild(
    const NodeId target)
{
    if (!nodes_.contains(target))
    {
        throw std::invalid_argument(
            "Cannot build unknown procedural node.");
    }

    requestedTargets_.insert(target);
}

u64 ProceduralGraph::InputRevisionHash(
    const NodeRecord& node) const noexcept
{
    u64 hash =
        Mix(
            0xcbf29ce484222325ULL,
            node.configurationRevision);

    for (const NodeId dependency :
         node.dependencies)
    {
        const auto found =
            nodes_.find(dependency);

        if (found == nodes_.end())
        {
            continue;
        }

        hash = Mix(
            hash,
            dependency.high);
        hash = Mix(
            hash,
            dependency.low);
        hash = Mix(
            hash,
            found->second.
                committedRevision);
    }

    return hash;
}

void ProceduralGraph::Schedule(
    const NodeId node)
{
    NodeRecord& record =
        nodes_.at(node);

    const u64 inputHash =
        InputRevisionHash(record);
    const u64 generation =
        record.requestedGeneration;
    const u64 configurationRevision =
        record.configurationRevision;

    auto pending =
        std::make_shared<PendingBuild>();

    pending->generation = generation;
    pending->inputRevisionHash =
        inputHash;

    record.pending = pending;
    record.jobGroup =
        std::make_unique<jobs::JobGroup>();
    record.state = NodeState::Building;
    record.error.clear();

    const BuildFunction build =
        record.build;
    const BuildContext context{
        .node = node,
        .generation = generation,
        .inputRevisionHash = inputHash,
        .configurationRevision =
            configurationRevision
    };

    jobs_.Submit(
        *record.jobGroup,
        [pending, build, context]
        {
            std::any product;
            std::string error;

            try
            {
                product = build(context);
            }
            catch (const std::exception& exception)
            {
                error = exception.what();
            }
            catch (...)
            {
                error =
                    "Unknown procedural build error.";
            }

            std::scoped_lock lock(
                pending->mutex);

            pending->product =
                std::move(product);
            pending->error =
                std::move(error);
        });
}

void ProceduralGraph::FinalizeCompleted()
{
    for (auto& [id, record] :
         nodes_)
    {
        static_cast<void>(id);

        if (record.state !=
                NodeState::Building ||
            record.jobGroup == nullptr ||
            !record.jobGroup->IsComplete())
        {
            continue;
        }

        std::any product;
        std::string error;
        u64 generation = 0;
        u64 inputHash = 0;

        {
            std::scoped_lock lock(
                record.pending->mutex);

            product =
                std::move(
                    record.pending->product);
            error =
                std::move(
                    record.pending->error);
            generation =
                record.pending->generation;
            inputHash =
                record.pending->
                    inputRevisionHash;
        }

        record.pending.reset();
        record.jobGroup.reset();

        const bool stale =
            generation !=
                record.requestedGeneration ||
            inputHash !=
                InputRevisionHash(record);

        if (stale)
        {
            record.state =
                NodeState::Dirty;
            continue;
        }

        if (!error.empty())
        {
            record.state =
                NodeState::Failed;
            record.error =
                std::move(error);
            continue;
        }

        record.product =
            std::move(product);
        record.lastInputRevisionHash =
            inputHash;
        ++record.committedRevision;
        record.state = NodeState::Clean;
        record.error.clear();
    }
}

bool ProceduralGraph::TryScheduleRecursive(
    const NodeId node,
    std::unordered_set<NodeId>& visiting)
{
    auto found = nodes_.find(node);

    if (found == nodes_.end())
    {
        return false;
    }

    NodeRecord& record =
        found->second;

    if (record.state == NodeState::Clean)
    {
        return true;
    }

    if (record.state == NodeState::Building ||
        record.state == NodeState::Failed)
    {
        return false;
    }

    if (!visiting.insert(node).second)
    {
        throw std::logic_error(
            "Procedural graph contains a dependency cycle.");
    }

    bool dependenciesReady = true;

    for (const NodeId dependency :
         record.dependencies)
    {
        if (!TryScheduleRecursive(
                dependency,
                visiting))
        {
            dependenciesReady = false;
        }
    }

    visiting.erase(node);

    if (!dependenciesReady)
    {
        return false;
    }

    if (!record.source &&
        record.state == NodeState::Dirty)
    {
        Schedule(node);
        return false;
    }

    return record.state ==
        NodeState::Clean;
}

void ProceduralGraph::Poll()
{
    FinalizeCompleted();

    std::vector<NodeId> completedTargets;

    for (const NodeId target :
         requestedTargets_)
    {
        std::unordered_set<NodeId> visiting;

        const bool ready =
            TryScheduleRecursive(
                target,
                visiting);

        const auto found =
            nodes_.find(target);

        if (ready ||
            (found != nodes_.end() &&
             found->second.state ==
                 NodeState::Failed))
        {
            completedTargets.push_back(
                target);
        }
    }

    for (const NodeId target :
         completedTargets)
    {
        requestedTargets_.erase(target);
    }
}

bool ProceduralGraph::BuildBlocking(
    const NodeId target)
{
    RequestBuild(target);

    while (true)
    {
        Poll();

        const auto status =
            Status(target);

        if (!status.has_value())
        {
            return false;
        }

        if (status->state ==
            NodeState::Clean)
        {
            return true;
        }

        if (status->state ==
            NodeState::Failed)
        {
            return false;
        }

        jobs_.WaitIdle();
    }
}

std::optional<NodeStatus>
ProceduralGraph::Status(
    const NodeId node) const
{
    const auto found = nodes_.find(node);

    if (found == nodes_.end())
    {
        return std::nullopt;
    }

    const NodeRecord& record =
        found->second;

    return NodeStatus{
        .state = record.state,
        .committedRevision =
            record.committedRevision,
        .requestedGeneration =
            record.requestedGeneration,
        .lastInputRevisionHash =
            record.lastInputRevisionHash,
        .error = record.error
    };
}
} // namespace orbit::procedural_graph
