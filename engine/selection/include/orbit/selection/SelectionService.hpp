#pragma once

#include <orbit/scene/ObjectStore.hpp>

#include <span>
#include <vector>

namespace orbit::selection
{
class SelectionService
{
public:
    void Set(
        std::span<const scene::ObjectId>
            objects);

    void Clear() noexcept;

    void Toggle(
        scene::ObjectId object);

    [[nodiscard]] bool Contains(
        scene::ObjectId object) const noexcept;

    [[nodiscard]] const std::vector<
        scene::ObjectId>&
    Ordered() const noexcept;

    [[nodiscard]] u64 Revision() const noexcept;

private:
    std::vector<scene::ObjectId> selected_;
    u64 revision_{0};
};
} // namespace orbit::selection
