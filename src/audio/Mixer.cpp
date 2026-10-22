#include "audio/Mixer.h"
#include "editor/Lton.h"
#include "editor/Assets.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace throne {
MixerDefinition MixerDefinition::defaults() {
    MixerDefinition m;
    for (const char* name : {"Music", "SFX", "UI", "Ambience"})
        m.buses.push_back({static_cast<std::uint32_t>(m.buses.size()+1),1,name});
    m.nextBusId=m.buses.size()+1;
    return m;
}
void MixerDefinition::validate() const {
    if(nextBusId<2 || nextBusId>std::uint64_t(UINT32_MAX)+1) throw std::runtime_error("Invalid next mixer bus ID");
    std::map<std::uint32_t, const MixerBus*> index;
    for (const auto& bus : buses) {
        if (!bus.id || !index.emplace(bus.id,&bus).second || bus.name.empty() || bus.name.size()>255 ||
            bus.name.find('\0')!=std::string::npos || !std::isfinite(bus.volume) || bus.volume<0 || bus.volume>100)
            throw std::runtime_error("Invalid or duplicate mixer bus");
    }
    if (!index.count(1) || index.at(1)->parent != 0 || index.at(1)->name != "Master")
        throw std::runtime_error("Mixer requires Master (id 1, parent 0)");
    for (const auto& bus : buses) {
        std::set<std::uint32_t> seen;
        auto id = bus.id;
        while (id != 1) {
            if (!seen.insert(id).second || !index.count(id)) throw std::runtime_error("Mixer cycle or missing parent");
            id = index.at(id)->parent;
        }
    }
}
void MixerDefinition::save(const std::filesystem::path& path) const {
    validate();
    lton::Writer out;
    const auto root=out.table(), list=out.table();
    out.set(root,"format",out.string("ogrethrone-mixer")); out.set(root,"version",lhat_integer(1));
    out.set(root,"id",lhat_integer(static_cast<LhatInteger>(id)));
    for (const auto& bus : buses) {
        const auto t=out.table();
        out.set(t,"id",lhat_integer(bus.id)); out.set(t,"parent",lhat_integer(bus.parent));
        out.set(t,"name",out.string(bus.name)); out.set(t,"volume",lhat_real(bus.volume));
        out.set(t,"mute",lhat_bool(bus.mute)); out.push(list,t);
    }
    auto next=nextBusId;
    for(const auto& bus:buses) next=(std::max)(next,std::uint64_t(bus.id)+1);
    out.set(root,"nextBusId",lhat_integer(static_cast<LhatInteger>(next)));
    out.set(root,"buses",list); out.save(root,path);
}
MixerDefinition MixerDefinition::load(const std::filesystem::path& path) {
    lton::Document doc(path); doc.expect("ogrethrone-mixer",1);
    MixerDefinition m; m.buses.clear();
    if(const auto own=lton::field(doc.root(),"id"); !lhat_is_nil(own)) m.id=lton::assetId(own);
    const auto& list=lton::table(lton::field(doc.root(),"buses"));
    for (std::size_t i=0,n=lton::sequence(list);i<n;++i) {
        const auto& t=lton::table(lton::at(list,i));
        const auto id=lton::integer(lton::field(t,"id")), parent=lton::integer(lton::field(t,"parent"));
        if(id<=0 || id>UINT32_MAX || parent<0 || parent>UINT32_MAX) lton::invalid();
        MixerBus b{static_cast<std::uint32_t>(id),static_cast<std::uint32_t>(parent),std::string(lton::string(lton::field(t,"name")))};
        b.volume=static_cast<float>(lton::number(lton::field(t,"volume")));
        const auto mute=lton::field(t,"mute"); if(!lhat_is_bool(mute)) lton::invalid(); b.mute=lhat_as_bool(mute);
        m.nextBusId=(std::max)(m.nextBusId,std::uint64_t(b.id)+1);
        m.buses.push_back(std::move(b));
    }
    if(const auto v=lton::field(doc.root(),"nextBusId"); !lhat_is_nil(v)) {
        const auto next=lton::integer(v);
        if(next<static_cast<LhatInteger>(m.nextBusId) || next>std::uint64_t(UINT32_MAX)+1) lton::invalid();
        m.nextBusId=static_cast<std::uint64_t>(next);
    }
    m.validate(); return m;
}
}
