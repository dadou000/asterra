#include <orbit/celestial_rings/RingSystem.hpp>
#include <cmath>
int main()
{
    using namespace orbit::celestial_rings;
    RingSystem s;
    s.bands.push_back({
        .semanticIdHigh=1,.semanticIdLow=2,
        .innerRadiusMeters=1.8,
        .outerRadiusMeters=3.0,
        .normalOpticalDepth=1.0,
        .singleScatteringAlbedo=0.7,
        .anisotropy=0.3,
        .colorLinear={0.8,0.7,0.6},
        .thicknessMeters=0.01
    });
    const auto fp=RingSystemFingerprint(s);
    if(fp==0U) return 1;
    if(!(RingTransmission(1.0,1.0)<1.0)) return 2;
    if(!(HenyeyGreensteinPhase(1.0,0.5)>HenyeyGreensteinPhase(-1.0,0.5))) return 3;
    const auto mesh=BuildRingMesh(s,1.0,32U);
    if(mesh.vertices.size()!=66U || mesh.indices.size()!=192U) return 4;
    const auto profile=BuildFarRingProfile(s,1.0,32U);
    if(profile.samples.size()!=32U) return 5;
    const double shadow=RingShadowTransmittanceAtSurface(
        s,1.0,{0.9950371902,-0.0995037190,0.0},
        {0.9950371902,0.0995037190,0.0});
    if(!(shadow<1.0)) return 6;
    const double bodyShadow=BodyShadowTransmittanceAtRingPoint(
        s,1.0,{2.5,0,0},{-1,0,0});
    if(bodyShadow!=0.0) return 7;
    return 0;
}
