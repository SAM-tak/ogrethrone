#include "editor/Scene.h"
#include "editor/PrimitiveGeometry.h"
#include "core/Ids.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <set>

namespace throne {
namespace {
constexpr std::string_view prefabFormat = "ogrethrone-prefab", sceneFormat = "ogrethrone-scene";
constexpr LhatInteger prefabVersion = 3, sceneVersion = 2;

/// Reads a name: 1 to 255 bytes without NUL.
std::string name(LhatValue value) {
    std::string result(lton::string(value));
    if (result.empty() || result.size() > 255 || result.find('\0') != std::string::npos) lton::invalid();
    return result;
}

/// Reads a node ID: a positive integer.
NodeId nodeId(LhatValue value) {
    const auto id = lton::integer(value);
    if (id <= 0) lton::invalid();
    return static_cast<NodeId>(id);
}

/// Reads an ID: a positive integer below UINT32_MAX.
std::uint32_t id(LhatValue value) {
    const auto id = lton::integer(value);
    if (id <= 0 || id >= std::numeric_limits<std::uint32_t>::max()) lton::invalid();
    return static_cast<std::uint32_t>(id);
}

/// Whether @p id is @p ancestor or lies below it, looking parents up in @p nodes.
bool within(const std::vector<SceneNode>& nodes, NodeId id, NodeId ancestor) {
    while (id != 0) {
        if (id == ancestor) return true;
        const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const SceneNode& node) { return node.id == id; });
        if (it == nodes.end()) return false;
        id = it->parent;
    }
    return false;
}

/// Writes a path as a sequence of IDs.
LhatValue writePath(lton::Writer& out, const NodePath& path) {
    const auto list = out.table();
    for (const auto step : path) out.push(list, lhat_integer(static_cast<LhatInteger>(step)));
    return list;
}

/// Reads what writePath wrote; nil^ reads as an empty path.
NodePath readPath(LhatValue value) {
    NodePath path;
    if (lhat_is_nil(value)) return path;
    const auto& list = lton::table(value);
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) path.push_back(nodeId(lton::at(list, i)));
    return path;
}

/// Writes one field value: a number, bool, string, or `{ scene = ..., node = ..., inner = {...} }`.
LhatValue writeField(lton::Writer& out, const FieldValue& value) {
    if (const auto* number = std::get_if<double>(&value)) return lhat_real(*number);
    if (const auto* flag = std::get_if<bool>(&value)) return lhat_bool(*flag);
    if (const auto* text = std::get_if<std::string>(&value)) return out.string(*text);
    const auto& ref = std::get<NodeRef>(value);
    const auto table = out.table();
    if (ref.scene) out.set(table, "scene", lhat_integer(ref.scene));
    out.set(table, "node", lhat_integer(static_cast<LhatInteger>(ref.node)));
    if (!ref.inner.empty()) out.set(table, "inner", writePath(out, ref.inner));
    return table;
}

/// Reads what writeField wrote.
FieldValue readField(LhatValue value) {
    if (lhat_is_bool(value)) return lhat_as_bool(value);
    if (lhat_is_integer(value) || lhat_is_real(value)) return lton::number(value);
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) return std::string(lton::string(value));
    const auto& table = lton::table(value);
    NodeRef ref;
    if (const auto scene = lton::field(table, "scene"); !lhat_is_nil(scene)) ref.scene = id(scene);
    const auto node = lton::field(table, "node");
    ref.node = lton::integer(node) == 0 ? 0 : nodeId(node);
    ref.inner = readPath(lton::field(table, "inner"));
    return ref;
}

/// Writes a field map as a table keyed by field name.
LhatValue writeFields(lton::Writer& out, const std::map<std::string, FieldValue>& fields) {
    const auto table = out.table();
    for (const auto& [key, value] : fields) out.set(table, key, writeField(out, value));
    return table;
}

/// Reads what writeFields wrote; nil^ reads as no fields.
std::map<std::string, FieldValue> readFields(LhatValue value) {
    std::map<std::string, FieldValue> fields;
    if (lhat_is_nil(value)) return fields;
    LhatWalkCursor walk{&lton::table(value), 0, 0, LHAT_WALK_PAIR};
    LhatValue key, field;
    while (lhat_table_walk(&walk, &key, &field)) fields[std::string(lton::string(key))] = readField(field);
    return fields;
}

/// Reads an optional finite scalar with bounds.
float scalar(const LhatTable& table, const char* key, float fallback, float low = 0, float high = 1.0e6f) {
    const auto value = lton::field(table, key);
    if (lhat_is_nil(value)) return fallback;
    const auto read = lton::number(value);
    if (read < low || read > high) lton::invalid();
    return static_cast<float>(read);
}
/// Reads an optional boolean.
bool flag(const LhatTable& table, const char* key, bool fallback) {
    const auto value = lton::field(table, key);
    if (lhat_is_nil(value)) return fallback;
    if (!lhat_is_bool(value)) lton::invalid();
    return lhat_as_bool(value);
}
/// Reads a collider's material and offset.
void readCollider(const LhatTable& table, Collider& value) {
    if (const auto v = lton::field(table, "centre"); !lhat_is_nil(v)) lton::vector(v, value.centre.data());
    value.density = scalar(table, "density", value.density, 0.001f);
    value.friction = scalar(table, "friction", value.friction, 0, 1);
    value.restitution = scalar(table, "restitution", value.restitution, 0, 1);
    value.sensor = flag(table, "sensor", value.sensor);
}
/// Reads a material's optional translucency table.
void readTranslucency(const LhatTable& material, Translucency& value) {
    const auto field = lton::field(material, "translucency");
    if (lhat_is_nil(field)) return;
    const auto& table = lton::table(field);
    if (const auto v = lton::field(table, "colour"); !lhat_is_nil(v)) lton::vector(v, value.colour.data());
    for (const float c : {value.colour.x, value.colour.y, value.colour.z}) if (!(c >= 0 && c <= 100)) lton::invalid();
    value.distortion = scalar(table, "distortion", value.distortion, 0, 1);
    value.power = scalar(table, "power", value.power, .1f, 256);
    value.scale = scalar(table, "scale", value.scale, 0, 100);
}
/// Reads the added built-in components, for nodes and their property overrides.
template <typename Node>
void readExtraComponents(const LhatTable& table, Node& node) {
    const auto read = [&](const char* key, auto& component, auto fields) {
        const auto value = lton::field(table, key);
        if (!lhat_is_nil(value)) fields(lton::table(value), component.emplace());
    };
    const auto colour = [](const LhatTable& t, auto& c) {
        if (const auto v = lton::field(t, "colour"); !lhat_is_nil(v)) lton::vector(v, c.colour.data());
        for (float v : c.colour) if (v < 0 || v > 1) lton::invalid();
    };
    read("particles", node.particles, [&](const auto& t, auto& c) {
        if(const auto v=lton::field(t,"definition"); !lhat_is_nil(v)) c.definition=name(v);
        if(const auto v=lton::field(t,"script"); !lhat_is_nil(v)) c.script=lton::relativePath(v);
        c.enabled=flag(t,"enabled",c.enabled);
    });
    read("audioSource", node.audioSource, [&](const auto& t, auto& c) {
        if(const auto v=lton::field(t,"clip"); !lhat_is_nil(v)) c.clip=lton::relativePath(v);
        c.enabled=flag(t,"enabled",c.enabled); c.spatial=flag(t,"spatial",c.spatial);
        c.loop=flag(t,"loop",c.loop); c.autoPlay=flag(t,"autoPlay",c.autoPlay); c.streaming=flag(t,"streaming",c.streaming);
        c.occlusion=flag(t,"occlusion",c.occlusion); c.transmission=flag(t,"transmission",c.transmission);
        c.reflections=flag(t,"reflections",c.reflections); c.reflectionVolume=scalar(t,"reflectionVolume",c.reflectionVolume,0,10);
        c.volume=scalar(t,"volume",c.volume,0,100);
        c.minDistance=scalar(t,"minDistance",c.minDistance); c.maxDistance=scalar(t,"maxDistance",c.maxDistance);
        if(const auto v=lton::field(t,"bus"); !lhat_is_nil(v)) {
            const auto n=lton::integer(v); if(n<=0 || n>UINT32_MAX) lton::invalid();
            c.bus=static_cast<std::uint32_t>(n);
        }
        c.validate();
    });
    read("acousticGeometry", node.acousticGeometry, [&](const auto& t, auto& c) {
        c.enabled=flag(t,"enabled",c.enabled);
        if(const auto v=lton::field(t,"shape"); !lhat_is_nil(v)) {
            const auto name=lton::string(v);
            if(name=="Render") c.shape=AcousticShape::Render;
            else if(name=="Box") c.shape=AcousticShape::Box;
            else lton::invalid();
        }
        for(const auto& [key,out]:{std::pair{"boxSize",&c.boxSize},std::pair{"absorption",&c.absorption},std::pair{"transmission",&c.transmission}})
            if(const auto v=lton::field(t,key); !lhat_is_nil(v)) lton::vector(v,out->data());
        c.scattering=scalar(t,"scattering",c.scattering,0,1); c.validate();
    });
    read("audioListener", node.audioListener, [&](const auto& t, auto& c) {
        c.enabled=flag(t,"enabled",c.enabled);
        if(const auto v=lton::field(t,"priority"); !lhat_is_nil(v)) {
            const auto n=lton::integer(v);
            if(n<std::numeric_limits<int>::min() || n>std::numeric_limits<int>::max()) lton::invalid();
            c.priority=static_cast<int>(n);
        }
    });
    read("animator", node.animator, [&](const auto& t, auto& c) {
        if(const auto v=lton::field(t,"clip"); !lhat_is_nil(v)) c.clip=std::string(lton::string(v));
        c.time=scalar(t,"time",c.time,0,1.0e6f);
        c.speed=scalar(t,"speed",c.speed,-100,100);
        c.loop=flag(t,"loop",c.loop);
        c.playing=flag(t,"playing",c.playing);
    });
    read("camera", node.camera, [&](const auto& t, auto& c) {
        c.fieldOfView=scalar(t,"fieldOfView",c.fieldOfView,1,170);
        c.nearClip=scalar(t,"nearClip",c.nearClip,.001f,1.0e6f);
        c.farClip=scalar(t,"farClip",c.farClip,.001f,1.0e7f);
        if(c.farClip<=c.nearClip) lton::invalid();
        c.focusDistance=scalar(t,"focusDistance",c.focusDistance,.01f,1.0e6f);
        c.fStop=scalar(t,"fStop",c.fStop,0,64);
        if(const auto v=lton::field(t,"antiAliasing"); !lhat_is_nil(v)) {
            const auto mode=lton::string(v);
            if(mode=="TAA") c.antiAliasing=AntiAliasing::Temporal;
            else if(mode=="SMAA") c.antiAliasing=AntiAliasing::Smaa;
            else if(mode=="None") c.antiAliasing=AntiAliasing::None;
            else lton::invalid();
        }
    });
    read("model", node.model, [&](const auto& t, auto& c) {
        if(const auto v=lton::field(t,"path"); !lhat_is_nil(v)) c.path=lton::relativePath(v);
        c.directXNormals=flag(t,"directXNormals",c.directXNormals);
        c.emission=scalar(t,"emission",c.emission,0,100000);
        c.alphaHashing=flag(t,"alphaHashing",c.alphaHashing);
        c.materials.clear();
        if(const auto list=lton::field(t,"materials"); !lhat_is_nil(list)) {
            const auto& items=lton::table(list);
            for(std::size_t i=0,count=lton::sequence(items);i<count;++i) {
                const auto& item=lton::table(lton::at(items,i));
                ModelMaterial m;
                m.name=std::string(lton::string(lton::field(item,"name")));
                if(const auto v=lton::field(item,"absorption"); !lhat_is_nil(v)) lton::vector(v,m.absorption.data());
                m.thickness=scalar(item,"thickness",m.thickness,0.0001f,100);
                for(const float a:{m.absorption.x,m.absorption.y,m.absorption.z}) if(!(a>=0)) lton::invalid();
                readTranslucency(item,m.translucency);
                m.subsurfaceWidth=scalar(item,"subsurfaceWidth",m.subsurfaceWidth,0,10);
                if(const auto v=lton::field(item,"diffuseMap"); !lhat_is_nil(v)) m.diffuseMap=lton::relativePath(v);
                c.materials.push_back(m);
            }
        }
    });
    read("material", node.material, [&](const auto& t, auto& c) {
        c.roughness = scalar(t,"roughness",c.roughness,0.02f,1);
        c.metalness = scalar(t,"metalness",c.metalness,-1,1);
        c.uvScale = scalar(t,"uvScale",c.uvScale,0.001f,10000);
        if (const auto v=lton::field(t,"alpha"); !lhat_is_nil(v)) {
            const auto mode=lton::string(v);
            if(mode=="Opaque") c.alpha=AlphaMode::Opaque;
            else if(mode=="Test") c.alpha=AlphaMode::Test;
            else if(mode=="Hash") c.alpha=AlphaMode::Hash;
            else lton::invalid();
        }
        c.alphaThreshold = scalar(t,"alphaThreshold",c.alphaThreshold,0,1);
        c.opacity = scalar(t,"opacity",c.opacity,0,1);
        readTranslucency(t,c.translucency);
        c.subsurfaceWidth = scalar(t,"subsurfaceWidth",c.subsurfaceWidth,0,10);
        for (const auto& entry : {std::pair{"fresnel",&c.fresnel},std::pair{"emissive",&c.emissive}}) {
            if (const auto v=lton::field(t,entry.first); !lhat_is_nil(v)) lton::vector(v,entry.second->data());
            for (float v:*entry.second) if (v<0 || v>(std::string_view(entry.first)=="fresnel"?1.0f:1.0e6f)) lton::invalid();
        }
        for (const auto& entry : {std::pair{"diffuseMap",&c.diffuseMap},std::pair{"normalMap",&c.normalMap},
            std::pair{"roughnessMap",&c.roughnessMap},std::pair{"specularMap",&c.specularMap},std::pair{"reflectionMap",&c.reflectionMap}})
            if (const auto v=lton::field(t,entry.first); !lhat_is_nil(v)) *entry.second=lton::relativePath(v);
    });
    read("light", node.light, [&](const auto& t, auto& c) {
        if (const auto v=lton::field(t,"type"); !lhat_is_nil(v)) {
            const auto type=lton::string(v);
            if(type=="Directional") c.type=LightType::Directional;
            else if(type=="Point") c.type=LightType::Point;
            else if(type=="Spot") c.type=LightType::Spot;
            else if(type=="AreaApprox") c.type=LightType::AreaApprox;
            else if(type=="AreaLtc") c.type=LightType::AreaLtc;
            else lton::invalid();
        }
        colour(t,c);
        c.power=scalar(t,"power",c.power);
        c.temperature=scalar(t,"temperature",c.temperature,0,40000);
        c.radius=scalar(t,"radius",c.radius,0.001f);
        c.threshold=scalar(t,"threshold",c.threshold,0.00001f,0.999f);
        c.innerAngle=scalar(t,"innerAngle",c.innerAngle,0.01f,179);
        c.outerAngle=scalar(t,"outerAngle",c.outerAngle,c.innerAngle,179);
        c.width=scalar(t,"width",c.width,0.001f); c.height=scalar(t,"height",c.height,0.001f);
        c.enabled=flag(t,"enabled",c.enabled); c.twoSided=flag(t,"twoSided",c.twoSided);
        c.showEmitter=flag(t,"showEmitter",c.showEmitter); c.castShadows=flag(t,"castShadows",c.castShadows);
        if(const auto v=lton::field(t,"direction"); !lhat_is_nil(v)) lton::vector(v,c.direction.data());
        if(c.direction.x*c.direction.x+c.direction.y*c.direction.y+c.direction.z*c.direction.z < 1.0e-10f) lton::invalid();
        if(const auto v=lton::field(t,"mask"); !lhat_is_nil(v)) c.mask=lton::relativePath(v);
    });
    read("sphere", node.sphere, [&](const auto& t, auto& c) {
        colour(t, c); c.radius = scalar(t, "radius", c.radius, 0.001f);
    });
    read("capsule", node.capsule, [&](const auto& t, auto& c) {
        colour(t, c); c.radius = scalar(t, "radius", c.radius, 0.001f);
        c.height = scalar(t, "height", (std::max)(c.height, 2 * c.radius), 2 * c.radius);
    });
    read("plane", node.plane, [&](const auto& t, auto& c) {
        colour(t, c); c.width = scalar(t, "width", c.width, 0.001f); c.depth = scalar(t, "depth", c.depth, 0.001f);
    });
    read("boxCollider", node.boxCollider, [&](const auto& t, auto& c) {
        readCollider(t, c);
        if (const auto v = lton::field(t, "size"); !lhat_is_nil(v)) lton::vector(v, c.size.data());
        for (float v : {c.size.x, c.size.y, c.size.z}) if (v < 0.001f) lton::invalid();
    });
    read("sphereCollider", node.sphereCollider, [&](const auto& t, auto& c) {
        readCollider(t, c); c.radius = scalar(t, "radius", c.radius, 0.001f);
    });
    read("capsuleCollider", node.capsuleCollider, [&](const auto& t, auto& c) {
        readCollider(t, c); c.radius = scalar(t, "radius", c.radius, 0.001f);
        c.height = scalar(t, "height", (std::max)(c.height, 2 * c.radius), 2 * c.radius);
    });
    read("planeCollider", node.planeCollider, [&](const auto& t, auto& c) {
        readCollider(t, c); c.width = scalar(t, "width", c.width, 0.001f); c.depth = scalar(t, "depth", c.depth, 0.001f);
    });
    read("rigidBody", node.rigidBody, [&](const auto& t, auto& c) {
        if (const auto v = lton::field(t, "type"); !lhat_is_nil(v)) {
            const auto type = lton::string(v);
            if (type == "Static") c.type = BodyType::Static;
            else if (type == "Kinematic") c.type = BodyType::Kinematic;
            else if (type == "Dynamic") c.type = BodyType::Dynamic;
            else lton::invalid();
        }
        if (const auto v = lton::field(t, "linearVelocity"); !lhat_is_nil(v)) lton::vector(v, c.linearVelocity.data());
        if (const auto v = lton::field(t, "angularVelocity"); !lhat_is_nil(v)) lton::vector(v, c.angularVelocity.data());
        c.linearDamping = scalar(t, "linearDamping", c.linearDamping);
        c.angularDamping = scalar(t, "angularDamping", c.angularDamping);
        c.gravityScale = scalar(t, "gravityScale", c.gravityScale, -1000, 1000);
        c.bullet = flag(t, "bullet", c.bullet);
    });
}
/// Writes materials and offsets into a collider table.
void writeCollider(lton::Writer& out, LhatValue table, const Collider& c) {
    out.set(table, "centre", out.vector(c.centre.data()));
    out.set(table, "density", lhat_real(c.density));
    out.set(table, "friction", lhat_real(c.friction));
    out.set(table, "restitution", lhat_real(c.restitution));
    out.set(table, "sensor", lhat_bool(c.sensor));
}
/// Writes a material's translucency, when it has any.
void writeTranslucency(lton::Writer& out, LhatValue material, const Translucency& c) {
    if (!c.enabled()) return;
    const auto table = out.table();
    out.set(table, "colour", out.vector(c.colour.data()));
    out.set(table, "distortion", lhat_real(c.distortion));
    out.set(table, "power", lhat_real(c.power));
    out.set(table, "scale", lhat_real(c.scale));
    out.set(material, "translucency", table);
}
/// Writes the added built-in components, shared with prefab overrides.
template <typename Node>
void writeExtraComponents(lton::Writer& out, LhatValue table, const Node& node) {
    const auto write = [&](const char* key, const auto& component, auto fields) {
        if (!component) return;
        const auto value = out.table(); fields(value, *component); out.set(table, key, value);
    };
    const auto colour = [&](LhatValue t, const auto& c) { out.set(t, "colour", out.vector(c.colour.data())); };
    write("particles", node.particles, [&](auto t,const auto& c) {
        out.set(t,"definition",out.string(c.definition)); out.set(t,"enabled",lhat_bool(c.enabled));
        if(!c.script.empty()) out.set(t,"script",out.string(c.script));
    });
    write("audioSource", node.audioSource, [&](auto t,const auto& c) {
        c.validate();
        if(!c.clip.empty()) out.set(t,"clip",out.string(c.clip));
        out.set(t,"enabled",lhat_bool(c.enabled)); out.set(t,"spatial",lhat_bool(c.spatial));
        out.set(t,"loop",lhat_bool(c.loop)); out.set(t,"autoPlay",lhat_bool(c.autoPlay)); out.set(t,"streaming",lhat_bool(c.streaming));
        out.set(t,"occlusion",lhat_bool(c.occlusion)); out.set(t,"transmission",lhat_bool(c.transmission));
        out.set(t,"reflections",lhat_bool(c.reflections)); out.set(t,"reflectionVolume",lhat_real(c.reflectionVolume));
        out.set(t,"volume",lhat_real(c.volume)); out.set(t,"minDistance",lhat_real(c.minDistance));
        out.set(t,"maxDistance",lhat_real(c.maxDistance)); out.set(t,"bus",lhat_integer(c.bus));
    });
    write("acousticGeometry", node.acousticGeometry, [&](auto t,const auto& c) {
        c.validate(); out.set(t,"enabled",lhat_bool(c.enabled));
        out.set(t,"shape",out.string(c.shape==AcousticShape::Render?"Render":"Box"));
        out.set(t,"boxSize",out.vector(c.boxSize.data())); out.set(t,"absorption",out.vector(c.absorption.data()));
        out.set(t,"scattering",lhat_real(c.scattering)); out.set(t,"transmission",out.vector(c.transmission.data()));
    });
    write("audioListener", node.audioListener, [&](auto t,const auto& c) {
        out.set(t,"enabled",lhat_bool(c.enabled)); out.set(t,"priority",lhat_integer(c.priority));
    });
    write("animator", node.animator, [&](auto t,const auto& c) {
        if(!c.clip.empty()) out.set(t,"clip",out.string(c.clip));
        out.set(t,"time",lhat_real(c.time)); out.set(t,"speed",lhat_real(c.speed));
        out.set(t,"loop",lhat_bool(c.loop)); out.set(t,"playing",lhat_bool(c.playing));
    });
    write("camera", node.camera, [&](auto t,const auto& c) {
        out.set(t,"fieldOfView",lhat_real(c.fieldOfView));
        out.set(t,"nearClip",lhat_real(c.nearClip)); out.set(t,"farClip",lhat_real(c.farClip));
        out.set(t,"focusDistance",lhat_real(c.focusDistance)); out.set(t,"fStop",lhat_real(c.fStop));
        const char* modes[]={"TAA","SMAA","None"};
        out.set(t,"antiAliasing",out.string(modes[static_cast<int>(c.antiAliasing)]));
    });
    write("model", node.model, [&](auto t,const auto& c) {
        out.set(t,"path",out.string(c.path)); out.set(t,"directXNormals",lhat_bool(c.directXNormals));
        out.set(t,"emission",lhat_real(c.emission));
        if(c.alphaHashing) out.set(t,"alphaHashing",lhat_bool(true));
        if(!c.materials.empty()) {
            const auto list=out.table();
            for(const auto& m:c.materials) {
                const auto item=out.table();
                out.set(item,"name",out.string(m.name));
                out.set(item,"absorption",out.vector(m.absorption.data()));
                out.set(item,"thickness",lhat_real(m.thickness));
                writeTranslucency(out,item,m.translucency);
                if(m.subsurfaceWidth>0) out.set(item,"subsurfaceWidth",lhat_real(m.subsurfaceWidth));
                if(!m.diffuseMap.empty()) out.set(item,"diffuseMap",out.string(m.diffuseMap));
                out.push(list,item);
            }
            out.set(t,"materials",list);
        }
    });
    write("material", node.material, [&](auto t, const auto& c) {
        out.set(t,"roughness",lhat_real(c.roughness)); out.set(t,"metalness",lhat_real(c.metalness));
        out.set(t,"fresnel",out.vector(c.fresnel.data())); out.set(t,"emissive",out.vector(c.emissive.data()));
        out.set(t,"uvScale",lhat_real(c.uvScale));
        if(c.alpha!=AlphaMode::Opaque) {
            const char* modes[]={"Opaque","Test","Hash"};
            out.set(t,"alpha",out.string(modes[static_cast<int>(c.alpha)]));
            out.set(t,"alphaThreshold",lhat_real(c.alphaThreshold)); out.set(t,"opacity",lhat_real(c.opacity));
        }
        writeTranslucency(out,t,c.translucency);
        if(c.subsurfaceWidth>0) out.set(t,"subsurfaceWidth",lhat_real(c.subsurfaceWidth));
        for(const auto& entry:{std::pair{"diffuseMap",&c.diffuseMap},std::pair{"normalMap",&c.normalMap},
            std::pair{"roughnessMap",&c.roughnessMap},std::pair{"specularMap",&c.specularMap},std::pair{"reflectionMap",&c.reflectionMap}})
            if(!entry.second->empty()) out.set(t,entry.first,out.string(*entry.second));
    });
    write("light", node.light, [&](auto t,const auto& c) {
        const char* types[]={"Directional","Point","Spot","AreaApprox","AreaLtc"};
        out.set(t,"type",out.string(types[static_cast<int>(c.type)])); colour(t,c);
        out.set(t,"direction",out.vector(c.direction.data()));
        for(const auto& entry:{std::pair{"power",c.power},std::pair{"temperature",c.temperature},std::pair{"radius",c.radius},std::pair{"threshold",c.threshold},
            std::pair{"innerAngle",c.innerAngle},std::pair{"outerAngle",c.outerAngle},std::pair{"width",c.width},std::pair{"height",c.height}})
            out.set(t,entry.first,lhat_real(entry.second));
        out.set(t,"enabled",lhat_bool(c.enabled)); out.set(t,"twoSided",lhat_bool(c.twoSided));
        out.set(t,"showEmitter",lhat_bool(c.showEmitter)); out.set(t,"castShadows",lhat_bool(c.castShadows));
        if(!c.mask.empty()) out.set(t,"mask",out.string(c.mask));
    });
    write("sphere", node.sphere, [&](auto t, const auto& c) { colour(t, c); out.set(t, "radius", lhat_real(c.radius)); });
    write("capsule", node.capsule, [&](auto t, const auto& c) {
        colour(t, c); out.set(t, "radius", lhat_real(c.radius)); out.set(t, "height", lhat_real(c.height));
    });
    write("plane", node.plane, [&](auto t, const auto& c) {
        colour(t, c); out.set(t, "width", lhat_real(c.width)); out.set(t, "depth", lhat_real(c.depth));
    });
    write("boxCollider", node.boxCollider, [&](auto t, const auto& c) { writeCollider(out, t, c); out.set(t, "size", out.vector(c.size.data())); });
    write("sphereCollider", node.sphereCollider, [&](auto t, const auto& c) { writeCollider(out, t, c); out.set(t, "radius", lhat_real(c.radius)); });
    write("capsuleCollider", node.capsuleCollider, [&](auto t, const auto& c) {
        writeCollider(out, t, c); out.set(t, "radius", lhat_real(c.radius)); out.set(t, "height", lhat_real(c.height));
    });
    write("planeCollider", node.planeCollider, [&](auto t, const auto& c) {
        writeCollider(out, t, c); out.set(t, "width", lhat_real(c.width)); out.set(t, "depth", lhat_real(c.depth));
    });
    write("rigidBody", node.rigidBody, [&](auto t, const auto& c) {
        out.set(t, "type", out.string(c.type == BodyType::Static ? "Static" : c.type == BodyType::Kinematic ? "Kinematic" : "Dynamic"));
        out.set(t, "linearVelocity", out.vector(c.linearVelocity.data()));
        out.set(t, "angularVelocity", out.vector(c.angularVelocity.data()));
        out.set(t, "linearDamping", lhat_real(c.linearDamping));
        out.set(t, "angularDamping", lhat_real(c.angularDamping));
        out.set(t, "gravityScale", lhat_real(c.gravityScale));
        out.set(t, "bullet", lhat_bool(c.bullet));
    });
}

/// Reads one node table (its `parent` too); an instance's overrides come along.
SceneNode readNode(const LhatTable& entry) {
    if (!lhat_is_nil(lton::field(entry,"behaviours")) || !lhat_is_nil(lton::field(entry,"behaviour"))) lton::invalid();
    SceneNode node;
    node.id = nodeId(lton::field(entry, "id"));
    node.name = name(lton::field(entry, "name"));
    const auto parent = lton::field(entry, "parent");
    node.parent = lton::integer(parent) == 0 ? 0 : nodeId(parent);
    // Components: each optional, each a table of its properties.
    if (const auto components = lton::field(entry, "components"); !lhat_is_nil(components)) {
        const auto& table = lton::table(components);
        node.behaviour=readBehaviourList(lton::field(table,"behaviour"));
        node.renderEvent=readBehaviourList(lton::field(table,"renderEvent"));
        readExtraComponents(table, node);
        if (const auto value = lton::field(table, "transform"); !lhat_is_nil(value)) {
            const auto& transform = lton::table(value);
            auto& read = node.transform.emplace();
            lton::vector(lton::field(transform, "position"), read.position.data());
            lton::vector(lton::field(transform, "rotation"), read.rotation.data());
            lton::vector(lton::field(transform, "scale"), read.scale.data());
            for (unsigned axis = 0; axis < 3; ++axis)
                if (read.scale.data()[axis] < 0.001f) lton::invalid();
        }
        if (const auto value = lton::field(table, "box"); !lhat_is_nil(value)) {
            auto& read = node.box.emplace();
            lton::vector(lton::field(lton::table(value), "colour"), read.colour.data());
            for (const auto component : read.colour)
                if (component < 0 || component > 1) lton::invalid();
        }
    }
    if (const auto prefab = lton::field(entry, "prefab"); !lhat_is_nil(prefab)) {
        node.prefab = lton::relativePath(prefab);
        node.overrides = readOverrides(lton::field(entry, "overrides"));
    }
    if (const auto components = lton::field(entry, "components"); !lhat_is_nil(components))
        if (!lhat_is_nil(lton::field(lton::table(components), "scripts"))) lton::invalid();
    try { primitiveShape(node); } catch (const std::exception&) { lton::invalid(); }
    return node;
}

/// Writes what readNode reads.
LhatValue writeNode(lton::Writer& out, const SceneNode& node) {
    primitiveShape(node);
    const auto entry = out.table();
    out.set(entry, "id", lhat_integer(static_cast<LhatInteger>(node.id)));
    out.set(entry, "name", out.string(node.name));
    out.set(entry, "parent", lhat_integer(static_cast<LhatInteger>(node.parent)));
    const auto components = out.table();
    if (node.transform) {
        const auto transform = out.table();
        out.set(transform, "position", out.vector(node.transform->position.data()));
        out.set(transform, "rotation", out.vector(node.transform->rotation.data()));
        out.set(transform, "scale", out.vector(node.transform->scale.data()));
        out.set(components, "transform", transform);
    }
    if (node.box) {
        const auto box = out.table();
        out.set(box, "colour", out.vector(node.box->colour.data()));
        out.set(components, "box", box);
    }
    if (!node.behaviour.empty()) out.set(components,"behaviour",writeBehaviourList(out,node.behaviour));
    if (!node.renderEvent.empty()) out.set(components,"renderEvent",writeBehaviourList(out,node.renderEvent));
    writeExtraComponents(out, components, node);
    out.set(entry, "components", components);
    if (!node.prefab.empty()) {
        out.set(entry, "prefab", out.string(node.prefab));
        if (!node.overrides.empty()) out.set(entry, "overrides", writeOverrides(out, node.overrides));
    }
    return entry;
}

/// Reads an optional three-component vector field into @p out.
template <typename T>
void readOptional(const LhatTable& table, std::string_view key, std::optional<T>& out) {
    if (const auto value = lton::field(table, key); !lhat_is_nil(value)) {
        out.emplace();
        lton::vector(value, out->data());
    }
}
}

bool Overrides::empty() const {
    return nodes.empty() && systems.empty() && added.empty() && removed.empty() && addedSystems.empty() &&
           removedSystems.empty();
}

NodeOverride& Overrides::node(const NodePath& path) {
    for (auto& entry : nodes)
        if (entry.path == path) return entry;
    nodes.push_back({path});
    return nodes.back();
}

FieldOverride& Overrides::fieldsOf(const NodePath& instance, std::uint32_t system, std::size_t behaviour) {
    for (auto& entry : systems)
        if (entry.instance == instance && entry.system == system && entry.behaviour == behaviour) return entry;
    systems.push_back({instance, system, behaviour, {}});
    return systems.back();
}

void Overrides::subtract(const Overrides& piece) {
    for (const auto& part : piece.nodes) {
        const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const NodeOverride& each) { return each.path == part.path; });
        if (it == nodes.end()) continue;
        forEachProperty([](const char*, auto& kept, const auto& gone) { unset(kept, gone); }, *it, part);
        if (it->empty()) nodes.erase(it);
    }
    for (const auto& part : piece.systems) {
        const auto it = std::find_if(systems.begin(), systems.end(), [&](const FieldOverride& each) {
            return each.instance == part.instance && each.system == part.system && each.behaviour == part.behaviour;
        });
        if (it == systems.end()) continue;
        for (const auto& [name, value] : part.fields) it->fields.erase(name);
        if (it->fields.empty()) systems.erase(it);
    }
    const auto listed = [](const auto& list, auto&& same) { return std::any_of(list.begin(), list.end(), same); };
    added.erase(std::remove_if(added.begin(), added.end(), [&](const AddedNode& entry) {
        return listed(piece.added, [&](const AddedNode& each) { return each.node.id == entry.node.id && each.parent == entry.parent; });
    }), added.end());
    removed.erase(std::remove_if(removed.begin(), removed.end(), [&](const NodePath& path) {
        return listed(piece.removed, [&](const NodePath& each) { return each == path; });
    }), removed.end());
    addedSystems.erase(std::remove_if(addedSystems.begin(), addedSystems.end(), [&](const System& system) {
        return listed(piece.addedSystems, [&](const System& each) { return each.id == system.id; });
    }), addedSystems.end());
    removedSystems.erase(std::remove_if(removedSystems.begin(), removedSystems.end(), [&](const RemovedSystem& entry) {
        return listed(piece.removedSystems,
                      [&](const RemovedSystem& each) { return each.instance == entry.instance && each.system == entry.system; });
    }), removedSystems.end());
}

bool AttachmentOverride::empty() const {
    return !script && !enabled && !priority && data.empty() &&
           std::none_of(phasePriorities.begin(), phasePriorities.end(), [](const auto& phase) { return phase.has_value(); });
}

void AttachmentOverride::applyTo(Behaviour& behaviour) const {
    if (script) behaviour.script = *script;
    if (enabled) behaviour.enabled = *enabled;
    if (priority) behaviour.priority = *priority;
    for (int phase = 0; phase < 3; ++phase)
        if (phasePriorities[phase]) behaviour.phasePriorities[phase] = *phasePriorities[phase];
    for (const auto& [name, value] : data) behaviour.data[name] = value;
}

AttachmentOverride& AttachmentsOverride::change(NodeId id) {
    for (auto& entry : changed)
        if (entry.id == id) return entry;
    changed.push_back({id});
    return changed.back();
}

void AttachmentsOverride::merge(const AttachmentsOverride& from) {
    const auto addedOne = [&](NodeId id) {
        return std::find_if(added.begin(), added.end(), [&](const Behaviour& each) { return each.id == id; });
    };
    // A change or removal of an attachment this override adds goes to the attachment itself.
    for (const auto& entry : from.changed) {
        if (const auto it = addedOne(entry.id); it != added.end()) {
            entry.applyTo(*it);
            continue;
        }
        auto& into = change(entry.id);
        if (entry.script) into.script = entry.script;
        if (entry.enabled) into.enabled = entry.enabled;
        if (entry.priority) into.priority = entry.priority;
        for (int phase = 0; phase < 3; ++phase)
            if (entry.phasePriorities[phase]) into.phasePriorities[phase] = entry.phasePriorities[phase];
        for (const auto& [name, value] : entry.data) into.data[name] = value;
    }
    added.insert(added.end(), from.added.begin(), from.added.end());
    for (const auto id : from.removed) {
        if (const auto it = addedOne(id); it != added.end()) {
            added.erase(it);
            continue;
        }
        changed.erase(std::remove_if(changed.begin(), changed.end(), [&](const AttachmentOverride& each) { return each.id == id; }),
                      changed.end());
        removed.push_back(id);
    }
    if (!from.order.empty()) order = from.order;
}

void AttachmentsOverride::subtract(const AttachmentsOverride& piece) {
    for (const auto& part : piece.changed) {
        const auto it = std::find_if(changed.begin(), changed.end(), [&](const AttachmentOverride& each) { return each.id == part.id; });
        if (it == changed.end()) continue;
        if (part.script) it->script.reset();
        if (part.enabled) it->enabled.reset();
        if (part.priority) it->priority.reset();
        for (int phase = 0; phase < 3; ++phase)
            if (part.phasePriorities[phase]) it->phasePriorities[phase].reset();
        for (const auto& [name, value] : part.data) it->data.erase(name);
        if (it->empty()) changed.erase(it);
    }
    added.erase(std::remove_if(added.begin(), added.end(), [&](const Behaviour& each) {
        return std::any_of(piece.added.begin(), piece.added.end(), [&](const Behaviour& gone) { return gone.id == each.id; });
    }), added.end());
    removed.erase(std::remove_if(removed.begin(), removed.end(), [&](NodeId id) {
        return std::find(piece.removed.begin(), piece.removed.end(), id) != piece.removed.end();
    }), removed.end());
    if (!piece.order.empty()) order.clear();
}

AttachmentsOverride AttachmentsOverride::apply(std::vector<Behaviour>& list) const {
    AttachmentsOverride unused;
    const auto find = [&](NodeId id) {
        return std::find_if(list.begin(), list.end(), [&](const Behaviour& each) { return each.id == id; });
    };
    for (const auto id : removed) {
        if (const auto it = find(id); it != list.end()) list.erase(it);
        else unused.removed.push_back(id);
    }
    for (const auto& entry : changed) {
        if (const auto it = find(entry.id); it != list.end()) entry.applyTo(*it);
        else unused.changed.push_back(entry);
    }
    list.insert(list.end(), added.begin(), added.end());
    // The listed attachments come first, as listed; the rest keep their order after them.
    if (!order.empty()) {
        std::map<NodeId, std::size_t> rank;
        for (std::size_t i = 0; i < order.size(); ++i) rank.emplace(order[i], i);
        const auto of = [&](const Behaviour& each) {
            const auto found = rank.find(each.id);
            return found == rank.end() ? order.size() : found->second;
        };
        std::stable_sort(list.begin(), list.end(), [&](const Behaviour& a, const Behaviour& b) { return of(a) < of(b); });
    }
    return unused;
}

bool NodeOverride::empty() const {
    bool set = false;
    forEachProperty([&](const char*, const auto& value) { set |= value.has_value(); }, *this);
    return !set;
}

NodeOverride applyOverride(SceneNode& node, const NodeOverride& layer) {
    NodeOverride unused{layer.path};
    if (layer.name) node.name = *layer.name;
    unused.behaviour = layer.behaviour.apply(node.behaviour);
    unused.renderEvent = layer.renderEvent.apply(node.renderEvent);
    if (auto& transform = node.transform) {
        if (layer.position) transform->position = *layer.position;
        if (layer.rotation) transform->rotation = *layer.rotation;
        if (layer.scale) transform->scale = *layer.scale;
    } else {
        unused.position = layer.position;
        unused.rotation = layer.rotation;
        unused.scale = layer.scale;
    }
    if (node.box) {
        if (layer.colour) node.box->colour = *layer.colour;
    } else {
        unused.colour = layer.colour;
    }
    forEachComponent([](const char*, auto& component, const auto& value, auto& left) {
        if (!value) return;
        if (component) component = value;
        else left = value;
    }, node, layer, unused);
    return unused;
}

LhatValue writeOverrides(lton::Writer& out, const Overrides& overrides) {
    const auto root = out.table();
    const auto nodes = out.table();
    for (const auto& entry : overrides.nodes) {
        const auto item = out.table();
        out.set(item, "path", writePath(out, entry.path));
        if (entry.name) out.set(item, "name", out.string(*entry.name));
        if (entry.position || entry.rotation || entry.scale) {
            const auto transform = out.table();
            if (entry.position) out.set(transform, "position", out.vector(entry.position->data()));
            if (entry.rotation) out.set(transform, "rotation", out.vector(entry.rotation->data()));
            if (entry.scale) out.set(transform, "scale", out.vector(entry.scale->data()));
            out.set(item, "transform", transform);
        }
        if (entry.colour) {
            const auto box = out.table();
            out.set(box, "colour", out.vector(entry.colour->data()));
            out.set(item, "box", box);
        }
        writeExtraComponents(out, item, entry);
        if (entry.behaviour.has_value()) out.set(item, "behaviour", writeAttachmentsOverride(out, entry.behaviour));
        if (entry.renderEvent.has_value()) out.set(item, "renderEvent", writeAttachmentsOverride(out, entry.renderEvent));
        out.push(nodes, item);
    }
    out.set(root, "nodes", nodes);
    const auto systems = out.table();
    for (const auto& entry : overrides.systems) {
        const auto item = out.table();
        if (!entry.instance.empty()) out.set(item, "instance", writePath(out, entry.instance));
        out.set(item, "system", lhat_integer(entry.system));
        out.set(item, "behaviour", lhat_integer(static_cast<LhatInteger>(entry.behaviour)));
        out.set(item, "data", writeFields(out, entry.fields));
        out.push(systems, item);
    }
    out.set(root, "systems", systems);
    if (!overrides.added.empty()) {
        const auto added = out.table();
        for (const auto& entry : overrides.added) {
            const auto item = out.table();
            out.set(item, "parent", writePath(out, entry.parent));
            out.set(item, "node", writeNode(out, entry.node));
            out.push(added, item);
        }
        out.set(root, "added", added);
    }
    if (!overrides.removed.empty()) {
        const auto removed = out.table();
        for (const auto& path : overrides.removed) out.push(removed, writePath(out, path));
        out.set(root, "removed", removed);
    }
    if (!overrides.addedSystems.empty()) out.set(root, "addedSystems", writeSystems(out, overrides.addedSystems));
    if (!overrides.removedSystems.empty()) {
        const auto removed = out.table();
        for (const auto& entry : overrides.removedSystems) {
            const auto item = out.table();
            if (!entry.instance.empty()) out.set(item, "instance", writePath(out, entry.instance));
            out.set(item, "system", lhat_integer(entry.system));
            out.push(removed, item);
        }
        out.set(root, "removedSystems", removed);
    }
    return root;
}

Overrides readOverrides(LhatValue value) {
    Overrides overrides;
    if (lhat_is_nil(value)) return overrides;
    const auto& root = lton::table(value);
    if (const auto nodes = lton::field(root, "nodes"); !lhat_is_nil(nodes)) {
        const auto& list = lton::table(nodes);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            NodeOverride entry;
            if (!lhat_is_nil(lton::field(item,"behaviours"))) lton::invalid();
            readExtraComponents(item, entry);
            entry.path = readPath(lton::field(item, "path"));
            if (const auto text = lton::field(item, "name"); !lhat_is_nil(text)) entry.name = name(text);
            if (const auto transform = lton::field(item, "transform"); !lhat_is_nil(transform)) {
                readOptional(lton::table(transform), "position", entry.position);
                readOptional(lton::table(transform), "rotation", entry.rotation);
                readOptional(lton::table(transform), "scale", entry.scale);
            }
            if (const auto box = lton::field(item, "box"); !lhat_is_nil(box))
                readOptional(lton::table(box), "colour", entry.colour);
            entry.behaviour = readAttachmentsOverride(lton::field(item, "behaviour"));
            entry.renderEvent = readAttachmentsOverride(lton::field(item, "renderEvent"));
            if (!lhat_is_nil(lton::field(item, "scripts"))) lton::invalid();
            overrides.nodes.push_back(std::move(entry));
        }
    }
    if (const auto systems = lton::field(root, "systems"); !lhat_is_nil(systems)) {
        const auto& list = lton::table(systems);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            FieldOverride entry;
            entry.instance = readPath(lton::field(item, "instance"));
            entry.system = id(lton::field(item, "system"));
            const auto behaviour = lton::integer(lton::field(item, "behaviour"));
            if (behaviour < 0) lton::invalid();
            entry.behaviour = static_cast<std::size_t>(behaviour);
            entry.fields = readFields(lton::field(item, "data"));
            overrides.systems.push_back(std::move(entry));
        }
    }
    if (const auto added = lton::field(root, "added"); !lhat_is_nil(added)) {
        const auto& list = lton::table(added);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            AddedNode entry{readPath(lton::field(item, "parent")), readNode(lton::table(lton::field(item, "node")))};
            entry.node.parent = 0;
            overrides.added.push_back(std::move(entry));
        }
    }
    if (const auto removed = lton::field(root, "removed"); !lhat_is_nil(removed)) {
        const auto& list = lton::table(removed);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            overrides.removed.push_back(readPath(lton::at(list, i)));
            if (overrides.removed.back().empty()) lton::invalid();
        }
    }
    std::uint32_t unused = 1;
    overrides.addedSystems = readSystems(lton::field(root, "addedSystems"), unused);
    if (const auto removed = lton::field(root, "removedSystems"); !lhat_is_nil(removed)) {
        const auto& list = lton::table(removed);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            overrides.removedSystems.push_back({readPath(lton::field(item, "instance")), id(lton::field(item, "system"))});
        }
    }
    return overrides;
}

NodeId newNodeId() {
    // Counting from 2026-01-01 UTC, the 41 bits of time last until 2095.
    static Ids ids(1767225600000);
    return ids.next();
}

std::uint32_t newSystemId() {
    static std::mt19937 generator(std::random_device{}());
    return std::uniform_int_distribution<std::uint32_t>(0x80000000u, 0xFFFFFFFEu)(generator);
}

SceneNode* Prefab::findNode(NodeId id) {
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const SceneNode& node) { return node.id == id; });
    return it == nodes.end() ? nullptr : &*it;
}

const SceneNode* Prefab::findNode(NodeId id) const { return const_cast<Prefab*>(this)->findNode(id); }

System* SystemSet::findSystem(std::uint32_t id) {
    const auto it = std::find_if(systems.begin(), systems.end(), [id](const System& system) { return system.id == id; });
    return it == systems.end() ? nullptr : &*it;
}

SceneNode& Prefab::addNode(const std::string& name, NodeId parent) {
    SceneNode node;
    do node.id = newNodeId();
    while (findNode(node.id));
    node.name = name;
    node.transform = Transform{{0, 0.5f, 0}};
    node.box.emplace();
    nodes.push_back(node);
    if (parent) reparent(node.id, parent);
    return *findNode(node.id);
}

System& SystemSet::addSystem(const std::string& name) {
    if (nextSystemId == std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("System ID limit reached");
    systems.push_back({nextSystemId++, name, {}});
    return systems.back();
}

void Prefab::removeNode(NodeId id) {
    const auto all = nodes;
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const SceneNode& node) { return within(all, node.id, id); }),
                nodes.end());
}

void SystemSet::removeSystem(std::uint32_t id) {
    systems.erase(std::remove_if(systems.begin(), systems.end(), [id](const System& system) { return system.id == id; }),
                  systems.end());
}

bool Prefab::isWithin(NodeId id, NodeId ancestor) const { return within(nodes, id, ancestor); }

bool Prefab::reparent(NodeId id, NodeId parent, NodeId before) {
    if (!findNode(id) || (parent && (!findNode(parent) || isWithin(parent, id)))) return false;
    if (before && (before == id || !findNode(before) || findNode(before)->parent != parent)) return false;
    // Keep parents before children: lift the subtree out, then put it before the
    // sibling, or after the new parent's last descendant (at the end for the root).
    std::vector<SceneNode> moved, kept;
    for (const auto& node : nodes) (isWithin(node.id, id) ? moved : kept).push_back(node);
    moved.front().parent = parent;
    auto at = kept.end();
    if (before) {
        at = std::find_if(kept.begin(), kept.end(), [before](const SceneNode& node) { return node.id == before; });
    } else if (parent) {
        for (auto it = kept.begin(); it != kept.end(); ++it)
            if (within(kept, it->id, parent)) at = it + 1;
    }
    kept.insert(at, moved.begin(), moved.end());
    nodes = std::move(kept);
    return true;
}

void Prefab::read(const LhatTable& root) {
    const auto& list = lton::table(lton::field(root, "nodes"));
    std::set<NodeId> seen;
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
        auto node = readNode(lton::table(lton::at(list, i)));
        // A parent written earlier is what keeps the order and rules out cycles.
        if ((node.parent && !seen.count(node.parent)) || !seen.insert(node.id).second) lton::invalid();
        nodes.push_back(std::move(node));
    }
    systems = readSystems(lton::field(root, "systems"), nextSystemId);
}

void Prefab::write(lton::Writer& out, LhatValue root) const {
    const auto list = out.table();
    for (const auto& node : nodes) out.push(list, writeNode(out, node));
    out.set(root, "nodes", list);
    out.set(root, "systems", writeSystems(out, systems));
}

Prefab Prefab::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(prefabFormat, prefabVersion);
    Prefab result;
    result.read(document.root());
    if (const auto base = lton::field(document.root(), "base"); !lhat_is_nil(base)) {
        result.base = lton::relativePath(base);
        result.overrides = readOverrides(lton::field(document.root(), "overrides"));
    }
    return result;
}

void Prefab::save(const std::filesystem::path& path) const {
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(prefabFormat));
    out.set(root, "version", lhat_integer(prefabVersion));
    write(out, root);
    if (variant()) {
        out.set(root, "base", out.string(base));
        out.set(root, "overrides", writeOverrides(out, overrides));
    }
    out.save(root, path);
}

Scene Scene::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(sceneFormat, sceneVersion);
    Scene result;
    result.read(document.root());
    if (const auto lighting = lton::field(document.root(), "lighting"); !lhat_is_nil(lighting))
        result.lighting = readLighting(lton::table(lighting));
    return result;
}

void Scene::save(const std::filesystem::path& path) const {
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(sceneFormat));
    out.set(root, "version", lhat_integer(sceneVersion));
    write(out, root);
    out.set(root, "lighting", writeLighting(out, lighting));
    out.save(root, path);
}

Scene Scene::example() {
    Scene scene;
    scene.addNode("SpinningCube");
    auto& second = scene.addNode("Cube 2");
    second.transform = Transform{{2, 0.75f, -1}, {}, {1, 1.5f, 1}};
    second.box = BoxPrimitive{{0.86f, 0.53f, 0.25f}};
    return scene;
}

LhatValue writeBehaviourList(lton::Writer& out, const std::vector<Behaviour>& behaviour) {
    const auto list = out.table();
    for (const auto& behaviour : behaviour) {
        const auto item = out.table();
        out.set(item, "script", out.string(behaviour.script));
        out.set(item,"id",lhat_integer(static_cast<LhatInteger>(behaviour.id)));
        if(!behaviour.enabled) out.set(item,"enabled",lhat_bool(false));
        const auto priority=[&](const char* key,const std::optional<double>& value) {
            if(value) {
                if(!std::isfinite(*value)) lton::invalid();
                out.set(item,key,lhat_real(*value));
            }
        };
        priority("priority",behaviour.priority);
        for(int phase=0;phase<3;++phase)
            priority((std::string(behaviourMethods[phase])+"Priority").c_str(),behaviour.phasePriorities[phase]);
        out.set(item, "data", writeFields(out, behaviour.data));
        out.push(list, item);
    }
    return list;
}

std::vector<Behaviour> readBehaviourList(LhatValue value) {
    std::vector<Behaviour> result;
    if (lhat_is_nil(value)) return result;
    const auto& list = lton::table(value);
    std::set<NodeId> identities;
    for (std::size_t i = 0, n = lton::sequence(list); i < n; ++i) {
        const auto& item = lton::table(lton::at(list, i));
        Behaviour behaviour;
        if (!lhat_is_nil(lton::field(item,"fields"))) lton::invalid();
        behaviour.script = lton::relativePath(lton::field(item, "script"));
        // Without an ID, its place in the list: a prefab's attachment keeps the ID overrides find it by.
        behaviour.id = i + 1;
        if(const auto value=lton::field(item,"id");!lhat_is_nil(value)) behaviour.id=nodeId(value);
        if(!identities.insert(behaviour.id).second) lton::invalid();
        if(const auto value=lton::field(item,"enabled");!lhat_is_nil(value)) {
            if(!lhat_is_bool(value)) lton::invalid();
            behaviour.enabled=lhat_as_bool(value);
        }
        const auto priority=[&](const char* key,std::optional<double>& into) {
            if(const auto value=lton::field(item,key);!lhat_is_nil(value)) {
                into=lton::number(value);
                if(!std::isfinite(*into)) lton::invalid();
            }
        };
        priority("priority",behaviour.priority);
        for(int phase=0;phase<3;++phase)
            priority((std::string(behaviourMethods[phase])+"Priority").c_str(),behaviour.phasePriorities[phase]);
        behaviour.data = readFields(lton::field(item, "data"));
        if (!lhat_is_nil(lton::field(item, "systemScript"))) lton::invalid();
        result.push_back(std::move(behaviour));
    }
    return result;
}

// A priority override is a number, or false^ for the script's default.
LhatValue writeAttachmentsOverride(lton::Writer& out, const AttachmentsOverride& overrides) {
    const auto root = out.table();
    if (!overrides.changed.empty()) {
        const auto changed = out.table();
        for (const auto& entry : overrides.changed) {
            const auto item = out.table();
            out.set(item, "id", lhat_integer(static_cast<LhatInteger>(entry.id)));
            if (entry.script) out.set(item, "script", out.string(*entry.script));
            if (entry.enabled) out.set(item, "enabled", lhat_bool(*entry.enabled));
            const auto priority = [&](const char* key, const std::optional<std::optional<double>>& value) {
                if (!value) return;
                if (*value && !std::isfinite(**value)) lton::invalid();
                out.set(item, key, *value ? lhat_real(**value) : lhat_bool(false));
            };
            priority("priority", entry.priority);
            for (int phase = 0; phase < 3; ++phase)
                priority((std::string(behaviourMethods[phase]) + "Priority").c_str(), entry.phasePriorities[phase]);
            if (!entry.data.empty()) out.set(item, "data", writeFields(out, entry.data));
            out.push(changed, item);
        }
        out.set(root, "changed", changed);
    }
    if (!overrides.added.empty()) out.set(root, "added", writeBehaviourList(out, overrides.added));
    if (!overrides.removed.empty()) {
        const auto removed = out.table();
        for (const auto id : overrides.removed) out.push(removed, lhat_integer(static_cast<LhatInteger>(id)));
        out.set(root, "removed", removed);
    }
    if (!overrides.order.empty()) {
        const auto order = out.table();
        for (const auto id : overrides.order) out.push(order, lhat_integer(static_cast<LhatInteger>(id)));
        out.set(root, "order", order);
    }
    return root;
}

AttachmentsOverride readAttachmentsOverride(LhatValue value) {
    AttachmentsOverride overrides;
    if (lhat_is_nil(value)) return overrides;
    const auto& root = lton::table(value);
    if (const auto changed = lton::field(root, "changed"); !lhat_is_nil(changed)) {
        const auto& list = lton::table(changed);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            AttachmentOverride entry;
            entry.id = nodeId(lton::field(item, "id"));
            if (const auto script = lton::field(item, "script"); !lhat_is_nil(script)) entry.script = lton::relativePath(script);
            if (const auto enabled = lton::field(item, "enabled"); !lhat_is_nil(enabled)) {
                if (!lhat_is_bool(enabled)) lton::invalid();
                entry.enabled = lhat_as_bool(enabled);
            }
            const auto priority = [&](const char* key, std::optional<std::optional<double>>& into) {
                const auto value = lton::field(item, key);
                if (lhat_is_nil(value)) return;
                if (lhat_is_bool(value)) {
                    if (lhat_as_bool(value)) lton::invalid();
                    into.emplace();
                    return;
                }
                into = lton::number(value);
                if (!std::isfinite(**into)) lton::invalid();
            };
            priority("priority", entry.priority);
            for (int phase = 0; phase < 3; ++phase)
                priority((std::string(behaviourMethods[phase]) + "Priority").c_str(), entry.phasePriorities[phase]);
            entry.data = readFields(lton::field(item, "data"));
            overrides.changed.push_back(std::move(entry));
        }
    }
    overrides.added = readBehaviourList(lton::field(root, "added"));
    if (const auto removed = lton::field(root, "removed"); !lhat_is_nil(removed)) {
        const auto& list = lton::table(removed);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) overrides.removed.push_back(nodeId(lton::at(list, i)));
    }
    if (const auto order = lton::field(root, "order"); !lhat_is_nil(order)) {
        const auto& list = lton::table(order);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) overrides.order.push_back(nodeId(lton::at(list, i)));
    }
    return overrides;
}

LhatValue writeSystems(lton::Writer& out, const std::vector<System>& systems) {
    const auto list = out.table();
    for (const auto& system : systems) {
        const auto entry = out.table();
        out.set(entry, "id", lhat_integer(system.id));
        out.set(entry, "name", out.string(system.name));
        out.set(entry, "behaviour", writeBehaviourList(out, system.behaviour));
        out.push(list, entry);
    }
    return list;
}

std::vector<System> readSystems(LhatValue value, std::uint32_t& nextId) {
    std::vector<System> result;
    if (lhat_is_nil(value)) return result;
    const auto& list = lton::table(value);
    std::set<std::uint32_t> seen;
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
        const auto& entry = lton::table(lton::at(list, i));
        System system;
        if (!lhat_is_nil(lton::field(entry,"behaviours"))) lton::invalid();
        system.id = id(lton::field(entry, "id"));
        if (!seen.insert(system.id).second) lton::invalid();
        system.name = name(lton::field(entry, "name"));
        system.behaviour = readBehaviourList(lton::field(entry, "behaviour"));
        nextId = std::max(nextId, system.id + 1);
        result.push_back(std::move(system));
    }
    return result;
}
}
