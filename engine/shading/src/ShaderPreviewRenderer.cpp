#include <orbit/shading/ShaderPreviewRenderer.hpp>

#include <orbit/math/Matrix.hpp>
#include <orbit/rhi/Pipeline.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace orbit::shading
{
namespace
{
struct Vertex
{
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
};

static_assert(sizeof(Vertex) == sizeof(MeshVertex),
              "The OBJ loader must produce the preview vertex layout.");

struct ShapeRange
{
    u32 firstIndex{0U};
    u32 indexCount{0U};
    i32 vertexOffset{0};
};

struct Geometry
{
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::array<ShapeRange, 3> ranges{};
};

void AddSphere(Geometry& geometry, ShapeRange& range)
{
    constexpr u32 kRings = 32U;
    constexpr u32 kSegments = 64U;

    range.vertexOffset = static_cast<i32>(geometry.vertices.size());
    range.firstIndex = static_cast<u32>(geometry.indices.size());
    const u32 base = 0U;

    for (u32 ring = 0U; ring <= kRings; ++ring)
    {
        const f32 v = static_cast<f32>(ring) / static_cast<f32>(kRings);
        const f32 phi = v * std::numbers::pi_v<f32>;

        for (u32 segment = 0U; segment <= kSegments; ++segment)
        {
            const f32 u = static_cast<f32>(segment) /
                static_cast<f32>(kSegments);
            const f32 theta = u * 2.0F * std::numbers::pi_v<f32>;

            const f32 x = std::sin(phi) * std::sin(theta);
            const f32 y = std::cos(phi);
            const f32 z = std::sin(phi) * std::cos(theta);
            geometry.vertices.push_back(
                {{x, y, z}, {x, y, z}, {u, v}});
        }
    }

    const u32 stride = kSegments + 1U;
    for (u32 ring = 0U; ring < kRings; ++ring)
    {
        for (u32 segment = 0U; segment < kSegments; ++segment)
        {
            const u32 a = base + ring * stride + segment;
            const u32 b = a + stride;
            geometry.indices.insert(
                geometry.indices.end(),
                {a, b, a + 1U, a + 1U, b, b + 1U});
        }
    }

    range.indexCount =
        static_cast<u32>(geometry.indices.size()) - range.firstIndex;
}

void AddPlane(Geometry& geometry, ShapeRange& range)
{
    // A flat-shaded grid, not a single quad: visually identical to the old
    // single quad for every existing flat-shading material (same normal,
    // same 0..1 UV), but a shader using the `height`+`displacement`
    // convention (see ShaderParameterLayout::HasDisplacement) needs enough
    // vertices to actually show a displaced ground's silhouette, not just a
    // per-pixel bump.
    constexpr f32 kExtent = 1.8F;
    constexpr u32 kSegments = 96U;

    range.vertexOffset = static_cast<i32>(geometry.vertices.size());
    range.firstIndex = static_cast<u32>(geometry.indices.size());
    // Indices are local to this shape (see AddSphere): DrawIndexed's base
    // vertex offset (range.vertexOffset, above) is what places them in the
    // shared buffer. Baking the absolute offset into the indices too would
    // double it, reading vertices from well past this shape's own range.
    const u32 base = 0U;

    for (u32 row = 0U; row <= kSegments; ++row)
    {
        const f32 v = static_cast<f32>(row) / static_cast<f32>(kSegments);
        const f32 z = (v * 2.0F - 1.0F) * kExtent;

        for (u32 column = 0U; column <= kSegments; ++column)
        {
            const f32 u = static_cast<f32>(column) / static_cast<f32>(kSegments);
            const f32 x = (u * 2.0F - 1.0F) * kExtent;
            geometry.vertices.push_back({{x, 0.0F, z}, {0, 1, 0}, {u, v}});
        }
    }

    const u32 stride = kSegments + 1U;
    for (u32 row = 0U; row < kSegments; ++row)
    {
        for (u32 column = 0U; column < kSegments; ++column)
        {
            const u32 a = base + row * stride + column;
            const u32 b = a + stride;
            geometry.indices.insert(
                geometry.indices.end(),
                {a, b, a + 1U, a + 1U, b, b + 1U});
        }
    }

    range.indexCount =
        static_cast<u32>(geometry.indices.size()) - range.firstIndex;
}

void AddCube(Geometry& geometry, ShapeRange& range)
{
    constexpr f32 h = 0.85F;
    range.vertexOffset = static_cast<i32>(geometry.vertices.size());
    range.firstIndex = static_cast<u32>(geometry.indices.size());

    struct Face
    {
        f32 normal[3];
        f32 u[3];
        f32 v[3];
    };

    constexpr Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };

    u32 index = 0U;
    for (const Face& face : faces)
    {
        for (u32 corner = 0U; corner < 4U; ++corner)
        {
            const f32 su = (corner == 1U || corner == 2U) ? 1.0F : -1.0F;
            const f32 sv = (corner >= 2U) ? 1.0F : -1.0F;
            Vertex vertex{};
            for (u32 axis = 0U; axis < 3U; ++axis)
            {
                vertex.position[axis] =
                    h * (face.normal[axis] + su * face.u[axis] +
                         sv * face.v[axis]);
                vertex.normal[axis] = face.normal[axis];
            }
            vertex.uv[0] = su * 0.5F + 0.5F;
            vertex.uv[1] = sv * 0.5F + 0.5F;
            geometry.vertices.push_back(vertex);
        }

        geometry.indices.insert(
            geometry.indices.end(),
            {index, index + 1U, index + 2U, index, index + 2U, index + 3U});
        index += 4U;
    }

    range.indexCount = 36U;
}

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}

// Converts the position of a Mat4 row into four push-constant dwords.
void PushRow(std::array<u32, 40>& out, const u32 row, const math::Mat4& m)
{
    for (u32 column = 0U; column < 4U; ++column)
    {
        out[row * 4U + column] = Bits(m.At(row, column));
    }
}
} // namespace

class ShaderPreviewRenderer::Impl
{
public:
    Impl(
        rhi::Device& deviceRef,
        const shader::Compiler& compilerRef,
        rhi::Queue& graphicsQueueRef)
        : device(deviceRef), compiler(compilerRef), graphicsQueue(graphicsQueueRef)
    {
        vertexShader = compiler.Compile({
            .source = PreviewVertexSource(),
            .entryPoint = "main",
            .stage = shader::Stage::Vertex});

        uploadAllocator = device.CreateCommandAllocator(rhi::QueueType::Graphics);
        uploadCommands = device.CreateCommandList(*uploadAllocator);
        uploadFence = device.CreateFence(0);

        const auto backgroundVertex = compiler.Compile({
            .source = BackgroundVertexSource(),
            .entryPoint = "main",
            .stage = shader::Stage::Vertex});
        const std::string backgroundSource = BackgroundPixelSource();
        const auto backgroundPixel = compiler.Compile({
            .source = backgroundSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel});

        background = device.CreateGraphicsPipeline({
            .vertexShader = View(backgroundVertex),
            .pixelShader = View(backgroundPixel),
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 24,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
            .colorAttachmentCount = 1U});

        const ShaderParameterLayout none;
        const std::string errorSource = BuildObjectPixelSource(
            "orbit-error-shader", ErrorShaderSource(), none);
        const auto errorPixel = compiler.Compile({
            .source = errorSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel});
        errorPipeline = MakeObjectPipeline(errorPixel, nullptr, 0U);
        active = errorPipeline.get();
        activeSampledTextures = 0U;

        BuildGeometry();
    }

    [[nodiscard]] static rhi::ShaderBytecodeView View(
        const shader::Binary& binary) noexcept
    {
        return {binary.bytecode.data(), binary.bytecode.size()};
    }

    // `displacementVertex` is null for the common case (the plain fixed
    // vertex shader); non-null when the program's layout.HasDisplacement().
    [[nodiscard]] std::unique_ptr<rhi::GraphicsPipeline> MakeObjectPipeline(
        const shader::Binary& pixel,
        const shader::Binary* const displacementVertex,
        const u32 sampledTextures)
    {
        static constexpr std::array<rhi::VertexAttribute, 3> kAttributes{{
            {0U, rhi::VertexFormat::Float3, 0U},
            {1U, rhi::VertexFormat::Float3, 12U},
            {2U, rhi::VertexFormat::Float2, 24U},
        }};

        return device.CreateGraphicsPipeline({
            .vertexShader = View(
                displacementVertex != nullptr ? *displacementVertex
                                               : vertexShader),
            .pixelShader = View(pixel),
            .vertexAttributes = kAttributes,
            .vertexStrideBytes = sizeof(Vertex),
            .pushConstantDwords = kPreviewPushDwords,
            .sampledTextures = sampledTextures,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            // Reverse-Z, matching the rest of the engine.
            .depthCompare = rhi::DepthCompare::GreaterEqual,
            .depthTest = true,
            .depthWrite = true,
            .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
            .colorAttachmentCount = 1U});
    }

    void BuildGeometry()
    {
        Geometry geometry;
        AddSphere(geometry, geometry.ranges[0]);
        AddPlane(geometry, geometry.ranges[1]);
        AddCube(geometry, geometry.ranges[2]);
        ranges = geometry.ranges;

        vertexBuffer = device.CreateBuffer({
            .sizeBytes = geometry.vertices.size() * sizeof(Vertex),
            .usage = rhi::BufferUsage::Vertex,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::VertexOrConstantBuffer});
        std::memcpy(
            vertexBuffer->Map(),
            geometry.vertices.data(),
            geometry.vertices.size() * sizeof(Vertex));
        vertexBuffer->Unmap();

        indexBuffer = device.CreateBuffer({
            .sizeBytes = geometry.indices.size() * sizeof(u32),
            .usage = rhi::BufferUsage::Index,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::IndexBuffer});
        std::memcpy(
            indexBuffer->Map(),
            geometry.indices.data(),
            geometry.indices.size() * sizeof(u32));
        indexBuffer->Unmap();
    }

    void Retire(std::unique_ptr<rhi::GraphicsPipeline> pipeline)
    {
        if (pipeline != nullptr)
        {
            retired.push_back({std::move(pipeline), nullptr, nullptr, nullptr, 0U});
        }
    }

    void RetireMesh()
    {
        if (meshVertices != nullptr || meshIndices != nullptr)
        {
            retired.push_back(
                {nullptr, std::move(meshVertices), std::move(meshIndices),
                 nullptr, 0U});
        }
        meshIndexCount = 0U;
    }

    void RetireTexture(std::unique_ptr<rhi::Texture> texture)
    {
        if (texture != nullptr)
        {
            retired.push_back({nullptr, nullptr, nullptr, std::move(texture), 0U});
        }
    }

    void TickRetirement()
    {
        for (auto& entry : retired)
        {
            ++entry.age;
        }
        std::erase_if(
            retired,
            [](const Retired& entry)
            {
                return entry.age >= kRetireAfterFrames;
            });
    }

    // Decodes-nothing, GPU-only: uploads pixels already decoded on the CPU
    // (by ShadingWorkspace, via content_wic) into a sampled Vulkan texture,
    // synchronously, on the shared graphics queue. Rare (a texture2d
    // parameter change or a hot-reloaded image), never per-frame.
    [[nodiscard]] std::unique_ptr<rhi::Texture> UploadTexture(
        const content::RuntimeTexture& data)
    {
        if (data.format != content::RuntimeTextureFormat::Rgba8Unorm)
        {
            throw std::runtime_error(
                "Only RGBA8 runtime textures are supported today.");
        }
        if (data.width == 0U || data.height == 0U || data.pixels.empty())
        {
            throw std::runtime_error("Empty texture.");
        }

        auto texture = device.CreateTexture({
            .width = data.width,
            .height = data.height,
            .format = rhi::TextureFormat::RGBA8_UNorm,
            .initialState = rhi::ResourceState::Common});

        auto staging = device.CreateBuffer({
            .sizeBytes = data.pixels.size(),
            .usage = rhi::BufferUsage::Generic,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::Common});
        std::memcpy(staging->Map(), data.pixels.data(), data.pixels.size());
        staging->Unmap();

        uploadAllocator->Reset();
        uploadCommands->Reset(*uploadAllocator);
        uploadCommands->Transition(
            *texture, rhi::ResourceState::Common,
            rhi::ResourceState::CopyDestination);
        uploadCommands->CopyBufferToTexture(*staging, 0, *texture);
        uploadCommands->Transition(
            *texture, rhi::ResourceState::CopyDestination,
            rhi::ResourceState::ShaderResource);
        uploadCommands->Close();

        graphicsQueue.Submit(*uploadCommands);
        graphicsQueue.Signal(*uploadFence, ++uploadFenceValue);
        uploadFence->Wait(uploadFenceValue);

        return texture;
    }

    struct Retired
    {
        std::unique_ptr<rhi::GraphicsPipeline> pipeline;
        std::unique_ptr<rhi::Buffer> vertices;
        std::unique_ptr<rhi::Buffer> indices;
        std::unique_ptr<rhi::Texture> texture;
        u32 age{0U};
    };

    rhi::Device& device;
    const shader::Compiler& compiler;
    rhi::Queue& graphicsQueue;
    shader::Binary vertexShader;
    std::unique_ptr<rhi::GraphicsPipeline> background;
    std::unique_ptr<rhi::GraphicsPipeline> errorPipeline;
    std::unique_ptr<rhi::GraphicsPipeline> userPipeline;
    rhi::GraphicsPipeline* active{nullptr};
    // How many of the active pipeline's texture slots Draw() must bind.
    u32 activeSampledTextures{0U};
    std::unique_ptr<rhi::Buffer> vertexBuffer;
    std::unique_ptr<rhi::Buffer> indexBuffer;
    std::array<ShapeRange, 3> ranges{};
    std::vector<Retired> retired;
    u64 builtRevision{~0ULL};

    // Preview mesh (PreviewShape::Mesh).
    std::unique_ptr<rhi::Buffer> meshVertices;
    std::unique_ptr<rhi::Buffer> meshIndices;
    u32 meshIndexCount{0U};
    u64 builtMeshRevision{~0ULL};

    // texture2d parameters, indexed by ShaderTextureDecl::slot.
    std::array<std::unique_ptr<rhi::Texture>, kMaxShaderTextures> textures{};
    std::array<u64, kMaxShaderTextures> builtTextureRevisions{~0ULL, ~0ULL};
    // Bound in a slot whose real texture hasn't uploaded yet (or failed).
    std::unique_ptr<rhi::Texture> dummyTexture;

    // One-shot command recording reused for every texture upload.
    std::unique_ptr<rhi::CommandAllocator> uploadAllocator;
    std::unique_ptr<rhi::CommandList> uploadCommands;
    std::unique_ptr<rhi::Fence> uploadFence;
    u64 uploadFenceValue{0U};
};

ShaderPreviewRenderer::ShaderPreviewRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler,
    rhi::Queue& graphicsQueue)
    : impl_(std::make_unique<Impl>(device, compiler, graphicsQueue))
{
    // Bound in place of a texture2d parameter's slot when the real upload
    // hasn't completed yet (or failed): a valid 1x1 texture rather than a
    // null binding.
    const std::array<std::byte, 4> white{
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    const content::RuntimeTexture whiteTexture{
        .width = 1U,
        .height = 1U,
        .format = content::RuntimeTextureFormat::Rgba8Unorm,
        .pixels = {white.begin(), white.end()}};
    impl_->dummyTexture = impl_->UploadTexture(whiteTexture);
}

ShaderPreviewRenderer::~ShaderPreviewRenderer() = default;

ShaderPreviewRenderer::UpdateResult ShaderPreviewRenderer::Update(
    const ShadingProgram* const program,
    const u64 revision,
    const MeshData* const mesh,
    const u64 meshRevision,
    const std::array<const content::RuntimeTexture*, kMaxShaderTextures>
        textures,
    const std::array<u64, kMaxShaderTextures> textureRevisions)
{
    UpdateResult result;
    impl_->TickRetirement();

    for (u32 slot = 0U; slot < kMaxShaderTextures; ++slot)
    {
        if (textureRevisions[slot] == impl_->builtTextureRevisions[slot])
        {
            continue;
        }
        impl_->builtTextureRevisions[slot] = textureRevisions[slot];

        if (textures[slot] == nullptr)
        {
            impl_->RetireTexture(std::move(impl_->textures[slot]));
            continue;
        }

        try
        {
            auto uploaded = impl_->UploadTexture(*textures[slot]);
            // The previous texture may still be referenced by frames in
            // flight: retire it, do not destroy it.
            impl_->RetireTexture(std::move(impl_->textures[slot]));
            impl_->textures[slot] = std::move(uploaded);
        }
        catch (const std::exception& exception)
        {
            // Keep whatever was bound in this slot (the dummy texture, if
            // nothing has ever loaded here).
            result.textureErrors[slot] = exception.what();
        }
    }

    if (meshRevision != impl_->builtMeshRevision)
    {
        impl_->builtMeshRevision = meshRevision;

        if (mesh == nullptr || mesh->indices.empty() || mesh->vertices.empty())
        {
            impl_->RetireMesh();
        }
        else
        {
            try
            {
                auto vertices = impl_->device.CreateBuffer({
                    .sizeBytes = mesh->vertices.size() * sizeof(MeshVertex),
                    .usage = rhi::BufferUsage::Vertex,
                    .memory = rhi::MemoryUsage::HostVisible,
                    .initialState = rhi::ResourceState::VertexOrConstantBuffer});
                std::memcpy(
                    vertices->Map(),
                    mesh->vertices.data(),
                    mesh->vertices.size() * sizeof(MeshVertex));
                vertices->Unmap();

                auto indices = impl_->device.CreateBuffer({
                    .sizeBytes = mesh->indices.size() * sizeof(u32),
                    .usage = rhi::BufferUsage::Index,
                    .memory = rhi::MemoryUsage::HostVisible,
                    .initialState = rhi::ResourceState::IndexBuffer});
                std::memcpy(
                    indices->Map(),
                    mesh->indices.data(),
                    mesh->indices.size() * sizeof(u32));
                indices->Unmap();

                // The previous buffers may still be referenced by frames in
                // flight: retire them, do not destroy them.
                impl_->RetireMesh();
                impl_->meshVertices = std::move(vertices);
                impl_->meshIndices = std::move(indices);
                impl_->meshIndexCount = static_cast<u32>(mesh->indices.size());
            }
            catch (const std::exception& exception)
            {
                result.meshError = exception.what();
            }
        }
    }

    if (revision == impl_->builtRevision)
    {
        return result;
    }
    impl_->builtRevision = revision;

    if (program == nullptr || !program->ok)
    {
        // Nothing valid: show the error checker. The user pipeline (if any)
        // is retired rather than destroyed under in-flight frames.
        if (impl_->userPipeline != nullptr)
        {
            impl_->Retire(std::move(impl_->userPipeline));
        }
        impl_->active = impl_->errorPipeline.get();
        impl_->activeSampledTextures = 0U;
        result.replaced = true;
        return result;
    }

    try
    {
        const bool displaced = program->layout.HasDisplacement();
        auto pipeline = impl_->MakeObjectPipeline(
            program->pixel,
            displaced ? &program->vertex : nullptr,
            static_cast<u32>(program->layout.textures.size()));
        impl_->Retire(std::move(impl_->userPipeline));
        impl_->userPipeline = std::move(pipeline);
        impl_->active = impl_->userPipeline.get();
        impl_->activeSampledTextures =
            static_cast<u32>(program->layout.textures.size());
        result.replaced = true;
    }
    catch (const std::exception& exception)
    {
        // Keep whatever is drawing now.
        result.error = exception.what();
    }

    return result;
}

void ShaderPreviewRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& color,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const PreviewState& state,
    const std::span<const f32, kMaxParameterFloats> parameters,
    const f32 timeSeconds)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    if (color.Format() != rhi::TextureFormat::RGBA16_Float)
    {
        throw std::logic_error(
            "The shading preview target must be RGBA16_Float.");
    }

    constexpr f32 kDegrees = std::numbers::pi_v<f32> / 180.0F;
    const PreviewCamera& camera = state.camera;

    const f32 pitch = std::clamp(camera.pitchRadians, -1.5F, 1.5F);
    const f32 distance = std::clamp(camera.distance, 1.2F, 20.0F);
    const math::Float3 eye{
        std::sin(camera.yawRadians) * std::cos(pitch) * distance,
        std::sin(pitch) * distance,
        -std::cos(camera.yawRadians) * std::cos(pitch) * distance};
    const math::Float3 target{0.0F, 0.0F, 0.0F};
    const math::Float3 upHint{0.0F, 1.0F, 0.0F};

    const f32 aspect =
        static_cast<f32>(width) / static_cast<f32>(height);
    const f32 fov = std::clamp(camera.verticalFovRadians, 0.2F, 2.0F);

    const math::Mat4 viewProjection = math::Multiply(
        math::LookAtLH(eye, target, upHint),
        math::PerspectiveReverseZLH(fov, aspect, 0.05F, 60.0F));

    const math::Float3 forward = math::Normalize(target - eye);
    const math::Float3 right = math::Normalize(math::Cross(upHint, forward));
    const math::Float3 up = math::Cross(forward, right);

    const f32 sunAzimuth = state.sunAzimuthDegrees * kDegrees;
    const f32 sunElevation = state.sunElevationDegrees * kDegrees;
    const f32 exposure = std::clamp(state.exposure, 0.01F, 64.0F);
    const f32 modelYaw = state.modelYawDegrees * kDegrees;

    std::array<u32, 40> object{};
    for (u32 row = 0U; row < 4U; ++row)
    {
        PushRow(object, row, viewProjection);
    }
    object[16] = Bits(eye.x);
    object[17] = Bits(eye.y);
    object[18] = Bits(eye.z);
    object[19] = Bits(timeSeconds);
    object[20] = Bits(std::cos(modelYaw));
    object[21] = Bits(std::sin(modelYaw));
    object[22] = Bits(1.0F);
    object[23] = Bits(static_cast<f32>(static_cast<u8>(state.shape)));
    object[24] = Bits(static_cast<f32>(static_cast<u8>(state.lighting)));
    object[25] = Bits(sunAzimuth);
    object[26] = Bits(sunElevation);
    object[27] = Bits(exposure);
    for (u32 index = 0U; index < kMaxParameterFloats; ++index)
    {
        object[28U + index] = Bits(parameters[index]);
    }
    // misc.x: the background mode, so OrbitBackdrop() matches what is drawn.
    object[36] = Bits(static_cast<f32>(static_cast<u8>(state.background)));

    std::array<u32, 24> background{};
    const auto setVec3 = [&](const u32 slot, const math::Float3& v)
    {
        background[slot * 4U + 0U] = Bits(v.x);
        background[slot * 4U + 1U] = Bits(v.y);
        background[slot * 4U + 2U] = Bits(v.z);
    };
    setVec3(0U, forward);
    setVec3(1U, right);
    setVec3(2U, up);
    background[12] = Bits(std::tan(fov * 0.5F));
    background[13] = Bits(aspect);
    background[14] = Bits(static_cast<f32>(static_cast<u8>(state.background)));
    background[15] = Bits(static_cast<f32>(static_cast<u8>(state.lighting)));
    background[16] = Bits(sunAzimuth);
    background[17] = Bits(sunElevation);
    background[18] = Bits(exposure);
    background[19] = Bits(timeSeconds);
    background[20] = Bits(static_cast<f32>(width));
    background[21] = Bits(static_cast<f32>(height));

    commands.ClearColorTarget(color, {0.0F, 0.0F, 0.0F, 1.0F});
    // Reverse-Z: far is 0.
    commands.ClearDepthTarget(depth, 0.0F);
    commands.SetRenderTargets(color, depth);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)});

    commands.SetGraphicsPipeline(*impl_->background);
    commands.SetGraphicsConstants(background);
    commands.Draw(3);

    commands.SetGraphicsPipeline(*impl_->active);
    commands.SetGraphicsConstants(object);

    for (u32 slot = 0U; slot < impl_->activeSampledTextures; ++slot)
    {
        rhi::Texture& bound = impl_->textures[slot] != nullptr
            ? *impl_->textures[slot]
            : *impl_->dummyTexture;
        commands.SetGraphicsTexture(slot, bound);
    }

    if (state.shape == PreviewShape::Mesh && impl_->meshIndexCount > 0U)
    {
        commands.SetVertexBuffer(*impl_->meshVertices, sizeof(Vertex));
        commands.SetIndexBuffer(*impl_->meshIndices, rhi::IndexFormat::UInt32);
        commands.DrawIndexed(impl_->meshIndexCount, 0U, 0);
        return;
    }

    // Mesh selected but nothing loaded: a sphere stands in.
    const std::size_t shapeIndex = state.shape == PreviewShape::Mesh
        ? 0U
        : static_cast<std::size_t>(state.shape);
    const ShapeRange& range = impl_->ranges[shapeIndex];
    commands.SetVertexBuffer(*impl_->vertexBuffer, sizeof(Vertex));
    commands.SetIndexBuffer(*impl_->indexBuffer, rhi::IndexFormat::UInt32);
    commands.DrawIndexed(range.indexCount, range.firstIndex, range.vertexOffset);
}

bool ShaderPreviewRenderer::HasMesh() const noexcept
{
    return impl_->meshIndexCount > 0U;
}

bool ShaderPreviewRenderer::UsingErrorShader() const noexcept
{
    return impl_->active == impl_->errorPipeline.get();
}

u32 ShaderPreviewRenderer::RetiredPipelineCount() const noexcept
{
    return static_cast<u32>(impl_->retired.size());
}
} // namespace orbit::shading
