from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one seam, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# ---------------------------------------------------------------------------
# GPU particle state: retain body identity and add physical-page refinement.
# ---------------------------------------------------------------------------
state_cpp = "engine/volume_render/src/VolumeParticleGpuState.cpp"
replace_once(
    state_cpp,
    """    float physicalSurfaceEnabled;\n    float reserved2;\n    float reserved3;\n    float reserved4;\n};\n\nstruct Particle\n""",
    """    uint4 bodyIdentity;\n};\n\nstruct Particle\n""",
)
replace_once(
    state_cpp,
    """    float physicalSurfaceEnabled;\n    float reserved0;\n    float reserved1;\n    float reserved2;\n};\n\n[[vk::binding(0, 0)]]\n""",
    """    uint4 bodyIdentity;\n};\n\n[[vk::binding(0, 0)]]\n""",
)
replace_once(
    state_cpp,
    """                if (collisionMode != 0u && particle.physicalSurfaceEnabled > 0.5)\n""",
    """                if (collisionMode != 0u &&\n                    (particle.behaviorFlags & (1u << 4u)) != 0u)\n""",
)
replace_once(
    state_cpp,
    """        particle.gravitySofteningMeters = max(spawn.gravitySofteningMeters, 0.0);\n        particle.physicalSurfaceEnabled = spawn.physicalSurfaceEnabled;\n        particle.reserved0 = 0.0;\n        particle.reserved1 = 0.0;\n        particle.reserved2 = 0.0;\n        particle.generation = g.counts.y;\n""",
    """        particle.gravitySofteningMeters = max(spawn.gravitySofteningMeters, 0.0);\n        particle.bodyIdentity = spawn.bodyIdentity;\n        particle.generation = g.counts.y;\n""",
)

terrain_shader = r'''

constexpr const char* kTerrainCollisionShader = R"(
struct Particle
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float ageSeconds;
    float lifetimeSeconds;
    float linearDragPerSecond;
    float radiusMeters;
    float emissionScale;
    float gravityScale;
    float restitution;
    float3 baseColor;
    uint behaviorFlags;
    float3 emissionColor;
    uint generation;
    float3 bodyCenterMeters;
    float gravitationalParameterM3PerS2;
    float3 surfaceRadiiMeters;
    float gravitySofteningMeters;
    uint4 bodyIdentity;
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<Particle> g_particles : register(u0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_physicalPage : register(t1);

struct Push
{
    uint4 tile;
    uint4 meta;
    uint4 body;
};

[[vk::push_constant]]
Push g;

static const uint kPhysicalTexelStrideBytes = 8u;

void DirectionToCube(float3 direction, out uint face, out float2 uv)
{
    float3 unit = normalize(direction);
    float ax = abs(unit.x);
    float ay = abs(unit.y);
    float az = abs(unit.z);

    if (ax >= ay && ax >= az)
    {
        if (unit.x >= 0.0) { face = 0u; uv = float2(-unit.z / ax, unit.y / ax); }
        else { face = 1u; uv = float2(unit.z / ax, unit.y / ax); }
    }
    else if (ay >= ax && ay >= az)
    {
        if (unit.y >= 0.0) { face = 2u; uv = float2(unit.x / ay, -unit.z / ay); }
        else { face = 3u; uv = float2(unit.x / ay, unit.z / ay); }
    }
    else
    {
        if (unit.z >= 0.0) { face = 4u; uv = float2(unit.x / az, unit.y / az); }
        else { face = 5u; uv = float2(-unit.x / az, unit.y / az); }
    }

    uv = clamp(uv, float2(-1.0, -1.0), float2(1.0, 1.0));
}

uint CoordinateToTileIndex(float coordinate, uint count)
{
    float normalized = clamp(coordinate * 0.5 + 0.5, 0.0, 1.0);
    if (normalized >= 1.0) return count - 1u;
    return uint(normalized * float(count));
}

bool BelongsToPage(uint face, float2 uv)
{
    if (face != g.tile.x) return false;
    uint level = min(g.tile.y, 30u);
    uint count = 1u << level;
    return
        CoordinateToTileIndex(uv.x, count) == g.tile.z &&
        CoordinateToTileIndex(uv.y, count) == g.tile.w;
}

float2 PageBoundsMin()
{
    uint count = 1u << min(g.tile.y, 30u);
    return float2(
        -1.0 + 2.0 * float(g.tile.z) / float(count),
        -1.0 + 2.0 * float(g.tile.w) / float(count));
}

float2 PageBoundsMax()
{
    uint count = 1u << min(g.tile.y, 30u);
    return float2(
        -1.0 + 2.0 * float(g.tile.z + 1u) / float(count),
        -1.0 + 2.0 * float(g.tile.w + 1u) / float(count));
}

float2 LoadPhysicalTexel(uint x, uint y)
{
    uint resolution = g.meta.x;
    uint index = y * resolution + x;
    return asfloat(g_physicalPage.Load2(index * kPhysicalTexelStrideBytes));
}

float2 SamplePhysicalPage(float2 uv)
{
    float2 minimumUv = PageBoundsMin();
    float2 maximumUv = PageBoundsMax();
    float2 extent = max(maximumUv - minimumUv, float2(0.0000001, 0.0000001));
    float2 normalized = saturate((uv - minimumUv) / extent);
    float resolutionMinusOne = float(max(g.meta.x - 1u, 1u));
    float2 coordinate = normalized * resolutionMinusOne;
    uint2 p0 = uint2(floor(coordinate));
    uint2 p1 = min(p0 + uint2(1u, 1u), uint2(g.meta.x - 1u, g.meta.x - 1u));
    float2 fraction = coordinate - float2(p0);
    float2 a = lerp(LoadPhysicalTexel(p0.x, p0.y), LoadPhysicalTexel(p1.x, p0.y), fraction.x);
    float2 b = lerp(LoadPhysicalTexel(p0.x, p1.y), LoadPhysicalTexel(p1.x, p1.y), fraction.x);
    return lerp(a, b, fraction.y);
}

bool SameBody(uint4 a, uint4 b)
{
    return all(a == b);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    uint capacity = g.body.z;
    if (index >= capacity) return;

    Particle particle = g_particles[index];
    uint generation = g.meta.y;
    uint4 pageBody = uint4(g.meta.z, g.meta.w, g.body.x, g.body.y);
    uint collisionMode = (particle.behaviorFlags >> 2u) & 0x3u;

    if (particle.generation != generation ||
        collisionMode == 0u ||
        (particle.behaviorFlags & (1u << 4u)) == 0u ||
        !SameBody(particle.bodyIdentity, pageBody))
    {
        return;
    }

    float3 local = particle.positionMeters - particle.bodyCenterMeters;
    float radialDistance = length(local);
    if (radialDistance <= 0.000001) return;

    float3 direction = local / radialDistance;
    uint face;
    float2 uv;
    DirectionToCube(direction, face, uv);
    if (!BelongsToPage(face, uv)) return;

    float elevationMeters = SamplePhysicalPage(uv).x;
    float3 radii = max(particle.surfaceRadiiMeters, 0.001);
    float inverseReferenceRadius = sqrt(dot(direction / radii, direction / radii));
    if (inverseReferenceRadius <= 0.0) return;

    float referenceRadius = 1.0 / inverseReferenceRadius;
    float terrainRadius = referenceRadius + elevationMeters;
    float particleRadius = max(particle.radiusMeters, 0.001);

    if (radialDistance > terrainRadius + particleRadius)
    {
        return;
    }

    if (collisionMode == 1u)
    {
        particle.generation = 0u;
        g_particles[index] = particle;
        return;
    }

    float3 surfacePoint = direction * terrainRadius;
    float3 normal = normalize(surfacePoint / (radii * radii));
    particle.positionMeters =
        particle.bodyCenterMeters + surfacePoint + normal * particleRadius;

    float normalSpeed = dot(particle.velocityMetersPerSecond, normal);
    if (normalSpeed < 0.0)
    {
        if (collisionMode == 2u)
        {
            particle.velocityMetersPerSecond -= normal * normalSpeed;
        }
        else
        {
            particle.velocityMetersPerSecond -=
                normal * normalSpeed * (1.0 + saturate(particle.restitution));
        }
    }

    g_particles[index] = particle;
}
)";
'''
replace_once(
    state_cpp,
    """[[nodiscard]] u32 Bits(const f32 value) noexcept\n""",
    terrain_shader + "\n[[nodiscard]] u32 Bits(const f32 value) noexcept\n",
)

replace_once(
    state_cpp,
    """    simulationPipeline_ = device.CreateComputePipeline({\n        .computeShader = {\n            .data = compute.bytecode.data(),\n            .size = compute.bytecode.size()\n        },\n        .pushConstantDwords = 12U,\n        .shaderResourceBuffers = ComputeBufferCount,\n        .storageTextures = 0U,\n        .sampledTextures = 0U,\n        .accelerationStructures = 0U\n    });\n}\n""",
    """    simulationPipeline_ = device.CreateComputePipeline({\n        .computeShader = {\n            .data = compute.bytecode.data(),\n            .size = compute.bytecode.size()\n        },\n        .pushConstantDwords = 12U,\n        .shaderResourceBuffers = ComputeBufferCount,\n        .storageTextures = 0U,\n        .sampledTextures = 0U,\n        .accelerationStructures = 0U\n    });\n\n    const auto terrainCollision = compiler.Compile({\n        .source = kTerrainCollisionShader,\n        .entryPoint = \"main\",\n        .stage = shader::Stage::Compute,\n        .debug = false\n    });\n    if (terrainCollision.bytecode.empty())\n    {\n        throw std::runtime_error(\n            \"Orbit failed to compile the M38 physical terrain particle collision shader.\");\n    }\n\n    terrainCollisionPipeline_ = device.CreateComputePipeline({\n        .computeShader = {\n            .data = terrainCollision.bytecode.data(),\n            .size = terrainCollision.bytecode.size()\n        },\n        .pushConstantDwords = 12U,\n        .shaderResourceBuffers = 2U,\n        .storageTextures = 0U,\n        .sampledTextures = 0U,\n        .accelerationStructures = 0U\n    });\n}\n""",
)

terrain_method = r'''
void VolumeParticleGpuState::ApplyTerrainCollision(
    rhi::CommandList& commands,
    const std::span<const VolumeParticleTerrainCollisionPage> pages)
{
    if (!initialized_ || generation_ == 0U || pages.empty())
    {
        return;
    }

    rhi::Buffer& current = CurrentBuffer();
    rhi::ResourceState& currentState =
        currentIsA_ ? stateAState_ : stateBState_;

    TransitionState(
        commands,
        current,
        currentState,
        rhi::ResourceState::UnorderedAccess);

    commands.SetComputePipeline(*terrainCollisionPipeline_);

    for (const auto& page : pages)
    {
        if (!page.IsValid())
        {
            continue;
        }

        std::array<u32, 12U> constants{};
        constants[0] = page.face;
        constants[1] = page.level;
        constants[2] = page.tileX;
        constants[3] = page.tileY;
        constants[4] = page.resolution;
        constants[5] = generation_;
        constants[6] = page.bodyIdentity[0];
        constants[7] = page.bodyIdentity[1];
        constants[8] = page.bodyIdentity[2];
        constants[9] = page.bodyIdentity[3];
        constants[10] = MaximumParticleCount;

        commands.SetComputeConstants(constants);
        commands.SetComputeBuffer(0U, current);
        commands.SetComputeBuffer(1U, *page.samples);
        commands.Dispatch((MaximumParticleCount + 63U) / 64U, 1U, 1U);
        commands.UavBarrier(current);
    }

    TransitionState(
        commands,
        current,
        currentState,
        rhi::ResourceState::ShaderResource);
}

'''
replace_once(
    state_cpp,
    """void VolumeParticleGpuState::BindForGraphics(\n""",
    terrain_method + "void VolumeParticleGpuState::BindForGraphics(\n",
)

# ---------------------------------------------------------------------------
# Renderer wrapper and shader layout.
# ---------------------------------------------------------------------------
renderer_cpp = "engine/volume_render/src/VolumeParticleRenderer.cpp"
replace_once(
    renderer_cpp,
    """    float physicalSurfaceEnabled;\n    float reserved0;\n    float reserved1;\n    float reserved2;\n};\n\n[[vk::binding(2, 0)]]\n""",
    """    uint4 bodyIdentity;\n};\n\n[[vk::binding(2, 0)]]\n""",
)

renderer_methods = r'''
void VolumeParticleRenderer::UpdateTerrainCollisionPages(
    const std::array<u32, 4U>& bodyIdentity,
    const std::span<const VolumeParticleTerrainCollisionPage> pages)
{
    terrainCollisionPages_.erase(
        std::remove_if(
            terrainCollisionPages_.begin(),
            terrainCollisionPages_.end(),
            [&bodyIdentity](const auto& page)
            {
                return page.bodyIdentity == bodyIdentity;
            }),
        terrainCollisionPages_.end());

    for (const auto& page : pages)
    {
        if (page.IsValid())
        {
            terrainCollisionPages_.push_back(page);
        }
    }

    std::stable_sort(
        terrainCollisionPages_.begin(),
        terrainCollisionPages_.end(),
        [](const auto& a, const auto& b)
        {
            return a.level > b.level;
        });
}

std::vector<VolumeParticleTerrainCollisionPage>
VolumeParticleRenderer::TerrainCollisionPagesSnapshot() const
{
    return terrainCollisionPages_;
}

void VolumeParticleRenderer::ApplyTerrainCollision(
    rhi::CommandList& commands,
    const std::span<const VolumeParticleTerrainCollisionPage> pages)
{
    state_.ApplyTerrainCollision(commands, pages);
}

'''
replace_once(
    renderer_cpp,
    """void VolumeParticleRenderer::Draw(\n""",
    renderer_methods + "void VolumeParticleRenderer::Draw(\n",
)
replace_once(
    renderer_cpp,
    """void VolumeParticleRenderer::Reset() noexcept\n{\n    state_.Reset();\n}\n""",
    """void VolumeParticleRenderer::Reset() noexcept\n{\n    state_.Reset();\n    terrainCollisionPages_.clear();\n}\n""",
)

# ---------------------------------------------------------------------------
# Studio: feed the same M12 physical pages used by the terrain renderer into
# the M38 particle refinement pass and snapshot them into the render-graph pass.
# ---------------------------------------------------------------------------
studio_cpp = "engine/studio_ui/src/StudioViewportRenderer.cpp"
replace_once(
    studio_cpp,
    """            terrain.renderer->\n                SetPhysicalPages(\n                    physicalPages.pages,\n                    physicalPages.generation);\n\n            const auto surfaceEffects =\n""",
    """            terrain.renderer->\n                SetPhysicalPages(\n                    physicalPages.pages,\n                    physicalPages.generation);\n\n            const auto particleBodyIdentity =\n                VolumeParticleBodyIdentityWords(\n                    terrainRuntime->body);\n            std::vector<\n                volume_render::VolumeParticleTerrainCollisionPage>\n                particleCollisionPages;\n            particleCollisionPages.reserve(\n                physicalPages.pages.size());\n            for (const auto& page : physicalPages.pages)\n            {\n                if (!page.IsValid())\n                {\n                    continue;\n                }\n                particleCollisionPages.push_back({\n                    .samples = page.samples,\n                    .resolution = page.resolution,\n                    .face = static_cast<u32>(page.address.tile.face),\n                    .level = page.address.tile.level,\n                    .tileX = page.address.tile.x,\n                    .tileY = page.address.tile.y,\n                    .bodyIdentity = particleBodyIdentity\n                });\n            }\n            volumeParticleRenderer_.UpdateTerrainCollisionPages(\n                particleBodyIdentity,\n                particleCollisionPages);\n\n            const auto surfaceEffects =\n""",
)
replace_once(
    studio_cpp,
    """        {\n            // M38 particle simulation is shared by all Studio viewports. The\n""",
    """        {\n            const auto particleTerrainCollisionPages =\n                volumeParticleRenderer_.TerrainCollisionPagesSnapshot();\n\n            // M38 particle simulation is shared by all Studio viewports. The\n""",
)
replace_once(
    studio_cpp,
    """                     nextParticleOrigin](\n""",
    """                     nextParticleOrigin,\n                     particleTerrainCollisionPages](\n""",
)
replace_once(
    studio_cpp,
    """                            volumeParticleRenderer_.Advance(\n                                commands,\n                                particleFrameIndex,\n                                particleDeltaSeconds,\n                                previousParticleOrigin,\n                                nextParticleOrigin);\n                        }\n\n                        volumeParticleRenderer_.Draw(\n""",
    """                            volumeParticleRenderer_.Advance(\n                                commands,\n                                particleFrameIndex,\n                                particleDeltaSeconds,\n                                previousParticleOrigin,\n                                nextParticleOrigin);\n                            volumeParticleRenderer_.ApplyTerrainCollision(\n                                commands,\n                                particleTerrainCollisionPages);\n                        }\n\n                        volumeParticleRenderer_.Draw(\n""",
)

# Remove temporary inspection workflows from the tree before the integration
# commit. The runner workflow is removed by its own commit step.
for path in [
    ".github/workflows/m38-inspect-physical-page-seams.yml",
    ".github/workflows/m38-inspect-particle-seam.yml",
]:
    p = Path(path)
    if p.exists():
        p.unlink()

print("M38 physical terrain particle collision patch applied")
