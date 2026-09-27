// Glass — Low (LOD2, the cheap tier). No shape query, no local-space volume
// trace: a single reflection sample and a single naive one-bounce refraction
// sample, blended by Fresnel, tinted by a flat authored `thickness` instead
// of a real path length. That is a fixed 2 OrbitBackdrop samples per pixel
// (a little more with `frost`, see below) on every shape, including Plane
// and Mesh where Glass_Medium/Glass_High have no real geometry to fall back
// on either — this tier never branches on the shape at all. Use it for
// background glass and anything off a hero object; step up to
// Glass_Medium.shade.hlsl for correct edge bending on Sphere/Cube, or
// Glass_High.shade.hlsl for dispersion.
//
// The naive refraction assumes the far face is parallel to the near one (a
// thin-slab approximation, same idea as the Plane/Mesh fallback in the other
// tiers) so it barely deflects the transmitted ray and mostly just tints it —
// correct for a window pane, an approximation everywhere else, and the
// reason this tier is for background objects, not the one glass the camera
// is looking straight at.
//
// @param tint       color3 0.88 0.96 1.00
// @param ior        float  1.50 | 1.0 2.4
// @param absorption float  0.35 | 0 4
// @param thickness  float  0.6 | 0.05 3
// @param frost      float  0.0 | 0 1

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float ior = max(Param_ior(), 1.0);
    const float cosi = saturate(dot(n, v));

    const float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosi, 5.0);

    // A single extra jitter tap stands in for frost here instead of an
    // averaged blur: cheap, but grainier than Glass_Medium/Glass_High.
    const float frost = Param_frost();
    float3 jitter = float3(0.0, 0.0, 0.0);
    if (frost > 0.01)
        jitter = (OrbitHash3(s.positionWS * 61.0) - 0.5) * frost * 1.6;

    const float3 reflected = OrbitBackdrop(normalize(reflect(-v, n) + jitter));

    const float eta = 1.0 / ior;
    const float3 entered = refract(-v, n, eta);
    float3 exited = dot(entered, entered) < 1.0e-6
        ? reflect(-v, n) // total internal reflection
        : refract(entered, n, ior);
    if (dot(exited, exited) < 1.0e-6)
        exited = reflect(entered, -n);
    float3 transmitted = OrbitBackdrop(normalize(exited + jitter));

    transmitted *= exp(-(1.0 - Param_tint()) * Param_absorption() * Param_thickness());

    float3 color = lerp(transmitted, reflected, fresnel);

    // One cheap highlight instead of the three-light rig the other tiers sum.
    color += l.key.radiance * pow(saturate(dot(n, normalize(l.key.directionWS + v))), 200.0) * (0.25 + fresnel);

    return float4(color, 1.0);
}
