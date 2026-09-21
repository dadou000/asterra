#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orbit::volume_fields
{
enum class FieldValueKind : u8
{
    Scalar = 0U,
    Vector3 = 1U
};

struct FieldChannelInfo
{
    world_model::VolumeField field{
        world_model::VolumeField::Density};
    FieldValueKind kind{
        FieldValueKind::Scalar};
    u32 bytesPerCell{4U};
    u64 sizeBytes{0U};
};

struct TileCoord
{
    i32 x{0};
    i32 y{0};
    i32 z{0};

    [[nodiscard]] constexpr bool operator==(
        const TileCoord&) const noexcept = default;
};

struct TileSlot
{
    TileCoord coord{};
    u32 slot{0U};
    bool resident{false};
    bool valid{false};
};

struct ResidencyUpdate
{
    u32 reusedTiles{0U};
    u32 newTiles{0U};
    u32 evictedTiles{0U};
    math::Double3 movementTiles{};
};

struct VolumeFieldDiagnostics
{
    u32 resolution{0U};
    u32 tileEdge{0U};
    u32 tilesX{0U};
    u32 tilesY{0U};
    u32 tilesZ{0U};
    u32 residentTiles{0U};
    u32 validTiles{0U};
    u32 pendingTiles{0U};
    u64 channelBytes{0U};
    u64 residencyBytes{0U};
    u64 totalBytes{0U};
    ResidencyUpdate lastUpdate{};
    std::vector<FieldChannelInfo> channels;
};

struct ImportedVolumeFields
{
    struct Channel
    {
        world_model::VolumeField field{
            world_model::VolumeField::Density};
        render_graph::BufferHandle buffer{};
    };

    std::vector<Channel> channels;
    render_graph::BufferHandle residency{};
};

[[nodiscard]] FieldValueKind
FieldKind(
    world_model::VolumeField field) noexcept;

[[nodiscard]] u32
FieldBytesPerCell(
    world_model::VolumeField field) noexcept;

[[nodiscard]] std::vector<
    world_model::VolumeField>
FieldsFromMask(
    u64 mask);

class VolumeFieldStorage
{
public:
    VolumeFieldStorage(
        rhi::Device& device,
        const world_model::ResolvedVolumeDomain& domain,
        u32 tileEdge = 8U);

    void Reconfigure(
        const world_model::ResolvedVolumeDomain& domain);

    [[nodiscard]] ResidencyUpdate
    Recenter(
        math::Double3 centerMeters);

    void MarkAllResidentTilesValid() noexcept;

    [[nodiscard]] const VolumeFieldDiagnostics&
    Diagnostics() const noexcept;

    [[nodiscard]] std::span<const TileSlot>
    Tiles() const noexcept;

    [[nodiscard]] rhi::Buffer*
    Channel(
        world_model::VolumeField field) noexcept;

    [[nodiscard]] ImportedVolumeFields
    Import(
        render_graph::RenderGraph& graph,
        std::string_view namePrefix);

private:
    struct ChannelStorage
    {
        FieldChannelInfo info{};
        std::unique_ptr<rhi::Buffer> buffer;
    };

    void RebuildLayout(
        const world_model::ResolvedVolumeDomain& domain,
        bool preserveCoordinates);

    void RebuildChannels(
        u64 fieldMask);

    void UpdateResidencyUpload();

    [[nodiscard]] std::vector<TileCoord>
    DesiredTileCoords(
        math::Double3 centerMeters) const;

    rhi::Device* device_{nullptr};
    u32 resolution_{64U};
    u32 tileEdge_{8U};
    u32 tilesX_{0U};
    u32 tilesY_{0U};
    u32 tilesZ_{0U};
    math::Double3 centerMeters_{};
    math::Double3 halfExtentsMeters_{
        10.0, 10.0, 10.0};
    u64 fieldMask_{0U};

    std::vector<TileSlot> tiles_;
    std::vector<ChannelStorage> channels_;

    std::unique_ptr<rhi::Buffer>
        residencyUpload_;
    std::unique_ptr<rhi::Buffer>
        residencyGpu_;
    bool residencyDirty_{true};

    VolumeFieldDiagnostics diagnostics_{};
};

class VolumeFieldStorageService
{
public:
    explicit VolumeFieldStorageService(
        rhi::Device& device) noexcept;

    [[nodiscard]] VolumeFieldStorage&
    Ensure(
        const world_model::ResolvedVolumeDomain& domain);

    [[nodiscard]] VolumeFieldStorage*
    Find(
        scene::ObjectId volume) noexcept;

    [[nodiscard]] const VolumeFieldStorage*
    Find(
        scene::ObjectId volume) const noexcept;

    void RemoveMissing(
        const scene::ObjectStore& objects);

private:
    struct Entry
    {
        scene::ObjectId object{};
        std::unique_ptr<VolumeFieldStorage> storage;
    };

    rhi::Device* device_{nullptr};
    std::vector<Entry> entries_;
};
} // namespace orbit::volume_fields
