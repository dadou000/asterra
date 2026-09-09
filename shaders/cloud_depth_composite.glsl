#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler3D shape_noise;
layout(set = 0, binding = 3) uniform sampler3D detail_noise;
layout(set = 0, binding = 4) uniform sampler2D global_weather;
layout(set = 0, binding = 5) uniform sampler2D local_weather;
layout(set = 0, binding = 6) uniform sampler3D light_volume;

layout(push_constant, std430) uniform Params {
    vec4 camera_planet_radius;
    vec4 sun_dir_intensity;
    vec4 wind_steps;
    // xyz local weather/light-volume centre. abs(w)=span; w<0 means volume warmup.
    vec4 weather_center_span;
    mat4 inv_world_projection;
} params;

const float PI = 3.14159265358979323846;
const float ATMOSPHERE_TOP = 60000.0;
const float CLOUD_TOP = 14500.0;
const float CLOUD_FAIR_BASE = 1100.0;
const float CLOUD_STORM_BASE = 900.0;
const float CLOUD_SHAPE_SCALE = 0.0000520;
const float CLOUD_DETAIL_SCALE = 0.00042;
const float CLOUD_DETAIL_EROSION = 0.65;
const float CLOUD_EXTINCTION = 0.0010;
const float WORLEY_PERSISTENCE = 0.57;
const float BASE_STEP = 125.0;
const float ADAPTIVE_FACTOR = 0.006;
const float MAX_STEP = 2500.0;
const float LIGHT_DISTANCE = 2000.0;
const float ORBIT_FADE_START = 200000.0;
const float ORBIT_FADE_END = 260000.0;
const int MAX_PRIMARY_STEPS = 28;
const int MAX_LIGHT_STEPS = 4;
const int GODRAY_STEPS = 6;
const int AIR_STEPS = 4;

vec2 sphere_hit(vec3 o, vec3 d, float r) {
    float b = dot(d, o);
    float c = dot(o, o) - r * r;
    float q = b * b - c;
    if (q < 0.0) return vec2(1e30, -1e30);
    float s = sqrt(q);
    return vec2(-b - s, -b + s);
}

vec2 global_uv(vec3 d) {
    d = normalize(d);
    float lon = atan(d.z, d.x);
    if (lon < 0.0) lon += 2.0 * PI;
    float lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / (2.0 * PI), (PI * 0.5 - lat) / PI);
}

void local_basis(out vec3 center, out vec3 east, out vec3 north) {
    center = normalize(params.weather_center_span.xyz);
    vec3 pole = abs(center.y) > 0.92 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    east = normalize(cross(pole, center));
    north = normalize(cross(center, east));
}

vec4 weather_state(vec3 d, float radius) {
    d = normalize(d);
    vec4 g = textureLod(global_weather, global_uv(d), 0.0);
    vec3 center, east, north;
    local_basis(center, east, north);
    vec3 delta = d * radius - center * radius;
    float span = max(abs(params.weather_center_span.w), 1000.0);
    vec2 uv = vec2(dot(delta, east), dot(delta, north)) / span + vec2(0.5);
    float edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5));
    float blend = 1.0 - smoothstep(0.42, 0.50, edge);
    vec4 l = textureLod(local_weather, clamp(uv, vec2(0.0), vec2(1.0)), 0.0);
    return mix(g, l, blend);
}

float worley(vec3 uv) { return 1.0 - textureLod(shape_noise, uv, 0.0).r; }
float worley_fbm(vec3 p, float scale, vec3 wind) {
    vec3 uv = (p + wind) * scale;
    float a = worley(uv);
    float b = worley(uv * 2.03 + vec3(0.19, 0.61, 0.43));
    return (a + WORLEY_PERSISTENCE * b) / (1.0 + WORLEY_PERSISTENCE);
}

vec3 domain_warp(vec3 surface_p, vec3 wind) {
    vec3 uv = (surface_p + wind * 0.16) * (CLOUD_SHAPE_SCALE * 0.12);
    vec3 q = vec3(
        textureLod(shape_noise, uv + vec3(0.13,0.47,0.81), 0.0).r,
        textureLod(shape_noise, uv + vec3(0.71,0.23,0.37), 0.0).r,
        textureLod(shape_noise, uv + vec3(0.41,0.89,0.17), 0.0).r);
    return (q * 2.0 - 1.0) * 200.0;
}

float band(float h, float lo, float hi, float bs, float ts) {
    if (h <= lo || h >= hi) return 0.0;
    return smoothstep(lo, lo + bs, h) * (1.0 - smoothstep(hi - ts, hi, h));
}
float coverage_curve(float c, float conv) {
    return mix(smoothstep(0.035,0.72,c), smoothstep(0.020,0.58,c), conv);
}
float shape_from(float n, float c, float hardness) {
    float threshold = 1.0 - clamp(c,0.0,1.0) * 0.76;
    float width = mix(0.23, 0.045, hardness);
    return smoothstep(threshold, min(threshold + width, 0.999), n);
}
float fair_top(float t) {
    if (t < .2) return mix(3450.0,4450.0,t/.2);
    if (t < .4) return mix(4450.0,5450.0,(t-.2)/.2);
    if (t < .6) return mix(5450.0,6450.0,(t-.4)/.2);
    if (t < .8) return mix(6450.0,7450.0,(t-.6)/.2);
    return mix(7450.0,9300.0,(t-.8)/.2);
}

float density_at(vec3 p, float radius, vec3 wind, float detail_weight) {
    float alt = length(p) - radius;
    if (alt <= 0.0 || alt >= CLOUD_TOP) return 0.0;
    vec4 wx = weather_state(normalize(p), radius);
    float coverage = clamp(wx.r,0.0,1.0);
    float storm = clamp(wx.g,0.0,1.0);
    float precip = clamp(wx.b,0.0,1.0);
    float lowp = clamp((0.5 - wx.a) * 3.0,0.0,1.0);
    float type = clamp(storm*1.18 + precip*.10 + max(coverage-.62,0.0)*.18,0.0,1.0);
    vec3 surface_p = normalize(p) * radius;
    vec3 pp = p + domain_warp(surface_p, wind);

    float fair_cov = coverage_curve(coverage,type);
    float fair_macro = worley_fbm(pp,CLOUD_SHAPE_SCALE,wind);
    float fair_body = shape_from(fair_macro,fair_cov,mix(.90,.97,smoothstep(.08,.35,type)));
    fair_body *= band(alt,mix(CLOUD_FAIR_BASE,1000.0,type),fair_top(type),180.0,mix(600.0,1050.0,type));

    float deep = smoothstep(.42,.82,max(storm,precip*.70+lowp*.22));
    float sc = clamp(coverage*.72 + storm*.46 + lowp*.12,0.0,1.0);
    float core = shape_from(worley_fbm(pp,CLOUD_SHAPE_SCALE*.77,wind),sc,.96)
        * band(alt,1150.0,CLOUD_TOP,220.0,1050.0);
    float edge = shape_from(worley_fbm(pp,CLOUD_SHAPE_SCALE*.58,wind),sc*.88,.96)
        * band(alt,1000.0,13200.0,260.0,1500.0);
    float trail = shape_from(worley_fbm(pp,CLOUD_SHAPE_SCALE*.045,wind*.35),sc*.72,.50)
        * exp(-pow((alt-11250.0)/2100.0,2.0)) * smoothstep(.48,.78,deep);
    float storm_body = max(max(core,edge*.72),trail*.48) * deep;

    float body = max(fair_body,storm_body);
    if (detail_weight > .001 && body > .006) {
        float det = textureLod(detail_noise,(p+wind*1.31)*CLOUD_DETAIL_SCALE
            + vec3(.41,.17,.83),0.0).r;
        float boundary = 1.0 - smoothstep(.25,.88,body);
        body = max(body - (1.0-det)*CLOUD_DETAIL_EROSION*detail_weight
            * (.18+.82*boundary)*(1.0-body),0.0);
    }

    float rain_haze = 0.0;
    if (precip > .05 && alt < 2300.0) {
        rain_haze = smoothstep(0.0,180.0,alt)
            * (1.0-smoothstep(1800.0,2300.0,alt)) * precip * .24;
    }
    return (smoothstep(.006,.24,body)*(mix(.50,1.05,type)+deep*.35) + rain_haze);
}

float coarse_density(vec3 p,float radius,vec3 wind) {
    return density_at(p,radius,wind,0.0);
}

float hg(float mu,float g) {
    float gg=g*g;
    return (1.0-gg)/(4.0*PI*pow(max(1.0+gg-2.0*g*mu,1e-4),1.5));
}
float phase_single(float mu) { return hg(mu,.95)*.10 + hg(mu,.80)*.20; }
float phase_multiple(float mu) { return hg(mu,.20)*3.0 + hg(mu,-.40)*.30; }

float planet_sun_visibility(vec3 p,vec3 sun_dir,float radius) {
    vec2 h=sphere_hit(p,sun_dir,radius);
    return h.y > 0.0 && h.x > 0.001 ? 0.0 : 1.0;
}

float sun_transmittance(vec3 p,vec3 sun_dir,float radius,vec3 wind,int steps) {
    steps=clamp(steps,1,MAX_LIGHT_STEPS);
    float dl=LIGHT_DISTANCE/float(steps);
    float od=0.0;
    for(int i=0;i<MAX_LIGHT_STEPS;i++) {
        if(i>=steps) break;
        float f=(float(i)+.55)/float(steps);
        float shaped=mix(f,f*f,.65);
        od += coarse_density(p+sun_dir*(shaped*LIGHT_DISTANCE),radius,wind)*dl;
    }
    return exp(-od*CLOUD_EXTINCTION*.90);
}

bool sample_light_volume(vec3 p,float radius,out vec4 lv) {
    lv=vec4(1.0,0.0,.05,0.0);
    if(params.weather_center_span.w >= 0.0) {
        vec3 center,east,north; local_basis(center,east,north);
        vec3 d=normalize(p);
        vec3 delta=d*radius-center*radius;
        float span=max(abs(params.weather_center_span.w),1000.0);
        vec2 uv=vec2(dot(delta,east),dot(delta,north))/span+vec2(.5);
        float alt=length(p)-radius;
        vec3 uvw=vec3(uv,alt/CLOUD_TOP);
        if(all(greaterThanEqual(uvw,vec3(0.0))) && all(lessThanEqual(uvw,vec3(1.0)))) {
            lv=textureLod(light_volume,uvw,0.0);
            return true;
        }
    }
    return false;
}

vec2 cloud_segment(vec3 o,vec3 d,float radius,float scene_dist) {
    vec2 outer=sphere_hit(o,d,radius+CLOUD_TOP);
    if(outer.y<=0.0) return vec2(1e30,-1e30);
    float start=max(outer.x,0.0), end=min(outer.y,scene_dist);
    vec2 ground=sphere_hit(o,d,radius);
    if(ground.x>0.0) end=min(end,ground.x);
    if(end<=start) return vec2(1e30,-1e30);
    return vec2(start,end);
}

vec4 detailed_clouds(vec3 o,vec3 d,float radius,float scene_dist,vec3 sun_dir,
        float irradiance,vec3 wind,int requested_steps,out float first_t) {
    first_t=-1.0;
    vec2 seg=cloud_segment(o,d,radius,scene_dist);
    if(seg.x>seg.y) return vec4(0.0,0.0,0.0,1.0);
    int budget=clamp(requested_steps,6,MAX_PRIMARY_STEPS);
    float t=seg.x + BASE_STEP*(.35+.55*fract(sin(dot(d,vec3(91.17,37.53,141.73)))*43758.5));
    float trans=1.0;
    vec3 radiance=vec3(0.0);
    float mu=dot(d,sun_dir);
    float ps=phase_single(mu), pm=phase_multiple(mu);
    int used=0;
    while(t<seg.y && used<budget && trans>.012) {
        float step_len=min(BASE_STEP*(1.0+t*ADAPTIVE_FACTOR),MAX_STEP);
        step_len=min(step_len,seg.y-t);
        vec3 p=o+d*(t+.5*step_len);
        float detail=1.0-smoothstep(42000.0,115000.0,t);
        float den=density_at(p,radius,wind,detail);
        if(den>.006) {
            if(first_t<0.0) first_t=t;
            float a=1.0-exp(-den*CLOUD_EXTINCTION*step_len);
            float pv=planet_sun_visibility(p,sun_dir,radius);
            int ls=clamp(2+budget/10,2,MAX_LIGHT_STEPS);
            float lt=sun_transmittance(p,sun_dir,radius,wind,ls)*pv;
            vec4 lv; bool have_lv=sample_light_volume(p,radius,lv);
            if(have_lv) lt=mix(lt,lv.r*pv,.30);
            float powder=1.0-exp(-den*CLOUD_EXTINCTION*step_len*2.2);
            float sun_elev=dot(normalize(p),sun_dir);
            vec3 tint=mix(vec3(1.0,.34,.10),vec3(1.0,.98,.94),smoothstep(-.02,.28,sun_elev));
            vec3 direct=tint*irradiance*ps*lt*mix(.58,1.03,powder);
            float day=smoothstep(-.12,.15,sun_elev)*pv;
            float ms=have_lv ? lv.g : (0.010+0.032*(1.0-lt))*(.55+.45*powder);
            float sky=have_lv ? lv.b : mix(.008,.065,day);
            vec3 multiple=tint*irradiance*pm*ms*day;
            vec3 ambient=vec3(.45,.62,.90)*sky*mix(.18,1.0,day);
            radiance += trans*a*(direct+multiple+ambient);
            trans *= 1.0-a;
        }
        t += step_len;
        used++;
    }
    return vec4(radiance,clamp(trans,0.0,1.0));
}

vec4 orbital_clouds(vec3 o,vec3 d,float radius,float scene_dist,vec3 sun_dir,float irradiance,vec3 wind) {
    float shell=radius+6500.0;
    vec2 h=sphere_hit(o,d,shell);
    float t=h.x>0.0?h.x:h.y;
    if(t<=0.0 || t>=scene_dist) return vec4(0.0,0.0,0.0,1.0);
    vec3 p=o+d*t;
    vec4 wx=weather_state(normalize(p),radius);
    float cov=coverage_curve(clamp(wx.r,0.0,1.0),clamp(wx.g,0.0,1.0));
    float macro=worley_fbm(p,CLOUD_SHAPE_SCALE*.18,wind*.25);
    float body=shape_from(macro,cov,mix(.75,.92,wx.g));
    float storm=clamp(wx.g*.8+wx.b*.3,0.0,1.0);
    body=max(body,shape_from(worley_fbm(p,CLOUD_SHAPE_SCALE*.045,wind*.2),cov*.72,.5)*storm*.75);
    float alpha=clamp(body*(.62+.28*storm),0.0,.94);
    if(alpha<.003) return vec4(0.0,0.0,0.0,1.0);
    float ndl=max(dot(normalize(p),sun_dir),0.0);
    vec3 c=mix(vec3(.08,.10,.14),vec3(.72,.78,.86),sqrt(ndl))*irradiance*.12;
    return vec4(c*alpha,1.0-alpha);
}

vec3 god_rays(vec3 o,vec3 d,float radius,float max_t,vec3 sun_dir,float irradiance) {
    if(params.weather_center_span.w < 0.0) return vec3(0.0);
    float camera_alt=length(o)-radius;
    if(camera_alt>ATMOSPHERE_TOP) return vec3(0.0);
    float forward=pow(max(dot(d,sun_dir),0.0),8.0);
    if(forward<.002) return vec3(0.0);
    vec2 ah=sphere_hit(o,d,radius+ATMOSPHERE_TOP);
    float end=min(max_t,ah.y);
    if(end<=0.0) return vec3(0.0);
    end=min(end,50000.0);
    float dl=end/float(GODRAY_STEPS);
    float accum=0.0;
    for(int i=0;i<GODRAY_STEPS;i++) {
        float t=(float(i)+.5)*dl;
        vec3 p=o+d*t;
        float alt=max(length(p)-radius,0.0);
        float air=exp(-alt/8000.0)*.72+exp(-alt/1200.0)*.28;
        vec4 lv;
        if(sample_light_volume(p,radius,lv)) accum += air*lv.r*dl;
    }
    float strength=(1.0-exp(-accum*2.2e-5))*forward;
    return vec3(1.0,.86,.67)*strength*irradiance*.035;
}

float foreground_air_t(vec3 o,vec3 d,float first_t,float radius) {
    if(first_t<=0.0) return 1.0;
    vec2 h=sphere_hit(o,d,radius+ATMOSPHERE_TOP);
    float start=max(h.x,0.0),end=min(h.y,first_t);
    if(end<=start) return 1.0;
    float dl=(end-start)/float(AIR_STEPS),od=0.0;
    for(int i=0;i<AIR_STEPS;i++) {
        float t=start+(float(i)+.5)*dl;
        float alt=max(length(o+d*t)-radius,0.0);
        od += (.72*exp(-alt/8000.0)+.28*exp(-alt/1200.0))*dl;
    }
    return exp(-od*1.55e-5);
}

void main() {
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
    ivec2 size=imageSize(color_image);
    if(any(greaterThanEqual(pixel,size))) return;
    vec2 uv=(vec2(pixel)+vec2(.5))/vec2(size);
    float depth=textureLod(depth_texture,uv,0.0).r;
    vec3 ndc=vec3(uv*2.0-1.0,depth);
    vec4 wh=params.inv_world_projection*vec4(ndc,1.0);
    vec3 world_offset=wh.xyz/max(abs(wh.w),1e-8)*sign(wh.w);
    float scene_dist=depth<=1e-6?1e30:length(world_offset);
    if(!(scene_dist>0.0)) return;
    vec3 ray=normalize(world_offset);
    vec3 camera=params.camera_planet_radius.xyz;
    float radius=params.camera_planet_radius.w;
    vec3 sun=normalize(params.sun_dir_intensity.xyz);
    float irradiance=params.sun_dir_intensity.w;
    vec3 wind=params.wind_steps.xyz;
    int steps=int(clamp(floor(params.wind_steps.w),6.0,float(MAX_PRIMARY_STEPS)));
    float camera_alt=length(camera)-radius;
    float orbital_w=smoothstep(ORBIT_FADE_START,ORBIT_FADE_END,camera_alt);

    float first_t=-1.0;
    vec4 detailed=vec4(0.0,0.0,0.0,1.0);
    if(orbital_w<.999) detailed=detailed_clouds(camera,ray,radius,max(scene_dist-.5,0.0),sun,irradiance,wind,steps,first_t);
    vec4 orbital=vec4(0.0,0.0,0.0,1.0);
    if(orbital_w>.001) orbital=orbital_clouds(camera,ray,radius,max(scene_dist-.5,0.0),sun,irradiance,wind);
    vec4 cloud=mix(detailed,orbital,orbital_w);

    vec4 base=imageLoad(color_image,pixel);
    float air_t=foreground_air_t(camera,ray,first_t,radius);
    vec3 result=cloud.rgb*air_t+base.rgb*cloud.a;
    float ray_end=first_t>0.0?first_t:min(scene_dist,50000.0);
    result += god_rays(camera,ray,radius,ray_end,sun,irradiance)*(1.0-cloud.a*.45);
    imageStore(color_image,pixel,vec4(result,base.a));
}
