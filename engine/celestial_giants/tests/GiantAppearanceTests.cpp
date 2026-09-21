#include <orbit/celestial_giants/GiantAppearance.hpp>

#include <cmath>

int main()
{
    using namespace orbit::celestial_giants;

    GiantAppearanceParameters gas{};
    GiantAppearanceConfig config{
        .faceResolution=17U
    };

    const auto a=BuildGiantAppearance(gas,config);
    const auto b=BuildGiantAppearance(gas,config);

    if(a.fingerprint!=b.fingerprint ||
       a.texels.size()!=6U*17U*17U)
        return 1;

    bool varied=false;
    const auto first=a.texels.front().albedoLinear;
    for(const auto& t:a.texels)
    {
        if(std::abs(t.albedoLinear.x-first.x)>1e-4F ||
           std::abs(t.albedoLinear.y-first.y)>1e-4F ||
           std::abs(t.albedoLinear.z-first.z)>1e-4F)
        {
            varied=true;
            break;
        }
    }
    if(!varied) return 2;

    auto ice=gas;
    ice.giantClass=GiantClass::IceGiant;
    ice.baseColorLinear={0.18,0.42,0.58};
    ice.bandColorLinear={0.30,0.60,0.72};

    if(GiantAppearanceFingerprint(ice,config)==a.fingerprint)
        return 3;

    const auto equator=EvaluateGiantColor(gas,{1.0,0.0,0.0});
    const auto pole=EvaluateGiantColor(gas,{0.0,1.0,0.0});

    if(std::abs(equator.x-pole.x)<1e-5 &&
       std::abs(equator.y-pole.y)<1e-5 &&
       std::abs(equator.z-pole.z)<1e-5)
        return 4;

    return 0;
}
