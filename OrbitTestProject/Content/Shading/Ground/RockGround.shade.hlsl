// Rock Ground: a genuinely displaced ground surface, not a bump-mapped flat
// one. `height` moves the actual vertex positions (see
// ShaderParameterLayout::HasDisplacement -- the reserved `height` texture2d +
// `displacement` float pair the engine wires into the preview's vertex
// stage); the Plane preview shape is a 96x96 grid exactly so that real
// silhouette has enough vertices to show. `albedo` supplies the surface
// colour; a normal perturbed from the height map's own gradient adds the
// fine rock detail the coarse vertex grid can't carry on its own.
//
// @param albedo       texture2d Content/Textures/RockGround/RockGround_Diffuse.jpg
// @param height       texture2d Content/Textures/RockGround/RockGround_Displacement.jpg
// @param displacement float     0.25 | 0 1
// @param detail       float     1.0  | 0 4
// @param roughness    float     0.9  | 0.05 1

// Central-difference gradient of the height map, in tangent space. `strength`
// exaggerates it into a usable slope; the imported map is 1024x1024.
float3 DetailNormal(float2 uv, float strength)
{
    const float texel = 1.0 / 1024.0;
    const float hL = SampleLevel_height(uv - float2(texel, 0.0), 0.0).r;
    const float hR = SampleLevel_height(uv + float2(texel, 0.0), 0.0).r;
    const float hD = SampleLevel_height(uv - float2(0.0, texel), 0.0).r;
    const float hU = SampleLevel_height(uv + float2(0.0, texel), 0.0).r;
    return normalize(float3(-(hR - hL) * strength, -(hU - hD) * strength, 2.0));
}

// Lambert + GGX for one light, against a rock's low, mostly dielectric
// reflectance (no metallic/tint knobs -- the albedo texture already carries
// the look).
float3 RockLight(float3 n, float3 v, OrbitLight L, float3 albedo, float rough)
{
    const float3 h = normalize(L.directionWS + v);
    const float nl = saturate(dot(n, L.directionWS));
    const float nv = saturate(dot(n, v)) + 1.0e-4;
    const float nh = saturate(dot(n, h));

    const float a = rough * rough;
    const float a2 = a * a;
    const float d = nh * nh * (a2 - 1.0) + 1.0;
    const float D = a2 / (ORBIT_PI * d * d);
    const float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    const float G = (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
    const float3 f0 = float3(0.03, 0.03, 0.03);

    const float3 specular = D * G * f0 / (4.0 * nl * nv + 1.0e-4);
    const float3 diffuse = albedo / ORBIT_PI;
    return (diffuse + specular) * L.radiance * nl;
}

float4 Shade(OrbitSurface s, OrbitLighting l)
{
    const float3 albedo = Sample_albedo(s.uv).rgb;
    const float rough = clamp(Param_roughness(), 0.05, 1.0);

    // Detail normal in tangent space, rotated into the real (displaced)
    // geometric frame.
    const float3 detail = DetailNormal(s.uv, Param_detail() * 8.0);
    const float3 n = normalize(
        s.tangentWS * detail.x + s.bitangentWS * detail.y + s.normalWS * detail.z);

    float3 color = RockLight(n, s.viewWS, l.key, albedo, rough)
        + RockLight(n, s.viewWS, l.fill, albedo, rough)
        + RockLight(n, s.viewWS, l.rim, albedo, rough);

    color += lerp(l.ambientGround, l.ambientSky, n.y * 0.5 + 0.5) * albedo;

    return float4(color, 1.0);
}
