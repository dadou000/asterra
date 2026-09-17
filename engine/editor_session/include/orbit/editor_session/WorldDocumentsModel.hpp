#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::editor_session
{
struct WorldDocumentItem
{
    documents::WorldDescriptor descriptor;
    bool active{false};
    bool valid{true};
    std::string diagnostic;
};

// Project/world document presentation model shared by Studio surfaces.
// It owns no parallel catalog: every query is reconstructed from
// ProjectDocument and active state comes from EditorWorldSession. Invalid or
// incompatible world files are represented as diagnostic catalog entries so a
// single damaged document cannot hide the rest of a project's worlds.
class WorldDocumentsModel
{
public:
    explicit WorldDocumentsModel(
        EditorWorldSession& session) noexcept;

    [[nodiscard]] std::vector<WorldDocumentItem>
    Catalog() const;

    [[nodiscard]] std::optional<WorldDocumentItem>
    Active() const;

    [[nodiscard]] documents::WorldDescriptor Create(
        const std::filesystem::path& relativePath,
        std::string_view displayName);

    [[nodiscard]] documents::WorldDescriptor Rename(
        const std::filesystem::path& relativePath,
        std::string_view displayName);

    [[nodiscard]] documents::WorldDescriptor SetStartup(
        const std::filesystem::path& relativePath);

    [[nodiscard]] documents::WorldDescriptor Open(
        const std::filesystem::path& relativePath);

    void Close();

private:
    EditorWorldSession& session_;
};
} // namespace orbit::editor_session
