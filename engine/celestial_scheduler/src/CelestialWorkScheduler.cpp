#include <orbit/celestial_scheduler/CelestialWorkScheduler.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace orbit::celestial_scheduler
{
std::size_t WorkKeyHash::operator()(
    const WorkKey& key) const noexcept
{
    const auto mix =
        [](u64 value)
        {
            value ^= value >> 30U;
            value *= 0xbf58476d1ce4e5b9ULL;
            value ^= value >> 27U;
            value *= 0x94d049bb133111ebULL;
            value ^= value >> 31U;
            return value;
        };

    u64 hash =
        mix(key.subjectHigh);
    hash ^=
        mix(
            key.subjectLow +
            0x9e3779b97f4a7c15ULL);
    hash ^=
        mix(
            static_cast<u64>(key.kind) +
            0x632be59bd9b4e019ULL);

    return
        static_cast<std::size_t>(
            hash);
}

CelestialWorkScheduler::CelestialWorkScheduler(
    SchedulerBudget budget)
{
    SetBudget(budget);
}

void CelestialWorkScheduler::ValidateBudget(
    const SchedulerBudget& budget) const
{
    if (budget.maxCpuJobsPerFrame == 0U ||
        budget.maxGpuJobsPerFrame == 0U ||
        budget.maxCpuCostUnitsPerFrame == 0U ||
        budget.maxGpuCostUnitsPerFrame == 0U ||
        budget.maxPendingRequests == 0U)
    {
        throw std::invalid_argument(
            "Celestial scheduler budgets must be non-zero.");
    }
}

void CelestialWorkScheduler::SetBudget(
    const SchedulerBudget budget)
{
    ValidateBudget(budget);
    budget_ = budget;
    TrimPending();
}

const SchedulerBudget&
CelestialWorkScheduler::Budget() const noexcept
{
    return budget_;
}

void CelestialWorkScheduler::ValidateRequest(
    const WorkRequest& request) const
{
    if ((request.key.subjectHigh == 0U &&
         request.key.subjectLow == 0U) ||
        request.key.kind == WorkKind::Count ||
        request.authorityRevision == 0U ||
        request.costUnits == 0U)
    {
        throw std::invalid_argument(
            "Celestial work request is invalid.");
    }
}

void CelestialWorkScheduler::Enqueue(
    const WorkRequest& request)
{
    ValidateRequest(request);

    currentRevision_.insert_or_assign(
        request.key,
        request.authorityRevision);

    auto pending =
        std::find_if(
            pending_.begin(),
            pending_.end(),
            [&](const PendingEntry& entry)
            {
                return entry.request.key ==
                    request.key;
            });

    if (pending != pending_.end())
    {
        pending->request = request;
        pending->sequence =
            ++sequence_;
    }
    else
    {
        pending_.push_back({
            .request = request,
            .sequence = ++sequence_
        });
    }

    TrimPending();
}

void CelestialWorkScheduler::TrimPending()
{
    if (pending_.size() <=
        budget_.maxPendingRequests)
    {
        return;
    }

    const auto worse =
        [](const PendingEntry& a,
           const PendingEntry& b)
        {
            if (a.request.visible !=
                b.request.visible)
            {
                return
                    !a.request.visible &&
                    b.request.visible;
            }

            if (a.request.priority !=
                b.request.priority)
            {
                return
                    a.request.priority <
                    b.request.priority;
            }

            // Newer deferred requests are more valuable than older equal work.
            return a.sequence < b.sequence;
        };

    while (pending_.size() >
           budget_.maxPendingRequests)
    {
        const auto victim =
            std::max_element(
                pending_.begin(),
                pending_.end(),
                [&](const PendingEntry& a,
                    const PendingEntry& b)
                {
                    return worse(b, a);
                });

        if (victim == pending_.end())
            break;

        currentRevision_.erase(
            victim->request.key);
        pending_.erase(victim);
        ++queueDrops_;
    }
}

std::vector<WorkGrant>
CelestialWorkScheduler::BuildFramePlan()
{
    lastFrame_ = {};

    std::stable_sort(
        pending_.begin(),
        pending_.end(),
        [](const PendingEntry& a,
           const PendingEntry& b)
        {
            if (a.request.visible !=
                b.request.visible)
            {
                return
                    a.request.visible >
                    b.request.visible;
            }

            if (a.request.priority !=
                b.request.priority)
            {
                return
                    a.request.priority >
                    b.request.priority;
            }

            return
                a.sequence <
                b.sequence;
        });

    std::vector<WorkGrant> result;

    u32 cpuJobs = 0U;
    u32 gpuJobs = 0U;
    u32 cpuCost = 0U;
    u32 gpuCost = 0U;

    for (auto it = pending_.begin();
         it != pending_.end();)
    {
        const auto& request =
            it->request;

        if (inFlight_.contains(
                request.key))
        {
            ++it;
            continue;
        }

        bool fits = false;

        if (request.backend ==
            WorkBackend::Cpu)
        {
            fits =
                cpuJobs <
                    budget_.
                        maxCpuJobsPerFrame &&
                request.costUnits <=
                    budget_.
                        maxCpuCostUnitsPerFrame -
                    cpuCost;
        }
        else
        {
            fits =
                gpuJobs <
                    budget_.
                        maxGpuJobsPerFrame &&
                request.costUnits <=
                    budget_.
                        maxGpuCostUnitsPerFrame -
                    gpuCost;
        }

        if (!fits)
        {
            ++it;
            continue;
        }

        WorkGrant grant{
            .key = request.key,
            .authorityRevision =
                request.authorityRevision,
            .backend =
                request.backend,
            .costUnits =
                request.costUnits
        };

        if (grant.backend ==
            WorkBackend::Cpu)
        {
            ++cpuJobs;
            cpuCost +=
                grant.costUnits;
        }
        else
        {
            ++gpuJobs;
            gpuCost +=
                grant.costUnits;
        }

        inFlight_.insert_or_assign(
            grant.key,
            InFlightEntry{
                .grant = grant
            });

        result.push_back(grant);
        it = pending_.erase(it);
    }

    lastFrame_.cpuJobsGranted =
        cpuJobs;
    lastFrame_.gpuJobsGranted =
        gpuJobs;
    lastFrame_.cpuCostGranted =
        cpuCost;
    lastFrame_.gpuCostGranted =
        gpuCost;
    lastFrame_.pendingRequests =
        static_cast<u32>(
            pending_.size());
    lastFrame_.inFlightRequests =
        static_cast<u32>(
            inFlight_.size());
    lastFrame_.staleCompletionsRejected =
        staleRejected_;
    lastFrame_.queueDrops =
        queueDrops_;

    return result;
}

bool CelestialWorkScheduler::Complete(
    const WorkKey& key,
    const u64 authorityRevision)
{
    const auto inFlight =
        inFlight_.find(key);

    if (inFlight == inFlight_.end() ||
        inFlight->second.grant.
                authorityRevision !=
            authorityRevision)
    {
        ++staleRejected_;
        return false;
    }

    inFlight_.erase(inFlight);

    const auto current =
        currentRevision_.find(key);

    if (current == currentRevision_.end() ||
        current->second != authorityRevision)
    {
        ++staleRejected_;
        return false;
    }

    return true;
}

void CelestialWorkScheduler::Invalidate(
    const WorkKey& key)
{
    currentRevision_.erase(key);

    std::erase_if(
        pending_,
        [&](const PendingEntry& entry)
        {
            return
                entry.request.key ==
                key;
        });
}

std::optional<u64>
CelestialWorkScheduler::CurrentAuthorityRevision(
    const WorkKey& key) const noexcept
{
    const auto found =
        currentRevision_.find(key);

    return
        found == currentRevision_.end()
            ? std::nullopt
            : std::optional(
                  found->second);
}

SchedulerFrameStats
CelestialWorkScheduler::Stats() const noexcept
{
    auto result =
        lastFrame_;

    result.pendingRequests =
        static_cast<u32>(
            pending_.size());
    result.inFlightRequests =
        static_cast<u32>(
            inFlight_.size());
    result.staleCompletionsRejected =
        staleRejected_;
    result.queueDrops =
        queueDrops_;

    return result;
}
} // namespace orbit::celestial_scheduler
