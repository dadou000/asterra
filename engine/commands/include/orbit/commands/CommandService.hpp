#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::commands
{
class CommandService
{
public:
    CommandService(
        scene::ObjectStore& objects,
        const schema::SchemaRegistry& schemas);

    [[nodiscard]] scene::ObjectId CreateObject(
        schema::TypeId type,
        std::string_view name,
        std::optional<scene::ObjectId> parent =
            std::nullopt,
        i64 sortOrder = 0);

    void RenameObject(
        scene::ObjectId object,
        std::string name);

    void ReparentObject(
        scene::ObjectId object,
        std::optional<scene::ObjectId> parent);

    void SetProperty(
        scene::ObjectId object,
        schema::PropertyId property,
        schema::PropertyValue value);

    void BeginTransaction(
        std::string label);
    void CommitTransaction();
    void RollbackTransaction();

    [[nodiscard]] bool HasActiveTransaction() const noexcept;
    [[nodiscard]] bool CanUndo() const noexcept;
    [[nodiscard]] bool CanRedo() const noexcept;

    void Undo();
    void Redo();

private:
    struct Action
    {
        std::function<void(scene::MutationKey)> redo;
        std::function<void(scene::MutationKey)> undo;
    };

    struct Transaction
    {
        std::string label;
        std::vector<Action> actions;
    };

    void ApplyAndRecord(
        std::string_view label,
        Action action);

    [[nodiscard]] scene::ObjectRecord
    RequireObject(scene::ObjectId id) const;

    scene::ObjectStore& objects_;
    const schema::SchemaRegistry& schemas_;
    std::optional<Transaction> activeTransaction_;
    std::vector<Transaction> undoStack_;
    std::vector<Transaction> redoStack_;
};
} // namespace orbit::commands
