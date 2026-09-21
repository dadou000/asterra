#include <orbit/render_graph/RenderGraph.hpp>

#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace orbit::render_graph
{
struct RenderGraph::TextureEntry
{
    std::string name;
    rhi::Texture* texture{nullptr};
    std::unique_ptr<rhi::Texture> owned;
    rhi::ResourceState currentState{
        rhi::ResourceState::Common};
    std::optional<Access> lastAccess;
};

struct RenderGraph::BufferEntry
{
    std::string name;
    rhi::Buffer* buffer{nullptr};
    std::unique_ptr<rhi::Buffer> owned;
    rhi::ResourceState currentState{
        rhi::ResourceState::Common};
    std::optional<Access> lastAccess;
};

struct RenderGraph::PassEntry
{
    std::string name;
    std::vector<TextureUse> textureUses;
    std::vector<BufferUse> bufferUses;
    PassCallback callback;
    std::vector<u32> dependencies;
};

rhi::Texture& Resources::Texture(
    const TextureHandle handle) const
{
    if (textureEntries_ == nullptr ||
        !handle.IsValid() ||
        handle.index >= textureEntries_->size() ||
        (*textureEntries_)[handle.index].texture ==
            nullptr)
    {
        throw std::out_of_range(
            "RenderGraph resource handle is invalid.");
    }

    return *(*textureEntries_)[handle.index].texture;
}

rhi::Buffer& Resources::Buffer(
    const BufferHandle handle) const
{
    if (bufferEntries_ == nullptr ||
        !handle.IsValid() ||
        handle.index >= bufferEntries_->size() ||
        (*bufferEntries_)[handle.index].buffer ==
            nullptr)
    {
        throw std::out_of_range(
            "RenderGraph buffer handle is invalid.");
    }

    return *(*bufferEntries_)[handle.index].buffer;
}

RenderGraph::RenderGraph(
    rhi::Device& device)
    : device_(device)
{
}

RenderGraph::~RenderGraph() = default;

TextureHandle RenderGraph::ImportTexture(
    const std::string_view name,
    rhi::Texture& texture,
    const rhi::ResourceState currentState)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Imported render texture requires a name.");
    }

    const TextureHandle handle{
        .index =
            static_cast<u32>(
                textures_.size())
    };

    textures_.push_back({
        .name = std::string(name),
        .texture = &texture,
        .currentState = currentState
    });

    compiled_ = false;
    return handle;
}

TextureHandle RenderGraph::CreateTexture(
    const std::string_view name,
    const rhi::TextureDesc& desc)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Render texture requires a name.");
    }

    auto texture =
        device_.CreateTexture(desc);

    rhi::Texture* pointer =
        texture.get();

    const TextureHandle handle{
        .index =
            static_cast<u32>(
                textures_.size())
    };

    textures_.push_back({
        .name = std::string(name),
        .texture = pointer,
        .owned = std::move(texture),
        .currentState =
            desc.initialState
    });

    compiled_ = false;
    return handle;
}

BufferHandle RenderGraph::ImportBuffer(
    const std::string_view name,
    rhi::Buffer& buffer,
    const rhi::ResourceState currentState)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Imported render buffer requires a name.");
    }

    const BufferHandle handle{
        .index =
            static_cast<u32>(
                buffers_.size())
    };

    buffers_.push_back({
        .name = std::string(name),
        .buffer = &buffer,
        .currentState = currentState
    });

    compiled_ = false;
    return handle;
}

BufferHandle RenderGraph::CreateBuffer(
    const std::string_view name,
    const rhi::BufferDesc& desc)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Render buffer requires a name.");
    }

    auto buffer =
        device_.CreateBuffer(desc);

    rhi::Buffer* pointer =
        buffer.get();

    const BufferHandle handle{
        .index =
            static_cast<u32>(
                buffers_.size())
    };

    buffers_.push_back({
        .name = std::string(name),
        .buffer = pointer,
        .owned = std::move(buffer),
        .currentState =
            desc.initialState
    });

    compiled_ = false;
    return handle;
}

void RenderGraph::AddPass(
    const std::string_view name,
    std::vector<TextureUse> textureUses,
    PassCallback callback)
{
    AddPass(
        name,
        std::move(textureUses),
        {},
        std::move(callback));
}

void RenderGraph::AddPass(
    const std::string_view name,
    std::vector<TextureUse> textureUses,
    std::vector<BufferUse> bufferUses,
    PassCallback callback)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Render pass requires a name.");
    }

    std::unordered_set<u32> uniqueTextures;
    for (const TextureUse& use : textureUses)
    {
        static_cast<void>(
            RequireTexture(
                use.texture));

        if (!uniqueTextures.insert(
                use.texture.index).
                second)
        {
            throw std::invalid_argument(
                "A render pass may declare each texture only once.");
        }
    }

    std::unordered_set<u32> uniqueBuffers;
    for (const BufferUse& use : bufferUses)
    {
        static_cast<void>(
            RequireBuffer(
                use.buffer));

        if (!uniqueBuffers.insert(
                use.buffer.index).
                second)
        {
            throw std::invalid_argument(
                "A render pass may declare each buffer only once.");
        }
    }

    passes_.push_back({
        .name = std::string(name),
        .textureUses = std::move(textureUses),
        .bufferUses = std::move(bufferUses),
        .callback = std::move(callback)
    });

    compiled_ = false;
}

RenderGraph::TextureEntry&
RenderGraph::RequireTexture(
    const TextureHandle handle)
{
    if (!handle.IsValid() ||
        handle.index >= textures_.size())
    {
        throw std::out_of_range(
            "RenderGraph texture handle is invalid.");
    }

    return textures_[handle.index];
}

const RenderGraph::TextureEntry&
RenderGraph::RequireTexture(
    const TextureHandle handle) const
{
    if (!handle.IsValid() ||
        handle.index >= textures_.size())
    {
        throw std::out_of_range(
            "RenderGraph texture handle is invalid.");
    }

    return textures_[handle.index];
}

RenderGraph::BufferEntry&
RenderGraph::RequireBuffer(
    const BufferHandle handle)
{
    if (!handle.IsValid() ||
        handle.index >= buffers_.size())
    {
        throw std::out_of_range(
            "RenderGraph buffer handle is invalid.");
    }

    return buffers_[handle.index];
}

const RenderGraph::BufferEntry&
RenderGraph::RequireBuffer(
    const BufferHandle handle) const
{
    if (!handle.IsValid() ||
        handle.index >= buffers_.size())
    {
        throw std::out_of_range(
            "RenderGraph buffer handle is invalid.");
    }

    return buffers_[handle.index];
}

void RenderGraph::Compile()
{
    for (PassEntry& pass : passes_)
    {
        pass.dependencies.clear();
    }

    struct HazardState
    {
        std::optional<u32> writer;
        std::vector<u32> readers;
    };

    std::vector<HazardState> textureHazards(
        textures_.size());
    std::vector<HazardState> bufferHazards(
        buffers_.size());

    const auto addDependency =
        [this](
            const u32 passIndex,
            const u32 dependency)
        {
            if (passIndex == dependency)
            {
                return;
            }

            auto& dependencies =
                passes_[passIndex].
                    dependencies;

            if (std::find(
                    dependencies.begin(),
                    dependencies.end(),
                    dependency) ==
                dependencies.end())
            {
                dependencies.push_back(
                    dependency);
            }
        };

    for (u32 passIndex = 0;
         passIndex <
            static_cast<u32>(
                passes_.size());
         ++passIndex)
    {
        const auto processHazard =
            [&](HazardState& hazard,
                const Access access)
            {
                if (access == Access::Read)
                {
                    if (hazard.writer.has_value())
                    {
                        addDependency(
                            passIndex,
                            *hazard.writer);
                    }

                    hazard.readers.push_back(
                        passIndex);
                }
                else
                {
                    if (hazard.writer.has_value())
                    {
                        addDependency(
                            passIndex,
                            *hazard.writer);
                    }

                    for (const u32 reader :
                         hazard.readers)
                    {
                        addDependency(
                            passIndex,
                            reader);
                    }

                    hazard.readers.clear();
                    hazard.writer =
                        passIndex;
                }
            };

        for (const TextureUse& use :
             passes_[passIndex].textureUses)
        {
            processHazard(
                textureHazards[
                    use.texture.index],
                use.access);
        }

        for (const BufferUse& use :
             passes_[passIndex].bufferUses)
        {
            processHazard(
                bufferHazards[
                    use.buffer.index],
                use.access);
        }
    }

    std::vector<u32> indegree(
        passes_.size(),
        0);
    std::vector<std::vector<u32>>
        dependents(
            passes_.size());

    for (u32 passIndex = 0;
         passIndex <
            static_cast<u32>(
                passes_.size());
         ++passIndex)
    {
        indegree[passIndex] =
            static_cast<u32>(
                passes_[passIndex].
                    dependencies.size());

        for (const u32 dependency :
             passes_[passIndex].
                 dependencies)
        {
            dependents[dependency].
                push_back(passIndex);
        }
    }

    std::deque<u32> ready;

    for (u32 index = 0;
         index <
            static_cast<u32>(
                indegree.size());
         ++index)
    {
        if (indegree[index] == 0)
        {
            ready.push_back(index);
        }
    }

    executionOrder_.clear();

    while (!ready.empty())
    {
        const u32 next =
            ready.front();
        ready.pop_front();
        executionOrder_.
            push_back(next);

        for (const u32 dependent :
             dependents[next])
        {
            if (--indegree[dependent] ==
                0)
            {
                ready.push_back(
                    dependent);
            }
        }
    }

    if (executionOrder_.size() !=
        passes_.size())
    {
        throw std::logic_error(
            "RenderGraph contains a dependency cycle.");
    }

    compiled_ = true;
}

void RenderGraph::Execute(
    rhi::CommandList& commands)
{
    if (!compiled_)
    {
        Compile();
    }

    std::vector<Resources::TextureEntryView>
        textureViews;
    textureViews.reserve(textures_.size());

    for (const TextureEntry& entry :
         textures_)
    {
        textureViews.push_back({
            .texture = entry.texture
        });
    }

    std::vector<Resources::BufferEntryView>
        bufferViews;
    bufferViews.reserve(buffers_.size());

    for (const BufferEntry& entry :
         buffers_)
    {
        bufferViews.push_back({
            .buffer = entry.buffer
        });
    }

    for (auto& texture : textures_)
    {
        texture.lastAccess.reset();
    }
    for (auto& buffer : buffers_)
    {
        buffer.lastAccess.reset();
    }

    const Resources resources(
        &textureViews,
        &bufferViews);

    for (const u32 passIndex :
         executionOrder_)
    {
        PassEntry& pass =
            passes_[passIndex];

        for (const TextureUse& use :
             pass.textureUses)
        {
            TextureEntry& texture =
                textures_[
                    use.texture.index];

            if (texture.currentState !=
                use.state)
            {
                commands.Transition(
                    *texture.texture,
                    texture.currentState,
                    use.state);

                texture.currentState =
                    use.state;
            }
            else if (
                use.state ==
                    rhi::ResourceState::
                        UnorderedAccess &&
                texture.lastAccess.has_value() &&
                (*texture.lastAccess ==
                     Access::Write ||
                 use.access == Access::Write))
            {
                commands.UavBarrier(
                    *texture.texture);
            }

            texture.lastAccess =
                use.access;
        }

        for (const BufferUse& use :
             pass.bufferUses)
        {
            BufferEntry& buffer =
                buffers_[
                    use.buffer.index];

            if (buffer.currentState !=
                use.state)
            {
                commands.Transition(
                    *buffer.buffer,
                    buffer.currentState,
                    use.state);

                buffer.currentState =
                    use.state;
            }
            else if (
                use.state ==
                    rhi::ResourceState::
                        UnorderedAccess &&
                buffer.lastAccess.has_value() &&
                (*buffer.lastAccess ==
                     Access::Write ||
                 use.access == Access::Write))
            {
                commands.UavBarrier(
                    *buffer.buffer);
            }

            buffer.lastAccess =
                use.access;
        }

        if (pass.callback)
        {
            pass.callback(
                commands,
                resources);
        }
    }
}

rhi::Texture& RenderGraph::Texture(
    const TextureHandle handle)
{
    return *RequireTexture(handle).texture;
}

rhi::Buffer& RenderGraph::Buffer(
    const BufferHandle handle)
{
    return *RequireBuffer(handle).buffer;
}

std::size_t RenderGraph::PassCount() const noexcept
{
    return passes_.size();
}

std::vector<std::string>
RenderGraph::CompiledPassNames() const
{
    if (!compiled_)
    {
        throw std::logic_error(
            "RenderGraph pass order requested before Compile().");
    }

    std::vector<std::string> result;
    result.reserve(
        executionOrder_.size());

    for (const u32 passIndex :
         executionOrder_)
    {
        result.push_back(
            passes_[passIndex].name);
    }

    return result;
}
} // namespace orbit::render_graph
