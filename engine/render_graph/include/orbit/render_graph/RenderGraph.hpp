#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
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

class Resources
{
public:
    [[nodiscard]] rhi::Texture& Texture(
        TextureHandle handle) const;

private:
    struct EntryView
    {
        rhi::Texture* texture{nullptr};
    };

    explicit Resources(
        const std::vector<EntryView>* entries)
        : entries_(entries)
    {
    }

    const std::vector<EntryView>* entries_{nullptr};

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

    [[nodiscard]] TextureHandle ImportTexture(
        std::string_view name,
        rhi::Texture& texture,
        rhi::ResourceState currentState);

    [[nodiscard]] TextureHandle CreateTexture(
        std::string_view name,
        const rhi::TextureDesc& desc);

    void AddPass(
        std::string_view name,
        std::vector<TextureUse> uses,
        PassCallback callback);

    // Validates hazards and topologically orders passes. Execute()
    // calls Compile() automatically when necessary.
    void Compile();

    void Execute(rhi::CommandList& commands);

    [[nodiscard]] rhi::Texture& Texture(
        TextureHandle handle);

    [[nodiscard]] std::size_t PassCount() const noexcept;
    [[nodiscard]] std::vector<std::string>
    CompiledPassNames() const;

private:
    struct TextureEntry;
    struct PassEntry;

    [[nodiscard]] TextureEntry& RequireTexture(
        TextureHandle handle);
    [[nodiscard]] const TextureEntry& RequireTexture(
        TextureHandle handle) const;

    rhi::Device& device_;
    std::vector<TextureEntry> textures_;
    std::vector<PassEntry> passes_;
    std::vector<u32> executionOrder_;
    bool compiled_{false};
};
} // namespace orbit::render_graph
