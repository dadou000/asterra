#include <orbit/volume_fields/VolumeFieldStorage.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <unordered_map>

namespace orbit::volume_fields
{
namespace
{
struct GpuResidencyRecord
{
    i32 x{0};
    i32 y{0};
    i32 z{0};
    u32 slot{0U};
    u32 resident{0U};
    u32 valid{0U};
    u32 padding0{0U};
    u32 padding1{0U};
};

[[nodiscard]] u32 CeilDiv(
    const u32 value,
    const u32 divisor)
{
    return
        (value + divisor - 1U) /
        divisor;
}

struct TileCoordHash
{
    [[nodiscard]] std::size_t operator()(
        const TileCoord coord) const noexcept
    {
        std::size_t seed =
            static_cast<std::size_t>(
                static_cast<u32>(
                    coord.x));
        seed ^=
            static_cast<std::size_t>(
                static_cast<u32>(
                    coord.y)) +
            0x9e3779b9U +
            (seed << 6U) +
            (seed >> 2U);
        seed ^=
            static_cast<std::size_t>(
                static_cast<u32>(
                    coord.z)) +
            0x9e3779b9U +
            (seed << 6U) +
            (seed >> 2U);
        return seed;
    }
};

[[nodiscard]] bool SameLayout(
    const math::Double3 a,
    const math::Double3 b) noexcept
{
    return
        std::abs(a.x - b.x) <= 1.0e-9 &&
        std::abs(a.y - b.y) <= 1.0e-9 &&
        std::abs(a.z - b.z) <= 1.0e-9;
}
} // namespace

FieldValueKind
FieldKind(
    const world_model::VolumeField field) noexcept
{
    return
        field ==
                world_model::
                    VolumeField::Velocity
            ? FieldValueKind::Vector3
            : FieldValueKind::Scalar;
}

u32 FieldBytesPerCell(
    const world_model::VolumeField field) noexcept
{
    return
        FieldKind(field) ==
                FieldValueKind::Vector3
            ? 16U
            : 4U;
}

std::vector<world_model::VolumeField>
FieldsFromMask(
    const u64 mask)
{
    constexpr std::array fields{
        world_model::VolumeField::Density,
        world_model::VolumeField::Velocity,
        world_model::VolumeField::Temperature,
        world_model::VolumeField::Pressure,
        world_model::VolumeField::Fuel,
        world_model::VolumeField::Emission,
        world_model::VolumeField::Moisture,
        world_model::VolumeField::Sediment
    };

    std::vector<
        world_model::VolumeField>
        result;

    for (const auto field : fields)
    {
        const u64 bit =
            static_cast<u64>(field);

        if ((mask & bit) != 0U)
        {
            result.push_back(field);
        }
    }

    return result;
}

VolumeFieldStorage::VolumeFieldStorage(
    rhi::Device& device,
    const world_model::ResolvedVolumeDomain& domain,
    const u32 tileEdge)
    : device_(&device),
      tileEdge_(std::max(tileEdge, 1U))
{
    RebuildLayout(
        domain,
        false);
}

void VolumeFieldStorage::Reconfigure(
    const world_model::ResolvedVolumeDomain& domain)
{
    const bool layoutChanged =
        resolution_ != domain.resolution ||
        !SameLayout(
            halfExtentsMeters_,
            domain.halfExtentsMeters);

    if (layoutChanged)
    {
        RebuildLayout(
            domain,
            false);
        return;
    }

    if (fieldMask_ != domain.fieldMask)
    {
        RebuildChannels(
            domain.fieldMask);
    }

    if (!SameLayout(
            centerMeters_,
            domain.centerMeters))
    {
        static_cast<void>(
            Recenter(
                domain.centerMeters));
    }
}

ResidencyUpdate
VolumeFieldStorage::Recenter(
    const math::Double3 centerMeters)
{
    const auto desired =
        DesiredTileCoords(
            centerMeters);

    std::unordered_map<
        TileCoord,
        u32,
        TileCoordHash>
        oldSlots;
    oldSlots.reserve(
        tiles_.size());

    for (const auto& tile : tiles_)
    {
        if (tile.resident)
        {
            oldSlots.emplace(
                tile.coord,
                tile.slot);
        }
    }

    std::vector<bool>
        slotClaimed(
            tiles_.size(),
            false);
    std::vector<TileSlot>
        next(
            tiles_.size());

    ResidencyUpdate update{};

    const auto tileWorld =
        math::Double3{
            (halfExtentsMeters_.x * 2.0 /
             static_cast<f64>(resolution_)) *
                static_cast<f64>(tileEdge_),
            (halfExtentsMeters_.y * 2.0 /
             static_cast<f64>(resolution_)) *
                static_cast<f64>(tileEdge_),
            (halfExtentsMeters_.z * 2.0 /
             static_cast<f64>(resolution_)) *
                static_cast<f64>(tileEdge_)
        };

    update.movementTiles = {
        tileWorld.x > 0.0
            ? (centerMeters.x - centerMeters_.x) /
                  tileWorld.x
            : 0.0,
        tileWorld.y > 0.0
            ? (centerMeters.y - centerMeters_.y) /
                  tileWorld.y
            : 0.0,
        tileWorld.z > 0.0
            ? (centerMeters.z - centerMeters_.z) /
                  tileWorld.z
            : 0.0
    };

    std::vector<u32> freeSlots;

    for (const auto coord : desired)
    {
        const auto found =
            oldSlots.find(
                coord);

        if (found != oldSlots.end() &&
            tiles_[found->second].coord ==
                coord)
        {
            const u32 slot =
                found->second;

            next[slot] =
                tiles_[slot];
            next[slot].coord =
                coord;
            next[slot].resident =
                true;
            slotClaimed[slot] =
                true;
            ++update.reusedTiles;
        }
    }

    for (u32 slot = 0U;
         slot <
            static_cast<u32>(
                tiles_.size());
         ++slot)
    {
        if (!slotClaimed[slot])
        {
            freeSlots.push_back(
                slot);

            if (tiles_[slot].resident)
            {
                ++update.evictedTiles;
            }
        }
    }

    std::size_t freeIndex = 0U;

    for (const auto coord : desired)
    {
        const auto found =
            oldSlots.find(
                coord);

        if (found != oldSlots.end() &&
            tiles_[found->second].coord ==
                coord &&
            slotClaimed[found->second])
        {
            continue;
        }

        if (freeIndex >=
            freeSlots.size())
        {
            throw std::logic_error(
                "Volume tile remap ran out of physical slots.");
        }

        const u32 slot =
            freeSlots[
                freeIndex++];

        next[slot] = {
            .coord = coord,
            .slot = slot,
            .resident = true,
            .valid = false
        };

        ++update.newTiles;
    }

    tiles_ =
        std::move(next);
    centerMeters_ =
        centerMeters;
    diagnostics_.lastUpdate =
        update;

    u32 valid = 0U;
    u32 resident = 0U;

    for (const auto& tile : tiles_)
    {
        resident +=
            tile.resident ? 1U : 0U;
        valid +=
            tile.resident &&
                    tile.valid
                ? 1U
                : 0U;
    }

    diagnostics_.residentTiles =
        resident;
    diagnostics_.validTiles =
        valid;
    diagnostics_.pendingTiles =
        resident - valid;

    residencyDirty_ =
        true;
    UpdateResidencyUpload();

    return update;
}

void VolumeFieldStorage::
MarkAllResidentTilesValid()
{
    for (auto& tile : tiles_)
    {
        if (tile.resident)
        {
            tile.valid = true;
        }
    }

    diagnostics_.validTiles =
        diagnostics_.residentTiles;
    diagnostics_.pendingTiles =
        0U;
    residencyDirty_ =
        true;
    UpdateResidencyUpload();
}

u32 VolumeFieldStorage::InvalidateBounds(
    const world_model::VolumeInvalidationBounds& bounds)
{
    if (!bounds.IsValid())
    {
        return 0U;
    }

    const math::Double3 cellSize{
        halfExtentsMeters_.x * 2.0 /
            static_cast<f64>(resolution_),
        halfExtentsMeters_.y * 2.0 /
            static_cast<f64>(resolution_),
        halfExtentsMeters_.z * 2.0 /
            static_cast<f64>(resolution_)
    };

    const math::Double3 tileSize{
        cellSize.x * static_cast<f64>(tileEdge_),
        cellSize.y * static_cast<f64>(tileEdge_),
        cellSize.z * static_cast<f64>(tileEdge_)
    };

    const TileCoord minimum{
        static_cast<i32>(
            std::floor(
                bounds.minimumMeters.x /
                tileSize.x)),
        static_cast<i32>(
            std::floor(
                bounds.minimumMeters.y /
                tileSize.y)),
        static_cast<i32>(
            std::floor(
                bounds.minimumMeters.z /
                tileSize.z))
    };

    const TileCoord maximum{
        static_cast<i32>(
            std::floor(
                bounds.maximumMeters.x /
                tileSize.x)),
        static_cast<i32>(
            std::floor(
                bounds.maximumMeters.y /
                tileSize.y)),
        static_cast<i32>(
            std::floor(
                bounds.maximumMeters.z /
                tileSize.z))
    };

    u32 invalidated = 0U;

    for (auto& tile : tiles_)
    {
        if (!tile.resident ||
            !tile.valid)
        {
            continue;
        }

        const bool overlaps =
            tile.coord.x >= minimum.x &&
            tile.coord.x <= maximum.x &&
            tile.coord.y >= minimum.y &&
            tile.coord.y <= maximum.y &&
            tile.coord.z >= minimum.z &&
            tile.coord.z <= maximum.z;

        if (overlaps)
        {
            tile.valid = false;
            ++invalidated;
        }
    }

    if (invalidated == 0U)
    {
        return 0U;
    }

    diagnostics_.validTiles -=
        std::min(
            diagnostics_.validTiles,
            invalidated);
    diagnostics_.pendingTiles =
        diagnostics_.residentTiles -
        diagnostics_.validTiles;

    residencyDirty_ = true;
    UpdateResidencyUpload();

    return invalidated;
}

const VolumeFieldDiagnostics&
VolumeFieldStorage::Diagnostics() const noexcept
{
    return diagnostics_;
}

std::span<const TileSlot>
VolumeFieldStorage::Tiles() const noexcept
{
    return tiles_;
}

rhi::Buffer*
VolumeFieldStorage::Channel(
    const world_model::VolumeField field) noexcept
{
    const auto found =
        std::find_if(
            channels_.begin(),
            channels_.end(),
            [field](const ChannelStorage& item)
            {
                return item.info.field ==
                    field;
            });

    return found != channels_.end()
        ? found->buffer.get()
        : nullptr;
}

ImportedVolumeFields
VolumeFieldStorage::Import(
    render_graph::RenderGraph& graph,
    const std::string_view namePrefix)
{
    if (residencyDirty_)
    {
        UpdateResidencyUpload();
    }

    ImportedVolumeFields result;

    for (auto& channel : channels_)
    {
        const auto name =
            std::string(namePrefix) +
            ".Field." +
            std::to_string(
                static_cast<u64>(
                    channel.info.field));

        result.channels.push_back({
            .field =
                channel.info.field,
            .buffer =
                graph.ImportBuffer(
                    name,
                    *channel.buffer,
                    rhi::ResourceState::
                        ShaderResource)
        });
    }

    const auto uploadHandle =
        graph.ImportBuffer(
            std::string(namePrefix) +
                ".ResidencyUpload",
            *residencyUpload_,
            rhi::ResourceState::
                CopySource);

    result.residency =
        graph.ImportBuffer(
            std::string(namePrefix) +
                ".Residency",
            *residencyGpu_,
            rhi::ResourceState::
                ShaderResource);

    if (residencyDirty_)
    {
        graph.AddPass(
            std::string(namePrefix) +
                ".ResidencyUpload",
            {},
            {
                {
                    .buffer = uploadHandle,
                    .state =
                        rhi::ResourceState::
                            CopySource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .buffer = result.residency,
                    .state =
                        rhi::ResourceState::
                            CopyDestination,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [uploadHandle,
             residency =
                 result.residency](
                rhi::CommandList& commands,
                const render_graph::Resources& resources)
            {
                auto& source =
                    resources.Buffer(
                        uploadHandle);
                auto& destination =
                    resources.Buffer(
                        residency);

                commands.CopyBuffer(
                    source,
                    0U,
                    destination,
                    0U,
                    destination.SizeBytes());
            });

        graph.AddPass(
            std::string(namePrefix) +
                ".ResidencyReady",
            {},
            {
                {
                    .buffer =
                        result.residency,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                }
            },
            [](
                rhi::CommandList&,
                const render_graph::Resources&)
            {
            });

        residencyDirty_ =
            false;
    }

    return result;
}

void VolumeFieldStorage::RebuildLayout(
    const world_model::ResolvedVolumeDomain& domain,
    const bool)
{
    if (domain.resolution == 0U)
    {
        throw std::invalid_argument(
            "Volume field resolution must be non-zero.");
    }

    resolution_ =
        domain.resolution;
    centerMeters_ =
        domain.centerMeters;
    halfExtentsMeters_ = {
        std::max(
            domain.halfExtentsMeters.x,
            1.0e-6),
        std::max(
            domain.halfExtentsMeters.y,
            1.0e-6),
        std::max(
            domain.halfExtentsMeters.z,
            1.0e-6)
    };

    tilesX_ =
        CeilDiv(
            resolution_,
            tileEdge_);
    tilesY_ =
        CeilDiv(
            resolution_,
            tileEdge_);
    tilesZ_ =
        CeilDiv(
            resolution_,
            tileEdge_);

    const u64 totalTiles =
        static_cast<u64>(tilesX_) *
        tilesY_ *
        tilesZ_;

    if (totalTiles == 0U ||
        totalTiles >
            static_cast<u64>(
                std::numeric_limits<u32>::max()))
    {
        throw std::invalid_argument(
            "Volume tile count is outside supported range.");
    }

    tiles_.assign(
        static_cast<std::size_t>(
            totalTiles),
        {});

    const auto desired =
        DesiredTileCoords(
            centerMeters_);

    for (u32 index = 0U;
         index <
            static_cast<u32>(
                tiles_.size());
         ++index)
    {
        tiles_[index] = {
            .coord = desired[index],
            .slot = index,
            .resident = true,
            .valid = false
        };
    }

    const u64 residencyBytes =
        static_cast<u64>(
            tiles_.size()) *
        sizeof(GpuResidencyRecord);

    residencyUpload_ =
        device_->CreateBuffer({
            .sizeBytes =
                residencyBytes,
            .usage =
                rhi::BufferUsage::
                    Structured,
            .memory =
                rhi::MemoryUsage::
                    HostVisible,
            .initialState =
                rhi::ResourceState::
                    CopySource
        });

    residencyGpu_ =
        device_->CreateBuffer({
            .sizeBytes =
                residencyBytes,
            .usage =
                rhi::BufferUsage::
                    Structured,
            .memory =
                rhi::MemoryUsage::
                    GpuOnly,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    diagnostics_ = {
        .resolution = resolution_,
        .tileEdge = tileEdge_,
        .tilesX = tilesX_,
        .tilesY = tilesY_,
        .tilesZ = tilesZ_,
        .residentTiles =
            static_cast<u32>(
                tiles_.size()),
        .validTiles = 0U,
        .pendingTiles =
            static_cast<u32>(
                tiles_.size()),
        .residencyBytes =
            residencyBytes
    };

    RebuildChannels(
        domain.fieldMask);

    residencyDirty_ =
        true;
    UpdateResidencyUpload();
}

void VolumeFieldStorage::RebuildChannels(
    const u64 fieldMask)
{
    const auto requested =
        FieldsFromMask(
            fieldMask);

    const u64 tileCellCount =
        static_cast<u64>(
            tileEdge_) *
        tileEdge_ *
        tileEdge_;
    const u64 totalCells =
        tileCellCount *
        static_cast<u64>(
            tiles_.size());

    std::vector<ChannelStorage>
        next;
    next.reserve(
        requested.size());

    for (const auto field : requested)
    {
        auto found =
            std::find_if(
                channels_.begin(),
                channels_.end(),
                [field](
                    const ChannelStorage& item)
                {
                    return item.info.field ==
                        field;
                });

        const u32 bytesPerCell =
            FieldBytesPerCell(
                field);
        const u64 bytes =
            totalCells *
            bytesPerCell;

        if (found != channels_.end() &&
            found->info.sizeBytes ==
                bytes)
        {
            next.push_back(
                std::move(*found));
        }
        else
        {
            ChannelStorage channel{
                .info = {
                    .field = field,
                    .kind =
                        FieldKind(field),
                    .bytesPerCell =
                        bytesPerCell,
                    .sizeBytes =
                        bytes
                },
                .buffer =
                    device_->CreateBuffer({
                        .sizeBytes =
                            bytes,
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                GpuOnly,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    })
            };

            next.push_back(
                std::move(channel));
        }
    }

    channels_ =
        std::move(next);
    fieldMask_ =
        fieldMask;

    diagnostics_.channels.clear();
    diagnostics_.channelBytes =
        0U;

    for (const auto& channel :
         channels_)
    {
        diagnostics_.channels.push_back(
            channel.info);
        diagnostics_.channelBytes +=
            channel.info.sizeBytes;
    }

    diagnostics_.totalBytes =
        diagnostics_.channelBytes +
        diagnostics_.residencyBytes;
}

void VolumeFieldStorage::
UpdateResidencyUpload()
{
    if (residencyUpload_ == nullptr)
    {
        return;
    }

    auto* mapped =
        residencyUpload_->Map();

    if (mapped == nullptr)
    {
        throw std::runtime_error(
            "Volume residency staging buffer could not be mapped.");
    }

    auto* records =
        reinterpret_cast<
            GpuResidencyRecord*>(
                mapped);

    for (std::size_t index = 0U;
         index < tiles_.size();
         ++index)
    {
        const auto& tile =
            tiles_[index];

        records[index] = {
            .x = tile.coord.x,
            .y = tile.coord.y,
            .z = tile.coord.z,
            .slot = tile.slot,
            .resident =
                tile.resident ? 1U : 0U,
            .valid =
                tile.valid ? 1U : 0U
        };
    }

    residencyUpload_->Unmap();
}

std::vector<TileCoord>
VolumeFieldStorage::DesiredTileCoords(
    const math::Double3 centerMeters) const
{
    const math::Double3 cellSize{
        halfExtentsMeters_.x * 2.0 /
            static_cast<f64>(
                resolution_),
        halfExtentsMeters_.y * 2.0 /
            static_cast<f64>(
                resolution_),
        halfExtentsMeters_.z * 2.0 /
            static_cast<f64>(
                resolution_)
    };

    const math::Double3 tileSize{
        cellSize.x *
            static_cast<f64>(
                tileEdge_),
        cellSize.y *
            static_cast<f64>(
                tileEdge_),
        cellSize.z *
            static_cast<f64>(
                tileEdge_)
    };

    const math::Double3 minimum{
        centerMeters.x -
            halfExtentsMeters_.x,
        centerMeters.y -
            halfExtentsMeters_.y,
        centerMeters.z -
            halfExtentsMeters_.z
    };

    const TileCoord origin{
        static_cast<i32>(
            std::floor(
                minimum.x /
                tileSize.x)),
        static_cast<i32>(
            std::floor(
                minimum.y /
                tileSize.y)),
        static_cast<i32>(
            std::floor(
                minimum.z /
                tileSize.z))
    };

    std::vector<TileCoord>
        result;
    result.reserve(
        static_cast<std::size_t>(
            tilesX_) *
        tilesY_ *
        tilesZ_);

    for (u32 z = 0U;
         z < tilesZ_;
         ++z)
    {
        for (u32 y = 0U;
             y < tilesY_;
             ++y)
        {
            for (u32 x = 0U;
                 x < tilesX_;
                 ++x)
            {
                result.push_back({
                    origin.x +
                        static_cast<i32>(x),
                    origin.y +
                        static_cast<i32>(y),
                    origin.z +
                        static_cast<i32>(z)
                });
            }
        }
    }

    return result;
}

VolumeFieldStorageService::
VolumeFieldStorageService(
    rhi::Device& device) noexcept
    : device_(&device)
{
}

VolumeFieldStorage&
VolumeFieldStorageService::Ensure(
    const world_model::ResolvedVolumeDomain& domain)
{
    auto found =
        std::find_if(
            entries_.begin(),
            entries_.end(),
            [&domain](const Entry& entry)
            {
                return entry.object ==
                    domain.object;
            });

    if (found == entries_.end())
    {
        entries_.push_back({
            .object =
                domain.object,
            .storage =
                std::make_unique<
                    VolumeFieldStorage>(
                        *device_,
                        domain),
            .inputs = {},
            .lastInvalidatedTiles = 0U
        });

        return
            *entries_.back().
                storage;
    }

    found->storage->
        Reconfigure(
            domain);

    return
        *found->storage;
}

u32 VolumeFieldStorageService::SyncAuthoredInputs(
    const scene::ObjectStore& objects,
    const scene::ObjectId volume)
{
    auto found =
        std::find_if(
            entries_.begin(),
            entries_.end(),
            [volume](const Entry& entry)
            {
                return entry.object == volume;
            });

    if (found == entries_.end() ||
        found->storage == nullptr)
    {
        return 0U;
    }

    const auto current =
        world_model::ResolveVolumeInputs(
            objects,
            volume);

    u32 invalidated = 0U;

    for (const auto& previous :
         found->inputs)
    {
        const auto matching =
            std::find_if(
                current.begin(),
                current.end(),
                [&previous](
                    const world_model::
                        ResolvedVolumeInput& input)
                {
                    return input.object ==
                        previous.object;
                });

        if (matching == current.end())
        {
            invalidated +=
                found->storage->
                    InvalidateBounds(
                        previous.bounds);
            continue;
        }

        if (matching->fingerprint !=
            previous.fingerprint)
        {
            invalidated +=
                found->storage->
                    InvalidateBounds(
                        world_model::
                            UnionVolumeInvalidationBounds(
                                previous.bounds,
                                matching->bounds));
        }
    }

    for (const auto& input :
         current)
    {
        const auto previous =
            std::find_if(
                found->inputs.begin(),
                found->inputs.end(),
                [&input](
                    const world_model::
                        ResolvedVolumeInput& item)
                {
                    return item.object ==
                        input.object;
                });

        if (previous ==
            found->inputs.end())
        {
            invalidated +=
                found->storage->
                    InvalidateBounds(
                        input.bounds);
        }
    }

    found->inputs =
        current;
    found->lastInvalidatedTiles =
        invalidated;

    return invalidated;
}

u32 VolumeFieldStorageService::LastInvalidatedTiles(
    const scene::ObjectId volume) const noexcept
{
    const auto found =
        std::find_if(
            entries_.begin(),
            entries_.end(),
            [volume](const Entry& entry)
            {
                return entry.object == volume;
            });

    return found != entries_.end()
        ? found->lastInvalidatedTiles
        : 0U;
}

VolumeFieldStorage*
VolumeFieldStorageService::Find(
    const scene::ObjectId volume) noexcept
{
    const auto found =
        std::find_if(
            entries_.begin(),
            entries_.end(),
            [volume](const Entry& entry)
            {
                return entry.object ==
                    volume;
            });

    return found != entries_.end()
        ? found->storage.get()
        : nullptr;
}

const VolumeFieldStorage*
VolumeFieldStorageService::Find(
    const scene::ObjectId volume) const noexcept
{
    const auto found =
        std::find_if(
            entries_.begin(),
            entries_.end(),
            [volume](const Entry& entry)
            {
                return entry.object ==
                    volume;
            });

    return found != entries_.end()
        ? found->storage.get()
        : nullptr;
}

void VolumeFieldStorageService::RemoveMissing(
    const scene::ObjectStore& objects)
{
    std::erase_if(
        entries_,
        [&objects](const Entry& entry)
        {
            const auto record =
                objects.Find(
                    entry.object);

            return
                !record.has_value() ||
                record->type !=
                    world_model::kVolumeType;
        });
}
} // namespace orbit::volume_fields
