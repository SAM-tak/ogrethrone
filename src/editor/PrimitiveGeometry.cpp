#include "editor/PrimitiveGeometry.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace throne {
PrimitiveShape primitiveShape(const SceneNode& node) {
    if (int(bool(node.box))+int(bool(node.sphere))+int(bool(node.capsule))+int(bool(node.plane)) > 1)
        throw std::runtime_error(node.name + ": a node may have only one render primitive");
    PrimitiveShape s;
    if (node.box) s.kind=PrimitiveKind::Box;
    if (node.sphere) { s.kind=PrimitiveKind::Sphere; s.radius=node.sphere->radius; }
    if (node.capsule) { s.kind=PrimitiveKind::Capsule; s.radius=node.capsule->radius; s.height=node.capsule->height; }
    if (node.plane) { s.kind=PrimitiveKind::Plane; s.width=node.plane->width; s.depth=node.plane->depth; }
    return s;
}
std::array<float,3> primitiveColour(const SceneNode& node) {
    if(node.box) return node.box->colour;
    if(node.sphere) return node.sphere->colour;
    if(node.capsule) return node.capsule->colour;
    if(node.plane) return node.plane->colour;
    return {1,1,1};
}
void setPrimitive(SceneNode& node, PrimitiveKind kind) {
    node.box.reset(); node.sphere.reset(); node.capsule.reset(); node.plane.reset();
    if(kind==PrimitiveKind::Box) node.box.emplace();
    if(kind==PrimitiveKind::Sphere) node.sphere.emplace();
    if(kind==PrimitiveKind::Capsule) node.capsule.emplace();
    if(kind==PrimitiveKind::Plane) node.plane.emplace();
}
PrimitiveMesh primitiveMesh(const PrimitiveShape& s) {
    PrimitiveMesh mesh;
    const auto vertex=[&](Vec3 p,Vec3 normal) { mesh.vertices.push_back({p,normal}); };
    const auto quad=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t d) {
        mesh.indices.insert(mesh.indices.end(),{a,b,c,a,c,d});
    };
    constexpr float pi=3.14159265358979323846f;
    if(s.kind==PrimitiveKind::Box) {
        for(int axis=0;axis<3;++axis) for(float side:{-.5f,.5f}) {
            const auto base=static_cast<std::uint32_t>(mesh.vertices.size());
            for(int corner=0;corner<4;++corner) {
                Vec3 p; p.data()[axis]=side;
                p.data()[(axis+1)%3]=(corner==1 || corner==2)?.5f:-.5f;
                p.data()[(axis+2)%3]=corner>=2?.5f:-.5f;
                Vec3 normal; normal.data()[axis]=side<0?-1.0f:1.0f;
                vertex(p,normal);
                auto& v=mesh.vertices.back();
                v.u=p.data()[(axis+1)%3]+.5f; v.v=p.data()[(axis+2)%3]+.5f;
                v.tangent={}; v.tangent.data()[(axis+1)%3]=1;
            }
            if(side<0) quad(base,base+3,base+2,base+1);
            else quad(base,base+1,base+2,base+3);
        }
    } else if(s.kind==PrimitiveKind::Plane) {
        vertex({-s.width/2,0,-s.depth/2},{0,1,0}); vertex({s.width/2,0,-s.depth/2},{0,1,0});
        vertex({s.width/2,0,s.depth/2},{0,1,0}); vertex({-s.width/2,0,s.depth/2},{0,1,0});
        for(auto& v:mesh.vertices) { v.u=v.position.x/s.width+.5f; v.v=.5f-v.position.z/s.depth; }
        quad(0,3,2,1);
    } else if(s.kind==PrimitiveKind::Sphere || s.kind==PrimitiveKind::Capsule) {
        const float half=s.kind==PrimitiveKind::Capsule?(std::max)(0.0f,s.height/2-s.radius):0;
        std::vector<std::pair<float,float>> rings;
        for(int hemisphere=0;hemisphere<2;++hemisphere)
            for(int ring=0;ring<=8;++ring) {
                const float latitude=-pi/2 + (hemisphere*8+ring)*pi/16;
                rings.push_back({latitude,hemisphere==0?-half:half});
            }
        for(const auto& [latitude,offset]:rings) for(int segment=0;segment<=32;++segment) {
            const float longitude=segment*2*pi/32;
            const Vec3 normal{std::cos(latitude)*std::cos(longitude),std::sin(latitude),std::cos(latitude)*std::sin(longitude)};
            vertex(normal*s.radius+Vec3{0,offset,0},normal);
            auto& v=mesh.vertices.back(); v.u=segment/32.0f; v.v=.5f-latitude/pi;
            v.tangent={-std::sin(longitude),0,std::cos(longitude)};
        }
        for(std::uint32_t ring=0;ring+1<rings.size();++ring)
            for(std::uint32_t segment=0;segment<32;++segment) {
                const auto a=ring*33+segment;
                quad(a,a+33,a+34,a+1);
            }
    }
    if(!mesh.vertices.empty()) {
        mesh.lower=mesh.upper=mesh.vertices.front().position;
        for(const auto& v:mesh.vertices) for(int axis=0;axis<3;++axis) {
            mesh.lower.data()[axis]=(std::min)(mesh.lower.data()[axis],v.position.data()[axis]);
            mesh.upper.data()[axis]=(std::max)(mesh.upper.data()[axis],v.position.data()[axis]);
        }
    }
    return mesh;
}
}
