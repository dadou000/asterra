// Lit: Lambert diffuse + GGX specular, lit by the preset's key/fill/rim.
//
// Parameters are declared with // @param lines. They pack in declaration order
// and are read through the generated Param_<name>() functions.
//
// @param tint      color3 0.80 0.80 0.80
// @param roughness float  0.55 | 0.03 1
// @param metallic  float  0.0  | 0 1

float3 LitLight(OrbitSurface s, OrbitLight L, float3 albedo, float rough, float3 f0)
{
    const float3 n = s.normalWS;
    const float3 v = s.viewWS;
    const float3 h = normalize(L.directionWS + v);
    const float nl = saturate(dot(n, L.directionWS));
    const float nv = saturate(dot(n, v)) + 1.0e-4;
    const float nh = saturate(dot(n, h));
    const float vh = saturate(dot(v, h));

    const float a = rough * rough;
    const float a2 = a * a;
    const float d = nh * nh * (a2 - 1.0) + 1.0;
    const float D = a2 / (ORBIT_PI * d * d);
    const float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    const float G = (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
    const float3 F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);

    const float3 specular = D * G * F / (4.0 * nl * nv + 1.0e-4);
    const float3 diffuse = albedo * (1.0 - F) / ORBIT_PI;
    return (diffuse + specular) * L.radiance * nl;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 tint = Param_tint();
    const float rough = clamp(Param_roughness(), 0.03, 1.0);
    const float metal = saturate(Param_metallic());

    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), tint, metal);
    const float3 albedo = tint * (1.0 - metal);

    float3 color = LitLight(s, l.key, albedo, rough, f0)
        + LitLight(s, l.fill, albedo, rough, f0)
        + LitLight(s, l.rim, albedo, rough, f0);

    color += lerp(l.ambientGround, l.ambientSky, s.normalWS.y * 0.5 + 0.5) * albedo;
    color += OrbitEnvironment(reflect(-s.viewWS, s.normalWS))
        * f0 * (1.0 - rough * 0.85);

    return float4(color, 1.0);
}
