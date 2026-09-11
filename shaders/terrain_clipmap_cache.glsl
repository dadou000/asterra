#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray macro_tex;
layout(set = 0, binding = 1) uniform sampler2DArray soil_tex;
layout(set = 0, binding = 2) uniform sampler2DArray surface_tex;
layout(set = 0, binding = 3) uniform sampler2DArray geology_tex;
layout(set = 0, binding = 4) uniform sampler2DArray structure_tex;
layout(set = 0, binding = 5) uniform sampler2DArray climate_tex;
layout(set = 0, binding = 6) uniform sampler2DArray hydrology_tex;
layout(rgba32f, set = 0, binding = 7) uniform writeonly image2DArray cache_image;

layout(push_constant, std430) uniform Params {
    vec4 anchor_dir_radius;
    vec4 anchor_right_base;
    vec4 anchor_up_macro;
    vec4 context_detail;
    ivec4 rect;
    ivec4 meta;
} pc;

const float PI = 3.14159265358979323846;
const float TANGENT_FAST_MAX_ARC_M = 70000.0;
const float PROJECTION_BLEND_END_M = 110000.0;
const float MAX_PROJECTED_THETA = 1.62;
const float MACRO_FILTER_FOOTPRINT_VERTICES = 3.0;
const float MACRO_MAX_LOD = 5.0;
const int MACRO_MAX_MIP = 5;

uint gm_hash(ivec3 p, uint seed) {
    uint h = uint(p.x) * 0x8da6b343u;
    h ^= uint(p.y) * 0xd8163841u;
    h ^= uint(p.z) * 0xcb1ab31fu;
    h ^= seed * 0x9e3779b9u;
    h ^= h >> 16u; h *= 0x7feb352du; h ^= h >> 15u;
    h *= 0x846ca68bu; h ^= h >> 16u;
    return h;
}
float gm_rand(ivec3 p, uint seed) { return float(gm_hash(p, seed) & 0x00ffffffu) / 16777215.0; }
vec3 gm_rand3(ivec3 p, uint seed) {
    uint h0 = gm_hash(p, seed);
    uint h1 = (h0 ^ (h0 >> 13u) ^ 0x68bc21ebu) * 0x9e3779b9u;
    uint h2 = (h0 ^ (h0 << 11u) ^ 0x02e5be93u) * 0x85ebca6bu;
    h1 ^= h1 >> 16u; h2 ^= h2 >> 15u;
    return vec3(float(h0 & 0x00ffffffu), float(h1 & 0x00ffffffu),
        float(h2 & 0x00ffffffu)) / 16777215.0;
}
float gm_value(vec3 p, uint seed) {
    ivec3 i = ivec3(floor(p)); vec3 f = fract(p);
    f = f*f*f*(f*(f*6.0-15.0)+10.0);
    float a=mix(gm_rand(i+ivec3(0,0,0),seed),gm_rand(i+ivec3(1,0,0),seed),f.x);
    float b=mix(gm_rand(i+ivec3(0,1,0),seed),gm_rand(i+ivec3(1,1,0),seed),f.x);
    float c=mix(gm_rand(i+ivec3(0,0,1),seed),gm_rand(i+ivec3(1,0,1),seed),f.x);
    float d=mix(gm_rand(i+ivec3(0,1,1),seed),gm_rand(i+ivec3(1,1,1),seed),f.x);
    return mix(mix(a,b,f.y),mix(c,d,f.y),f.z)*2.0-1.0;
}
float gm_fbm(vec3 p,uint seed){float h=0.0,a=0.52;for(int i=0;i<3;i++){h+=gm_value(p,seed+uint(i*17))*a;p=p*2.01+vec3(17,-11,7);a*=0.48;}return h*1.0957;}
float gm_fbm_warp(vec3 p,uint seed){float h=0.0,a=0.52;for(int i=0;i<2;i++){h+=gm_value(p,seed+uint(i*17))*a;p=p*2.01+vec3(17,-11,7);a*=0.48;}return h*1.2662;}
float gm_ridged(vec3 p,uint seed){return 1.0-abs(gm_fbm(p,seed));}
vec3 gm_domain_warp(vec3 p,uint seed,float strength){return p+vec3(gm_fbm_warp(p+vec3(13.1,7.7,-4.3),seed+101u),gm_fbm_warp(p+vec3(-5.7,19.3,8.9),seed+211u),gm_fbm_warp(p+vec3(9.2,-3.8,23.4),seed+307u))*strength;}
float gm_cellular_ridge(vec3 p,uint seed){ivec3 base=ivec3(floor(p));float f1=1e9,f2=1e9;for(int z=-1;z<=1;z++)for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){ivec3 c=base+ivec3(x,y,z);vec3 feature=vec3(c)+vec3(0.25)+gm_rand3(c,seed+3u)*0.50;vec3 delta=p-feature;float d2=dot(delta,delta);if(d2<f1){f2=f1;f1=d2;}else if(d2<f2){f2=d2;}}return clamp(1.0-(f2-f1)*2.8,0.0,1.0);}

// Near-field material microrelief, ported from gpu_material_microrelief.gdshaderinc
// / gpu_surface_antitile.gdshaderinc's spat_periodic_value so scatter's height cache
// (sg_rendered_l0_sample in gpu_scatter_common.gdshaderinc) agrees with the actual
// rendered surface, which adds this same displacement for level-0 vertices near the
// camera (spherical_geometry_clipmap_cached/global_surface.gdshader). Those .gdshaderinc
// files declare their scale controls as ordinary Godot ShaderMaterial uniforms, which
// have no meaning in a raw #[compute] RDShaderFile, so the pure math is duplicated here
// with every u_microrelief_*_scale baked to its 1.0 default instead of #included.
//
// detail_position deliberately uses the local anchor-relative offset_m/final_h (tens of
// kilometres at most), not the planet-absolute dir*radius (millions of metres) already
// used for geomorph() below: geomorph's finest band is 24 m and tolerates the latter,
// but microrelief's noise cells go down to 1 m, where dir*radius would lose enough
// float32 precision to make the pattern jitter from cell to cell.
int spat_posmod(int value, int period) {
	int m = value % period;
	return m < 0 ? m + period : m;
}
ivec3 spat_wrap_cell(ivec3 cell, int period) {
	int safe_period = max(period, 1);
	return ivec3(spat_posmod(cell.x, safe_period), spat_posmod(cell.y, safe_period),
		spat_posmod(cell.z, safe_period));
}
vec3 spat_rand3(ivec3 cell, int period, uint seed) {
	uint h = gm_hash(spat_wrap_cell(cell, period), seed);
	vec3 r = vec3(float(h & 0x000003ffu), float((h >> uint(10)) & 0x000003ffu),
		float((h >> uint(20)) & 0x000003ffu));
	return r / 511.5 - 1.0;
}
vec3 spat_periodic_value(vec3 p, int period, uint seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	vec3 c000 = spat_rand3(i + ivec3(0, 0, 0), period, seed);
	vec3 c100 = spat_rand3(i + ivec3(1, 0, 0), period, seed);
	vec3 c010 = spat_rand3(i + ivec3(0, 1, 0), period, seed);
	vec3 c110 = spat_rand3(i + ivec3(1, 1, 0), period, seed);
	vec3 c001 = spat_rand3(i + ivec3(0, 0, 1), period, seed);
	vec3 c101 = spat_rand3(i + ivec3(1, 0, 1), period, seed);
	vec3 c011 = spat_rand3(i + ivec3(0, 1, 1), period, seed);
	vec3 c111 = spat_rand3(i + ivec3(1, 1, 1), period, seed);
	vec3 z0 = mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y);
	vec3 z1 = mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y);
	return mix(z0, z1, f.z);
}

float mmr_scalar(vec3 v) { return dot(v, vec3(0.577350269, 0.577350269, 0.577350269)); }
float mmr_noise(vec3 p, float cell_m, int period, uint seed) {
	return clamp(mmr_scalar(spat_periodic_value(p / cell_m, period, seed)), -1.0, 1.0);
}
float mmr_ridge(float n) { return 1.0 - abs(n); }

float mmr_rock_height(vec3 p, float rock_id, vec4 geology, vec4 structure) {
	float n4 = mmr_noise(p, 4.0, 1024, 0x9127u);
	float n2 = mmr_noise(p, 2.0, 2048, 0x53d1u);
	float n1 = mmr_noise(p, 1.0, 4096, 0xc781u);
	float fault = structure.g;
	float erodible = geology.r;
	float dip = geology.g;
	float block = (mmr_ridge(n4) - 0.52) * 0.12 + n2 * 0.055 + n1 * 0.022;
	float fracture = pow(clamp(mmr_ridge(n2), 0.0, 1.0), 7.0);
	block -= fracture * (0.035 + fault * 0.055);
	float phase_a = (p.x + 2.0 * p.y + p.z) * TAU / 4.0;
	float phase_b = (2.0 * p.x - p.y + p.z) * TAU / 2.0;
	float bedding = sin(phase_a + n4 * 1.5) * 0.5 + sin(phase_b + n2) * 0.22;
	float bed_step = (smoothstep(-0.20, 0.22, sin(phase_a + n4)) - 0.5) * 2.0;
	float h = block;
	if (rock_id < 0.5) { h = block * 1.15 + n4 * 0.040; }
	else if (rock_id < 1.5) { h = block * 0.82 - fracture * 0.030; }
	else if (rock_id < 2.5) { h = block * 1.08 + n1 * 0.030; }
	else if (rock_id < 3.5) { h = block * 0.55 + bedding * 0.095 * (0.55 + dip * 0.45); }
	else if (rock_id < 4.5) { h = block * 0.42 + bedding * 0.125 * (0.65 + dip * 0.50); }
	else if (rock_id < 5.5) { h = block * 0.30 + bed_step * 0.105 + bedding * 0.035; }
	else if (rock_id < 6.5) { h = block * 0.20 + bed_step * 0.055 + bedding * 0.030; }
	else if (rock_id < 7.5) { h = block * 0.52 + bed_step * 0.075 - fracture * 0.030; }
	else if (rock_id < 8.5) { h = block * 0.58 + bed_step * 0.065; }
	else if (rock_id < 9.5) {
		float clast = pow(clamp(mmr_ridge(n1), 0.0, 1.0), 3.0) - 0.45;
		h = block * 0.55 + clast * 0.095;
	} else if (rock_id < 10.5) { h = block * 1.22 + mmr_ridge(n2) * 0.050 - 0.025; }
	else if (rock_id < 11.5) {
		h = block * 0.62 + n1 * 0.045 - pow(clamp(mmr_ridge(n1), 0.0, 1.0), 6.0) * 0.025;
	} else { h = block * 0.48 + bedding * 0.070 + n2 * 0.035; }
	return h * mix(1.08, 0.72, clamp(erodible, 0.0, 1.0)) * (1.0 + fault * 0.16);
}

// rock_id/biome are approximated as 0.0: the render shader's rock-type texture isn't
// bound in this compute pass, and biome is accepted but never actually read by this
// function (see gpu_material_microrelief.gdshaderinc's own top comment).
float mmr_height(vec3 detail_position, vec4 soil, vec4 surface, vec4 geology,
		vec4 structure, vec4 climate, vec4 hydro, vec4 landform) {
	float soil_depth = surface.r * 14.0;
	float temp_c = mix(-40.0, 50.0, climate.r);
	float precip = climate.g * 3000.0;
	float thin = 1.0 - smoothstep(0.08, 0.62, soil_depth);
	float mountain = landform.r;
	float arid = landform.g;
	float depositional = landform.a;
	float cold = 1.0 - smoothstep(-7.0, 2.0, temp_c);
	float climatic_bare = smoothstep(0.58, 0.94, arid) * (1.0 - smoothstep(240.0, 720.0, precip));
	climatic_bare = max(climatic_bare, 1.0 - smoothstep(-15.0, -4.0, temp_c));
	float alpine_cold = mountain * (1.0 - smoothstep(-3.0, 5.0, temp_c));
	float rock_w = clamp(max(thin * (0.45 + mountain * 0.65), climatic_bare * thin * 0.72)
		* (1.0 - surface.b * 0.45), 0.0, 1.0);
	rock_w = max(rock_w, alpine_cold * thin * 0.72);
	float sand_w = soil.r * arid * (1.0 - rock_w) * (0.45 + surface.a * 0.55);
	float mud_w = surface.g * (soil.g * 0.45 + soil.b * 0.80) * depositional * (1.0 - rock_w);
	float snow_w = cold * (0.35 + alpine_cold * 0.65) * (1.0 - rock_w * 0.50);
	float gravel_w = hydro.b * (0.30 + surface.a * 0.70) * (1.0 - mud_w);
	float scree_w = mountain * surface.a * thin * (1.0 - rock_w * 0.35);
	float loose_total = sand_w + mud_w + snow_w + gravel_w + scree_w;
	float soil_w = clamp((1.0 - rock_w) * (1.0 - min(loose_total, 1.0)), 0.0, 1.0);
	float n8 = mmr_noise(detail_position, 8.0, 512, 0x2db7u);
	float n4 = mmr_noise(detail_position, 4.0, 1024, 0x7a31u);
	float n2 = mmr_noise(detail_position, 2.0, 2048, 0xb921u);
	float n1 = mmr_noise(detail_position, 1.0, 4096, 0xe173u);
	float rock_h = mmr_rock_height(detail_position, 0.0, geology, structure);
	float soil_h = n2 * 0.018 + n1 * 0.008;
	float sand_h = (mmr_ridge(n2) - 0.52) * 0.045 + n4 * 0.012;
	float mud_h = n2 * 0.018 + n1 * 0.008 - pow(clamp(mmr_ridge(n1), 0.0, 1.0), 7.0) * 0.012;
	float snow_h = n4 * 0.035 + n2 * 0.018;
	float gravel_h = (pow(clamp(mmr_ridge(n1), 0.0, 1.0), 3.0) - 0.42) * 0.070 + n2 * 0.018;
	float scree_h = (pow(clamp(mmr_ridge(n2), 0.0, 1.0), 2.2) - 0.45) * 0.105 + n4 * 0.025;
	float weighted = rock_h * rock_w + soil_h * soil_w + sand_h * sand_w + mud_h * mud_w
		+ snow_h * snow_w + gravel_h * gravel_w + scree_h * scree_w;
	float total = max(rock_w + soil_w + sand_w + mud_w + snow_w + gravel_w + scree_w, 1.0);
	return weighted / total + n8 * 0.010;
}

vec3 surface_face_uv(vec3 dir) {
    vec3 d=normalize(dir),ad=abs(d);int face=0;vec3 axis=vec3(1,0,0),right=vec3(0,0,-1),up=vec3(0,1,0);
    if(ad.x>=ad.y&&ad.x>=ad.z){if(d.x>=0.0){face=0;}else{face=1;axis=vec3(-1,0,0);right=vec3(0,0,1);}}
    else if(ad.y>=ad.z){if(d.y>=0.0){face=2;axis=vec3(0,1,0);right=vec3(-1,0,0);up=vec3(0,0,1);}else{face=3;axis=vec3(0,-1,0);right=vec3(-1,0,0);up=vec3(0,0,-1);}}
    else{if(d.z>=0.0){face=4;axis=vec3(0,0,1);right=vec3(1,0,0);}else{face=5;axis=vec3(0,0,-1);right=vec3(-1,0,0);}}
    float denom=max(dot(axis,d),1e-8),q=PI*0.25;
    return vec3(atan(dot(right,d)/denom)/q*0.5+0.5,atan(dot(up,d)/denom)/q*0.5+0.5,float(face));
}
vec3 array_coord(vec3 dir,float face_res,sampler2DArray tex){vec3 fuv=surface_face_uv(dir);ivec3 s=textureSize(tex,0);float tr=float(s.x),gutter=max((tr-face_res)*0.5,0.0);return vec3((fuv.xy*face_res+vec2(gutter))/max(tr,1.0),fuv.z);}
vec4 ctx_soil(vec3 d){return textureLod(soil_tex,array_coord(d,pc.context_detail.x,soil_tex),0.0);}vec4 ctx_surface(vec3 d){return textureLod(surface_tex,array_coord(d,pc.context_detail.x,surface_tex),0.0);}vec4 ctx_geology(vec3 d){return textureLod(geology_tex,array_coord(d,pc.context_detail.x,geology_tex),0.0);}vec4 ctx_structure(vec3 d){return textureLod(structure_tex,array_coord(d,pc.context_detail.x,structure_tex),0.0);}vec4 ctx_climate(vec3 d){return textureLod(climate_tex,array_coord(d,pc.context_detail.x,climate_tex),0.0);}vec4 ctx_hydro(vec3 d){return textureLod(hydrology_tex,array_coord(d,pc.context_detail.x,hydrology_tex),0.0);}
float ctx_temperature_c(vec4 c){return mix(-40.0,50.0,c.r);}float ctx_precip_mm(vec4 c){return c.g*3000.0;}float ctx_uplift_signed(vec4 s){return s.r*2.0-1.0;}vec2 ctx_flow(vec4 h){return h.rg*2.0-1.0;}
vec4 landform_weights(float macro_h,vec4 soil,vec4 surface,vec4 geology,vec4 structure,vec4 climate,vec4 hydro){float uplift=max(ctx_uplift_signed(structure),0.0);float altitude=smoothstep(450.0,2600.0,max(macro_h,0.0));float tectonic=clamp(uplift*0.82+structure.g*0.50+geology.a*0.28,0.0,1.0);float mountain=clamp(max(tectonic,altitude*0.50),0.0,1.0);float arid=clamp((1.0-surface.g)*(1.0-clamp(ctx_precip_mm(climate)/900.0,0.0,1.0))*(0.35+soil.r*0.85)*(1.0-surface.b*0.55),0.0,1.0);float cold=1.0-smoothstep(-12.0,4.0,ctx_temperature_c(climate));float glacial=clamp(cold*(0.35+altitude*0.75),0.0,1.0);float depositional=clamp(max(hydro.a,structure.b*0.85+structure.a*0.9)*(0.5+geology.r*0.35),0.0,1.0);return vec4(mountain,arid,glacial,depositional);}
float band_weight(float wavelength_m,float spacing_m){float four_spacing=spacing_m*4.0;float x=clamp((wavelength_m-four_spacing)/max(four_spacing,1e-5),0.0,1.0);return x*x*(3.0-2.0*x);}
vec3 flow_world(vec3 dir,vec2 flow_xy){float flow_len_sq=dot(flow_xy,flow_xy);if(flow_len_sq<=1e-10)return vec3(0.0);vec3 basis_up=abs(dir.y)>0.999?vec3(0,0,1):vec3(0,1,0);vec3 east=normalize(cross(basis_up,dir));vec3 north=normalize(cross(dir,east));vec3 flow=east*flow_xy.x+north*flow_xy.y;return flow*inversesqrt(max(dot(flow,flow),1e-10));}
float geomorph(vec3 dir,float spacing_m,float macro_h){float radius=pc.anchor_dir_radius.w;uint seed=uint(max(pc.context_detail.y,1.0));vec3 world_m=dir*radius;vec4 soil=ctx_soil(dir),surface=ctx_surface(dir),geology=ctx_geology(dir),structure=ctx_structure(dir),climate=ctx_climate(dir),hydro=ctx_hydro(dir),weights=landform_weights(macro_h,soil,surface,geology,structure,climate,hydro);float mountain=weights.r,arid=weights.g,glacial=weights.b,depositional=weights.a,hardness=1.20-geology.r*0.48,h=0.0;float w16k=band_weight(16000.0,spacing_m);if(w16k>0.001){vec3 p=gm_domain_warp(world_m/16000.0,seed+11u,0.8);h+=gm_fbm(p,seed+13u)*mix(24.0,125.0,mountain)*w16k;}float w6k=band_weight(6000.0,spacing_m),mountain_gate=smoothstep(0.04,0.08,mountain);if(w6k>0.001&&mountain>0.04){vec3 p=gm_domain_warp(world_m/6000.0,seed+31u,1.1);float cells=gm_cellular_ridge(p,seed+37u),ridge=gm_ridged(p*1.55,seed+41u);h+=(mix(ridge,cells,0.58)*2.0-1.0)*210.0*mountain*mountain_gate*hardness*w6k;}float w1400=band_weight(1400.0,spacing_m);if(w1400>0.001){vec3 p=gm_domain_warp(world_m/1400.0,seed+53u,0.72);float ridge=gm_ridged(p*1.25,seed+59u),detail=gm_fbm(p*2.1,seed+61u);h+=((ridge*2.0-1.0)*72.0*mountain+detail*24.0)*w1400;}float w420=band_weight(420.0,spacing_m);if(w420>0.001){vec2 raw_flow=ctx_flow(hydro);float flow_mag=length(raw_flow),flow_gate=smoothstep(0.02,0.20,flow_mag);vec3 sample_m=world_m;if(flow_mag>1e-5){vec3 flow=flow_world(dir,raw_flow);float along_q=dot(world_m,flow);vec3 along=flow*along_q,warped_m=along*0.42+(world_m-along)*1.45;sample_m=mix(world_m,warped_m,flow_gate);}vec3 p=gm_domain_warp(sample_m/420.0,seed+71u,0.55);float channel=pow(gm_ridged(p,seed+73u),4.6);h-=channel*mix(2.0,34.0,hydro.b)*(1.0-depositional*0.78)*w420;float depositional_gate=smoothstep(0.0,0.03,depositional);if(depositional>0.0){float fan=pow(gm_ridged(p*0.48+vec3(9.0),seed+79u),2.2);h+=fan*depositional*depositional_gate*mix(1.0,12.0,surface.a)*w420;}}float w120=band_weight(120.0,spacing_m);if(w120>0.001){h+=gm_fbm(world_m/120.0,seed+89u)*4.5*(1.0-glacial*0.6)*w120;float dune_weight=arid*soil.r,dune_gate=smoothstep(0.0,0.03,dune_weight);if(dune_weight>0.0){float dune=gm_ridged(gm_domain_warp(world_m/180.0,seed+97u,0.45),seed+101u);h+=(dune*2.0-1.0)*9.0*dune_weight*dune_gate*w120;}}float w24=band_weight(24.0,spacing_m);if(w24>0.001)h+=gm_value(world_m/24.0,seed+109u)*0.9*w24;float glacial_gate=smoothstep(0.0,0.05,glacial);if(glacial>0.0){float ice=gm_fbm(world_m/2600.0+vec3(2.0,-7.0,5.0),seed+127u);h=mix(h,h*0.62+ice*52.0,glacial*glacial_gate*0.72);}return h;}

vec4 cubic_weights(float t){float t2=t*t,t3=t2*t;return vec4((1.0-3.0*t+3.0*t2-t3)/6.0,(4.0-6.0*t2+3.0*t3)/6.0,(1.0+3.0*t+3.0*t2-3.0*t3)/6.0,t3/6.0);}
float macro_bicubic_mip(vec3 coord,int mip){ivec3 size3=textureSize(macro_tex,mip);vec2 size2=vec2(size3.xy);vec2 p=coord.xy*size2-vec2(0.5),base=floor(p),f=fract(p);vec4 wx=cubic_weights(f.x),wy=cubic_weights(f.y);float gx0=wx.x+wx.y,gx1=wx.z+wx.w,gy0=wy.x+wy.y,gy1=wy.z+wy.w;float x0=base.x-1.0+wx.y/max(gx0,1e-9),x1=base.x+1.0+wx.w/max(gx1,1e-9),y0=base.y-1.0+wy.y/max(gy0,1e-9),y1=base.y+1.0+wy.w/max(gy1,1e-9);vec2 uv00=(vec2(x0,y0)+vec2(0.5))/size2,uv10=(vec2(x1,y0)+vec2(0.5))/size2,uv01=(vec2(x0,y1)+vec2(0.5))/size2,uv11=(vec2(x1,y1)+vec2(0.5))/size2;float fmip=float(mip);float s00=textureLod(macro_tex,vec3(uv00,coord.z),fmip).r,s10=textureLod(macro_tex,vec3(uv10,coord.z),fmip).r,s01=textureLod(macro_tex,vec3(uv01,coord.z),fmip).r,s11=textureLod(macro_tex,vec3(uv11,coord.z),fmip).r;return mix(mix(s00,s10,gx1),mix(s01,s11,gx1),gy1);}
float smootherstep01(float x){x=clamp(x,0.0,1.0);return x*x*x*(x*(x*6.0-15.0)+10.0);}
float macro_height(vec3 dir,float spacing_m){float face_res=pc.anchor_up_macro.w;vec3 coord=array_coord(dir,face_res,macro_tex);float macro_texel_m=(PI*0.5*pc.anchor_dir_radius.w)/max(face_res,1.0);float footprint_m=max(spacing_m*MACRO_FILTER_FOOTPRINT_VERTICES,macro_texel_m);float lod=clamp(log2(footprint_m/macro_texel_m),0.0,MACRO_MAX_LOD);int mip0=clamp(int(floor(lod)),0,MACRO_MAX_MIP),mip1=min(mip0+1,MACRO_MAX_MIP);float t=fract(lod),h0=macro_bicubic_mip(coord,mip0);if(mip0==mip1||t<=0.001)return h0;return mix(h0,macro_bicubic_mip(coord,mip1),smootherstep01(t));}

float projected_theta(float arc,float radius){float q=arc/max(radius,1.0);float g=atan(q);float blend=smootherstep01((arc-TANGENT_FAST_MAX_ARC_M)/(PROJECTION_BLEND_END_M-TANGENT_FAST_MAX_ARC_M));return min(mix(g,q,blend),MAX_PROJECTED_THETA);}
vec3 direction_for_anchor_offset(vec2 offset_m){vec3 center=normalize(pc.anchor_dir_radius.xyz),right=normalize(pc.anchor_right_base.xyz),up=normalize(pc.anchor_up_macro.xyz);float radius=max(pc.anchor_dir_radius.w,1.0),arc=length(offset_m);if(arc<1e-5)return center;if(arc<=TANGENT_FAST_MAX_ARC_M)return normalize(center+right*(offset_m.x/radius)+up*(offset_m.y/radius));float theta=projected_theta(arc,radius);vec3 tangent=normalize(right*offset_m.x+up*offset_m.y);return normalize(center*cos(theta)+tangent*sin(theta));}

uint cache_key(ivec2 cell,int level,int generation){uint h=uint(cell.x)*0x8da6b343u;h^=uint(cell.y)*0xd8163841u;h^=uint(level+1)*0xcb1ab31fu;h^=uint(generation)*0x9e3779b9u;h^=h>>16u;h*=0x7feb352du;h^=h>>15u;h*=0x846ca68bu;h^=h>>16u;return(h&0x00ffffffu)+1u;}

void main() {
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
	if (local.x >= pc.rect.z || local.y >= pc.rect.w) return;
	int level = clamp(pc.meta.x, 0, 14), cache_res = pc.meta.y;
	ivec2 world_cell = pc.rect.xy + local;
	float spacing = pc.anchor_right_base.w * exp2(float(level));
	vec2 offset_m = vec2(world_cell) * spacing;
	vec3 dir = direction_for_anchor_offset(offset_m);
	float macro_h = macro_height(dir, spacing);
	float coast_guard = mix(0.38, 1.0, smootherstep01((abs(macro_h) - 45.0) / (320.0 - 45.0)));
	float final_h = macro_h + geomorph(dir, spacing, macro_h) * coast_guard * pc.context_detail.z;

	// P-012: scatter reads this cache as ground truth (sg_rendered_l0_sample in
	// gpu_scatter_common.gdshaderinc) but until now it never carried the near-field
	// microrelief displacement the actual level-0 surface shaders add on top, so
	// scatter placed itself on a systematically different surface than what's
	// rendered -- floating or sinking relative to the visible ground. Only level 0
	// matches the surface shaders' own `level == 0` gate on this detail.
	if (level == 0) {
		vec4 soil = ctx_soil(dir);
		vec4 surface = ctx_surface(dir);
		vec4 geology = ctx_geology(dir);
		vec4 structure = ctx_structure(dir);
		vec4 climate = ctx_climate(dir);
		vec4 hydro = ctx_hydro(dir);
		vec4 landform = landform_weights(macro_h, soil, surface, geology, structure, climate, hydro);
		vec3 detail_position = vec3(offset_m.x, final_h, offset_m.y);
		final_h += mmr_height(detail_position, soil, surface, geology, structure, climate, hydro, landform);
	}

	int mask = cache_res - 1;
	ivec2 physical = ivec2(world_cell.x & mask, world_cell.y & mask);
	float key = float(cache_key(world_cell, level, pc.meta.z));
	imageStore(cache_image, ivec3(physical, level), vec4(final_h, macro_h, key, 1.0));
}
