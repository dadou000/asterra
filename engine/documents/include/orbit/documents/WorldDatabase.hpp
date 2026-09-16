#pragma once

#include <orbit/core/StrongId.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::documents
{
struct WorldIdTag;
using WorldId = core::StrongId<WorldIdTag>;

inline constexpr i32 kCurrentWorldSchemaVersion = 2;

class WorldDatabase
{
public:
    explicit WorldDatabase(
        const std::filesystem::path& path);
    ~WorldDatabase();

    WorldDatabase(const WorldDatabase&) = delete;
    WorldDatabase& operator=(const WorldDatabase&) = delete;

    WorldDatabase(WorldDatabase&&) noexcept;
    WorldDatabase& operator=(WorldDatabase&&) noexcept;

    [[nodiscard]] const std::filesystem::path&
    Path() const noexcept;

    [[nodiscard]] i32 SchemaVersion() const noexcept;
    [[nodiscard]] WorldId Id() const noexcept;

    [[nodiscard]] std::optional<std::string>
    GetMetadata(std::string_view key) const;

    void SetMetadata(
        std::string_view key,
        std::string_view value);

    // SQLite transactions are atomic. If callback throws, the RAII
    // transaction rolls back and the exception is propagated.
    void RunTransaction(
        const std::function<void(WorldDatabase&)>&
            callback);

    // Flushes the WAL into the main database and truncates the WAL.
    void Checkpoint();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::documents
