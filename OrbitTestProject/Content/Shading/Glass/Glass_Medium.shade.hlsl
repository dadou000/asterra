// Glass — Medium (LOD1, the balanced tier). Keeps the exact ray-traced
// entry-to-exit path on Sphere and Cube (correct edge bending, correct
// per-pixel absorption thickness) but drops the two priciest parts of
// Glass_High.shade.hlsl: no per-channel dispersion trace (one refraction
// instead of three) and a 3-tap frost blur instead of 6. Worst case that is
// 6 OrbitBackdrop samples per pixel instead of High's 24 — no rainbow
// fringing, but a correctly bent, correctly tinted piece of glass. This is
// the right default for most glass in a scene; reach for Glass_High.shade.hlsl
// only for a hero object and Glass_Low.shade.hlsl for background dressing.
//
// Plane and Mesh have no known back-face to ray-trace against, so this falls
// back to a thin parallel-faced slab there, same as the other tiers: it barely
// deflects the transmitted ray and only tints it by the authored `thickness`.
//
// @param tint       color3 0.88 0.96 1.00
// @param ior        float  1.50 | 1.0 2.4
// @param absorption float  0.35 | 0 4
// @param frost      float  0.0 | 0 1
// @param thickness  float  0.6 | 0.05 3

// Backdrop lookup, blurred by `frost` (3-tap jittered average).
float3 GlassSee(float3 d, float frost, float3 seed)
{
    if (frost < 0.01)
        return OrbitBackdrop(d);

    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        const float3 j = OrbitHash3(seed * 61.0 + (float)i * 11.7) - 0.5;
        sum += OrbitBackdrop(normalize(d + j * frost * 1.6));
    }
    return sum / 3.0;
}

// Exact single-channel refraction through a solid Sphere or Cube (see
// Glass_High.shade.hlsl for the per-channel dispersive version).
float3 GlassThroughSolid(uint shape, float3 posWS, float3 nWS, float3 v, float eta, out float thickness)
{
    const float3 entered = refract(-v, nWS, eta);
    if (dot(entered, entered) < 1.0e-6)
    {
        thickness = 0.0;
        return reflect(-v, nWS);
    }

    const float3 localP = OrbitToLocal(posWS);
    const float3 localD = OrbitToLocal(entered);
    float3 localExit;
    float3 exitN;

    if (shape == 0u) // Sphere: radius 1 (AddSphere).
    {
        thickness = OrbitSphereExitDistance(localP, localD);
        localExit = localP + localD * thickness;
        exitN = normalize(localExit);
    }
    else // Cube: half-extent 0.85 (AddCube).
    {
        thickness = OrbitBoxExitDistance(localP, localD, 0.85);
        localExit = localP + localD * thickness;
        const float3 a = abs(localExit);
        exitN = (a.x >= a.y && a.x >= a.z) ? float3(sign(localExit.x), 0.0, 0.0)
              : (a.y >= a.z)               ? float3(0.0, sign(localExit.y), 0.0)
                                           : float3(0.0, 0.0, sign(localExit.z));
    }

    float3 exited = refract(localD, -exitN, 1.0 / eta);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(localD, exitN);
    return OrbitToWorld(exited);
}

// Plane / Mesh fallback: a thin parallel-faced slab (see Glass_High.shade.hlsl).
float3 GlassThroughThin(float3 nWS, float3 v, float eta)
{
    const float3 entered = refract(-v, nWS, eta);
    if (dot(entered, entered) < 1.0e-6)
        return reflect(-v, nWS);
    float3 exited = refract(entered, nWS, 1.0 / eta);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(entered, -nWS);
    return exited;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float ior = max(Param_ior(), 1.0);
    const float cosi = saturate(dot(n, v));

    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosi, 5.0);

    const float frost = Param_frost();
    const float3 seed = s.positionWS;
    const uint shape = OrbitShapeId();
    const bool solid = (shape == 0u) || (shape == 2u);

    const float3 reflected = GlassSee(reflect(-v, n), frost, seed);

    const float eta = 1.0 / ior;
    float3 transmitted;
    float thickness;
    if (solid)
    {
        transmitted = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta, thickness), frost, seed);
    }
    else
    {
        transmitted = GlassSee(GlassThroughThin(n, v, eta), frost, seed);
        thickness = Param_thickness();
    }

    const float3 absorb = exp(-(1.0 - Param_tint()) * Param_absorption() * thickness);
    transmitted *= absorb;

    float3 color = lerp(transmitted, reflected, fresnel);

    const float shininess = lerp(300.0, 30.0, frost);
    float3 highlights = float3(0.0, 0.0, 0.0);
    highlights += l.key.radiance * pow(saturate(dot(n, normalize(l.key.directionWS + v))), shininess);
    highlights += l.fill.radiance * pow(saturate(dot(n, normalize(l.fill.directionWS + v))), shininess);
    color += highlights * (0.25 + fresnel);

    return float4(color, 1.0);
}
