#pragma once
namespace orbit::lighting
{
// Declares no bindings: consumers supply g_reflectionTriangles,
// g_reflectionNodes, g_reflectionLighting and REFLECTION_ORIGIN_COUNT
// (xyz scene origin relative to camera, w node count). Stackless preorder BVH.
inline constexpr const char* kReflectionTraceHlsl = R"(
struct ReflectionTriangle
{
    float4 p0;
    float4 edge1;
    float4 edge2;
    float4 normal0;
    float4 normal1;
    float4 normal2;
    float4 albedoMetallic;
    float4 emissionRoughness;
};
struct ReflectionNode
{
    float3 minimum; uint first;
    float3 maximum; uint count;
    uint escape; uint3 reserved;
};
struct ReflectionHit
{
    uint status; // 0 unresolved (budget exhausted), 1 hit, 2 complete miss
    float t;
    float3 position;
    float3 normal;
    float3 geometricNormal;
    float3 albedo;
    float3 emission;
    float metallic;
    float roughness;
};
bool ReflectionBox(float3 lo, float3 hi, float3 origin, float3 dir, float minimumT, float maximumT)
{
    [unroll] for (uint a = 0u; a < 3u; ++a)
    {
        if (abs(dir[a]) < 1.0e-12)
        {
            if (origin[a] < lo[a] || origin[a] > hi[a]) return false;
        }
        else
        {
            const float t0 = (lo[a] - origin[a]) / dir[a];
            const float t1 = (hi[a] - origin[a]) / dir[a];
            minimumT = max(minimumT, min(t0, t1));
            maximumT = min(maximumT, max(t0, t1));
            if (minimumT > maximumT) return false;
        }
    }
    return true;
}
ReflectionHit ReflectionTrace(float3 origin, float3 direction, float minimumT, float maximumT)
{
    ReflectionHit hit = (ReflectionHit)0;
    hit.t = maximumT;
    const float3 localOrigin = origin - REFLECTION_ORIGIN_COUNT.xyz;
    const uint nodeCount = (uint)REFLECTION_ORIGIN_COUNT.w;
    uint node = 0u;
    const uint budget = (uint)g_reflectionLighting[3].w;
    [loop] for (uint visit = 0u; visit < budget && node < nodeCount; ++visit)
    {
        const ReflectionNode n = g_reflectionNodes[node];
        if (!ReflectionBox(n.minimum, n.maximum, localOrigin, direction, minimumT, hit.t))
        {
            node = n.escape;
            continue;
        }
        [loop] for (uint i = n.first; i < n.first + n.count; ++i)
        {
            const ReflectionTriangle tri = g_reflectionTriangles[i];
            const float3 p = cross(direction, tri.edge2.xyz);
            const float determinant = dot(tri.edge1.xyz, p);
            if (abs(determinant) < 1.0e-10) continue;
            const float inverse = 1.0 / determinant;
            const float3 s = localOrigin - tri.p0.xyz;
            const float u = dot(s, p) * inverse;
            const float3 q = cross(s, tri.edge1.xyz);
            const float v = dot(direction, q) * inverse;
            const float t = dot(tri.edge2.xyz, q) * inverse;
            if (u < 0.0 || v < 0.0 || u + v > 1.0 || t < minimumT || t > hit.t) continue;
            hit.status = 1u;
            hit.t = t;
            hit.position = origin + direction * t;
            hit.geometricNormal = normalize(cross(tri.edge1.xyz, tri.edge2.xyz));
            hit.normal = normalize(tri.normal0.xyz * (1.0-u-v) + tri.normal1.xyz * u + tri.normal2.xyz * v);
            if (dot(hit.normal, direction) > 0.0) hit.normal = -hit.normal;
            if (dot(hit.geometricNormal, direction) > 0.0) hit.geometricNormal = -hit.geometricNormal;
            hit.albedo = saturate(tri.albedoMetallic.rgb);
            hit.metallic = saturate(tri.albedoMetallic.w);
            hit.emission = max(tri.emissionRoughness.rgb, 0.0);
            hit.roughness = clamp(tri.emissionRoughness.w, 0.045, 1.0);
        }
        ++node;
    }
    if (node < nodeCount) hit.status = 0u; // never advertise a partial nearest hit as exact
    else if (hit.status == 0u) hit.status = 2u;
    return hit;
}

#ifdef REFLECTION_HARDWARE
// The current RHI exposes procedural AABB acceleration. Hardware prunes the
// triangle bounds; the narrow phase still intersects the actual triangle.
ReflectionHit ReflectionTraceHardware(float3 origin, float3 direction, float minimumT, float maximumT)
{
    ReflectionHit hit = (ReflectionHit)0;
    hit.t = maximumT;
    RayQuery<RAY_FLAG_NONE> query;
    RayDesc ray;
    ray.Origin = origin - REFLECTION_ORIGIN_COUNT.xyz;
    ray.Direction = direction;
    ray.TMin = minimumT;
    ray.TMax = maximumT;
    query.TraceRayInline(g_reflectionScene, RAY_FLAG_NONE, 0xFF, ray);
    uint count, stride;
    g_reflectionTriangles.GetDimensions(count, stride);
    uint visits = 0u;
    while (query.Proceed())
    {
        if (++visits > (uint)g_reflectionLighting[3].w) return hit;
        if (query.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE) continue;
        const uint index = query.CandidatePrimitiveIndex();
        if (index >= count) continue;
        const ReflectionTriangle tri = g_reflectionTriangles[index];
        const float3 p = cross(direction, tri.edge2.xyz);
        const float determinant = dot(tri.edge1.xyz, p);
        if (abs(determinant) < 1.0e-10) continue;
        const float inverse = 1.0 / determinant;
        const float3 s = ray.Origin - tri.p0.xyz;
        const float u = dot(s, p) * inverse;
        const float3 q = cross(s, tri.edge1.xyz);
        const float v = dot(direction, q) * inverse;
        const float t = dot(tri.edge2.xyz, q) * inverse;
        if (u >= 0.0 && v >= 0.0 && u+v <= 1.0 && t >= minimumT && t <= maximumT)
            query.CommitProceduralPrimitiveHit(t);
    }
    if (query.CommittedStatus() != COMMITTED_PROCEDURAL_PRIMITIVE_HIT)
    { hit.status = 2u; return hit; }
    const uint index = query.CommittedPrimitiveIndex();
    if (index >= count) return hit;
    const ReflectionTriangle tri = g_reflectionTriangles[index];
    hit.status = 1u;
    hit.t = query.CommittedRayT();
    hit.position = origin + direction * hit.t;
    const float3 p = cross(direction, tri.edge2.xyz);
    const float inverse = 1.0 / dot(tri.edge1.xyz, p);
    const float3 s = ray.Origin - tri.p0.xyz;
    const float u = dot(s, p) * inverse;
    const float v = dot(direction, cross(s, tri.edge1.xyz)) * inverse;
    hit.geometricNormal = normalize(cross(tri.edge1.xyz, tri.edge2.xyz));
    hit.normal = normalize(tri.normal0.xyz*(1.0-u-v)+tri.normal1.xyz*u+tri.normal2.xyz*v);
    if (dot(hit.normal,direction)>0.0) hit.normal=-hit.normal;
    if (dot(hit.geometricNormal,direction)>0.0) hit.geometricNormal=-hit.geometricNormal;
    hit.albedo = saturate(tri.albedoMetallic.rgb);
    hit.metallic = saturate(tri.albedoMetallic.w);
    hit.emission = max(tri.emissionRoughness.rgb, 0.0);
    hit.roughness = clamp(tri.emissionRoughness.w,0.045,1.0);
    return hit;
}
#define ReflectionTrace ReflectionTraceHardware
#endif

// Direct shading at the actual hit. Nearby hits use exact-triangle sun
// occlusion; farther hits reuse the SDF's already lit material when available.
float3 ReflectionShadeDirect(ReflectionHit hit, float3 toViewer, bool hasField)
{
    const float3 toSun = normalize(g_reflectionLighting[0].xyz);
    const float3 irradiance = max(g_reflectionLighting[1].rgb, 0.0);
    float visibility = 1.0;
    if (dot(hit.normal, toSun) > 0.0 && any(irradiance > 0.0))
    {
        const ReflectionHit shadow = ReflectionTrace(
            hit.position + hit.geometricNormal * 0.003, toSun, 0.001, 100.0);
        visibility = shadow.status == 1u ? 0.0 : shadow.status == 2u ? 1.0 : 0.0;
        if (hasField)
            visibility = min(visibility, SdfSoftShadow(
                hit.position + hit.geometricNormal * (1.1*SDF_VOXEL), toSun, 60.0, 12.0));
    }
    const float nl = saturate(dot(hit.normal, toSun));
    const float nv = max(saturate(dot(hit.normal, toViewer)), 1.0e-4);
    const float3 halfway = normalize(toSun + toViewer + 1.0e-9);
    const float nh = saturate(dot(hit.normal, halfway));
    const float vh = saturate(dot(toViewer, halfway));
    const float3 f0 = lerp(0.04.xxx, hit.albedo, hit.metallic);
    const float3 fresnel = f0 + (1.0 - f0) * pow(1.0-vh, 5.0);
    const float alpha = hit.roughness * hit.roughness;
    const float a2 = alpha * alpha;
    const float denominator = nh * nh * (a2 - 1.0) + 1.0;
    const float distribution = a2 / max(3.14159265 * denominator * denominator, 1.0e-7);
    const float k = (hit.roughness + 1.0) * (hit.roughness + 1.0) * 0.125;
    const float geometry = (nl / max(nl * (1.0-k) + k, 1.0e-4)) *
                           (nv / max(nv * (1.0-k) + k, 1.0e-4));
    const float3 diffuse = hit.albedo * (1.0-hit.metallic) * (1.0-fresnel) / 3.14159265;
    const float3 specular = distribution * geometry * fresnel / max(4.0*nl*nv, 1.0e-4);
    return hit.emission + (diffuse + specular) * irradiance * nl * visibility;
}
float3 ReflectionShadeHit(ReflectionHit hit, float3 toViewer, bool hasField)
{
    if (hasField)
    {
        const SdfSurfaceSample cached = SdfFetchSurface(hit.position, toViewer);
        if (cached.valid > 0.5 && dot(cached.normal, hit.normal) > 0.75)
        {
            if (hit.t > 4.0) return cached.radiance;
            // Relight near hits at their barycentric normal. Reuse material
            // colour from the field and remove its old direct term before
            // adding direct light, so reflection energy is not counted twice.
            hit.albedo = cached.albedo;
            const float3 sun = max(g_reflectionLighting[1].rgb, 0.0);
            const float3 toSun = normalize(g_reflectionLighting[0].xyz);
            const float shadow = SdfSoftShadow(hit.position + cached.normal * (1.1*SDF_VOXEL), toSun, 60.0, 12.0);
            const float3 oldDirect = cached.albedo / 3.14159265 * sun *
                saturate(dot(cached.normal, toSun)) * shadow;
            const float3 indirect = max(cached.radiance - oldDirect - hit.emission, 0.0);
            return ReflectionShadeDirect(hit, toViewer, hasField) + indirect;
        }
    }
    const float skyWeight = saturate(0.5 + 0.5 * dot(hit.normal, g_reflectionLighting[3].xyz));
    return ReflectionShadeDirect(hit, toViewer, hasField) + hit.albedo * (1.0-hit.metallic) *
        max(g_reflectionLighting[2].rgb, 0.0) * skyWeight / 3.14159265;
}
)";
} // namespace orbit::lighting
