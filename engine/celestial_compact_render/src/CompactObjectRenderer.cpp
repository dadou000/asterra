#include <orbit/celestial_compact_render/CompactObjectRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace orbit::celestial_compact_render
{
namespace
{
constexpr const char* kVs = R"(
struct VSOut
{
    float4 position : SV_Position;
    float2 p : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    static const float2 k[6] = {
        float2(-1,-1), float2(-1,1), float2(1,1),
        float2(-1,-1), float2(1,1), float2(1,-1)
    };

    VSOut o;
    o.position=float4(k[id],0,1);
    o.p=k[id];
    return o;
}
)";

constexpr const char* kPs = R"(
struct Constants
{
    float4 screen;
    float4 compact;
    float4 flowColor;
    float4 flow;
    float4 cameraForward;
    float4 cameraUp;
    float4 flowAxis;
};
[[vk::push_constant]] Constants g;

struct VSOut
{
    float4 position : SV_Position;
    float2 p : TEXCOORD0;
};

float4 main(VSOut i) : SV_Target0
{
    const float aspect=max(g.screen.y,0.001);
    const float shadowRadiusNdc=max(g.screen.x,1e-7);

    const float2 q=
        float2(i.p.x*aspect,i.p.y)/
        shadowRadiusNdc;

    const float r=length(q);
    const float ringRadius=max(g.compact.x,1e-4);
    const float ringIntensity=max(g.compact.y,0.0);
    const float opacity=saturate(g.compact.z);
    const float fluxScale=saturate(g.screen.w);

    const float3 forward=
        normalize(g.cameraForward.xyz);
    const float3 requestedUp=
        normalize(g.cameraUp.xyz);
    const float3 right=
        normalize(cross(forward,requestedUp));
    const float3 up=
        normalize(cross(right,forward));
    const float3 axis=
        normalize(g.flowAxis.xyz);

    const float axisR=dot(axis,right);
    const float axisU=dot(axis,up);
    const float axisV=abs(dot(axis,forward));

    float2 screenNormal=float2(axisR,-axisU);
    const float screenNormalLength=length(screenNormal);
    if(screenNormalLength<1e-5)
        screenNormal=float2(0,1);
    else
        screenNormal/=screenNormalLength;

    const float2 major=float2(-screenNormal.y,screenNormal.x);
    const float2 minor=screenNormal;

    const float majorCoord=dot(q,major);
    const float minorCoord=
        dot(q,minor)/
        max(axisV,0.08);

    const float discRadius=
        length(float2(majorCoord,minorCoord));

    const float inner=max(g.flow.x,1.0);
    const float outer=max(g.flow.y,inner+1e-4);
    const float thickness=max(g.flow.z,0.001);
    const float doppler=saturate(g.flow.w);

    float3 color=float3(0,0,0);
    float alpha=0.0;

    if(g.screen.z>0.5 &&
       discRadius>=inner &&
       discRadius<=outer)
    {
        const float radialT=
            saturate(
                (discRadius-inner)/
                max(outer-inner,1e-4));

        const float radialEmission=
            pow(
                max(inner/discRadius,1e-4),
                2.0)*
            (1.0-sqrt(
                saturate(inner/discRadius)));

        const float side=
            dot(
                normalize(float2(
                    majorCoord,
                    minorCoord)),
                float2(1,0));

        const float boost=
            max(
                0.15,
                1.0+doppler*side);

        const float edge=
            smoothstep(
                0.0,
                max(thickness*0.5,0.005),
                min(
                    discRadius-inner,
                    outer-discRadius));

        color+=
            max(g.flowColor.xyz,0.0)*
            max(g.flowColor.w,0.0)*
            radialEmission*
            boost*
            edge*
            fluxScale;

        alpha=max(
            alpha,
            saturate(
                (0.18+
                 0.82*radialEmission)*
                edge*
                fluxScale));
    }

    const float ringWidth=
        max(
            0.018,
            0.035/
            max(ringRadius,0.25));

    const float ring=
        exp(
            -0.5*
            ((r-ringRadius)/ringWidth)*
            ((r-ringRadius)/ringWidth));

    color+=
        max(g.flowColor.xyz,float3(1,0.72,0.38))*
        ring*
        ringIntensity*
        fluxScale;

    alpha=max(
        alpha,
        saturate(
            ring*
            ringIntensity*
            fluxScale));

    if(r<=1.0)
    {
        // The capture shadow is optical, not a physical surface.
        color=float3(0,0,0);
        alpha=opacity*fluxScale;
    }

    if(alpha<=1e-4)
        discard;

    return float4(color,alpha);
}
)";
} // namespace

CompactObjectRenderer::CompactObjectRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs=
        compiler.Compile({
            .source=kVs,
            .entryPoint="main",
            .stage=shader::Stage::Vertex,
            .debug=false
        });

    const auto ps=
        compiler.Compile({
            .source=kPs,
            .entryPoint="main",
            .stage=shader::Stage::Pixel,
            .debug=false
        });

    pipeline_=
        device.CreateGraphicsPipeline({
            .vertexShader={
                .data=vs.bytecode.data(),
                .size=vs.bytecode.size()
            },
            .pixelShader={
                .data=ps.bytecode.data(),
                .size=ps.bytecode.size()
            },
            .vertexAttributes={},
            .vertexStrideBytes=0U,
            .pushConstantDwords=28U,
            .topology=
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode=
                rhi::FillMode::Solid,
            .cullMode=
                rhi::CullMode::None,
            .blendMode=
                rhi::BlendMode::Alpha,
            .depthTest=false,
            .depthWrite=false,
            .colorAttachmentFormats={
                rhi::TextureFormat::
                    RGBA16_Float
            },
            .colorAttachmentCount=1U
        });
}

void CompactObjectRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const CompactObjectDraw& draw)
{
    if(width==0U ||
       height==0U ||
       draw.opacity<=0.0F ||
       draw.projectedShadowRadiusPixels<=0.0)
    {
        return;
    }

    const auto bits=[](const f32 value)
    {
        return std::bit_cast<u32>(value);
    };

    const f32 aspect=
        static_cast<f32>(width)/
        static_cast<f32>(
            std::max(height,1U));

    const f64 actualOpticalRadiusPixels =
        std::max(
            draw.projectedOpticalRadiusPixels,
            draw.projectedShadowRadiusPixels);

    constexpr f64 kMinimumPointRasterRadiusPixels =
        0.75;

    const f64 pointWeight =
        std::clamp(
            static_cast<f64>(
                draw.pointProxyWeight),
            0.0,
            1.0);

    const f64 targetOpticalRadiusPixels =
        std::max(
            actualOpticalRadiusPixels,
            kMinimumPointRasterRadiusPixels);

    const f64 effectiveOpticalRadiusPixels =
        std::lerp(
            actualOpticalRadiusPixels,
            targetOpticalRadiusPixels,
            pointWeight);

    const f64 rasterScale =
        actualOpticalRadiusPixels > 1.0e-9
            ? effectiveOpticalRadiusPixels /
                  actualOpticalRadiusPixels
            : 1.0;

    const f64 effectiveShadowRadiusPixels =
        draw.projectedShadowRadiusPixels *
        rasterScale;

    const f32 fluxScale =
        static_cast<f32>(
            std::clamp(
                actualOpticalRadiusPixels *
                    actualOpticalRadiusPixels /
                std::max(
                    effectiveOpticalRadiusPixels *
                        effectiveOpticalRadiusPixels,
                    1.0e-12),
                0.0,
                1.0));

    const f32 shadowRadiusNdc=
        static_cast<f32>(
            2.0*
            effectiveShadowRadiusPixels/
            static_cast<f64>(
                std::max(height,1U)));

    const f64 shadowRadius=
        std::max(
            draw.compact.shadowRadiusMeters,
            1.0);

    const f32 ringRatio=
        static_cast<f32>(
            draw.compact.
                photonRingRadiusMeters/
            shadowRadius);

    math::Float3 flowColor{
        1.0F,0.72F,0.38F};
    f32 flowIntensity=0.0F;
    f32 innerRatio=2.0F;
    f32 outerRatio=4.0F;
    f32 thicknessRatio=0.08F;
    f32 dopplerStrength=0.0F;
    math::Float3 flowAxis{
        0.0F,0.0F,1.0F};

    if(draw.accretion.has_value())
    {
        const auto& flow=
            *draw.accretion;

        const f64 rg=
            draw.compact.scales.
                gravitationalRadiusMeters;

        flowColor={
            static_cast<f32>(
                flow.colorLinear.x),
            static_cast<f32>(
                flow.colorLinear.y),
            static_cast<f32>(
                flow.colorLinear.z)
        };

        flowIntensity=
            static_cast<f32>(
                std::max(
                    flow.intensity,
                    0.0));

        innerRatio=
            static_cast<f32>(
                flow.innerRadiusRg*
                rg/
                shadowRadius);

        outerRatio=
            static_cast<f32>(
                flow.outerRadiusRg*
                rg/
                shadowRadius);

        thicknessRatio=
            static_cast<f32>(
                std::clamp(
                    flow.thicknessRatio,
                    0.0,
                    1.0));

        dopplerStrength=
            static_cast<f32>(
                std::clamp(
                    flow.dopplerStrength,
                    0.0,
                    1.0));

        const auto axis=
            math::Normalize(
                flow.axis);

        flowAxis={
            static_cast<f32>(axis.x),
            static_cast<f32>(axis.y),
            static_cast<f32>(axis.z)
        };
    }

    const std::array<u32,28>
        constants{
            bits(shadowRadiusNdc),
            bits(aspect),
            bits(draw.accretion.has_value()
                     ?1.0F:0.0F),
            bits(fluxScale),

            bits(ringRatio),
            bits(static_cast<f32>(
                std::max(
                    draw.compact.
                        photonRingIntensity *
                    draw.compact.
                        lensingStrength,
                    0.0))),
            bits(std::clamp(
                draw.opacity,
                0.0F,
                1.0F)),
            0U,

            bits(flowColor.x),
            bits(flowColor.y),
            bits(flowColor.z),
            bits(flowIntensity),

            bits(innerRatio),
            bits(outerRatio),
            bits(thicknessRatio),
            bits(dopplerStrength),

            bits(draw.camera.forward.x),
            bits(draw.camera.forward.y),
            bits(draw.camera.forward.z),
            0U,

            bits(draw.camera.up.x),
            bits(draw.camera.up.y),
            bits(draw.camera.up.z),
            0U,

            bits(flowAxis.x),
            bits(flowAxis.y),
            bits(flowAxis.z),
            0U
        };

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x=0.0F,
        .y=0.0F,
        .width=static_cast<f32>(width),
        .height=static_cast<f32>(height),
        .minDepth=0.0F,
        .maxDepth=1.0F
    });
    commands.SetScissor({
        .left=0,
        .top=0,
        .right=static_cast<i32>(width),
        .bottom=static_cast<i32>(height)
    });
    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.Draw(6U);
}
} // namespace orbit::celestial_compact_render
