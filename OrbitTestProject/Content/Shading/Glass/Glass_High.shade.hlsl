// Glass — High (LOD0, the hero tier). Ray-traces the *exact* entry-to-exit
// path through the medium on the Sphere and Cube preview shapes (they are
// analytic primitives centred on the origin, so OrbitSphereExitDistance /
// OrbitBoxExitDistance give the real path length), traces that path three
// times for per-channel dispersion, and blurs both the reflection and the
// transmission with a 6-tap jittered average for frost. Worst case (frost
// and dispersion both active) that is up to 24 OrbitBackdrop samples per
// pixel — use this tier for a hero shot or a close-up gem, not for a scene
// full of glass. See Glass_Medium.shade.hlsl and Glass_Low.shade.hlsl for
// cheaper tiers, and pick per-material, not by editing this file down.
//
// Plane and Mesh have no known back-face to ray-trace against, so this falls
// back to a thin parallel-faced slab there: parallel faces barely deflect the
// transmitted ray (correctly — a window pane doesn't either), only tinting it
// by the authored `thickness`.
//
// The preview blends opaquely and cannot read the scene behind the object, so
// even the exact path refracts into OrbitBackdrop() — the environment or the
// checker behind the object — not other objects in front of it.
//
// @param tint       color3 0.88 0.96 1.00
// @param ior        float  1.50 | 1.0 2.4
// @param absorption float  0.35 | 0 4
// @param dispersion float  0.015 | 0 0.08
// @param frost      float  0.0 | 0 1
// @param thickness  float  0.6 | 0.05 3

// Backdrop lookup, blurred by `frost` (6-tap jittered average).
float3 GlassSee(float3 d, float frost, float3 seed)
{
    if (frost < 0.01)
        return OrbitBackdrop(d);

    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        const float3 j = OrbitHash3(seed * 61.0 + (float)i * 7.13) - 0.5;
        sum += OrbitBackdrop(normalize(d + j * frost * 1.6));
    }
    return sum / 6.0;
}

// Exact double refraction through a solid Sphere or Cube: bend on entry,
// ray-trace to the real exit point, bend again there. `eta` is n_air/n_glass
// for this colour channel (dispersion perturbs it per channel).
float3 GlassThroughSolid(uint shape, float3 posWS, float3 nWS, float3 v, float eta, out float thickness)
{
    const float3 entered = refract(-v, nWS, eta);
    if (dot(entered, entered) < 1.0e-6)
    {
        thickness = 0.0;
        return reflect(-v, nWS); // total internal reflection on entry
    }

    const float3 localP = OrbitToLocal(posWS);
    const float3 localD = OrbitToLocal(entered);
    float3 localExit;
    float3 exitN; // outward normal at the exit point, in local space

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

    // Exit is glass -> air, the reverse of entry's eta.
    float3 exited = refract(localD, -exitN, 1.0 / eta);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(localD, exitN); // total internal reflection on exit
    return OrbitToWorld(exited);
}

// Plane / Mesh fallback: no known back-face to ray-trace against, so this
// treats the medium as a thin slab with the entry face's own normal on both
// sides. Parallel faces barely deflect the transmitted ray (correctly, for a
// window pane); `thickness` still drives absorption below.
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

    // Fresnel (Schlick with the dielectric F0 from the index of refraction).
    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosi, 5.0);

    const float frost = Param_frost();
    const float3 seed = s.positionWS;
    const uint shape = OrbitShapeId();
    const bool solid = (shape == 0u) || (shape == 2u); // Sphere or Cube

    // Reflection off the front face.
    const float3 reflected = GlassSee(reflect(-v, n), frost, seed);

    // Refraction with per-channel dispersion, and per-channel path length
    // through the medium for absorption.
    const float d = Param_dispersion();
    const float eta = 1.0 / ior;
    float3 transmitted;
    float3 thicknessRGB;
    if (solid)
    {
        transmitted.r = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta * (1.0 - d), thicknessRGB.r), frost, seed).r;
        transmitted.g = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta,             thicknessRGB.g), frost, seed).g;
        transmitted.b = GlassSee(GlassThroughSolid(shape, s.positionWS, n, v, eta * (1.0 + d), thicknessRGB.b), frost, seed).b;
    }
    else
    {
        transmitted.r = GlassSee(GlassThroughThin(n, v, eta * (1.0 - d)), frost, seed).r;
        transmitted.g = GlassSee(GlassThroughThin(n, v, eta),             frost, seed).g;
        transmitted.b = GlassSee(GlassThroughThin(n, v, eta * (1.0 + d)), frost, seed).b;
        thicknessRGB = Param_thickness().xxx;
    }

    // Beer-Lambert absorption over the real path length.
    const float3 absorb = exp(-(1.0 - Param_tint()) * Param_absorption() * thicknessRGB);
    transmitted *= absorb;

    float3 color = lerp(transmitted, reflected, fresnel);

    // Sharp highlights from the rig; frost widens them.
    const float shininess = lerp(400.0, 30.0, frost);
    float3 highlights = float3(0.0, 0.0, 0.0);
    highlights += l.key.radiance * pow(saturate(dot(n, normalize(l.key.directionWS + v))), shininess);
    highlights += l.fill.radiance * pow(saturate(dot(n, normalize(l.fill.directionWS + v))), shininess);
    highlights += l.rim.radiance * pow(saturate(dot(n, normalize(l.rim.directionWS + v))), shininess);
    color += highlights * (0.25 + fresnel);

    // Light focused through the body lands on the far side: a soft bright
    // spot opposite the key light, seen through the glass.
    const float3 focus = normalize(-l.key.directionWS + n * 0.35);
    color += l.key.radiance * Param_tint() * pow(saturate(dot(focus, v)), 20.0)
        * 0.12 * (1.0 - fresnel);

    return float4(color, 1.0);
}
