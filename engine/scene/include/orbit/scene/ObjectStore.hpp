#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orbit::commands
{
class CommandService;
}

namespace orbit::scene
{
struct ObjectIdTag;
using ObjectId = core::StrongId<ObjectIdTag>;

struct ObjectRecord
{
    ObjectId id{};
    std::optional<ObjectId> parent;
    schema::TypeId type{};
    std::string name;
    i64 sortOrder{0};

    [[nodiscard]] bool operator==(
        const ObjectRecord&) const noexcept = default;
};

// Capability token: only the command layer can construct one, so editor
// panels, plugins and future MCP adapters cannot mutate persistent scene
// state by reaching around validation/undo.
class MutationKey
{
private:
    MutationKey() = default;
    friend class commands::CommandService;
};

class ObjectStore
{
public:
    explicit ObjectStore(
        const documents::WorldDatabase& world);
    ~ObjectStore();

    ObjectStore(const ObjectStore&) = delete;
    ObjectStore& operator=(const ObjectStore&) = delete;

    [[nodiscard]] std::optional<ObjectRecord>
    Find(ObjectId id) const;

    [[nodiscard]] std::vector<ObjectRecord>
    Roots() const;

    [[nodiscard]] std::vector<ObjectRecord>
    Children(ObjectId parent) const;

    [[nodiscard]] std::optional<
        schema::PropertyValue>
    GetProperty(
        ObjectId object,
        schema::PropertyId property) const;

    // Mutation methods require a key only CommandService can create.
    void Insert(
        MutationKey,
        const ObjectRecord& object);

    void Erase(
        MutationKey,
        ObjectId object);

    void Rename(
        MutationKey,
        ObjectId object,
        std::string name);

    void Reparent(
        MutationKey,
        ObjectId object,
        std::optional<ObjectId> parent);

    void SetProperty(
        MutationKey,
        ObjectId object,
        schema::PropertyId property,
        const schema::PropertyValue& value);

    void RemoveProperty(
        MutationKey,
        ObjectId object,
        schema::PropertyId property);

    // Explicit editor transactions use the same SQLite connection as the
    // scene mutations, making multi-command commits genuinely atomic on
    // disk rather than merely grouped in the undo history.
    void BeginTransaction(MutationKey);
    void CommitTransaction(MutationKey);
    void RollbackTransaction(MutationKey) noexcept;

    [[nodiscard]] bool TransactionActive() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::scene
