#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/ReflectionScene.hpp>
#include <orbit/mesh_import/MeshAsset.hpp>
#include <orbit/mesh_sdf/MeshSdf.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace orbit::mesh_render
{
// Texture roles a material can bind, in pipeline slot order.
inline constexpr u32 kMeshTextureSlots = 4U;
enum class MeshTextureRole : u32
{
    BaseColor = 0U,         // sRGB
    Normal = 1U,            // linear, tangent space
    MetallicRoughness = 2U, // linear, G = roughness, B = metallic
    Emissive = 3U           // sRGB
};

enum class MeshLoadState : u8
{
    Loading,
    Ready,
    Failed
};

struct MeshStatus
{
    std::string path;
    MeshLoadState state{MeshLoadState::Loading};
    std::string error;
    u32 triangles{0U};
    u32 parts{0U};
    u32 materials{0U};
    u32 texturesTotal{0U};
    u32 texturesResident{0U};
    std::array<f64, 3> boundsMin{};
    std::array<f64, 3> boundsMax{};
    std::vector<std::string> warnings;
};

// Snapshot of every library entry as of the most recent Pump(), readable
// from any thread (the mesh.status RPC). Empty before the first pump.
[[nodiscard]] std::vector<MeshStatus> LatestMeshStatuses();

// A resident imported mesh: GPU vertex/index buffers plus its materials and
// streamed textures. Owned by MeshLibrary; instances only borrow it for the
// duration of a frame.
class MeshModel
{
public:
    struct Material
    {
        std::array<f32, 4> baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
        std::array<f32, 3> emissiveFactor{};
        f32 metallicFactor{1.0F};
        f32 roughnessFactor{1.0F};
        f32 normalScale{1.0F};
        f32 alphaCutoff{0.0F}; // 0 = opaque, otherwise discard below
        // Index into textures_ per role, or -1 for the default texture.
        std::array<i32, kMeshTextureSlots> texture{-1, -1, -1, -1};
    };

    struct Part
    {
        u32 firstIndex{0U};
        u32 indexCount{0U};
        u32 material{0U};
    };

    [[nodiscard]] rhi::Buffer* VertexBuffer() const noexcept
    {
        return vertices_.get();
    }
    [[nodiscard]] rhi::Buffer* IndexBuffer() const noexcept
    {
        return indices_.get();
    }
    [[nodiscard]] const std::vector<Part>& Parts() const noexcept
    {
        return parts_;
    }
    [[nodiscard]] const std::vector<Material>& Materials() const noexcept
    {
        return materials_;
    }
    // The texture for a material role, or nullptr while it is still
    // streaming in (the renderer substitutes a neutral default).
    [[nodiscard]] rhi::Texture* Texture(i32 index) const noexcept
    {
        return index >= 0 &&
                       static_cast<std::size_t>(index) < textures_.size()
                   ? textures_[static_cast<std::size_t>(index)].texture.get()
                   : nullptr;
    }
    [[nodiscard]] const std::array<f64, 3>& BoundsMin() const noexcept
    {
        return boundsMin_;
    }
    [[nodiscard]] const std::array<f64, 3>& BoundsMax() const noexcept
    {
        return boundsMax_;
    }
    [[nodiscard]] u32 TriangleCount() const noexcept
    {
        return triangles_;
    }
    // GPU copy of the model's distance field and surface attributes (see
    // mesh_sdf::MeshSdf). Null until the field has been generated and uploaded;
    // the field is built on the loader thread after the geometry is ready, so
    // it can arrive later than the model itself.
    struct SdfVolume
    {
        std::array<u32, 3> dimensions{};
        std::array<f32, 3> origin{};
        f32 voxelSize{0.15F};
        f32 emissiveScale{1.0F};
        f32 maximumDistance{0.0F};
        std::unique_ptr<rhi::Buffer> distance; // f32 per voxel
        std::unique_ptr<rhi::Buffer> albedo;   // RGBA8, a = on surface
        std::unique_ptr<rhi::Buffer> normal;   // octahedral 2 x snorm16
        std::unique_ptr<rhi::Buffer> emissive; // RGB8 * emissiveScale
    };
    [[nodiscard]] const SdfVolume* Sdf() const noexcept
    {
        return sdf_.distance != nullptr ? &sdf_ : nullptr;
    }

    [[nodiscard]] const std::vector<lighting::ReflectionTriangle>&
    ReflectionTriangles() const noexcept { return reflectionTriangles_; }

    // Monotonic: changes whenever the model's GPU resources are replaced.
    [[nodiscard]] u64 Generation() const noexcept
    {
        return generation_;
    }

private:
    friend class MeshLibrary;

    struct TextureSlot
    {
        std::unique_ptr<rhi::Texture> texture;
        // Decoded pixels awaiting upload; released after upload.
        u32 width{0U};
        u32 height{0U};
        bool srgb{false};
        bool repeat{true};
        std::vector<std::byte> pixels;
        bool failed{false};
    };

    std::unique_ptr<rhi::Buffer> vertices_;
    std::unique_ptr<rhi::Buffer> indices_;
    std::vector<Part> parts_;
    std::vector<lighting::ReflectionTriangle> reflectionTriangles_;
    std::vector<Material> materials_;
    std::vector<TextureSlot> textures_;
    std::array<f64, 3> boundsMin_{};
    std::array<f64, 3> boundsMax_{};
    u32 triangles_{0U};
    u64 generation_{0U};
    SdfVolume sdf_;
};

// Loads imported meshes on a worker thread (glTF parse and image decode),
// then uploads buffers and streams textures from the render thread inside
// the caller's command list, so no queue wait is ever needed. Replaced and
// removed models are retired after a fixed number of draws so in-flight
// frames never lose a resource (see ORBIT_HOT_ITERATION rule 9).
class MeshLibrary
{
public:
    explicit MeshLibrary(rhi::Device& device);
    ~MeshLibrary();

    MeshLibrary(const MeshLibrary&) = delete;
    MeshLibrary& operator=(const MeshLibrary&) = delete;

    // Marks `file` (absolute path) as wanted and returns its model when the
    // geometry is resident, nullptr while loading or after a failure. The
    // first call starts the background import.
    [[nodiscard]] const MeshModel* Acquire(const std::filesystem::path& file);

    // Like Acquire for a model that has no file: `build` produces the
    // MeshAsset (it runs once, on the loader thread, and must only touch what
    // it captured by value). `key` identifies the model, so it has to encode
    // every input of `build`; changing an input means asking with a new key,
    // and the unused model is released after the usual idle time. Generated
    // models are never hot-reloaded from disk.
    [[nodiscard]] const MeshModel* AcquireGenerated(
        const std::string& key,
        const std::function<mesh_import::MeshAsset()>& build);

    // Render thread, once per draw, before any instance is drawn: adopts
    // finished loads, records pending texture uploads into `commands`
    // (bounded per call), detects on-disk changes for hot reload and retires
    // stale resources. Must be called outside an active render pass.
    void Pump(rhi::CommandList& commands);

    [[nodiscard]] std::vector<MeshStatus> Statuses() const;

    // Stand-in textures bound when a slot has no (or not yet) texture.
    [[nodiscard]] rhi::Texture& DefaultTexture(MeshTextureRole role) const;

private:
    struct LoadedImage
    {
        u32 width{0U};
        u32 height{0U};
        std::vector<std::byte> pixels; // RGBA8
        bool failed{false};
    };

    struct LoadResult
    {
        std::filesystem::path file;
        mesh_import::MeshAsset asset;
        std::vector<lighting::ReflectionTriangle> reflectionTriangles;
        // Decoded pixels per (image, srgb) pair needed by the materials.
        std::map<std::pair<i32, bool>, LoadedImage> images;
        // Distance field + surface attributes, generated after the images are
        // decoded (shared so Adopt can keep the CPU copy until upload).
        std::shared_ptr<const mesh_sdf::MeshSdf> sdf;
        std::string error;
        std::filesystem::file_time_type modified{};
    };

    struct Entry
    {
        std::filesystem::path file;
        std::unique_ptr<MeshModel> model;
        MeshLoadState state{MeshLoadState::Loading};
        std::string error;
        std::vector<std::string> warnings;
        std::filesystem::file_time_type modified{};
        bool loadInFlight{true};
        // Built from a registered generator instead of a file on disk.
        bool generated{false};
        // Last Acquire(); a model idle for a while is released by Pump().
        std::chrono::steady_clock::time_point lastRequested{};
        u32 texturesTotal{0U};
        u32 texturesResident{0U};
        u32 triangles{0U};
        u32 parts{0U};
        u32 materials{0U};
        std::array<f64, 3> boundsMin{};
        std::array<f64, 3> boundsMax{};
        std::chrono::steady_clock::time_point nextStatCheck{};
        // Textures whose pixels are decoded but not yet on the GPU.
        std::vector<std::size_t> pendingTextures;
    };

    struct Retired
    {
        std::unique_ptr<MeshModel> model;
        std::vector<std::unique_ptr<rhi::Buffer>> staging;
        u64 retireAtTick{0U};
    };

    void WorkerMain();
    void Enqueue(const std::filesystem::path& file);
    void Adopt(
        Entry& entry,
        LoadResult&& result,
        rhi::CommandList& commands);
    void UploadPending(Entry& entry, rhi::CommandList& commands);

    rhi::Device& device_;
    std::map<std::string, Entry> entries_;
    std::vector<Retired> retired_;
    u64 tick_{0U};

    std::array<std::unique_ptr<rhi::Texture>, kMeshTextureSlots> defaults_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::filesystem::path> queue_;
    // Generator per generated key, read by the loader thread.
    std::map<std::string, std::function<mesh_import::MeshAsset()>> generators_;
    std::vector<LoadResult> finished_;
    bool stopping_{false};
    std::thread worker_;
};
} // namespace orbit::mesh_render
