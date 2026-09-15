#include <orbit/documents/WorldDatabase.hpp>

#include <SQLiteCpp/SQLiteCpp.h>

#include <stdexcept>
#include <utility>

namespace orbit::documents
{
class WorldDatabase::Impl
{
public:
    explicit Impl(
        std::filesystem::path databasePath)
        : path(std::move(databasePath)),
          database(
              path.string(),
              SQLite::OPEN_READWRITE |
                  SQLite::OPEN_CREATE)
    {
        database.exec(
            "PRAGMA foreign_keys = ON;");
        database.exec(
            "PRAGMA journal_mode = WAL;");
        database.exec(
            "PRAGMA synchronous = FULL;");

        InitializeAndMigrate();
        EnsureWorldId();
    }

    void InitializeAndMigrate()
    {
        if (!database.tableExists(
                "orbit_schema"))
        {
            SQLite::Transaction transaction(
                database);

            database.exec(
                "CREATE TABLE orbit_schema ("
                "version INTEGER NOT NULL"
                ");");
            database.exec(
                "INSERT INTO orbit_schema(version) "
                "VALUES (0);");

            transaction.commit();
        }

        SQLite::Statement versionQuery(
            database,
            "SELECT version FROM orbit_schema LIMIT 1;");

        if (!versionQuery.executeStep())
        {
            throw std::runtime_error(
                "World database schema table is empty.");
        }

        schemaVersion =
            versionQuery.getColumn(0).
                getInt();

        if (schemaVersion >
            kCurrentWorldSchemaVersion)
        {
            throw std::runtime_error(
                "World database was created by a newer Orbit schema.");
        }

        while (schemaVersion <
               kCurrentWorldSchemaVersion)
        {
            const i32 nextVersion =
                schemaVersion + 1;

            SQLite::Transaction transaction(
                database);

            switch (nextVersion)
            {
                case 1:
                    database.exec(
                        "CREATE TABLE IF NOT EXISTS "
                        "world_metadata ("
                        "key TEXT PRIMARY KEY NOT NULL,"
                        "value TEXT NOT NULL"
                        ");");
                    break;

                case 2:
                    database.exec(
                        "CREATE TABLE objects ("
                        "id TEXT PRIMARY KEY NOT NULL,"
                        "parent_id TEXT NULL "
                        "REFERENCES objects(id) "
                        "ON DELETE RESTRICT,"
                        "type_id TEXT NOT NULL,"
                        "name TEXT NOT NULL,"
                        "sort_order INTEGER NOT NULL DEFAULT 0"
                        ");");

                    database.exec(
                        "CREATE INDEX "
                        "objects_parent_order "
                        "ON objects(parent_id, sort_order, name, id);");

                    database.exec(
                        "CREATE TABLE object_properties ("
                        "object_id TEXT NOT NULL "
                        "REFERENCES objects(id) ON DELETE CASCADE,"
                        "property_id TEXT NOT NULL,"
                        "value_kind INTEGER NOT NULL,"
                        "value_integer INTEGER NULL,"
                        "value_real REAL NULL,"
                        "value_text TEXT NULL,"
                        "value_x REAL NULL,"
                        "value_y REAL NULL,"
                        "value_z REAL NULL,"
                        "PRIMARY KEY(object_id, property_id)"
                        ");");
                    break;

                default:
                    throw std::runtime_error(
                        "Missing world database migration.");
            }

            SQLite::Statement update(
                database,
                "UPDATE orbit_schema SET version = ?;");
            update.bind(
                1,
                nextVersion);
            update.exec();

            transaction.commit();
            schemaVersion =
                nextVersion;
        }
    }

    void EnsureWorldId()
    {
        SQLite::Statement query(
            database,
            "SELECT value FROM world_metadata "
            "WHERE key = 'world_id';");

        if (query.executeStep())
        {
            const std::string text =
                query.getColumn(0).
                    getString();

            const auto parsed =
                WorldId::Parse(text);

            if (!parsed.has_value())
            {
                throw std::runtime_error(
                    "World database contains an invalid world_id.");
            }

            worldId = *parsed;
            return;
        }

        worldId = WorldId::Random();

        SQLite::Statement insert(
            database,
            "INSERT INTO world_metadata(key, value) "
            "VALUES ('world_id', ?);");
        insert.bind(
            1,
            worldId.ToString());
        insert.exec();
    }

    std::filesystem::path path;
    SQLite::Database database;
    i32 schemaVersion{0};
    WorldId worldId{};
};

WorldDatabase::WorldDatabase(
    const std::filesystem::path& path)
    : impl_(
          std::make_unique<Impl>(
              path))
{
}

WorldDatabase::~WorldDatabase() = default;

WorldDatabase::WorldDatabase(
    WorldDatabase&&) noexcept = default;

WorldDatabase& WorldDatabase::operator=(
    WorldDatabase&&) noexcept = default;

const std::filesystem::path&
WorldDatabase::Path() const noexcept
{
    return impl_->path;
}

i32 WorldDatabase::SchemaVersion() const noexcept
{
    return impl_->schemaVersion;
}

WorldId WorldDatabase::Id() const noexcept
{
    return impl_->worldId;
}

std::optional<std::string>
WorldDatabase::GetMetadata(
    const std::string_view key) const
{
    SQLite::Statement query(
        impl_->database,
        "SELECT value FROM world_metadata "
        "WHERE key = ?;");
    query.bind(
        1,
        std::string(key));

    if (!query.executeStep())
    {
        return std::nullopt;
    }

    return query.getColumn(0).
        getString();
}

void WorldDatabase::SetMetadata(
    const std::string_view key,
    const std::string_view value)
{
    if (key.empty())
    {
        throw std::invalid_argument(
            "World metadata key must not be empty.");
    }

    SQLite::Statement statement(
        impl_->database,
        "INSERT INTO world_metadata(key, value) "
        "VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE "
        "SET value = excluded.value;");

    statement.bind(
        1,
        std::string(key));
    statement.bind(
        2,
        std::string(value));
    statement.exec();
}

void WorldDatabase::RunTransaction(
    const std::function<void(WorldDatabase&)>&
        callback)
{
    if (!callback)
    {
        throw std::invalid_argument(
            "World transaction callback must be valid.");
    }

    SQLite::Transaction transaction(
        impl_->database);

    callback(*this);

    transaction.commit();
}

void WorldDatabase::Checkpoint()
{
    impl_->database.exec(
        "PRAGMA wal_checkpoint(TRUNCATE);");
}
} // namespace orbit::documents
