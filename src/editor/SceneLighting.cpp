#include "editor/SceneLighting.h"
#include <utility>

namespace throne {
SceneLighting readLighting(const LhatTable& t) {
    SceneLighting r;
    for(const auto& v:{std::pair{"defaultSun",&r.defaultSun},std::pair{"sky",&r.sky},std::pair{"toneMapping",&r.toneMapping}})
        if(const auto f=lton::field(t,v.first); !lhat_is_nil(f)) {
            if(!lhat_is_bool(f)) lton::invalid();
            *v.second=lhat_as_bool(f);
        }
    for(const auto& v:{std::pair{"ambientUpper",&r.ambientUpper},std::pair{"ambientLower",&r.ambientLower},
        std::pair{"ambientDirection",&r.ambientDirection}})
        if(const auto f=lton::field(t,v.first); !lhat_is_nil(f)) lton::vector(f,v.second->data());
    for(const auto& v:{std::pair{"environmentScale",&r.environmentScale},std::pair{"environmentTemperature",&r.environmentTemperature},
        std::pair{"exposure",&r.exposure}})
        if(const auto f=lton::field(t,v.first); !lhat_is_nil(f)) *v.second=static_cast<float>(lton::number(f));
    if(const auto f=lton::field(t,"environment"); !lhat_is_nil(f)) r.environment=lton::relativePath(f);
    if(const auto f=lton::field(t,"gi"); !lhat_is_nil(f)) {
        const auto& gi=lton::table(f);
        r.gi.enabled=true;
        lton::vector(lton::field(gi,"lower"),r.gi.lower.data());
        lton::vector(lton::field(gi,"upper"),r.gi.upper.data());
        float reflections[3];
        lton::vector(lton::field(gi,"reflectionProbes"),reflections);
        for(int i=0;i<3;++i) {
            if(reflections[i]<1 || reflections[i]>16 || reflections[i]!=float(unsigned(reflections[i]))) lton::invalid();
            r.gi.reflectionProbes[i]=unsigned(reflections[i]);
        }
        if(const auto b=lton::field(gi,"bounces"); !lhat_is_nil(b)) {
            const auto bounces=lton::integer(b);
            if(bounces<1 || bounces>8) lton::invalid();
            r.gi.bounces=unsigned(bounces);
        }
        // One number (the same spacing along every axis) or one per axis.
        if(const auto s=lton::field(gi,"probeSpacing"); !lhat_is_nil(s)) {
            lton::vectorOrNumber(s,r.gi.probeSpacing.data());
            for(const float v:{r.gi.probeSpacing.x,r.gi.probeSpacing.y,r.gi.probeSpacing.z}) if(!(v>=.5f && v<=50)) lton::invalid();
        }
        if(const auto o=lton::field(gi,"probeOffset"); !lhat_is_nil(o)) lton::vector(o,r.gi.probeOffset.data());
        if(const auto s=lton::field(gi,"minObjectVolume"); !lhat_is_nil(s)) {
            r.gi.minObjectVolume=float(lton::number(s));
            if(!(r.gi.minObjectVolume>=0 && r.gi.minObjectVolume<=1e6f)) lton::invalid();
        }
        for(auto [key,flag]:{std::pair{"fillEmptySpaces",&r.gi.fillEmptySpaces},std::pair{"localProbeVolume",&r.gi.localProbeVolume},
                             std::pair{"fastBakeShadows",&r.gi.fastBakeShadows}})
            if(const auto v=lton::field(gi,key); !lhat_is_nil(v)) {
                if(!lhat_is_bool(v)) lton::invalid();
                *flag=lhat_as_bool(v);
            }
        for(const float d:{r.gi.upper.x-r.gi.lower.x,r.gi.upper.y-r.gi.lower.y,r.gi.upper.z-r.gi.lower.z}) if(!(d>0)) lton::invalid();
    }
    if(r.environmentScale<0 || r.environmentTemperature<0) lton::invalid();
    for(const auto* c:{&r.ambientUpper,&r.ambientLower}) for(float v:{c->x,c->y,c->z}) if(v<0) lton::invalid();
    if(r.ambientDirection.x*r.ambientDirection.x+r.ambientDirection.y*r.ambientDirection.y+r.ambientDirection.z*r.ambientDirection.z<1e-10f)
        lton::invalid();
    return r;
}

LhatValue writeLighting(lton::Writer& out, const SceneLighting& r) {
    const auto settings=out.table();
    for(const auto& v:{std::pair{"defaultSun",r.defaultSun},std::pair{"sky",r.sky},std::pair{"toneMapping",r.toneMapping}})
        out.set(settings,v.first,lhat_bool(v.second));
    for(const auto& v:{std::pair{"ambientUpper",&r.ambientUpper},std::pair{"ambientLower",&r.ambientLower},
        std::pair{"ambientDirection",&r.ambientDirection}}) out.set(settings,v.first,out.vector(v.second->data()));
    for(const auto& v:{std::pair{"environmentScale",r.environmentScale},std::pair{"environmentTemperature",r.environmentTemperature},
        std::pair{"exposure",r.exposure}})
        out.set(settings,v.first,lhat_real(v.second));
    if(!r.environment.empty()) out.set(settings,"environment",out.string(r.environment));
    if(r.gi.enabled) {
        const auto gi=out.table();
        out.set(gi,"lower",out.vector(r.gi.lower.data()));
        out.set(gi,"upper",out.vector(r.gi.upper.data()));
        const float reflections[3]={float(r.gi.reflectionProbes[0]),float(r.gi.reflectionProbes[1]),float(r.gi.reflectionProbes[2])};
        out.set(gi,"reflectionProbes",out.vector(reflections));
        out.set(gi,"bounces",lhat_integer(r.gi.bounces));
        out.set(gi,"probeSpacing",out.vector(r.gi.probeSpacing.data()));
        out.set(gi,"probeOffset",out.vector(r.gi.probeOffset.data()));
        out.set(gi,"minObjectVolume",lhat_real(r.gi.minObjectVolume));
        if(r.gi.fillEmptySpaces) out.set(gi,"fillEmptySpaces",lhat_bool(true));
        if(r.gi.localProbeVolume) out.set(gi,"localProbeVolume",lhat_bool(true));
        if(!r.gi.fastBakeShadows) out.set(gi,"fastBakeShadows",lhat_bool(false));
        out.set(settings,"gi",gi);
    }
    return settings;
}
}
