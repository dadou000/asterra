#include <orbit/editor_session/WorldDocumentsModel.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::editor_session
{
WorldDocumentsModel::WorldDocumentsModel(
    EditorWorldSession& session) noexcept
    : session_(session)
{
}

std::vector<WorldDocumentItem>
WorldDocumentsModel::Catalog() const
{
    const auto active = session_.ActiveWorld();
    std::vector<WorldDocumentItem> result;

    for (auto descriptor :
         session_.Project().Worlds())
    {
        const bool isActive =
            active.has_value() &&
            descriptor.id == active->id;

        result.push_back({
            .descriptor = std::move(descriptor),
            .active = isActive
        });
    }

    return result;
}

std::optional<WorldDocumentItem>
WorldDocumentsModel::Active() const
{
    const auto active = session_.ActiveWorld();

    if (!active.has_value())
    {
        return std::nullopt;
    }

    return WorldDocumentItem{
        .descriptor =
            session_.Project().DescribeWorld(
                active->relativePath),
        .active = true
    };
}

documents::WorldDescriptor
WorldDocumentsModel::Create(
    const std::filesystem::path& relativePath,
    const std::string_view displayName)
{
    const auto createdPath =
        session_.Project().CreateWorld(
            relativePath,
            displayName);

    return session_.Project().DescribeWorld(
        createdPath);
}

documents::WorldDescriptor
WorldDocumentsModel::Rename(
    const std::filesystem::path& relativePath,
    const std::string_view displayName)
{
    session_.Project().SetWorldDisplayName(
        relativePath,
        displayName);

    return session_.Project().DescribeWorld(
        relativePath);
}

documents::WorldDescriptor
WorldDocumentsModel::SetStartup(
    const std::filesystem::path& relativePath)
{
    session_.Project().SetStartupWorld(
        relativePath);

    return session_.Project().DescribeWorld(
        relativePath);
}

documents::WorldDescriptor
WorldDocumentsModel::Open(
    const std::filesystem::path& relativePath)
{
    session_.OpenWorld(relativePath);

    const auto active = Active();

    if (!active.has_value())
    {
        throw std::logic_error(
            "Editor world session did not retain the opened world.");
    }

    return active->descriptor;
}

void WorldDocumentsModel::Close()
{
    session_.CloseWorld();
}
} // namespace orbit::editor_session
