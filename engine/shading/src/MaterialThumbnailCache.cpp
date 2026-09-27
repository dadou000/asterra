#include <orbit/shading/MaterialThumbnailCache.hpp>

#include <orbit/shading/ShadingWorkspace.hpp>

namespace orbit::shading
{
namespace
{
// Mirrors ShadingWorkspace::ResolveShaderFor: a ShaderMaterial's shader path
// is relative to the material's own folder.
[[nodiscard]] std::filesystem::path ResolvedShaderPath(
    const content::AssetRecord& record)
{
    if (record.kind == content::AssetKind::ShadingShader)
    {
        return record.sourcePath;
    }

    if (record.kind == content::AssetKind::ShaderMaterial &&
        record.shaderMaterial.has_value())
    {
        return (record.sourcePath.parent_path() / record.shaderMaterial->shader)
            .lexically_normal();
    }

    return {};
}
} // namespace

MaterialThumbnailCache::MaterialThumbnailCache(
    rhi::Device& device,
    const shader::Compiler& compiler,
    rhi::Queue& graphicsQueue,
    content::ContentService& content)
    : content_(content),
      device_(device),
      compiler_(compiler),
      graphicsQueue_(graphicsQueue),
      renderer_(device, compiler, graphicsQueue)
{
    depth_ = device_.CreateTexture({
        .width = kSize,
        .height = kSize,
        .format = rhi::TextureFormat::D32_Float,
        .initialState = rhi::ResourceState::Common});

    allocator_ = device_.CreateCommandAllocator(rhi::QueueType::Graphics);
    commands_ = device_.CreateCommandList(*allocator_);
    fence_ = device_.CreateFence(0);
}

MaterialThumbnailCache::~MaterialThumbnailCache() = default;

bool MaterialThumbnailCache::IsFresh(
    const Entry& entry, const std::filesystem::path& path) const
{
    if (!entry.everRendered)
    {
        return false;
    }

    const auto* record = content_.FindByPath(path);
    if (record == nullptr || record->sourceHash != entry.materialHash)
    {
        return false;
    }

    if (record->kind == content::AssetKind::ShaderMaterial)
    {
        const auto* shaderRecord =
            content_.FindByPath(ResolvedShaderPath(*record));
        if (shaderRecord == nullptr || shaderRecord->sourceHash != entry.shaderHash)
        {
            return false;
        }
    }

    return true;
}

void MaterialThumbnailCache::Render(
    Entry& entry, const std::filesystem::path& path)
{
    const auto* record = content_.FindByPath(path);
    if (record == nullptr)
    {
        return;
    }

    entry.materialHash = record->sourceHash;
    entry.shaderHash = {};
    if (record->kind == content::AssetKind::ShaderMaterial)
    {
        if (const auto* shaderRecord =
                content_.FindByPath(ResolvedShaderPath(*record));
            shaderRecord != nullptr)
        {
            entry.shaderHash = shaderRecord->sourceHash;
        }
    }

    const auto preview = ComputeMaterialPreview(content_, &compiler_, path);
    if (!preview.has_value())
    {
        return;
    }

    if (entry.color == nullptr)
    {
        entry.color = device_.CreateTexture({
            .width = kSize,
            .height = kSize,
            .format = rhi::TextureFormat::RGBA16_Float,
            .initialState = rhi::ResourceState::Common});
    }

    const auto textures = preview->PackedTextures();
    std::array<u64, kMaxShaderTextures> textureRevisions{};
    for (u32 slot = 0U; slot < kMaxShaderTextures; ++slot)
    {
        textureRevisions[slot] = textures[slot] != nullptr ? 1U : 0U;
    }

    renderer_.Update(
        &preview->program, ++programRevision_, nullptr, 0U, textures,
        textureRevisions);

    allocator_->Reset();
    commands_->Reset(*allocator_);

    commands_->Transition(
        *entry.color,
        entry.everRendered ? rhi::ResourceState::ShaderResource
                            : rhi::ResourceState::Common,
        rhi::ResourceState::RenderTarget);
    commands_->Transition(
        *depth_,
        entry.everRendered ? rhi::ResourceState::DepthRead
                            : rhi::ResourceState::Common,
        rhi::ResourceState::DepthWrite);

    const PreviewState state; // defaults: Sphere, Studio, Environment.
    renderer_.Draw(
        *commands_, *entry.color, *depth_, kSize, kSize, state,
        preview->parameters, 0.0F);

    commands_->Transition(
        *entry.color, rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource);
    commands_->Transition(
        *depth_, rhi::ResourceState::DepthWrite, rhi::ResourceState::DepthRead);
    commands_->Close();

    graphicsQueue_.Submit(*commands_);
    graphicsQueue_.Signal(*fence_, ++fenceValue_);
    fence_->Wait(fenceValue_);

    entry.everRendered = true;
}

rhi::Texture* MaterialThumbnailCache::Get(const std::filesystem::path& path)
{
    const std::string key = path.generic_string();
    Entry& entry = entries_[key];

    if (!IsFresh(entry, path))
    {
        Render(entry, path);
    }

    return entry.color.get();
}
} // namespace orbit::shading
