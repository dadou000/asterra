#[compute]
#version 450

// One-point runtime terrain height query for gameplay ground contact.
// Evaluates the same resident global macro + optimized analytic geomorph field as L0 terrain.
// CPU receives only the resulting float asynchronously; no CPU terrain synthesis.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray macro_tex;
layout(set = 0, binding = 1) uniform sampler2DArray soil_tex;
layout(set = 0, binding = 2) uniform sampler2DArray surface_tex;
layout(set = 0, binding = 3) uniform sampler2DArray geology_tex;
layout(set = 0, binding = 4) uniform sampler2DArray structure_tex;
layout(set = 0, binding = 5) uniform sampler2DArray climate_tex;
layout(set = 0, binding = 6) uniform sampler2DArray hydrology_tex;
layout(set = 0, binding = 7, std430) restrict writeonly buffer HeightResult { vec4 value; } result;
layout(set = 0, binding = 8, std140) uniform QueryParams {
    vec4 direction_radius;
    vec4 texture_seed;
} params;

const float PI = 3.14159265358979323846;

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
    uint h0=gm_hash(p,seed);
    uint h1=(h0^(h0>>13u)^0x68bc21ebu)*0x9e3779b9u;
    uint h2=(h0^(h0<<11u)^0x02e5be93u)*0x85ebca6bu;
    h1^=h1>>16u; h2^=h2>>15u;
    return vec3(float(h0&0x00ffffffu),float(h1&0x00ffffffu),float(h2&0x00ffffffu))/16777215.0;
}
float gm_value(vec3 p, uint seed) {
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p); f = f*f*f*(f*(f*6.0-15.0)+10.0);
    float a = mix(gm_rand(i+ivec3(0,0,0),seed), gm_rand(i+ivec3(1,0,0),seed), f.x);
    float b = mix(gm_rand(i+ivec3(0,1,0),seed), gm_rand(i+ivec3(1,1,0),seed), f.x);
    float c = mix(gm_rand(i+ivec3(0,0,1),seed), gm_rand(i+ivec3(1,0,1),seed), f.x);
    float d = mix(gm_rand(i+ivec3(0,1,1),seed), gm_rand(i+ivec3(1,1,1),seed), f.x);
    return mix(mix(a,b,f.y), mix(c,d,f.y), f.z) * 2.0 - 1.0;
}
float gm_fbm(vec3 p, uint seed) {
    float h=0.0; float a=0.52;
    for(int i=0;i<3;i++){h+=gm_value(p,seed+uint(i*17))*a;p=p*2.01+vec3(17,-11,7);a*=0.48;}
    return h*1.0957;
}
float gm_fbm_warp(vec3 p,uint seed){
    float h=0.0; float a=0.52;
    for(int i=0;i<2;i++){h+=gm_value(p,seed+uint(i*17))*a;p=p*2.01+vec3(17,-11,7);a*=0.48;}
    return h*1.2662;
}
float gm_ridged(vec3 p, uint seed) { return 1.0 - abs(gm_fbm(p,seed)); }
vec3 gm_domain_warp(vec3 p, uint seed, float strength) {
    return p + vec3(gm_fbm_warp(p+vec3(13.1,7.7,-4.3),seed+101u),
        gm_fbm_warp(p+vec3(-5.7,19.3,8.9),seed+211u),
        gm_fbm_warp(p+vec3(9.2,-3.8,23.4),seed+307u))*strength;
}
float gm_cellular_ridge(vec3 p, uint seed) {
    ivec3 base=ivec3(floor(p)); float f1=1e9; float f2=1e9;
    for(int z=-1;z<=1;z++) for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
        ivec3 c=base+ivec3(x,y,z);
        vec3 jitter=gm_rand3(c,seed+3u);
        vec3 delta=p-(vec3(c)+vec3(0.25)+jitter*0.50); float d2=dot(delta,delta);
        if(d2<f1){f2=f1;f1=d2;} else if(d2<f2){f2=d2;}
    }
    return clamp(1.0-(f2-f1)*2.8,0.0,1.0);
}

vec3 surface_face_uv(vec3 dir) {
    vec3 d=normalize(dir), ad=abs(d); int face=0;
    vec3 axis=vec3(1,0,0), right=vec3(0,0,-1), up=vec3(0,1,0);
    if(ad.x>=ad.y && ad.x>=ad.z){
        if(d.x>=0.0){face=0;} else {face=1;axis=vec3(-1,0,0);right=vec3(0,0,1);}
    } else if(ad.y>=ad.z){
        if(d.y>=0.0){face=2;axis=vec3(0,1,0);right=vec3(-1,0,0);up=vec3(0,0,1);}
        else {face=3;axis=vec3(0,-1,0);right=vec3(-1,0,0);up=vec3(0,0,-1);}
    } else {
        if(d.z>=0.0){face=4;axis=vec3(0,0,1);right=vec3(1,0,0);}
        else {face=5;axis=vec3(0,0,-1);right=vec3(-1,0,0);}
    }
    float denom=max(dot(axis,d),1e-8), q=PI*0.25;
    return vec3(atan(dot(right,d)/denom)/q*0.5+0.5,
        atan(dot(up,d)/denom)/q*0.5+0.5,float(face));
}
vec3 array_coord(vec3 dir,float face_res,sampler2DArray tex){
    vec3 fuv=surface_face_uv(dir); ivec3 s=textureSize(tex,0); float tr=float(s.x);
    float gutter=max((tr-face_res)*0.5,0.0);
    return vec3((fuv.xy*face_res+vec2(gutter))/max(tr,1.0),fuv.z);
}
vec4 sample_linear(sampler2DArray tex,vec3 dir,float face_res){
    vec3 c=array_coord(dir,face_res,tex); ivec3 s=textureSize(tex,0);
    vec2 p=c.xy*vec2(s.xy)-vec2(0.5), f=fract(p); ivec2 p0=ivec2(floor(p)), hi=s.xy-ivec2(1);
    ivec2 a=clamp(p0,ivec2(0),hi), b=clamp(p0+ivec2(1,0),ivec2(0),hi);
    ivec2 q=clamp(p0+ivec2(0,1),ivec2(0),hi), e=clamp(p0+ivec2(1,1),ivec2(0),hi); int l=int(c.z+0.5);
    return mix(mix(texelFetch(tex,ivec3(a,l),0),texelFetch(tex,ivec3(b,l),0),f.x),
        mix(texelFetch(tex,ivec3(q,l),0),texelFetch(tex,ivec3(e,l),0),f.x),f.y);
}
vec4 cubic_weights(float t){float t2=t*t,t3=t2*t;return vec4((1.0-3.0*t+3.0*t2-t3)/6.0,(4.0-6.0*t2+3.0*t3)/6.0,(1.0+3.0*t+3.0*t2-3.0*t3)/6.0,t3/6.0);}
float macro_height(vec3 dir){
    vec3 c=array_coord(dir,params.texture_seed.x,macro_tex); ivec3 s=textureSize(macro_tex,0); vec2 sz=vec2(s.xy);
    vec2 p=c.xy*sz-vec2(0.5), base=floor(p), f=fract(p); vec4 wx=cubic_weights(f.x), wy=cubic_weights(f.y);
    float gx0=wx.x+wx.y,gx1=wx.z+wx.w,gy0=wy.x+wy.y,gy1=wy.z+wy.w;
    float x0=base.x-1.0+wx.y/max(gx0,1e-9),x1=base.x+1.0+wx.w/max(gx1,1e-9);
    float y0=base.y-1.0+wy.y/max(gy0,1e-9),y1=base.y+1.0+wy.w/max(gy1,1e-9);
    vec2 uv00=(vec2(x0,y0)+0.5)/sz,uv10=(vec2(x1,y0)+0.5)/sz,uv01=(vec2(x0,y1)+0.5)/sz,uv11=(vec2(x1,y1)+0.5)/sz;
    float a=textureLod(macro_tex,vec3(uv00,c.z),0.0).r,b=textureLod(macro_tex,vec3(uv10,c.z),0.0).r;
    float q=textureLod(macro_tex,vec3(uv01,c.z),0.0).r,e=textureLod(macro_tex,vec3(uv11,c.z),0.0).r;
    return mix(mix(a,b,gx1),mix(q,e,gx1),gy1);
}
vec4 ctx_soil(vec3 d){return sample_linear(soil_tex,d,params.texture_seed.y);} vec4 ctx_surface(vec3 d){return sample_linear(surface_tex,d,params.texture_seed.y);}
vec4 ctx_geology(vec3 d){return sample_linear(geology_tex,d,params.texture_seed.y);} vec4 ctx_structure(vec3 d){return sample_linear(structure_tex,d,params.texture_seed.y);}
vec4 ctx_climate(vec3 d){return sample_linear(climate_tex,d,params.texture_seed.y);} vec4 ctx_hydro(vec3 d){return sample_linear(hydrology_tex,d,params.texture_seed.y);}
float ctx_temp(vec4 c){return mix(-40.0,50.0,c.r);} float ctx_precip(vec4 c){return c.g*3000.0;} float ctx_uplift(vec4 s){return s.r*2.0-1.0;} vec2 ctx_flow(vec4 h){return h.rg*2.0-1.0;}
vec4 landform(float mh,vec4 soil,vec4 surf,vec4 geo,vec4 st,vec4 clim,vec4 hyd){
    float altitude=smoothstep(450.0,2600.0,max(mh,0.0));
    float mountain=clamp(max(clamp(max(ctx_uplift(st),0.0)*0.82+st.g*0.50+geo.a*0.28,0.0,1.0),altitude*0.50),0.0,1.0);
    float arid=clamp((1.0-surf.g)*(1.0-clamp(ctx_precip(clim)/900.0,0.0,1.0))*(0.35+soil.r*0.85)*(1.0-surf.b*0.55),0.0,1.0);
    float cold=1.0-smoothstep(-12.0,4.0,ctx_temp(clim)); float glacial=clamp(cold*(0.35+altitude*0.75),0.0,1.0);
    float dep=clamp(max(hyd.a,st.b*0.85+st.a*0.9)*(0.5+geo.r*0.35),0.0,1.0); return vec4(mountain,arid,glacial,dep);
}
float band_weight(float w,float spacing){float fs=spacing*4.0;float x=clamp((w-fs)/max(fs,1e-5),0.0,1.0);return x*x*(3.0-2.0*x);}
void tangent_basis(vec3 d,out vec3 right,out vec3 north){vec3 bu=abs(d.y)>0.999?vec3(0,0,1):vec3(0,1,0);right=normalize(cross(bu,d));north=normalize(cross(d,right));}
vec3 flow_world(vec3 d,vec2 xy){if(dot(xy,xy)<=1e-5)return vec3(0);vec3 r,n;tangent_basis(d,r,n);return normalize(r*xy.x+n*xy.y);}
float geomorph(vec3 dir,float mh){
    float radius=params.direction_radius.w, spacing=params.texture_seed.w; uint seed=uint(max(params.texture_seed.z,1.0)); vec3 wm=dir*radius;
    vec4 soil=ctx_soil(dir),surf=ctx_surface(dir),geo=ctx_geology(dir),st=ctx_structure(dir),clim=ctx_climate(dir),hyd=ctx_hydro(dir),w=landform(mh,soil,surf,geo,st,clim,hyd);
    float mountain=w.r,arid=w.g,glacial=w.b,dep=w.a,hardness=1.20-geo.r*0.48,h=0.0,bw;
    bw=band_weight(16000.0,spacing); if(bw>0.001){vec3 p=gm_domain_warp(wm/16000.0,seed+11u,0.8);h+=gm_fbm(p,seed+13u)*mix(24.0,125.0,mountain)*bw;}
    bw=band_weight(6000.0,spacing); if(bw>0.001&&mountain>0.08){vec3 p=gm_domain_warp(wm/6000.0,seed+31u,1.1);float cells=gm_cellular_ridge(p,seed+37u),ridge=gm_ridged(p*1.55,seed+41u);h+=(mix(ridge,cells,0.58)*2.0-1.0)*210.0*mountain*hardness*bw;}
    bw=band_weight(1400.0,spacing); if(bw>0.001){vec3 p=gm_domain_warp(wm/1400.0,seed+53u,0.72);h+=((gm_ridged(p*1.25,seed+59u)*2.0-1.0)*72.0*mountain+gm_fbm(p*2.1,seed+61u)*24.0)*bw;}
    bw=band_weight(420.0,spacing); if(bw>0.001){vec3 f=flow_world(dir,ctx_flow(hyd)),sm=wm;if(dot(f,f)>0.1){float q=dot(sm,f);vec3 along=f*q;sm=along*0.42+(sm-along)*1.45;}vec3 p=gm_domain_warp(sm/420.0,seed+71u,0.55);h-=pow(gm_ridged(p,seed+73u),4.6)*mix(2.0,34.0,hyd.b)*(1.0-dep*0.78)*bw;if(dep>0.006)h+=pow(gm_ridged(p*0.48+vec3(9),seed+79u),2.2)*dep*mix(1.0,12.0,surf.a)*bw;}
    bw=band_weight(120.0,spacing); if(bw>0.001){h+=gm_fbm(wm/120.0,seed+89u)*4.5*(1.0-glacial*0.6)*bw;float dw=arid*soil.r;if(dw>0.004){float dune=gm_ridged(gm_domain_warp(wm/180.0,seed+97u,0.45),seed+101u);h+=(dune*2.0-1.0)*9.0*dw*bw;}}
    bw=band_weight(24.0,spacing); if(bw>0.001)h+=gm_value(wm/24.0,seed+109u)*0.9*bw;
    if(glacial>0.01){float ice=gm_fbm(wm/2600.0+vec3(2,-7,5),seed+127u);h=mix(h,h*0.62+ice*52.0,glacial*0.72);} return h;
}

void main(){
    // Contact height mirrors the render: the coarse elevation map only. Authored
    // biome displacement is added on the CPU side by the contact query wrapper.
    vec3 d=normalize(params.direction_radius.xyz);
    result.value=vec4(macro_height(d),0.0,0.0,1.0);
}