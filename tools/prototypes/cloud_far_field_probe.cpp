// Measurement probe (not built by CMake): compares the shipping far-field cloud model with the area-averaged
// near-field model. Ports Worley/BakeCloudNoise and the base-shape threshold from
// engine/celestial_clouds/src/CloudRenderer.cpp. Build: g++ -O2 -std=c++20 cloud_far_field_probe.cpp
// Results are recorded in /rendering/unified-transport/design-review. Re-run after changing the bake.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
using f32=float; using u32=uint32_t; using i32=int32_t;
constexpr u32 kNoiseGrid=32;
u32 HashCell(u32 x,u32 y,u32 z,const u32 salt){u32 h=x*0x8DA6B343U^y*0xD8163841U^z*0xCB1AB31FU^salt*0x165667B1U;h^=h>>15U;h*=0x2C1B3C6DU;h^=h>>12U;h*=0x297A2D39U;h^=h>>15U;return h;}
f32 Worley(u32 x,u32 y,u32 z,u32 cells,u32 salt){
 const f32 scale=(f32)cells/(f32)kNoiseGrid;const f32 px=((f32)x+0.5f)*scale,py=((f32)y+0.5f)*scale,pz=((f32)z+0.5f)*scale;
 const i32 cx=(i32)std::floor(px),cy=(i32)std::floor(py),cz=(i32)std::floor(pz);const i32 n=(i32)cells;f32 best=4.f;
 for(i32 dz=-1;dz<=1;++dz)for(i32 dy=-1;dy<=1;++dy)for(i32 dx=-1;dx<=1;++dx){
  const i32 gx=cx+dx,gy=cy+dy,gz=cz+dz;
  const u32 h=HashCell((u32)(((gx%n)+n)%n),(u32)(((gy%n)+n)%n),(u32)(((gz%n)+n)%n),salt);
  const f32 fx=(f32)gx+(f32)(h&0xFFU)/255.f,fy=(f32)gy+(f32)((h>>8)&0xFFU)/255.f,fz=(f32)gz+(f32)((h>>16)&0xFFU)/255.f;
  const f32 ex=fx-px,ey=fy-py,ez=fz-pz;best=std::min(best,ex*ex+ey*ey+ez*ez);}
 return std::sqrt(best);}
std::vector<f32> noise; // R channel only
f32 R(int x,int y,int z){x&=31;y&=31;z&=31;return noise[(z*32+y)*32+x];}
f32 sstep(f32 t){return t*t*(3-2*t);}
f32 SampleR(f32 px,f32 py,f32 pz){f32 fx=std::floor(px),fy=std::floor(py),fz=std::floor(pz);f32 tx=sstep(px-fx),ty=sstep(py-fy),tz=sstep(pz-fz);int x=(int)fx,y=(int)fy,z=(int)fz;
 auto L=[](f32 a,f32 b,f32 t){return a+(b-a)*t;};
 return L(L(L(R(x,y,z),R(x+1,y,z),tx),L(R(x,y+1,z),R(x+1,y+1,z),tx),ty),L(L(R(x,y,z+1),R(x+1,y,z+1),tx),L(R(x,y+1,z+1),R(x+1,y+1,z+1),tx),ty),tz);}
int main(){
 noise.resize(32*32*32);f32 lo=1e9,hi=-1e9;
 for(u32 z=0;z<32;++z)for(u32 y=0;y<32;++y)for(u32 x=0;x<32;++x){
  f32 w3=1-std::min(Worley(x,y,z,3,1),1.f),w6=1-std::min(Worley(x,y,z,6,2),1.f),w12=1-std::min(Worley(x,y,z,12,3),1.f);
  f32 v=0.6f*w3+0.3f*w6+0.1f*w12;noise[(z*32+y)*32+x]=v;lo=std::min(lo,v);hi=std::max(hi,v);}
 // NOTE: shader stretches every channel with its own min/max; R only here.
 for(auto&v:noise)v=(v-lo)/std::max(hi-lo,1e-6f);
 // baseValue distribution as in Extinction() at tiny footprint without billow:
 std::mt19937 rng(1);std::uniform_real_distribution<f32> U(0,32);
 const int S=400000;std::vector<f32> base(S);double mean=0;
 for(int i=0;i<S;++i){f32 a=SampleR(U(rng),U(rng),U(rng)),b=SampleR(U(rng),U(rng),U(rng));
  f32 m=0.6f*a+0.4f*b;f32 v=std::clamp(0.5f+(m-0.5f)*1.6f,0.f,1.f);base[i]=v;mean+=v;}
 mean/=S;std::printf("baseValue mean %.3f\n",mean);
 std::sort(base.begin(),base.end());
 for(double q: {0.1,0.25,0.5,0.75,0.9})std::printf(" q%.2f=%.3f",q,base[(size_t)(q*S)]);std::printf("\n");

 const f32 w=2.6f;
 // Candidate compact statistics for E[exp(-K*s)] (s = shape in [0,1], tau = K*s):
 //  (1) two-moment "clumped" model: occupied fraction f and mean shape over occupied part
 //  (2) Laplace samples at K_j = 0.5,2,8,32 interpolated linearly in log K of -ln T
 const double Ks[4]={0.5,2,8,32};
 double worst1=0,worst2=0,worst3=0; 
 std::printf("\n c    K     truthT   2-moment   laplace4(logK interp)   mean-tau only exp(-K E[s])\n");
 for(double c=0.1;c<=0.91;c+=0.2){
  double f=0,msum=0;for(int i=0;i<S;++i){double s=std::clamp(w*(base[i]-(1-c))/std::max(c,0.05),0.0,1.0);if(s>0){f+=1;msum+=s;}}
  double mocc=f>0?msum/f:0;f/=S;
  double meanS=f*mocc; double Lj[4];
  for(int j=0;j<4;++j){double t=0;for(int i=0;i<S;++i){double s=std::clamp(w*(base[i]-(1-c))/std::max(c,0.05),0.0,1.0);t+=std::exp(-Ks[j]*s);}Lj[j]=t/S;}
  for(double K: {0.3,1.0,3.0,10.0,25.0}){
   double t=0;for(int i=0;i<S;++i){double s=std::clamp(w*(base[i]-(1-c))/std::max(c,0.05),0.0,1.0);t+=std::exp(-K*s);}t/=S;
   double m2=(1-f)+f*std::exp(-K*mocc);
   // laplace interp in log K of -ln L
   double lk=std::log(K),r=0;
   int a=0;while(a<2&&lk>std::log(Ks[a+1]))++a;
   double ta=std::log(Ks[a]),tb=std::log(Ks[a+1]);double u=std::clamp((lk-ta)/(tb-ta),-0.5,1.5);
   double ya=-std::log(std::max(Lj[a],1e-9)),yb=-std::log(std::max(Lj[a+1],1e-9));
   // interpolate in log(-lnT) space for smoothness (guard zero)
   double la=std::log(std::max(ya,1e-9)),lb=std::log(std::max(yb,1e-9));double e=la+(lb-la)*u;double T3=std::exp(-std::exp(e));
   double m1=std::exp(-K*meanS);
   worst1=std::max(worst1,std::abs(m2-t));worst2=std::max(worst2,std::abs(T3-t));worst3=std::max(worst3,std::abs(m1-t));
   std::printf(" %.1f %5.1f  %.4f   %.4f     %.4f                 %.4f\n",c,K,t,m2,T3,m1);}
 }
 std::printf("\nmax |T error|: 2-moment %.4f  laplace4 %.4f  mean-tau %.4f\n",worst1,worst2,worst3);
}
