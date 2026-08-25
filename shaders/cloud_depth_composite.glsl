#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler3D shape_noise;
layout(set = 0, binding = 3) uniform sampler3D detail_noise;
layout(set = 0, binding = 4) uniform sampler2D global_weather;
layout(set = 0, binding = 5) uniform sampler2D local_weather;

layout(push_constant, std430) uniform Params {
	vec4 camera_planet_radius;
	vec4 camera_rotation;
	vec4 sun_dir_intensity;
	vec4 wind_steps;
	vec4 weather_center_span;
	vec4 weather_east;
	vec4 weather_north;
	mat4 inv_projection;
} params;

const float PI = 3.14159265358979323846;
const float CLOUD_BASE = 700.0;
const float CLOUD_TOP = 14500.0;
const float CLOUD_DENSITY = 1.15;
const float CLOUD_SHAPE_SCALE = 0.000055;
const float CLOUD_DETAIL_SCALE = 0.00042;
const float CLOUD_DETAIL_STRENGTH = 0.38;
const float CLOUD_EXTINCTION = 0.0010;
const int CLOUD_MAX_PRIMARY_STEPS = 28;
const int CLOUD_MAX_LIGHT_STEPS = 4;

vec3 quat_rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
vec2 sphere_intersect(vec3 o, vec3 d, float r) { float b=dot(d,o), c=dot(o,o)-r*r, disc=b*b-c; if(disc<0.0)return vec2(1e30,-1e30);float s=sqrt(disc);return vec2(-b-s,-b+s); }
float remap01(float v,float a,float b){return clamp((v-a)/max(b-a,1e-5),0.0,1.0);}

vec2 global_weather_uv(vec3 d) {
	d = normalize(d);
	float lon = atan(d.z, d.x);
	if (lon < 0.0) lon += 2.0 * PI;
	float lat = asin(clamp(d.y, -1.0, 1.0));
	return vec2(lon / (2.0 * PI), (PI * 0.5 - lat) / PI);
}

vec4 weather_state(vec3 surface_p, float radius) {
	vec3 d = normalize(surface_p);
	vec4 g = textureLod(global_weather, global_weather_uv(d), 0.0);
	vec3 center = normalize(params.weather_center_span.xyz);
	vec3 tangent_delta = d * radius - center * radius;
	float span = max(params.weather_center_span.w, 1000.0);
	vec2 luv = vec2(dot(tangent_delta, normalize(params.weather_east.xyz)),
		dot(tangent_delta, normalize(params.weather_north.xyz))) / span + vec2(0.5);
	float edge = max(abs(luv.x - 0.5), abs(luv.y - 0.5));
	float local_weight = 1.0 - smoothstep(0.42, 0.50, edge);
	vec4 l = textureLod(local_weather, clamp(luv, vec2(0.0), vec2(1.0)), 0.0);
	return mix(g, l, local_weight);
}

float vertical_profile(float h, float storm) {
	float base = smoothstep(0.0, mix(0.055, 0.025, storm), h);
	float top_start = mix(0.30, 0.84, storm);
	float top = 1.0 - smoothstep(top_start, 1.0, h);
	float anvil = smoothstep(0.58, 0.78, storm) * exp(-pow((h - 0.84) / 0.10, 2.0));
	return max(base * top, anvil * 0.72);
}

float density_coarse(vec3 p,float radius,vec3 wind){
	float alt=length(p)-radius;if(alt<=CLOUD_BASE||alt>=CLOUD_TOP)return 0.0;
	float h=remap01(alt,CLOUD_BASE,CLOUD_TOP);vec4 wx=weather_state(normalize(p)*radius,radius);
	float coverage=clamp(wx.r+(0.5-wx.a)*0.20+wx.b*0.12,0.01,0.99);
	float threshold=1.0-coverage*0.82;
	float macro=textureLod(shape_noise,(p+wind)*CLOUD_SHAPE_SCALE,0.0).r;
	float body=smoothstep(threshold,min(threshold+0.18,0.995),macro);
	return body*vertical_profile(h,wx.b)*CLOUD_DENSITY*(0.78+0.42*wx.b+0.20*wx.z);
}

float density_at(vec3 p,float radius,vec3 wind,float detail_weight){
	float alt=length(p)-radius;if(alt<=CLOUD_BASE||alt>=CLOUD_TOP)return 0.0;
	float h=remap01(alt,CLOUD_BASE,CLOUD_TOP);vec4 wx=weather_state(normalize(p)*radius,radius);
	float coverage=clamp(wx.r+(0.5-wx.a)*0.20+wx.b*0.14,0.01,0.99);
	float threshold=1.0-coverage*0.82;
	vec3 sp=(p+wind)*CLOUD_SHAPE_SCALE;
	float n0=textureLod(shape_noise,sp,0.0).r;
	float n1=textureLod(shape_noise,sp*2.03+vec3(0.19,0.61,0.43),0.0).r;
	float macro=mix(n0,n1,0.24);
	// Mesoscale storm cells get a mild convergent column bias; synoptic placement
	// still comes entirely from the simulated weather texture.
	float column=textureLod(shape_noise,normalize(p)*0.73+vec3(0.37,0.11,0.71),0.0).r;
	macro += (column-0.5)*wx.b*0.22;
	float body=smoothstep(threshold,min(threshold+0.18,0.995),macro)*vertical_profile(h,wx.b);
	if(detail_weight>0.001&&body>0.012){vec3 dp=(p+wind*1.31)*CLOUD_DETAIL_SCALE;float detail=textureLod(detail_noise,dp+vec3(0.41,0.17,0.83),0.0).r;float edge=1.0-smoothstep(0.28,0.86,body);body=max(body-(1.0-detail)*CLOUD_DETAIL_STRENGTH*detail_weight*(0.20+0.80*edge),0.0);}
	return smoothstep(0.012,0.28,body)*CLOUD_DENSITY*(0.78+0.46*wx.b+0.22*wx.z);
}

float hg(float mu,float g){float gg=g*g;return(1.0-gg)/(4.0*PI*pow(max(1.0+gg-2.0*g*mu,1e-4),1.5));}
float cloud_phase(float mu){return hg(mu,0.65)*0.86+hg(mu,-0.24)*0.14;}
float planet_horizon_cosine(float sr,float pr){float r=max(sr,pr+0.01),ratio=clamp(pr/r,0.0,1.0);return-sqrt(max(1.0-ratio*ratio,0.0));}
float planet_sun_visibility(vec3 p,vec3 sun_dir,float pr){vec3 s=normalize(sun_dir);float r=length(p);if(r<1.0)return 0.0;vec3 up=p/r;vec3 ro=p;if(r<=pr+0.5){ro=up*(pr+0.5);r=pr+0.5;}float b=dot(ro,s),c=dot(ro,ro)-pr*pr,disc=b*b-c;if(disc>0.0){float tn=-b-sqrt(disc);if(tn>1.0)return 0.0;}float h=planet_horizon_cosine(r,pr),mu=dot(up,s);return smoothstep(h-0.0062,h+0.0062,mu);}

float sun_transmittance(vec3 p,vec3 sd,float radius,vec3 wind,int ls){vec2 hit=sphere_intersect(p,sd,radius+CLOUD_TOP);float md=min(max(hit.y,0.0),90000.0);if(md<=1.0)return 1.0;float sl=md/float(ls),od=0.0;for(int j=0;j<CLOUD_MAX_LIGHT_STEPS;j++){if(j>=ls)break;float f=(float(j)+0.55)/float(ls),sh=f*f*0.65+f*0.35;od+=density_coarse(p+sd*(sh*md),radius,wind)*sl;if(od*CLOUD_EXTINCTION>9.0)break;}return exp(-od*CLOUD_EXTINCTION*0.82);}

vec2 cloud_segment(vec3 o,vec3 d,float r,float scene_d){float ir=r+CLOUD_BASE,orr=r+CLOUD_TOP;vec2 oh=sphere_intersect(o,d,orr);if(oh.y<=0.0)return vec2(1e30,-1e30);float cr=length(o),rs=max(oh.x,0.0),re=min(oh.y,scene_d);vec2 ih=sphere_intersect(o,d,ir);if(cr<ir)rs=max(rs,ih.y);else if(cr<orr){rs=0.0;if(ih.x>0.0&&ih.x<re)re=ih.x;}else if(ih.x>rs&&ih.x<re)re=ih.x;vec2 gh=sphere_intersect(o,d,r);if(gh.x>0.0)re=min(re,gh.x);if(re<=rs)return vec2(1e30,-1e30);return vec2(rs,re);}

vec4 raymarch_clouds(vec3 o,vec3 d,float radius,float scene_d,vec3 sd,float irr,vec3 wind,int req,out float first_t){first_t=-1.0;vec2 seg=cloud_segment(o,d,radius,scene_d);if(seg.x>seg.y)return vec4(0,0,0,1);int steps=clamp(req,6,CLOUD_MAX_PRIMARY_STEPS),ls=clamp(2+steps/10,2,CLOUD_MAX_LIGHT_STEPS);float sl=(seg.y-seg.x)/float(steps),t=seg.x+sl*0.5,tr=1.0;vec3 rad=vec3(0);float phase=cloud_phase(dot(d,sd));for(int i=0;i<CLOUD_MAX_PRIMARY_STEPS;i++){if(i>=steps||tr<0.012||t>=seg.y)break;vec3 p=o+d*t;float dw=1.0-smoothstep(28000.0,90000.0,t);float den=density_at(p,radius,wind,dw);if(den>0.008){if(first_t<0.0)first_t=t;float a=1.0-exp(-den*CLOUD_EXTINCTION*sl);vec3 up=normalize(p);float sm=dot(up,sd),pv=planet_sun_visibility(p,sd,radius);float sa=mix(0.04,1.0,smoothstep(-0.08,0.32,sm));vec3 tint=mix(vec3(1,.34,.10),vec3(1,.98,.94),smoothstep(-.02,.28,sm));float lt=pv>0.001?sun_transmittance(p,sd,radius,wind,ls):0.0;float powder=1.0-exp(-den*CLOUD_EXTINCTION*sl*2.2);vec3 direct=tint*irr*phase*lt*pv*sa*mix(.58,1.03,powder);float day=smoothstep(-.08,.20,sm)*pv;float tw=smoothstep(-.24,-.02,sm)*(1.0-smoothstep(-.02,.14,sm));vec3 amb=vec3(.00018,.00030,.00065)+vec3(.010,.014,.026)*tw+vec3(.050,.071,.102)*day;vec3 multi=tint*irr*(.010+.030*(1.0-lt))*day;rad+=tr*a*(direct+amb+multi);tr*=1.0-a;}t+=sl;}return vec4(rad,clamp(tr,0.0,1.0));}

float foreground_air_transmittance(vec3 cp,vec3 d,float ft){if(ft<=0.0)return 1.0;float elev=clamp(dot(d,normalize(cp)),-.15,1.0);float ps=mix(26000.0,110000.0,smoothstep(-.05,.45,elev));return exp(-ft/max(ps,1.0)*.55);}
vec3 foreground_atmosphere_restore(vec3 cp,vec3 d,float ft,float ct,float at,vec3 sd,float pr){if(ft<=0.0||ct>.999)return vec3(0);vec3 mp=cp+d*(ft*.5),up=normalize(mp);float sm=dot(up,sd),sv=planet_sun_visibility(mp,sd,pr),day=smoothstep(-.08,.20,sm)*sv,tw=smoothstep(-.24,-.02,sm)*(1.0-smoothstep(-.02,.14,sm));float e=clamp(dot(d,normalize(cp)),-.15,1.0);vec3 dh=mix(vec3(.30,.34,.38),vec3(.19,.36,.52),smoothstep(-.08,.25,e));vec3 hc=vec3(.00012,.00020,.00045)+vec3(.12,.045,.018)*tw+dh*day;return hc*(1.0-at)*(1.0-ct)*.55;}

void main(){ivec2 px=ivec2(gl_GlobalInvocationID.xy),sz=imageSize(color_image);if(px.x>=sz.x||px.y>=sz.y)return;vec2 uv=(vec2(px)+.5)/vec2(sz);float depth=textureLod(depth_texture,uv,0).r;vec3 ndc=vec3(uv*2.0-1.0,depth);vec4 vh=params.inv_projection*vec4(ndc,1);vec3 vp=vh.xyz/max(abs(vh.w),1e-8)*sign(vh.w);float scene_d=depth<=1e-6?1e30:length(vp);if(!(scene_d>0.0))return;vec3 rw=normalize(quat_rotate(params.camera_rotation,normalize(vp)));vec3 cp=params.camera_planet_radius.xyz;float pr=params.camera_planet_radius.w;vec3 sd=normalize(params.sun_dir_intensity.xyz);float irr=params.sun_dir_intensity.w;vec3 wind=params.wind_steps.xyz;int steps=int(clamp(floor(params.wind_steps.w+.5),6.0,float(CLOUD_MAX_PRIMARY_STEPS)));float ft;vec4 cloud=raymarch_clouds(cp,rw,pr,max(scene_d-.5,0.0),sd,irr,wind,steps,ft);if(cloud.a>.9999)return;vec4 base=imageLoad(color_image,px);float air=foreground_air_transmittance(cp,rw,ft);vec3 result=cloud.rgb*air+base.rgb*cloud.a+foreground_atmosphere_restore(cp,rw,ft,cloud.a,air,sd,pr);imageStore(color_image,px,vec4(result,base.a));}
