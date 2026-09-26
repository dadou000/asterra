// Lunar regolith: Lommel-Seeliger photometry with an opposition surge and
// procedural craters. Airless-body shading: a single hard sun, no ambient.
//
// Try the "Space" lighting preset with the Sphere shape.
//
// @param albedo      color3 0.11 0.105 0.10
// @param freshBoost  float  0.80 | 0 3
// @param craters     float  0.65 | 0 1
// @param opposition  float  0.90 | 0 4
// @param oppWidth    float  0.05 | 0.005 0.3

// Returns 0 outside a crater, up to 1 at the centre of the deepest bowl.
float CraterMask(float3 p, float density)
{
    const float scale = 3.2;
    const float3 cell = floor(p * scale);
    float best = 0.0;

    for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
    for (int z = -1; z <= 1; ++z)
    {
        const float3 c = cell + float3(x, y, z);
        const float3 center = c + OrbitHash3(c);
        const float radius = 0.18 + 0.32 * OrbitHash(c + 41.0);
        const float present = step(1.0 - density, OrbitHash(c + 7.0));
        const float d = length(p * scale - center) / radius;
        best = max(best, present * saturate(1.0 - d * d));
    }

    return best;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float3 sun = l.key.directionWS;

    const float crater = CraterMask(normalize(s.positionWS), Param_craters());
    const float fine = OrbitHash(floor(normalize(s.positionWS) * 260.0));
    // Fresh ejecta is the same regolith, less space-weathered: brighter.
    const float3 albedo = lerp(Param_albedo(), Param_albedo() * (1.0 + Param_freshBoost()), crater)
        * (0.85 + 0.3 * fine);

    const float mu0 = saturate(dot(n, sun));
    const float mu = saturate(dot(n, v));
    const float lommelSeeliger = mu0 / (mu0 + mu + 1.0e-4);

    const float phase = acos(clamp(dot(sun, v), -1.0, 1.0));
    const float opposition =
        1.0 + Param_opposition() / (1.0 + tan(phase * 0.5) / max(Param_oppWidth(), 1.0e-4));

    const float3 color = albedo * lommelSeeliger * opposition * l.key.radiance * 0.9
        + albedo * l.ambientSky * 0.5;
    return float4(color, 1.0);
}
