#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::render_graph
{
struct TextureHandle
{
    u32 index{~0U};

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return index != ~0U;
    }

    [[nodiscard]] constexpr bool operator==(
        const TextureHandle&) const noexcept = default;
};

struct BufferHandle
{
    u32 index{~0U};

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return index != ~0U;
    }

    [[nodiscard]] constexpr bool operator==(
        const BufferHandle&) const noexcept = default;
};

enum class Access : u8
{
    Read,
    Write
};

struct TextureUse
{
    TextureHandle texture{};
    rhi::ResourceState state{
        rhi::ResourceState::Common};
    Access access{Access::Read};
};

struct BufferUse
{
    BufferHandle buffer{};
    rhi::ResourceState state{
        rhi::ResourceState::Common};
    Access access{Access::Read};
};

class Resources
{
public:
    [[nodiscard]] rhi::Texture& Texture(
        TextureHandle handle) const;

    [[nodiscard]] rhi::Buffer& Buffer(
        BufferHandle handle) const;

private:
    struct TextureEntryView
    {
        rhi::Texture* texture{nullptr};
    };

    struct BufferEntryView
    {
        rhi::Buffer* buffer{nullptr};
    };

    explicit Resources(
        const std::vector<TextureEntryView>* textureEntries,
        const std::vector<BufferEntryView>* bufferEntries)
        : textureEntries_(textureEntries),
          bufferEntries_(bufferEntries)
    {
    }

    const std::vector<TextureEntryView>* textureEntries_{nullptr};
    const std::vector<BufferEntryView>* bufferEntries_{nullptr};

    friend class RenderGraph;
};

using PassCallback =
    std::function<void(
        rhi::CommandList&,
        const Resources&)>;

class RenderGraph
{
public:
    explicit RenderGraph(rhi::Device& device);
    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    // Reimporting the same resource returns its existing handle. The first
    // import establishes the state; all passes share one hazard authority.
    [[nodiscard]] TextureHandle ImportTexture(
        std::string_view name,
        rhi::Texture& texture,
        rhi::ResourceState currentState);

    [[nodiscard]] TextureHandle CreateTexture(
        std::string_view name,
        const rhi::TextureDesc& desc);

    [[nodiscard]] BufferHandle ImportBuffer(
        std::string_view name,
        rhi::Buffer& buffer,
        rhi::ResourceState currentState);

    [[nodiscard]] BufferHandle CreateBuffer(
        std::string_view name,
        const rhi::BufferDesc& desc);

    void AddPass(
        std::string_view name,
        std::vector<TextureUse> textureUses,
        PassCallback callback);

    void AddPass(
        std::string_view name,
        std::vector<TextureUse> textureUses,
        std::vector<BufferUse> bufferUses,
        PassCallback callback);

    // Validates hazards and topologically orders passes. Execute()
    // calls Compile() automatically when necessary.
    void Compile();

    void Execute(rhi::CommandList& commands);

    [[nodiscard]] rhi::Texture& Texture(
        TextureHandle handle);

    [[nodiscard]] rhi::Buffer& Buffer(
        BufferHandle handle);

    [[nodiscard]] std::size_t PassCount() const noexcept;
    [[nodiscard]] std::vector<std::string>
    CompiledPassNames() const;

private:
    struct TextureEntry;
    struct BufferEntry;
    struct PassEntry;

    [[nodiscard]] TextureEntry& RequireTexture(
        TextureHandle handle);
    [[nodiscard]] const TextureEntry& RequireTexture(
        TextureHandle handle) const;

    [[nodiscard]] BufferEntry& RequireBuffer(
        BufferHandle handle);
    [[nodiscard]] const BufferEntry& RequireBuffer(
        BufferHandle handle) const;

    rhi::Device& device_;
    std::vector<TextureEntry> textures_;
    std::vector<BufferEntry> buffers_;
    std::unordered_map<rhi::Texture*, TextureHandle> textureHandles_;
    std::unordered_map<rhi::Buffer*, BufferHandle> bufferHandles_;
    std::vector<PassEntry> passes_;
    std::vector<u32> executionOrder_;
    bool compiled_{false};
};
} // namespace orbit::render_graph
