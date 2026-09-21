#pragma once

#include <orbit/celestial_representation/RepresentationResolver.hpp>
#include <orbit/core/StrongId.hpp>

#include <unordered_map>

namespace orbit::celestial_representation
{
struct RepresentationSubjectIdTag;
using RepresentationSubjectId =
    core::StrongId<RepresentationSubjectIdTag>;

class RepresentationTracker
{
public:
    [[nodiscard]] Decision ResolveFor(
        RepresentationSubjectId subject,
        ResolveInput input);

    void Reset(
        RepresentationSubjectId subject) noexcept;
    void Clear() noexcept;

    [[nodiscard]] std::optional<Representation>
    Previous(
        RepresentationSubjectId subject) const noexcept;

private:
    std::unordered_map<
        RepresentationSubjectId,
        Representation> previous_;
};
} // namespace orbit::celestial_representation
