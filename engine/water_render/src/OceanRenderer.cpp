#include <orbit/water_render/OceanRenderer.hpp>

#include <orbit/math/Matrix.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::water_render
{
namespace
{
void UploadBuffer(
    rhi::Buffer& buffer,
    const void* source,
    const std::size_t bytes)
{
    if (bytes == 0)
    {
        return;
    }

    std::byte* destination =
        buffer.Map();

    std::memcpy(
        destination,
        source,
        bytes);

    buffer.Unmap();
}

[[nodiscard]] f64 EffectiveOceanRadiusMeters(
    const world::PlanetDefinition& planet,
    const world::WorldPosition& observer,
    const OceanRendererConfig& config) noexcept
{
    const f64 seaRadius =
        planet.radiusMeters +
        config.seaLevelMeters;

    const f64 observerRadius =
        math::Length(
            observer.meters);

    const f64 horizonArcMeters =
        world::HorizonArcDistanceMeters(
            seaRadius,
            observerRadius);

    return std::clamp(
        horizonArcMeters *
            config.horizonOverscan,
        config.minimumRadiusMeters,
        config.maximumRadiusMeters);
}

[[nodiscard]] std::array<u32, 40>
BuildConstants(
    const math::Mat4& matrix,
    const world::PlanetDefinition& planet,
    const world::WorldPosition& observer,
    const world::SurfaceFrame& observerFrame,
    const OceanRendererConfig& config,
    const f64 maximumRadiusMeters,
    const f32 timeSeconds) noexcept
{
    std::array<u32, 40> result{};

    static_assert(
        sizeof(matrix.values) ==
        16U * sizeof(u32));

    std::memcpy(
        result.data(),
        matrix.values.data(),
        sizeof(matrix.values));

    const auto store =
        [&result](
            const u32 index,
            const f32 value)
        {
            result[index] =
                std::bit_cast<u32>(
                    value);
        };

    store(
        16,
        static_cast<f32>(
            planet.radiusMeters));

    store(
        17,
        static_cast<f32>(
            math::Length(
                observer.meters)));

    store(
        18,
        static_cast<f32>(
            config.seaLevelMeters));

    store(
        19,
        static_cast<f32>(
            config.mesh.radialRings));

    store(
        20,
        static_cast<f32>(
            config.mesh.angularSegments));

    store(
        21,
        static_cast<f32>(
            config.minimumRadiusMeters));

    store(
        22,
        static_cast<f32>(
            maximumRadiusMeters));

    store(
        23,
        timeSeconds);

    store(
        24,
        static_cast<f32>(
            observerFrame.up.x));

    store(
        25,
        static_cast<f32>(
            observerFrame.up.y));

    store(
        26,
        static_cast<f32>(
            observerFrame.up.z));

    store(27, 0.0F);

    store(
        28,
        static_cast<f32>(
            observerFrame.east.x));

    store(
        29,
        static_cast<f32>(
            observerFrame.east.y));

    store(
        30,
        static_cast<f32>(
            observerFrame.east.z));

    store(31, 0.0F);

    store(
        32,
        static_cast<f32>(
            observerFrame.north.x));

    store(
        33,
        static_cast<f32>(
            observerFrame.north.y));

    store(
        34,
        static_cast<f32>(
            observerFrame.north.z));

    store(35, 0.0F);

    store(
        36,
        config.waveAmplitudeScale);

    store(37, 0.0F);
    store(38, 0.0F);
    store(39, 0.0F);

    return result;
}

constexpr const char* kVertexShader = R"(
cbuffer DrawConstants : register(b0)
{
    row_major float4x4 g_mvp;

    float4 g_planet;
    float4 g_mesh;

    float4 g_centerUpPlanet;
    float4 g_centerEastPlanet;
    float4 g_centerNorthPlanet;

    float4 g_waves;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 localPosition : TEXCOORD0;
    float3 surfaceNormal : TEXCOORD1;
    float waveHeight : TEXCOORD2;
    float horizonClip : SV_ClipDistance0;
};

float OceanWaveHeight(
    float3 globalSurfacePosition,
    float timeSeconds)
{
    const float3 d0 =
        normalize(
            float3(
                0.86,
                0.12,
                0.49));

    const float3 d1 =
        normalize(
            float3(
                -0.31,
                0.44,
                0.84));

    const float3 d2 =
        normalize(
            float3(
                0.18,
                -0.93,
                0.32));

    const float3 d3 =
        normalize(
            float3(
                -0.72,
                -0.21,
                0.66));

    const float wave0 =
        0.25 *
        sin(
            dot(
                globalSurfacePosition,
                d0) *
                0.02094395 +
            timeSeconds *
                1.15);

    const float wave1 =
        0.35 *
        sin(
            dot(
                globalSurfacePosition,
                d1) *
                0.01047198 +
            timeSeconds *
                0.86 +
            1.7);

    const float wave2 =
        0.48 *
        sin(
            dot(
                globalSurfacePosition,
                d2) *
                0.00418879 +
            timeSeconds *
                0.54 +
            3.1);

    const float wave3 =
        0.62 *
        sin(
            dot(
                globalSurfacePosition,
                d3) *
                0.00196350 +
            timeSeconds *
                0.36 +
            0.8);

    return
        (wave0 +
         wave1 +
         wave2 +
         wave3) *
        g_waves.x;
}

VSOutput main(uint vertexId : SV_VertexID)
{
    const float planetRadius =
        g_planet.x;

    const float observerRadius =
        g_planet.y;

    const float seaLevel =
        g_planet.z;

    const uint radialRings =
        (uint)round(
            g_planet.w);

    const uint angularSegments =
        (uint)round(
            g_mesh.x);

    float radiusMeters = 0.0;
    float theta = 0.0;

    if (vertexId != 0u)
    {
        const uint encoded =
            vertexId - 1u;

        const uint ring =
            encoded /
            angularSegments;

        const uint segment =
            encoded %
            angularSegments;

        const float t =
            radialRings > 1u
                ? (float)ring /
                    (float)(
                        radialRings -
                        1u)
                : 0.0;

        const float minimumRadius =
            max(
                g_mesh.y,
                0.001);

        const float maximumRadius =
            max(
                g_mesh.z,
                minimumRadius);

        radiusMeters =
            minimumRadius *
            pow(
                maximumRadius /
                    minimumRadius,
                t);

        theta =
            ((float)segment /
             (float)angularSegments) *
            6.28318530718;
    }

    const float tangentX =
        cos(theta);

    const float tangentY =
        sin(theta);

    const float angle =
        radiusMeters /
        planetRadius;

    const float sinAngle =
        sin(angle);

    const float cosAngle =
        cos(angle);

    const float3 localSurfaceDirection =
        float3(
            tangentX *
                sinAngle,
            cosAngle,
            tangentY *
                sinAngle);

    const float3 tangentPlanet =
        g_centerEastPlanet.xyz *
            tangentX +
        g_centerNorthPlanet.xyz *
            tangentY;

    const float3 globalSurfaceDirection =
        normalize(
            g_centerUpPlanet.xyz *
                cosAngle +
            tangentPlanet *
                sinAngle);

    const float3 globalSurfacePosition =
        globalSurfaceDirection *
        planetRadius;

    const float waveHeight =
        OceanWaveHeight(
            globalSurfacePosition,
            g_mesh.w);

    const float displacedRadius =
        planetRadius +
        seaLevel +
        waveHeight;

    const float3 localPosition =
        localSurfaceDirection *
            displacedRadius -
        float3(
            0.0,
            observerRadius,
            0.0);

    VSOutput output;

    output.position =
        mul(
            float4(
                localPosition,
                1.0),
            g_mvp);

    output.localPosition =
        localPosition;

    output.surfaceNormal =
        localSurfaceDirection;

    output.waveHeight =
        waveHeight;

    const float seaRadius =
        planetRadius +
        seaLevel;

    const float horizonCosine =
        saturate(
            seaRadius /
            observerRadius);

    output.horizonClip =
        localSurfaceDirection.y -
        horizonCosine +
        0.000002;

    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float3 localPosition : TEXCOORD0;
    float3 surfaceNormal : TEXCOORD1;
    float waveHeight : TEXCOORD2;
    float horizonClip : SV_ClipDistance0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float3 normal =
        normalize(
            input.surfaceNormal);

    const float3 viewDirection =
        normalize(
            -input.localPosition);

    const float viewFacing =
        saturate(
            dot(
                normal,
                viewDirection));

    const float fresnel =
        pow(
            1.0 -
                viewFacing,
            5.0);

    const float crest =
        saturate(
            input.waveHeight *
                0.35 +
            0.5);

    const float3 deepColor =
        float3(
            0.012,
            0.075,
            0.13);

    const float3 surfaceColor =
        float3(
            0.025,
            0.20,
            0.29);

    const float3 horizonColor =
        float3(
            0.16,
            0.34,
            0.42);

    float3 color =
        lerp(
            deepColor,
            surfaceColor,
            0.30 +
            crest *
                0.25);

    color =
        lerp(
            color,
            horizonColor,
            saturate(
                0.12 +
                fresnel *
                    0.72));

    return float4(
        color,
        1.0);
}
)";
} // namespace

class OceanRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const world::PlanetDefinition planet,
        const world::WorldPosition& observer,
        const OceanRendererConfig config)
        : device_(device),
          planet_(planet),
          config_(config),
          startTime_(
              std::chrono::steady_clock::now())
    {
        if (planet_.radiusMeters <= 0.0 ||
            !std::isfinite(
                config_.seaLevelMeters) ||
            !std::isfinite(
                config_.minimumRadiusMeters) ||
            !std::isfinite(
                config_.maximumRadiusMeters) ||
            config_.minimumRadiusMeters <= 0.0 ||
            config_.maximumRadiusMeters <=
                config_.minimumRadiusMeters ||
            config_.maximumRadiusMeters >=
                planet_.radiusMeters *
                    3.14159265358979323846 ||
            !std::isfinite(
                config_.horizonOverscan) ||
            config_.horizonOverscan < 1.0 ||
            config_.waveAmplitudeScale < 0.0F ||
            config_.nearPlaneMeters <= 0.0F ||
            config_.farPlaneMeters <=
                config_.nearPlaneMeters)
        {
            throw std::invalid_argument(
                "Orbit ocean renderer configuration is invalid.");
        }

        const std::vector<u32> indices =
            BuildOceanIndices(
                config_.mesh);

        indexCount_ =
            static_cast<u32>(
                indices.size());

        stats_.vertices =
            OceanVertexCount(
                config_.mesh);

        stats_.indices =
            indexCount_;

        indexBuffer_ =
            device_.CreateBuffer({
                .sizeBytes =
                    static_cast<u64>(
                        indices.size()) *
                    sizeof(u32),
                .usage =
                    rhi::BufferUsage::Index,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        IndexBuffer
            });

        UploadBuffer(
            *indexBuffer_,
            indices.data(),
            indices.size() *
                sizeof(u32));

        CreatePipeline(
            shaderCompiler);

        SetObserver(observer);
    }

    void UpdateObserver(
        const world::WorldPosition& observer)
    {
        SetObserver(observer);
    }

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& colorTarget,
        rhi::Texture& depthTarget,
        const u32 targetWidth,
        const u32 targetHeight,
        const OceanCamera& camera)
    {
        stats_.drawCallsLastFrame = 0;

        if (targetWidth == 0 ||
            targetHeight == 0)
        {
            return;
        }

        math::Float3 cameraForward =
            math::Normalize(
                camera.forward);

        if (math::LengthSquared(
                cameraForward) <=
            1.0e-8F)
        {
            cameraForward =
                math::Normalize(
                    math::Float3{
                        0.0F,
                        -0.28F,
                        1.0F
                    });
        }

        math::Float3 cameraUp =
            math::Normalize(
                camera.up);

        if (math::LengthSquared(
                cameraUp) <=
                1.0e-8F ||
            math::LengthSquared(
                math::Cross(
                    cameraUp,
                    cameraForward)) <=
                1.0e-8F)
        {
            cameraUp = {
                0.0F,
                1.0F,
                0.0F
            };
        }

        const f32 aspect =
            static_cast<f32>(
                targetWidth) /
            static_cast<f32>(
                targetHeight);

        const math::Mat4 view =
            math::LookAtLH(
                {0.0F, 0.0F, 0.0F},
                cameraForward,
                cameraUp);

        const math::Mat4 projection =
            math::PerspectiveReverseZLH(
                config_.verticalFovRadians,
                aspect,
                config_.nearPlaneMeters,
                config_.farPlaneMeters);

        const auto now =
            std::chrono::steady_clock::now();

        const f32 timeSeconds =
            std::chrono::duration<f32>(
                now -
                startTime_).
                count();

        const f64 effectiveRadiusMeters =
            EffectiveOceanRadiusMeters(
                planet_,
                observer_,
                config_);

        stats_.effectiveRadiusMeters =
            effectiveRadiusMeters;

        const auto constants =
            BuildConstants(
                math::Multiply(
                    view,
                    projection),
                planet_,
                observer_,
                observerFrame_,
                config_,
                effectiveRadiusMeters,
                timeSeconds);

        commandList.SetRenderTargets(
            colorTarget,
            depthTarget);

        commandList.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width =
                static_cast<f32>(
                    targetWidth),
            .height =
                static_cast<f32>(
                    targetHeight),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });

        commandList.SetScissor({
            .left = 0,
            .top = 0,
            .right =
                static_cast<i32>(
                    targetWidth),
            .bottom =
                static_cast<i32>(
                    targetHeight)
        });

        commandList.SetGraphicsPipeline(
            *pipeline_);

        commandList.SetGraphicsConstants(
            constants);

        commandList.SetIndexBuffer(
            *indexBuffer_,
            rhi::IndexFormat::UInt32);

        commandList.DrawIndexed(
            indexCount_);

        stats_.drawCallsLastFrame = 1;
    }

    [[nodiscard]] const OceanRenderStats&
    Stats() const noexcept
    {
        return stats_;
    }

private:
    void SetObserver(
        const world::WorldPosition& observer)
    {
        const f64 observerRadius =
            math::Length(
                observer.meters);

        if (observerRadius <=
            planet_.radiusMeters +
                config_.seaLevelMeters)
        {
            throw std::invalid_argument(
                "Orbit ocean observer must be above sea level.");
        }

        observer_ = observer;

        if (!observerFrameInitialized_)
        {
            observerFrame_ =
                world::MakeSurfaceFrame(
                    observer.meters);

            observerFrameInitialized_ =
                true;
        }
        else
        {
            observerFrame_ =
                world::
                    TransportSurfaceFrameToDirection(
                        observerFrame_,
                        observer.meters);
        }
    }

    void CreatePipeline(
        const shader::Compiler& shaderCompiler)
    {
        const shader::Binary vertexShader =
            shaderCompiler.Compile({
                .source = kVertexShader,
                .entryPoint = "main",
                .stage =
                    shader::Stage::Vertex,
                .debug = false
            });

        const shader::Binary pixelShader =
            shaderCompiler.Compile({
                .source = kPixelShader,
                .entryPoint = "main",
                .stage =
                    shader::Stage::Pixel,
                .debug = false
            });

        pipeline_ =
            device_.
                CreateGraphicsPipeline({
                    .vertexShader = {
                        .data =
                            vertexShader.
                                bytecode.
                                data(),
                        .size =
                            vertexShader.
                                bytecode.
                                size()
                    },
                    .pixelShader = {
                        .data =
                            pixelShader.
                                bytecode.
                                data(),
                        .size =
                            pixelShader.
                                bytecode.
                                size()
                    },
                    .vertexAttributes = {},
                    .vertexStrideBytes = 0,
                    .pushConstantDwords = 40,
                    .shaderResourceBuffers = 0,
                    .topology =
                        rhi::
                            PrimitiveTopology::
                                TriangleList,
                    .fillMode =
                        rhi::FillMode::Solid,
                    .cullMode =
                        rhi::CullMode::None,
                    .depthCompare =
                        rhi::DepthCompare::GreaterEqual,
                    .depthTest = true,
                    .depthWrite = true
                });
    }

    rhi::Device& device_;
    world::PlanetDefinition planet_{};
    OceanRendererConfig config_{};

    world::WorldPosition observer_{};
    world::SurfaceFrame observerFrame_{};
    bool observerFrameInitialized_{false};

    std::unique_ptr<
        rhi::GraphicsPipeline>
        pipeline_;

    std::unique_ptr<
        rhi::Buffer>
        indexBuffer_;

    u32 indexCount_{0};

    std::chrono::steady_clock::time_point
        startTime_;

    OceanRenderStats stats_{};
};

OceanRenderer::OceanRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const world::PlanetDefinition planet,
    const world::WorldPosition& observer,
    const OceanRendererConfig config)
    : impl_(
        std::make_unique<Impl>(
            device,
            shaderCompiler,
            planet,
            observer,
            config))
{
}

OceanRenderer::~OceanRenderer() =
    default;

void OceanRenderer::UpdateObserver(
    const world::WorldPosition& observer)
{
    impl_->UpdateObserver(observer);
}

void OceanRenderer::Draw(
    rhi::CommandList& commandList,
    rhi::Texture& colorTarget,
    rhi::Texture& depthTarget,
    const u32 targetWidth,
    const u32 targetHeight,
    const OceanCamera& camera)
{
    impl_->Draw(
        commandList,
        colorTarget,
        depthTarget,
        targetWidth,
        targetHeight,
        camera);
}

const OceanRenderStats&
OceanRenderer::Stats() const noexcept
{
    return impl_->Stats();
}
} // namespace orbit::water_render
