#include <orbit/mesh_render/MeshLibrary.hpp>

#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/content_wic/WicTextureImporter.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/mesh_import/GltfImporter.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <set>
#include <utility>

namespace orbit::mesh_render
{
namespace
{
// Resources of a replaced model stay alive for this many Pump() calls so
// frames still in flight on the GPU never lose them.
constexpr u64 kRetireTicks = 24U;
// Upload throttle per Pump(): keeps a 70-texture model from stalling one
// frame. Whichever limit is hit first ends the batch.
constexpr u64 kUploadByteBudget = 24ULL * 1024ULL * 1024ULL;
constexpr std::size_t kUploadTextureBudget = 16U;
constexpr auto kStatInterval = std::chrono::milliseconds(750);
// A model nobody asked for this long is released (Pump retires its buffers).
constexpr auto kIdleRelease = std::chrono::seconds(30);

[[nodiscard]] u32 MipChainLength(const u32 width, const u32 height) noexcept
{
    u32 levels = 1U;
    for (u32 extent = std::max(width, height); extent > 1U; extent >>= 1U)
    {
        ++levels;
    }
    return levels;
}

[[nodiscard]] std::unique_ptr<rhi::Buffer> MakeStaging(
    rhi::Device& device,
    const void* data,
    const std::size_t bytes)
{
    auto staging = device.CreateBuffer({
        .sizeBytes = bytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::Common});
    std::memcpy(staging->Map(), data, bytes);
    staging->Unmap();
    return staging;
}

std::mutex g_statusMutex;
std::vector<MeshStatus> g_latestStatuses;

[[nodiscard]] std::string Key(const std::filesystem::path& file)
{
    return file.lexically_normal().generic_string();
}
} // namespace

std::vector<MeshStatus> LatestMeshStatuses()
{
    const std::lock_guard lock(g_statusMutex);
    return g_latestStatuses;
}

MeshLibrary::MeshLibrary(rhi::Device& device)
    : device_(device),
      worker_([this] { WorkerMain(); })
{
}

MeshLibrary::~MeshLibrary()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void MeshLibrary::Enqueue(const std::filesystem::path& file)
{
    {
        const std::lock_guard lock(mutex_);
        queue_.push_back(file);
    }
    wake_.notify_one();
}

void MeshLibrary::WorkerMain()
{
    for (;;)
    {
        std::filesystem::path file;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
            {
                return;
            }
            file = std::move(queue_.front());
            queue_.pop_front();
        }

        LoadResult result;
        result.file = file;
        try
        {
            std::error_code error;
            result.modified = std::filesystem::last_write_time(file, error);

            std::function<mesh_import::MeshAsset()> generator;
            {
                const std::lock_guard lock(mutex_);
                if (const auto found = generators_.find(Key(file));
                    found != generators_.end())
                {
                    generator = found->second;
                }
            }

            result.asset = generator ? generator()
                                     : mesh_import::ImportGltfFile(file);
            if (generator && result.asset.vertices.empty())
            {
                throw std::runtime_error(
                    "generated mesh has no geometry");
            }

            // Decode every image a material actually samples, once each.
            std::set<i32> wanted;
            const auto note = [&](const mesh_import::TextureBinding& binding)
            {
                if (binding.Present())
                {
                    wanted.insert(
                        result.asset
                            .textures[static_cast<std::size_t>(binding.texture)]
                            .image);
                }
            };
            for (const auto& material : result.asset.materials)
            {
                note(material.baseColorTexture);
                note(material.normalTexture);
                note(material.metallicRoughnessTexture);
                note(material.emissiveTexture);
            }

            // Images whose colours feed the distance field's albedo / emissive.
            std::set<i32> colourImages;
            for (const auto& material : result.asset.materials)
            {
                for (const auto* binding :
                     {&material.baseColorTexture, &material.emissiveTexture})
                {
                    if (binding->Present())
                    {
                        colourImages.insert(
                            result.asset
                                .textures[static_cast<std::size_t>(binding->texture)]
                                .image);
                    }
                }
            }

            for (const i32 image : wanted)
            {
                LoadedImage decoded;
                const auto& source =
                    result.asset.images[static_cast<std::size_t>(image)];
                try
                {
                    auto texture =
                        content_wic::DecodeTextureMemory(source.encoded);
                    decoded.width = texture.width;
                    decoded.height = texture.height;
                    decoded.pixels = std::move(texture.pixels);
                }
                catch (const std::exception& decodeError)
                {
                    decoded.failed = true;
                    result.asset.warnings.push_back(
                        "image " + std::to_string(image) +
                        " could not be decoded: " + decodeError.what());
                }
                // The encoded bytes are no longer needed.
                result.asset.images[static_cast<std::size_t>(image)]
                    .encoded.clear();
                result.asset.images[static_cast<std::size_t>(image)]
                    .encoded.shrink_to_fit();
                result.images.emplace(
                    std::pair{image, false}, std::move(decoded));
            }

            // Distance field + surface attributes of the whole mesh, using the
            // decoded base-colour / emissive images for albedo.
            std::vector<mesh_sdf::SdfImage> sdfImages(result.asset.images.size());
            for (const i32 image : colourImages)
            {
                const auto found = result.images.find({image, false});
                if (found == result.images.end() || found->second.failed ||
                    image < 0 ||
                    static_cast<std::size_t>(image) >= sdfImages.size())
                {
                    continue;
                }
                auto& target = sdfImages[static_cast<std::size_t>(image)];
                target.width = found->second.width;
                target.height = found->second.height;
                target.rgba = found->second.pixels;
            }
            result.sdf = std::make_shared<const mesh_sdf::MeshSdf>(
                mesh_sdf::BuildMeshSdf(result.asset, sdfImages));
        }
        catch (const std::exception& error)
        {
            result.error = error.what();
        }

        {
            const std::lock_guard lock(mutex_);
            finished_.push_back(std::move(result));
        }
    }
}

const MeshModel* MeshLibrary::Acquire(const std::filesystem::path& file)
{
    const std::string key = Key(file);
    auto [found, inserted] = entries_.try_emplace(key);
    Entry& entry = found->second;
    entry.lastRequested = std::chrono::steady_clock::now();

    if (inserted)
    {
        entry.file = file;
        entry.loadInFlight = true;
        entry.nextStatCheck =
            std::chrono::steady_clock::now() + kStatInterval;
        Enqueue(file);
    }

    return entry.model != nullptr && entry.state == MeshLoadState::Ready
               ? entry.model.get()
               : nullptr;
}

const MeshModel* MeshLibrary::AcquireGenerated(
    const std::string& key,
    const std::function<mesh_import::MeshAsset()>& build)
{
    const std::filesystem::path file(key);
    const std::string normalised = Key(file);
    auto [found, inserted] = entries_.try_emplace(normalised);
    Entry& entry = found->second;
    entry.lastRequested = std::chrono::steady_clock::now();

    if (inserted)
    {
        entry.file = file;
        entry.generated = true;
        entry.loadInFlight = true;
        {
            const std::lock_guard lock(mutex_);
            generators_[normalised] = build;
        }
        Enqueue(file);
    }

    return entry.model != nullptr && entry.state == MeshLoadState::Ready
               ? entry.model.get()
               : nullptr;
}

void MeshLibrary::Adopt(
    Entry& entry,
    LoadResult&& result,
    rhi::CommandList& commands)
{
    entry.loadInFlight = false;
    entry.nextStatCheck = std::chrono::steady_clock::now() + kStatInterval;

    if (!result.error.empty())
    {
        // A failed (re)load keeps the previous model on screen.
        entry.error = result.error;
        if (entry.model == nullptr)
        {
            entry.state = MeshLoadState::Failed;
        }
        log::Warning(
            "Mesh import failed for " + result.file.string() + ": " +
            result.error);
        return;
    }

    auto& asset = result.asset;
    auto model = std::make_unique<MeshModel>();
    model->generation_ =
        entry.model != nullptr ? entry.model->generation_ + 1U : 1U;
    model->boundsMin_ = asset.boundsMin;
    model->boundsMax_ = asset.boundsMax;
    model->triangles_ = asset.TriangleCount();

    // Geometry: device-local buffers filled through the caller's commands.
    const std::size_t vertexBytes =
        asset.vertices.size() * sizeof(mesh_import::MeshVertex);
    const std::size_t indexBytes = asset.indices.size() * sizeof(u32);

    Retired uploads;
    uploads.retireAtTick = tick_ + kRetireTicks;

    model->vertices_ = device_.CreateBuffer({
        .sizeBytes = vertexBytes,
        .usage = rhi::BufferUsage::Vertex,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination});
    model->indices_ = device_.CreateBuffer({
        .sizeBytes = indexBytes,
        .usage = rhi::BufferUsage::Index,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination});

    uploads.staging.push_back(
        MakeStaging(device_, asset.vertices.data(), vertexBytes));
    uploads.staging.push_back(
        MakeStaging(device_, asset.indices.data(), indexBytes));

    commands.CopyBuffer(
        *uploads.staging[0], 0U, *model->vertices_, 0U, vertexBytes);
    commands.CopyBuffer(
        *uploads.staging[1], 0U, *model->indices_, 0U, indexBytes);
    commands.Transition(
        *model->vertices_,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::VertexOrConstantBuffer);
    commands.Transition(
        *model->indices_,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::IndexBuffer);

    for (const auto& part : asset.parts)
    {
        model->parts_.push_back(
            {part.firstIndex, part.indexCount, part.material});
    }

    // The distance field and its surface attributes, as storage buffers
    // (the RHI has no 3D textures; shaders sample them with manual trilinear).
    if (result.sdf != nullptr && result.sdf->VoxelCount() > 0U)
    {
        const mesh_sdf::MeshSdf& field = *result.sdf;
        auto& volume = model->sdf_;
        volume.dimensions = field.dimensions;
        volume.origin = field.origin;
        volume.voxelSize = field.voxelSize;
        volume.emissiveScale = field.emissiveScale;
        volume.maximumDistance = field.maximumDistance;

        const auto upload = [&](const void* data, const std::size_t bytes)
        {
            auto buffer = device_.CreateBuffer({
                .sizeBytes = bytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::CopyDestination});
            uploads.staging.push_back(MakeStaging(device_, data, bytes));
            commands.CopyBuffer(*uploads.staging.back(), 0U, *buffer, 0U, bytes);
            commands.Transition(
                *buffer,
                rhi::ResourceState::CopyDestination,
                rhi::ResourceState::UnorderedAccess);
            return buffer;
        };

        const std::size_t voxels = field.VoxelCount();
        volume.distance = upload(field.distance.data(), voxels * sizeof(f32));
        volume.albedo = upload(field.albedo.data(), voxels * sizeof(u32));
        volume.normal = upload(field.normal.data(), voxels * sizeof(u32));
        volume.emissive = upload(field.emissive.data(), voxels * sizeof(u32));
    }

    // Materials and texture slots. A slot is one (image, colour space)
    // pair; several materials share slots.
    std::map<std::pair<i32, bool>, i32> slotOf;
    const auto bind = [&](const mesh_import::TextureBinding& binding,
                          const bool srgb) -> i32
    {
        if (!binding.Present())
        {
            return -1;
        }
        const auto& texture =
            asset.textures[static_cast<std::size_t>(binding.texture)];
        const auto decoded =
            result.images.find({texture.image, false});
        if (decoded == result.images.end() || decoded->second.failed)
        {
            return -1;
        }

        const std::pair<i32, bool> id{texture.image, srgb};
        if (const auto existing = slotOf.find(id); existing != slotOf.end())
        {
            return existing->second;
        }

        MeshModel::TextureSlot slot;
        slot.width = decoded->second.width;
        slot.height = decoded->second.height;
        slot.srgb = srgb;
        slot.repeat = texture.repeatU || texture.repeatV;
        slot.pixels = decoded->second.pixels; // shared by both variants
        const auto index = static_cast<i32>(model->textures_.size());
        model->textures_.push_back(std::move(slot));
        slotOf.emplace(id, index);
        return index;
    };

    for (const auto& source : asset.materials)
    {
        MeshModel::Material material;
        material.baseColorFactor = source.baseColorFactor;
        material.emissiveFactor = source.emissiveFactor;
        material.metallicFactor = source.metallicFactor;
        material.roughnessFactor = source.roughnessFactor;
        material.normalScale = source.normalTexture.scale;
        material.alphaCutoff =
            source.alphaMode == mesh_import::AlphaMode::Opaque
                ? 0.0F
                : std::max(source.alphaCutoff, 0.001F);
        // Blend has no sorted pass yet; a 0.5 cutoff approximates it.
        if (source.alphaMode == mesh_import::AlphaMode::Blend)
        {
            material.alphaCutoff = 0.5F;
        }
        material.texture[static_cast<std::size_t>(MeshTextureRole::BaseColor)] =
            bind(source.baseColorTexture, true);
        material.texture[static_cast<std::size_t>(MeshTextureRole::Normal)] =
            bind(source.normalTexture, false);
        material.texture[static_cast<std::size_t>(
            MeshTextureRole::MetallicRoughness)] =
            bind(source.metallicRoughnessTexture, false);
        material.texture[static_cast<std::size_t>(MeshTextureRole::Emissive)] =
            bind(source.emissiveTexture, true);
        model->materials_.push_back(material);
    }

    entry.pendingTextures.clear();
    for (std::size_t i = 0U; i < model->textures_.size(); ++i)
    {
        entry.pendingTextures.push_back(i);
    }

    entry.texturesTotal = static_cast<u32>(model->textures_.size());
    entry.texturesResident = 0U;
    entry.triangles = model->triangles_;
    entry.parts = static_cast<u32>(model->parts_.size());
    entry.materials = static_cast<u32>(model->materials_.size());
    entry.boundsMin = asset.boundsMin;
    entry.boundsMax = asset.boundsMax;
    entry.warnings = std::move(asset.warnings);
    entry.error.clear();
    entry.modified = result.modified;
    entry.state = MeshLoadState::Ready;

    if (entry.model != nullptr)
    {
        retired_.push_back(
            {std::move(entry.model), {}, tick_ + kRetireTicks});
    }
    entry.model = std::move(model);
    retired_.push_back(std::move(uploads));
}

void MeshLibrary::UploadPending(Entry& entry, rhi::CommandList& commands)
{
    if (entry.model == nullptr || entry.pendingTextures.empty())
    {
        return;
    }

    Retired batch;
    batch.retireAtTick = tick_ + kRetireTicks;
    u64 bytes = 0U;
    std::size_t uploaded = 0U;

    while (!entry.pendingTextures.empty() &&
           uploaded < kUploadTextureBudget &&
           bytes < kUploadByteBudget)
    {
        auto& slot = entry.model->textures_[entry.pendingTextures.back()];
        entry.pendingTextures.pop_back();

        try
        {
            slot.texture = device_.CreateTexture({
                .width = slot.width,
                .height = slot.height,
                .format = slot.srgb ? rhi::TextureFormat::RGBA8_SRGB
                                    : rhi::TextureFormat::RGBA8_UNorm,
                .initialState = rhi::ResourceState::Common,
                .mipLevels = MipChainLength(slot.width, slot.height),
                .repeatAddress = slot.repeat});

            batch.staging.push_back(
                MakeStaging(device_, slot.pixels.data(), slot.pixels.size()));

            commands.Transition(
                *slot.texture,
                rhi::ResourceState::Common,
                rhi::ResourceState::CopyDestination);
            commands.CopyBufferToTexture(*batch.staging.back(), 0U, *slot.texture);
            commands.GenerateMipmaps(*slot.texture);
            commands.Transition(
                *slot.texture,
                rhi::ResourceState::CopyDestination,
                rhi::ResourceState::ShaderResource);

            ++entry.texturesResident;
            bytes += slot.pixels.size();
        }
        catch (const std::exception& error)
        {
            slot.texture.reset();
            slot.failed = true;
            log::Warning(std::string("Mesh texture upload failed: ") + error.what());
        }

        slot.pixels.clear();
        slot.pixels.shrink_to_fit();
        ++uploaded;
    }

    if (!batch.staging.empty())
    {
        retired_.push_back(std::move(batch));
    }
}

void MeshLibrary::Pump(rhi::CommandList& commands)
{
    ++tick_;

    // One-time stand-in textures.
    if (defaults_[0] == nullptr)
    {
        struct Default
        {
            rhi::TextureFormat format;
            std::array<u8, 4> rgba;
        };
        const std::array<Default, kMeshTextureSlots> specs{{
            {rhi::TextureFormat::RGBA8_SRGB, {255, 255, 255, 255}},
            {rhi::TextureFormat::RGBA8_UNorm, {128, 128, 255, 255}},
            {rhi::TextureFormat::RGBA8_UNorm, {255, 255, 255, 255}},
            {rhi::TextureFormat::RGBA8_SRGB, {255, 255, 255, 255}}}};

        Retired batch;
        batch.retireAtTick = tick_ + kRetireTicks;
        for (std::size_t i = 0U; i < kMeshTextureSlots; ++i)
        {
            defaults_[i] = device_.CreateTexture({
                .width = 1U,
                .height = 1U,
                .format = specs[i].format,
                .initialState = rhi::ResourceState::Common,
                .repeatAddress = true});
            batch.staging.push_back(
                MakeStaging(device_, specs[i].rgba.data(), 4U));
            commands.Transition(
                *defaults_[i],
                rhi::ResourceState::Common,
                rhi::ResourceState::CopyDestination);
            commands.CopyBufferToTexture(*batch.staging.back(), 0U, *defaults_[i]);
            commands.Transition(
                *defaults_[i],
                rhi::ResourceState::CopyDestination,
                rhi::ResourceState::ShaderResource);
        }
        retired_.push_back(std::move(batch));
    }

    std::vector<LoadResult> finished;
    {
        const std::lock_guard lock(mutex_);
        finished.swap(finished_);
    }
    for (auto& result : finished)
    {
        if (const auto found = entries_.find(Key(result.file));
            found != entries_.end())
        {
            Adopt(found->second, std::move(result), commands);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto& [key, entry] : entries_)
    {
        (void)key;
        UploadPending(entry, commands);

        // Hot reload: re-import when the file on disk changed.
        if (!entry.generated && !entry.loadInFlight &&
            now >= entry.nextStatCheck)
        {
            entry.nextStatCheck = now + kStatInterval;
            std::error_code error;
            const auto modified =
                std::filesystem::last_write_time(entry.file, error);
            if (!error && modified != entry.modified)
            {
                entry.modified = modified;
                entry.loadInFlight = true;
                Enqueue(entry.file);
            }
        }
    }

    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (!it->second.loadInFlight &&
            now - it->second.lastRequested > kIdleRelease)
        {
            if (it->second.model != nullptr)
            {
                retired_.push_back(
                    {std::move(it->second.model), {}, tick_ + kRetireTicks});
            }
            if (it->second.generated)
            {
                const std::lock_guard lock(mutex_);
                generators_.erase(it->first);
            }
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    std::erase_if(
        retired_,
        [this](const Retired& retired)
        {
            return tick_ >= retired.retireAtTick;
        });

    {
        auto statuses = Statuses();
        const std::lock_guard lock(g_statusMutex);
        g_latestStatuses = std::move(statuses);
    }
}


std::vector<MeshStatus> MeshLibrary::Statuses() const
{
    std::vector<MeshStatus> out;
    for (const auto& [key, entry] : entries_)
    {
        out.push_back({
            .path = key,
            .state = entry.state,
            .error = entry.error,
            .triangles = entry.triangles,
            .parts = entry.parts,
            .materials = entry.materials,
            .texturesTotal = entry.texturesTotal,
            .texturesResident = entry.texturesResident,
            .boundsMin = entry.boundsMin,
            .boundsMax = entry.boundsMax,
            .warnings = entry.warnings});
    }
    return out;
}

rhi::Texture& MeshLibrary::DefaultTexture(const MeshTextureRole role) const
{
    return *defaults_[static_cast<std::size_t>(role)];
}
} // namespace orbit::mesh_render
