#include <orbit/scene/ObjectStore.hpp>

#include <SQLiteCpp/SQLiteCpp.h>

#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace orbit::scene
{
namespace
{
[[nodiscard]] ObjectId ParseObjectId(
    const std::string& text)
{
    const auto value =
        ObjectId::Parse(text);

    if (!value.has_value())
    {
        throw std::runtime_error(
            "World database contains invalid object ID.");
    }

    return *value;
}

[[nodiscard]] schema::TypeId ParseTypeId(
    const std::string& text)
{
    const auto value =
        schema::TypeId::Parse(text);

    if (!value.has_value())
    {
        throw std::runtime_error(
            "World database contains invalid schema type ID.");
    }

    return *value;
}

[[nodiscard]] ObjectRecord ReadObject(
    SQLite::Statement& query)
{
    ObjectRecord object{};
    object.id =
        ParseObjectId(
            query.getColumn(0).
                getString());

    if (!query.getColumn(1).isNull())
    {
        object.parent =
            ParseObjectId(
                query.getColumn(1).
                    getString());
    }

    object.type =
        ParseTypeId(
            query.getColumn(2).
                getString());
    object.name =
        query.getColumn(3).
            getString();
    object.sortOrder =
        static_cast<i64>(
            query.getColumn(4).
                getInt64());

    return object;
}
} // namespace

class ObjectStore::Impl
{
public:
    explicit Impl(
        const documents::WorldDatabase& world)
        : database(
              world.Path().string(),
              world.ReadOnly()
                  ? SQLite::OPEN_READONLY
                  : SQLite::OPEN_READWRITE),
          readOnly(
              world.ReadOnly())
    {
        database.exec(
            "PRAGMA foreign_keys = ON;");
        database.exec(
            "PRAGMA busy_timeout = 5000;");
    }

    void RequireWritable() const
    {
        if (readOnly)
        {
            throw std::logic_error(
                "Scene object store is read-only.");
        }
    }

    void MarkChanged() noexcept
    {
        ++previewRevision;
        if (transaction != nullptr)
        {
            transactionDirty = true;
        }
        else
        {
            ++revision;
        }
    }

    // Read memoization. Every query is otherwise a SQLite read transaction
    // (WAL shared lock, file-size probe, statement prepare), and the render
    // and UI loops ask the same questions every frame. All object writes go
    // through this store and bump previewRevision, which invalidates the lot.
    void SyncCache() const
    {
        if (cacheTag != previewRevision)
        {
            findCache.clear();
            rootsCache.reset();
            childrenCache.clear();
            propertyCache.clear();
            cacheTag = previewRevision;
        }
    }

    mutable std::mutex cacheMutex;
    mutable u64 cacheTag{~u64{0}};
    mutable std::map<std::pair<u64, u64>, std::optional<ObjectRecord>>
        findCache;
    mutable std::optional<std::vector<ObjectRecord>> rootsCache;
    mutable std::map<std::pair<u64, u64>, std::vector<ObjectRecord>>
        childrenCache;
    mutable std::map<std::string, std::optional<schema::PropertyValue>>
        propertyCache;

    SQLite::Database database;
    bool readOnly{false};
    std::unique_ptr<SQLite::Transaction>
        transaction;
    u64 revision{0};
    u64 previewRevision{0};
    bool transactionDirty{false};
};

ObjectStore::ObjectStore(
    const documents::WorldDatabase& world)
    : impl_(
          std::make_unique<Impl>(
              world))
{
}

ObjectStore::~ObjectStore() = default;

std::optional<ObjectRecord>
ObjectStore::Find(
    const ObjectId id) const
{
    std::lock_guard cacheLock(impl_->cacheMutex);
    impl_->SyncCache();
    const auto cacheKey = std::make_pair(
        static_cast<u64>(id.high), static_cast<u64>(id.low));
    if (const auto hit = impl_->findCache.find(cacheKey);
        hit != impl_->findCache.end())
    {
        return hit->second;
    }

    std::optional<ObjectRecord> found;
    {
    SQLite::Statement query(
        impl_->database,
        "SELECT id, parent_id, type_id, name, sort_order "
        "FROM objects WHERE id = ?;");
    query.bind(
        1,
        id.ToString());

    if (query.executeStep())
    {
        found = ReadObject(query);
    }
    }

    impl_->findCache.emplace(cacheKey, found);
    return found;
}

std::vector<ObjectRecord>
ObjectStore::Roots() const
{
    std::lock_guard cacheLock(impl_->cacheMutex);
    impl_->SyncCache();
    if (impl_->rootsCache.has_value())
    {
        return *impl_->rootsCache;
    }

    SQLite::Statement query(
        impl_->database,
        "SELECT id, parent_id, type_id, name, sort_order "
        "FROM objects WHERE parent_id IS NULL "
        "ORDER BY sort_order, name, id;");

    std::vector<ObjectRecord> result;

    while (query.executeStep())
    {
        result.push_back(
            ReadObject(query));
    }

    impl_->rootsCache = result;
    return result;
}

std::vector<ObjectRecord>
ObjectStore::Children(
    const ObjectId parent) const
{
    std::lock_guard cacheLock(impl_->cacheMutex);
    impl_->SyncCache();
    const auto cacheKey = std::make_pair(
        static_cast<u64>(parent.high), static_cast<u64>(parent.low));
    if (const auto hit = impl_->childrenCache.find(cacheKey);
        hit != impl_->childrenCache.end())
    {
        return hit->second;
    }

    SQLite::Statement query(
        impl_->database,
        "SELECT id, parent_id, type_id, name, sort_order "
        "FROM objects WHERE parent_id = ? "
        "ORDER BY sort_order, name, id;");
    query.bind(
        1,
        parent.ToString());

    std::vector<ObjectRecord> result;

    while (query.executeStep())
    {
        result.push_back(
            ReadObject(query));
    }

    impl_->childrenCache.emplace(cacheKey, result);
    return result;
}

std::vector<ObjectRecord>
ObjectStore::SearchByName(
    const std::string_view queryText,
    const u32 limit) const
{
    if (queryText.empty() ||
        limit == 0)
    {
        return {};
    }

    std::string escaped;
    escaped.reserve(
        queryText.size());

    for (const char character :
         queryText)
    {
        if (character == '%' ||
            character == '_' ||
            character == '\\')
        {
            escaped.push_back('\\');
        }

        escaped.push_back(
            character);
    }

    SQLite::Statement query(
        impl_->database,
        "SELECT id, parent_id, type_id, name, sort_order "
        "FROM objects "
        "WHERE name LIKE ? ESCAPE '\\' COLLATE NOCASE "
        "ORDER BY name, id LIMIT ?;");

    query.bind(
        1,
        "%" + escaped + "%");
    query.bind(
        2,
        static_cast<int>(
            limit));

    std::vector<ObjectRecord> result;

    while (query.executeStep())
    {
        result.push_back(
            ReadObject(query));
    }

    return result;
}

std::optional<schema::PropertyValue>
ObjectStore::GetProperty(
    const ObjectId object,
    const schema::PropertyId property) const
{
    std::lock_guard cacheLock(impl_->cacheMutex);
    impl_->SyncCache();
    const std::string cacheKey =
        object.ToString() + "/" + property.ToString();
    if (const auto hit = impl_->propertyCache.find(cacheKey);
        hit != impl_->propertyCache.end())
    {
        return hit->second;
    }

    const auto result = [&]() -> std::optional<schema::PropertyValue>
    {
    SQLite::Statement query(
        impl_->database,
        "SELECT value_kind, value_integer, value_real, "
        "value_text, value_x, value_y, value_z "
        "FROM object_properties "
        "WHERE object_id = ? AND property_id = ?;");

    query.bind(
        1,
        object.ToString());
    query.bind(
        2,
        property.ToString());

    if (!query.executeStep())
    {
        return std::nullopt;
    }

    const auto kind =
        static_cast<schema::PropertyKind>(
            query.getColumn(0).
                getInt());

    switch (kind)
    {
        case schema::PropertyKind::Boolean:
            return schema::PropertyValue(
                query.getColumn(1).
                    getInt() != 0);

        case schema::PropertyKind::Integer:
            return schema::PropertyValue(
                static_cast<i64>(
                    query.getColumn(1).
                        getInt64()));

        case schema::PropertyKind::Float:
            return schema::PropertyValue(
                query.getColumn(2).
                    getDouble());

        case schema::PropertyKind::String:
            return schema::PropertyValue(
                query.getColumn(3).
                    getString());

        case schema::PropertyKind::Vector3:
            return schema::PropertyValue(
                math::Double3{
                    query.getColumn(4).
                        getDouble(),
                    query.getColumn(5).
                        getDouble(),
                    query.getColumn(6).
                        getDouble()
                });

        case schema::PropertyKind::ObjectReference:
        {
            const auto parsed =
                ObjectId::Parse(
                    query.getColumn(3).
                        getString());

            if (!parsed.has_value())
            {
                throw std::runtime_error(
                    "Invalid object-reference property.");
            }

            return schema::PropertyValue(
                schema::ObjectReferenceValue{
                    .high = parsed->high,
                    .low = parsed->low
                });
        }
    }

    throw std::runtime_error(
        "Unknown stored property kind.");
    }();

    impl_->propertyCache.emplace(cacheKey, result);
    return result;
}

void ObjectStore::Insert(
    MutationKey,
    const ObjectRecord& object)
{
    impl_->RequireWritable();

    if (!object.id || !object.type ||
        object.name.empty())
    {
        throw std::invalid_argument(
            "Object requires ID, type and name.");
    }

    if (object.parent.has_value() &&
        !Find(*object.parent).has_value())
    {
        throw std::invalid_argument(
            "Object parent does not exist.");
    }

    SQLite::Statement statement(
        impl_->database,
        "INSERT INTO objects("
        "id, parent_id, type_id, name, sort_order"
        ") VALUES (?, ?, ?, ?, ?);");

    statement.bind(
        1,
        object.id.ToString());

    if (object.parent.has_value())
    {
        statement.bind(
            2,
            object.parent->ToString());
    }
    else
    {
        statement.bind(2);
    }

    statement.bind(
        3,
        object.type.ToString());
    statement.bind(
        4,
        object.name);
    statement.bind(
        5,
        object.sortOrder);
    statement.exec();
    impl_->MarkChanged();
}

void ObjectStore::Erase(
    MutationKey,
    const ObjectId object)
{
    impl_->RequireWritable();

    if (!Children(object).empty())
    {
        throw std::invalid_argument(
            "Cannot erase object while it has children.");
    }

    SQLite::Statement statement(
        impl_->database,
        "DELETE FROM objects WHERE id = ?;");
    statement.bind(
        1,
        object.ToString());

    if (statement.exec() != 1)
    {
        throw std::invalid_argument(
            "Cannot erase unknown object.");
    }

    impl_->MarkChanged();
}

void ObjectStore::Rename(
    MutationKey,
    const ObjectId object,
    std::string name)
{
    impl_->RequireWritable();

    if (name.empty())
    {
        throw std::invalid_argument(
            "Object name must not be empty.");
    }

    SQLite::Statement statement(
        impl_->database,
        "UPDATE objects SET name = ? WHERE id = ?;");
    statement.bind(
        1,
        name);
    statement.bind(
        2,
        object.ToString());

    if (statement.exec() != 1)
    {
        throw std::invalid_argument(
            "Cannot rename unknown object.");
    }

    impl_->MarkChanged();
}

void ObjectStore::Reparent(
    MutationKey,
    const ObjectId object,
    const std::optional<ObjectId> parent)
{
    impl_->RequireWritable();

    if (!Find(object).has_value())
    {
        throw std::invalid_argument(
            "Cannot reparent unknown object.");
    }

    if (parent.has_value())
    {
        if (*parent == object)
        {
            throw std::invalid_argument(
                "Object cannot parent itself.");
        }

        const auto parentRecord =
            Find(*parent);

        if (!parentRecord.has_value())
        {
            throw std::invalid_argument(
                "New parent does not exist.");
        }

        std::optional<ObjectId> ancestor =
            parentRecord->parent;

        while (ancestor.has_value())
        {
            if (*ancestor == object)
            {
                throw std::invalid_argument(
                    "Reparent would create a hierarchy cycle.");
            }

            const auto record =
                Find(*ancestor);

            if (!record.has_value())
            {
                throw std::runtime_error(
                    "Object hierarchy references a missing ancestor.");
            }

            ancestor = record->parent;
        }
    }

    SQLite::Statement statement(
        impl_->database,
        "UPDATE objects SET parent_id = ? WHERE id = ?;");

    if (parent.has_value())
    {
        statement.bind(
            1,
            parent->ToString());
    }
    else
    {
        statement.bind(1);
    }

    statement.bind(
        2,
        object.ToString());

    if (statement.exec() != 1)
    {
        throw std::invalid_argument(
            "Cannot reparent unknown object.");
    }

    impl_->MarkChanged();
}

void ObjectStore::SetProperty(
    MutationKey,
    const ObjectId object,
    const schema::PropertyId property,
    const schema::PropertyValue& value)
{
    impl_->RequireWritable();

    if (!Find(object).has_value())
    {
        throw std::invalid_argument(
            "Cannot set property on unknown object.");
    }

    SQLite::Statement statement(
        impl_->database,
        "INSERT INTO object_properties("
        "object_id, property_id, value_kind, "
        "value_integer, value_real, value_text, "
        "value_x, value_y, value_z"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(object_id, property_id) "
        "DO UPDATE SET "
        "value_kind=excluded.value_kind, "
        "value_integer=excluded.value_integer, "
        "value_real=excluded.value_real, "
        "value_text=excluded.value_text, "
        "value_x=excluded.value_x, "
        "value_y=excluded.value_y, "
        "value_z=excluded.value_z;");

    statement.bind(
        1,
        object.ToString());
    statement.bind(
        2,
        property.ToString());

    for (int index = 4;
         index <= 9;
         ++index)
    {
        statement.bind(index);
    }

    std::visit(
        [&statement](const auto& item)
        {
            using Value =
                std::decay_t<
                    decltype(item)>;

            if constexpr (
                std::is_same_v<Value, bool>)
            {
                statement.bind(
                    3,
                    static_cast<int>(
                        schema::PropertyKind::
                            Boolean));
                statement.bind(
                    4,
                    item ? 1 : 0);
            }
            else if constexpr (
                std::is_same_v<Value, i64>)
            {
                statement.bind(
                    3,
                    static_cast<int>(
                        schema::PropertyKind::
                            Integer));
                statement.bind(
                    4,
                    item);
            }
            else if constexpr (
                std::is_same_v<Value, f64>)
            {
                statement.bind(
                    3,
                    static_cast<int>(
                        schema::PropertyKind::
                            Float));
                statement.bind(
                    5,
                    item);
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    std::string>)
            {
                statement.bind(
                    3,
                    static_cast<int>(
                        schema::PropertyKind::
                            String));
                statement.bind(
                    6,
                    item);
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    math::Double3>)
            {
                statement.bind(
                    3,
                    static_cast<int>(
                        schema::PropertyKind::
                            Vector3));
                statement.bind(
                    7,
                    item.x);
                statement.bind(
                    8,
                    item.y);
                statement.bind(
                    9,
                    item.z);
            }
            else
            {
                statement.bind(
                    3,
                    static_cast<int>(
                        schema::PropertyKind::
                            ObjectReference));

                const ObjectId referenced{
                    .high = item.high,
                    .low = item.low
                };

                statement.bind(
                    6,
                    referenced.ToString());
            }
        },
        value);

    statement.exec();
    impl_->MarkChanged();
}

void ObjectStore::RemoveProperty(
    MutationKey,
    const ObjectId object,
    const schema::PropertyId property)
{
    impl_->RequireWritable();

    SQLite::Statement statement(
        impl_->database,
        "DELETE FROM object_properties "
        "WHERE object_id = ? AND property_id = ?;");
    statement.bind(
        1,
        object.ToString());
    statement.bind(
        2,
        property.ToString());
    if (statement.exec() != 0)
    {
        impl_->MarkChanged();
    }
}

void ObjectStore::BeginTransaction(
    MutationKey)
{
    impl_->RequireWritable();

    if (impl_->transaction != nullptr)
    {
        throw std::logic_error(
            "Scene transaction is already active.");
    }

    impl_->transaction =
        std::make_unique<SQLite::Transaction>(
            impl_->database,
            SQLite::TransactionBehavior::
                IMMEDIATE);
    impl_->transactionDirty = false;
}

void ObjectStore::CommitTransaction(
    MutationKey)
{
    impl_->RequireWritable();

    if (impl_->transaction == nullptr)
    {
        throw std::logic_error(
            "No scene transaction is active.");
    }

    impl_->transaction->commit();
    impl_->transaction.reset();

    if (impl_->transactionDirty)
    {
        ++impl_->revision;
    }

    impl_->transactionDirty = false;
}

void ObjectStore::RollbackTransaction(
    MutationKey) noexcept
{
    if (impl_->transaction == nullptr)
    {
        return;
    }

    const bool changed = impl_->transactionDirty;
    try
    {
        impl_->transaction->rollback();
    }
    catch (...)
    {
        // The destructor performs a best-effort rollback as well. This
        // function is noexcept so command error paths cannot mask the
        // original edit failure.
    }

    impl_->transaction.reset();
    impl_->transactionDirty = false;
    if (changed)
    {
        ++impl_->previewRevision;
    }
}

bool ObjectStore::TransactionActive() const noexcept
{
    return impl_->transaction != nullptr;
}

u64 ObjectStore::Revision() const noexcept
{
    return impl_->revision;
}

u64 ObjectStore::PreviewRevision() const noexcept
{
    return impl_->previewRevision;
}
} // namespace orbit::scene
