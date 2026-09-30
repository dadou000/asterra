#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <cstddef>
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

    // Clones one leaf semantic object, including every explicitly stored
    // schema property, as a single undoable edit. Hierarchy duplication stays
    // explicit: callers must duplicate children themselves rather than
    // accidentally copying a large subtree from a generic shortcut.
    [[nodiscard]] scene::ObjectId DuplicateObject(
        scene::ObjectId object,
        std::string_view nameSuffix = " Copy");

    // Deletes one leaf semantic object as an undoable command. The complete
    // object record and all explicitly stored schema properties are restored
    // with the same stable ID on undo.
    void DeleteObject(scene::ObjectId object);

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

    // Removes an explicit property override so the schema default becomes
    // effective again. The removed value is restored by undo.
    void ResetProperty(
        scene::ObjectId object,
        schema::PropertyId property);

    void BeginTransaction(
        std::string label);
    void CommitTransaction();
    void RollbackTransaction();

    [[nodiscard]] bool HasActiveTransaction() const noexcept;
    [[nodiscard]] bool CanUndo() const noexcept;
    [[nodiscard]] bool CanRedo() const noexcept;

    // Newest-first transaction labels for lightweight history UIs. These are
    // presentation snapshots only; mutation remains exclusively Undo/Redo.
    [[nodiscard]] std::vector<std::string> UndoLabels(
        std::size_t limit = 32U) const;
    [[nodiscard]] std::vector<std::string> RedoLabels(
        std::size_t limit = 32U) const;

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
