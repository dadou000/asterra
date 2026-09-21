#include <orbit/celestial_representation/RepresentationTracker.hpp>

#include <stdexcept>

namespace orbit::celestial_representation
{
Decision RepresentationTracker::ResolveFor(
    const RepresentationSubjectId subject,
    ResolveInput input)
{
    if (!subject)
    {
        throw std::invalid_argument(
            "Representation tracker subject ID must be valid.");
    }

    const auto found =
        previous_.find(subject);

    input.previous =
        found == previous_.end()
            ? std::nullopt
            : std::optional(found->second);

    Decision decision =
        Resolve(input);

    previous_.insert_or_assign(
        subject,
        decision.representation);

    return decision;
}

void RepresentationTracker::Reset(
    const RepresentationSubjectId subject) noexcept
{
    previous_.erase(subject);
}

void RepresentationTracker::Clear() noexcept
{
    previous_.clear();
}

std::optional<Representation>
RepresentationTracker::Previous(
    const RepresentationSubjectId subject) const noexcept
{
    const auto found =
        previous_.find(subject);

    return found == previous_.end()
        ? std::nullopt
        : std::optional(found->second);
}
} // namespace orbit::celestial_representation
