#include <orbit/editor_model/ExplorerModel.hpp>

#include <array>
#include <span>
#include <utility>

namespace orbit::editor_model
{
ExplorerModel::ExplorerModel(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection)
    : objects_(objects),
      commands_(commands),
      selection_(selection)
{
}

std::vector<scene::ObjectRecord>
ExplorerModel::Roots() const
{
    return objects_.Roots();
}

std::vector<scene::ObjectRecord>
ExplorerModel::Children(
    const scene::ObjectId parent) const
{
    return objects_.Children(parent);
}

std::vector<scene::ObjectRecord>
ExplorerModel::Search(
    const std::string_view query,
    const u32 limit) const
{
    return objects_.SearchByName(
        query,
        limit);
}

void ExplorerModel::Select(
    const scene::ObjectId object,
    const bool additive)
{
    if (additive)
    {
        selection_.Toggle(object);
        return;
    }

    const std::array selected{
        object
    };

    selection_.Set(
        std::span(selected));
}

void ExplorerModel::Rename(
    const scene::ObjectId object,
    std::string name)
{
    commands_.RenameObject(
        object,
        std::move(name));
}

void ExplorerModel::Reparent(
    const scene::ObjectId object,
    const std::optional<scene::ObjectId> parent)
{
    commands_.ReparentObject(
        object,
        parent);
}
} // namespace orbit::editor_model
