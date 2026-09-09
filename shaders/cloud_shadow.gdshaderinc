/// Weather-driven EVE-style cloud shadows plus accumulated wet-surface response.
uniform sampler3D u_cloud_shadow_shape_noise : repeat_enable, filter_linear;
uniform sampler2D u_cloud_weather_global : filter_linear;
uniform sampler2D u_cloud_weather_local : filter_linear;
uniform float u_cloud_shadow_enabled = 0.0;
uniform float u_cloud_shadow_base = 800.0;
uniform float u_cloud_shadow_top = 14500.0;
uniform float u_cloud_shadow_coverage = 0.28;
uniform float u_cloud_shadow_weather_variation = 0.30;
uniform float u_cloud_shadow_density = 1.0;
uniform float u_cloud_shadow_shape_scale = 0.000052;
uniform float u_cloud_shadow_weather_scale = 0.0000085;
uniform float u_cloud_shadow_extinction = 0.0010;
uniform vec3 u_cloud_shadow_wind_offset = vec3(0.0);
uniform vec3 u_cloud_shadow_sun_dir = vec3(1.0, 0.0, 0.0);
uniform int u_cloud_shadow_steps = 6;
uniform vec3 u_cloud_weather_center = vec3(0.0, 1.0, 0.0);
uniform vec3 u_cloud_weather_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_cloud_weather_north = vec3(0.0, 0.0, 1.0);
uniform float u_cloud_weather_local_span = 422400.0;
uniform float u_weather_surface_wetness : hint_range(0.0, 1.0) = 0.0;
uniform float u_cloud_shadow_helion_radius_m = 696340000.0;
uniform float u_cloud_shadow_helion_distance_m = 149597870700.0;
uniform int u_cloud_shadow_disc_samples = 5;

const int CLOUD_SHADOW_MAX_STEPS = 10;
const int CLOUD_SHADOW_MAX_DISC_SAMPLES = 7;
const float CLOUD_SHADOW_GOLDEN_ANGLE = 2.399963229728653;
const float CLOUD_SHADOW_WORLEY_PERSISTENCE = 0.57;

vec2 cloud_shadow_sphere_hit(vec3 o, vec3 d, float r) {
    float b=dot(d,o), c=dot(o,o)-r*r, q=b*b-c;
    if(q<0.0) return vec2(1e9,-1e9);
    float s=sqrt(q); return vec2(-b-s,-b+s);
}
vec2 cloud_shadow_global_uv(vec3 d) {
    d=normalize(d); float lon=atan(d.z,d.x); if(lon<0.0) lon+=TAU;
    float lat=asin(clamp(d.y,-1.0,1.0)); return vec2(lon/TAU,(PI*.5-lat)/PI);
}
vec4 cloud_shadow_weather(vec3 surface_p,float radius) {
    vec3 d=normalize(surface_p);
    vec4 g=textureLod(u_cloud_weather_global,cloud_shadow_global_uv(d),0.0);
    vec3 center=normalize(u_cloud_weather_center);
    vec3 delta=d*radius-center*radius;
    vec2 uv=vec2(dot(delta,normalize(u_cloud_weather_east)),dot(delta,normalize(u_cloud_weather_north)))
        /max(u_cloud_weather_local_span,1000.0)+vec2(.5);
    float edge=max(abs(uv.x-.5),abs(uv.y-.5));
    float blend=1.0-smoothstep(.42,.50,edge);
    vec4 l=textureLod(u_cloud_weather_local,clamp(uv,vec2(0.0),vec2(1.0)),0.0);
    return mix(g,l,blend);
}
float cloud_shadow_worley(vec3 uv) { return 1.0-textureLod(u_cloud_shadow_shape_noise,uv,0.0).r; }
float cloud_shadow_fbm(vec3 p,float scale) {
    vec3 uv=(p+u_cloud_shadow_wind_offset)*scale;
    float a=cloud_shadow_worley(uv);
    float b=cloud_shadow_worley(uv*2.03+vec3(.19,.61,.43));
    return (a+CLOUD_SHADOW_WORLEY_PERSISTENCE*b)/(1.0+CLOUD_SHADOW_WORLEY_PERSISTENCE);
}
float cloud_shadow_shape(float n,float coverage,float hardness) {
    float t=1.0-clamp(coverage,0.0,1.0)*.76;
    float w=mix(.23,.045,hardness);
    return smoothstep(t,min(t+w,.999),n);
}
float cloud_shadow_band(float h,float lo,float hi,float bs,float ts) {
    if(h<=lo||h>=hi) return 0.0;
    return smoothstep(lo,lo+bs,h)*(1.0-smoothstep(hi-ts,hi,h));
}
float cloud_shadow_fair_top(float t) {
    if(t<.2) return mix(3450.0,4450.0,t/.2);
    if(t<.4) return mix(4450.0,5450.0,(t-.2)/.2);
    if(t<.6) return mix(5450.0,6450.0,(t-.4)/.2);
    if(t<.8) return mix(6450.0,7450.0,(t-.6)/.2);
    return mix(7450.0,9300.0,(t-.8)/.2);
}
float cloud_shadow_density_at(vec3 p,float radius) {
    float alt=length(p)-radius;
    if(alt<=u_cloud_shadow_base||alt>=u_cloud_shadow_top) return 0.0;
    vec4 wx=cloud_shadow_weather(normalize(p)*radius,radius);
    float coverage=clamp(wx.r,0.0,1.0), storm=clamp(wx.g,0.0,1.0), precip=clamp(wx.b,0.0,1.0);
    float lowp=clamp((.5-wx.a)*3.0,0.0,1.0);
    float type=clamp(storm*1.18+precip*.10+max(coverage-.62,0.0)*.18,0.0,1.0);
    float fair_cov=mix(smoothstep(.035,.72,coverage),smoothstep(.020,.58,coverage),type);
    float fair=cloud_shadow_shape(cloud_shadow_fbm(p,u_cloud_shadow_shape_scale),fair_cov,
        mix(.90,.97,smoothstep(.08,.35,type)));
    fair*=cloud_shadow_band(alt,mix(1100.0,1000.0,type),cloud_shadow_fair_top(type),180.0,mix(600.0,1050.0,type));
    fair=smoothstep(.006,.24,fair)*mix(.50,1.05,type);
    float deep=smoothstep(.42,.82,max(storm,precip*.70+lowp*.22));
    float sc=clamp(coverage*.72+storm*.46+lowp*.12,0.0,1.0);
    float core=cloud_shadow_shape(cloud_shadow_fbm(p,u_cloud_shadow_shape_scale*.77),sc,.96)
        *cloud_shadow_band(alt,1150.0,u_cloud_shadow_top,220.0,1050.0);
    float edge=cloud_shadow_shape(cloud_shadow_fbm(p,u_cloud_shadow_shape_scale*.58),sc*.88,.96)
        *cloud_shadow_band(alt,1000.0,13200.0,260.0,1500.0);
    float trail=cloud_shadow_shape(cloud_shadow_fbm(p,u_cloud_shadow_shape_scale*.045),sc*.72,.50)
        *exp(-pow((alt-11250.0)/2100.0,2.0))*smoothstep(.48,.78,deep);
    float conv=smoothstep(.005,.22,max(max(core,edge*.72),trail*.48))*deep*(.85+.45*storm);
    return (fair+conv)*u_cloud_shadow_density;
}
vec2 cloud_shadow_layer_segment(vec3 o,vec3 d,float radius) {
    vec2 outer=cloud_shadow_sphere_hit(o,d,radius+u_cloud_shadow_top);
    if(outer.y<=0.0) return vec2(1e9,-1e9);
    float start=max(outer.x,0.0),end=outer.y;
    vec2 ground=cloud_shadow_sphere_hit(o,d,radius);
    if(ground.x>0.0) end=min(end,ground.x);
    if(end<=start) return vec2(1e9,-1e9); return vec2(start,end);
}
float cloud_shadow_ray_transmittance(vec3 receiver,vec3 ray_dir,float radius) {
    vec2 seg=cloud_shadow_layer_segment(receiver,ray_dir,radius);
    if(seg.x>seg.y) return 1.0;
    int steps=clamp(u_cloud_shadow_steps,2,CLOUD_SHADOW_MAX_STEPS);
    float dl=(seg.y-seg.x)/float(steps),od=0.0;
    for(int i=0;i<CLOUD_SHADOW_MAX_STEPS;i++) {
        if(i>=steps) break;
        od+=cloud_shadow_density_at(receiver+ray_dir*(seg.x+(float(i)+.5)*dl),radius)*dl;
        if(od*u_cloud_shadow_extinction>9.0) return .00012;
    }
    return exp(-od*u_cloud_shadow_extinction);
}
float cloud_shadow_wet_response(float transmittance) {
    // Accumulated precipitation darkens the diffuse/direct terrain response. The
    // screen-space droplet pass supplies the near-camera specular/refraction cue.
    return transmittance*mix(1.0,.86,clamp(u_weather_surface_wetness,0.0,1.0));
}
float cloud_shadow_transmittance(vec3 receiver,vec3 sun_dir,float radius) {
    if(u_cloud_shadow_enabled<.5) return cloud_shadow_wet_response(1.0);
    vec3 s=normalize(sun_dir);
    int count=clamp(u_cloud_shadow_disc_samples,1,CLOUD_SHADOW_MAX_DISC_SAMPLES);
    if(count<=1) return cloud_shadow_wet_response(cloud_shadow_ray_transmittance(receiver,s,radius));
    float ratio=clamp(u_cloud_shadow_helion_radius_m/max(u_cloud_shadow_helion_distance_m,1.0),0.0,.999999);
    float slope=tan(asin(ratio));
    if(slope<1e-7) return cloud_shadow_wet_response(cloud_shadow_ray_transmittance(receiver,s,radius));
    vec3 axis=abs(s.z)<.999?vec3(0,0,1):vec3(0,1,0);
    vec3 tangent=normalize(cross(axis,s)),bitangent=normalize(cross(s,tangent));
    float sum=0.0;
    for(int j=0;j<CLOUD_SHADOW_MAX_DISC_SAMPLES;j++) {
        if(j>=count) break;
        float k=float(j)+.5, radial=sqrt(k/float(count)), phi=k*CLOUD_SHADOW_GOLDEN_ANGLE;
        vec2 q=vec2(cos(phi),sin(phi))*radial;
        vec3 rd=normalize(s+(tangent*q.x+bitangent*q.y)*slope);
        sum+=cloud_shadow_ray_transmittance(receiver,rd,radius);
    }
    return cloud_shadow_wet_response(sum/float(count));
}
