#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace orbit::editor_model
{
class ExplorerModel
{
public:
    ExplorerModel(
        scene::ObjectStore& objects,
        commands::CommandService& commands,
        selection::SelectionService& selection);

    [[nodiscard]] std::vector<scene::ObjectRecord>
    Roots() const;

    [[nodiscard]] std::vector<scene::ObjectRecord>
    Children(scene::ObjectId parent) const;

    [[nodiscard]] std::vector<scene::ObjectRecord>
    Search(
        std::string_view query,
        u32 limit = 128) const;

    void Select(
        scene::ObjectId object,
        bool additive);

    void Rename(
        scene::ObjectId object,
        std::string name);

    void Reparent(
        scene::ObjectId object,
        std::optional<scene::ObjectId> parent);

private:
    scene::ObjectStore& objects_;
    commands::CommandService& commands_;
    selection::SelectionService& selection_;
};
} // namespace orbit::editor_model
