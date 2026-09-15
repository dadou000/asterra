#include <orbit/render_graph/RenderGraph.hpp>

#include <algorithm>
#include <deque>
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
};

struct RenderGraph::PassEntry
{
    std::string name;
    std::vector<TextureUse> uses;
    PassCallback callback;
    std::vector<u32> dependencies;
};

rhi::Texture& Resources::Texture(
    const TextureHandle handle) const
{
    if (entries_ == nullptr ||
        !handle.IsValid() ||
        handle.index >= entries_->size() ||
        (*entries_)[handle.index].texture ==
            nullptr)
    {
        throw std::out_of_range(
            "RenderGraph resource handle is invalid.");
    }

    return *(*entries_)[handle.index].texture;
}

RenderGraph::RenderGraph(
    rhi::Device& device)
    : device_(device)
{
}

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

void RenderGraph::AddPass(
    const std::string_view name,
    std::vector<TextureUse> uses,
    PassCallback callback)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Render pass requires a name.");
    }

    std::unordered_set<u32> unique;

    for (const TextureUse& use : uses)
    {
        static_cast<void>(
            RequireTexture(
                use.texture));

        if (!unique.insert(
                use.texture.index).
                second)
        {
            throw std::invalid_argument(
                "A render pass may declare each texture only once.");
        }
    }

    passes_.push_back({
        .name = std::string(name),
        .uses = std::move(uses),
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

    std::vector<HazardState> hazards(
        textures_.size());

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
        for (const TextureUse& use :
             passes_[passIndex].uses)
        {
            HazardState& hazard =
                hazards[use.texture.index];

            if (use.access == Access::Read)
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

    std::vector<Resources::EntryView>
        views;
    views.reserve(textures_.size());

    for (const TextureEntry& entry :
         textures_)
    {
        views.push_back({
            .texture = entry.texture
        });
    }

    const Resources resources(&views);

    for (const u32 passIndex :
         executionOrder_)
    {
        PassEntry& pass =
            passes_[passIndex];

        for (const TextureUse& use :
             pass.uses)
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
