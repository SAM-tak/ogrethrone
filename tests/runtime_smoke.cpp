#include "runtime/PhysicsWorld.h"
#include "editor/PrimitiveGeometry.h"
#include "script/ScriptRuntime.h"
#include <filesystem>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
/// Keeps checks active in optimized builds.
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
/// A scene with a finite mesh floor and the three supported convex shapes.
throne::LevelExpansion fixture() {
    throne::World world;
    auto& scene=world.scenes["physics.scene.lton"];
    auto& floor=scene.addNode("Floor");
    floor.transform->position={0,0,0}; throne::setPrimitive(floor,throne::PrimitiveKind::Plane); floor.planeCollider.emplace();
    for (int kind=0;kind<3;++kind) {
        auto& node=scene.addNode("Falling body");
        node.transform->position={float(kind*2-2),4,0};
        node.rigidBody.emplace();
        if (kind==0) node.boxCollider.emplace();
        if (kind==1) {throne::setPrimitive(node,throne::PrimitiveKind::Sphere); node.sphereCollider.emplace();}
        if (kind==2) {throne::setPrimitive(node,throne::PrimitiveKind::Capsule); node.capsuleCollider.emplace();}
    }
    world.level.addScene("physics.scene.lton");
    throne::Library library;
    return throne::expand(world,library);
}
}
int main() {
    try {
        auto expanded=fixture();
        throne::PhysicsWorld physics;
        physics.load(expanded);
        require(physics.bodyCount()==4,"Scene bodies were not created");
        std::array<std::array<throne::Vec3,3>,240> first;
        for (auto& sample:first) {
            physics.prepare(expanded); physics.advance(1.0f/60); physics.apply(expanded);
            for (int i=0;i<3;++i) sample[i]=expanded.instances[0].expansion.nodes[i+1].node.transform->position;
        }
        for (int i=0;i<3;++i) {
            const float expected=i==2?1.0f:0.5f;
            require(std::abs(first.back()[i].y-expected)<0.15f,"Convex shape did not settle on the PlaneCollider");
            require(first.front()[i].y>first.back()[i].y+2,"Physics did not move the scene node");
        }
        auto serial=fixture();
        physics.setSerial(true); physics.load(serial);
        for (const auto& expected:first) {
            physics.prepare(serial); physics.advance(1.0f/60); physics.apply(serial);
            for (int i=0;i<3;++i)
                require(serial.instances[0].expansion.nodes[i+1].node.transform->position==expected[i],
                        "Serial and parallel physics diverged");
        }
        // Host queries and typed handles reach real physics components.
        {
            auto scripted=fixture();
            throne::World empty;
            throne::ScriptRuntime scripts(empty,scripted,{},[](const std::string&) {},throne::PlayScope::Level);
            require(scripts.evaluate("import^ throne").text.empty(),"Could not import the physics host API");
            const auto answer=scripts.evaluate(R"(for^ _^, node in^ throne.query("SpherePrimitive", "SphereCollider", "RigidBody") {
                let^ c=node.sphereCollider()
                let^ b=node.rigidBody()
                if^ c != nil^ and^ b != nil^ { b.setLinearVelocity(throne.vec3(c.radius(),2,0)) }
            })");
            require(answer.text.empty(),answer.text.c_str());
            require(scripted.instances[0].expansion.nodes[2].node.rigidBody->linearVelocity==throne::Vec3{.5f,2,0},
                    "Typed RigidBody setter/query did not reach the sphere");
        }
        // Mesh bounds agree with the geometry used by the viewport picker.
        for(const auto kind:{throne::PrimitiveKind::Box,throne::PrimitiveKind::Sphere,throne::PrimitiveKind::Capsule,throne::PrimitiveKind::Plane}) {
            throne::SceneNode node; throne::setPrimitive(node,kind);
            const auto mesh=throne::primitiveMesh(throne::primitiveShape(node));
            require(!mesh.vertices.empty() && !mesh.indices.empty(),"Primitive generated no triangles");
            for(auto index:mesh.indices) require(index<mesh.vertices.size(),"Primitive index exceeded its vertices");
            for(const auto& v:mesh.vertices) {
                for(int axis=0;axis<3;++axis)
                    require(std::isfinite(v.position.data()[axis]) && v.position.data()[axis]>=mesh.lower.data()[axis] &&
                            v.position.data()[axis]<=mesh.upper.data()[axis],"Primitive bounds did not enclose its vertices");
                const auto n=v.normal;
                require(std::abs(n.x*n.x+n.y*n.y+n.z*n.z-1)<1.0e-5f,"Primitive normal was not a unit vector");
            }
            for(std::size_t i=0;i<mesh.indices.size();i+=3) {
                const auto& a=mesh.vertices[mesh.indices[i]];
                const auto& b=mesh.vertices[mesh.indices[i+1]];
                const auto& c=mesh.vertices[mesh.indices[i+2]];
                const auto ab=b.position-a.position, ac=c.position-a.position;
                const throne::Vec3 cross{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
                const auto normal=a.normal+b.normal+c.normal;
                require(cross.x*normal.x+cross.y*normal.y+cross.z*normal.z>=-1e-8f,
                        "Primitive triangle winding opposed its outward normals");
            }
            if(kind==throne::PrimitiveKind::Capsule) require(std::abs(mesh.upper.y-1)<1e-5,"Capsule height excluded its hemispheres");
        }
        // All components and their prefab property overrides survive persistence.
        {
            const auto root=std::filesystem::temp_directory_path()/"throne_physics_components";
            std::filesystem::create_directories(root);
            throne::Scene scene;
            const auto initial=fixture();
            for(const auto& node:initial.instances[0].expansion.nodes) scene.nodes.push_back(node.node);
            scene.save(root/"scene.lton");
            const auto loaded=throne::Scene::load(root/"scene.lton");
            require(loaded.nodes[0].plane && loaded.nodes[0].planeCollider && loaded.nodes[1].boxCollider &&
                    loaded.nodes[2].sphere && loaded.nodes[2].sphereCollider && loaded.nodes[3].capsule &&
                    loaded.nodes[3].capsuleCollider && loaded.nodes[3].rigidBody,"Physics components lost on save/load");
            throne::Prefab base; base.nodes.push_back(loaded.nodes[2]);
            throne::Library library; library.put("base.lton",base);
            throne::Scene placed;
            auto& instance=placed.addNode("Instance"); instance.prefab="base.lton";
            auto& layer=instance.overrides.node({});
            layer.sphere=throne::SpherePrimitive{}; layer.sphere->radius=.8f;
            layer.sphereCollider=throne::SphereCollider{}; layer.sphereCollider->radius=.8f;
            layer.rigidBody=throne::RigidBody{}; layer.rigidBody->gravityScale=0;
            placed.save(root/"instance.lton");
            const auto expandedPrefab=throne::expand(throne::Scene::load(root/"instance.lton"),library);
            require(expandedPrefab.nodes[0].node.sphere->radius==.8f && expandedPrefab.nodes[0].node.sphereCollider->radius==.8f &&
                    expandedPrefab.nodes[0].node.rigidBody->gravityScale==0,"Physics prefab overrides were lost");
            std::filesystem::remove_all(root);
        }
        // UI/console commands between frames must not be overwritten by an older result.
        {
            auto commanded=fixture(); physics.load(commanded);
            physics.prepare(commanded); physics.advance(1.0f/60);
            auto& node=commanded.instances[0].expansion.nodes[2].node;
            node.transform->position.y=8; node.rigidBody->linearVelocity={0,3,0};
            physics.apply(commanded);
            require(node.transform->position.y==8 && node.rigidBody->linearVelocity.y==3,"Old physics result overwrote a new command");
            physics.prepare(commanded); physics.advance(1.0f/60); physics.apply(commanded);
            require(node.transform->position.y>8,"Teleport/velocity command did not enter Box3D");
        }
        {
            auto modes=fixture();
            auto& kinematic=modes.instances[0].expansion.nodes[1].node;
            kinematic.rigidBody->type=throne::BodyType::Kinematic;
            kinematic.rigidBody->linearVelocity={1,0,0};
            modes.instances[0].expansion.nodes[2].node.sphereCollider->sensor=true;
            physics.load(modes);
            for(int frame=0;frame<120;++frame) {physics.prepare(modes);physics.advance(1.0f/60);physics.apply(modes);}
            require(std::abs(kinematic.transform->position.x)<.02f && kinematic.transform->position.y==4,
                    "Kinematic body did not follow velocity independently of gravity");
            require(modes.instances[0].expansion.nodes[2].node.transform->position.y<-5,"Sensor produced a collision response");
        }
        // Static objects stay put and a scene instance's transform is respected.
        auto nested=fixture();
        nested.instances[0].transform.position={8,3,-2};
        nested.instances[0].transform.rotation={0,35,0};
        nested.instances[0].expansion.nodes[1].node.rigidBody->type=throne::BodyType::Static;
        physics.load(nested);
        for(int frame=0;frame<240;++frame) {
            physics.prepare(nested); physics.advance(1.0f/60); physics.apply(nested);
        }
        require(nested.instances[0].expansion.nodes[1].node.transform->position==throne::Vec3{-2,4,0},"Static body moved");
        require(std::abs(nested.instances[0].expansion.nodes[2].node.transform->position.y-.5f)<.15f,
                "Scene-instance physics conversion changed local coordinates");
        // A mesh floor has finite extent, rather than being an invisible infinite plane.
        auto outside=fixture();
        outside.instances[0].expansion.nodes[2].node.transform->position.x=20;
        physics.load(outside);
        for(int frame=0;frame<120;++frame) {
            physics.prepare(outside); physics.advance(1.0f/60); physics.apply(outside);
        }
        require(outside.instances[0].expansion.nodes[2].node.transform->position.y < -5,"PlaneCollider was not finite");
        outside.instances[0].expansion.nodes[0].node.rigidBody.emplace();
        bool refused=false;
        try {physics.load(outside);} catch(const std::runtime_error&) {refused=true;}
        require(refused,"Dynamic triangle-mesh PlaneCollider was accepted");
        // Exercise the actual sample script, incremental bodies and stable handles after growth.
        {
            const std::filesystem::path root = THRONE_PHYSICS_SAMPLE;
            throne::World world;
            world.level = throne::Level::load(root / "levels/main.level.lton");
            world.scenes["scenes/physics.scene.lton"] = throne::Scene::load(root / "scenes/physics.scene.lton");
            throne::Library library(root);
            auto growing = throne::expand(world, library);
            const auto initial = growing.instances[0].expansion.nodes.size();
            throne::ScriptRuntime scripts(world, growing, root, [](const std::string&) {}, throne::PlayScope::Level);
            throne::PhysicsWorld simulation;
            simulation.load(growing);
            const auto bodies = simulation.bodyCount();
            const auto tick = [&](float dt) {
                scripts.update(dt, [&](const std::string& stage) {
                    if (stage == "ApplyPhysics") simulation.apply(growing);
                    if (stage == "SyncRender") simulation.prepare(growing);
                    if (stage == "Box3D") simulation.advance(dt);
                });
            };
            tick(.05f);
            require(growing.instances[0].expansion.nodes.size() == initial, "Spawner fired before its interval");
            tick(.05f);
            require(growing.instances[0].expansion.nodes.size() == initial + 1, "Spawner did not fire at 0.1s");
            require(simulation.bodyCount() == bodies + 1, "Spawned box did not enter physics");
            tick(.35f);
            require(growing.instances[0].expansion.nodes.size() == initial + 4, "Spawner lost catch-up intervals");
            for (int i = 0; i < 600; ++i) tick(1.0f / 60);
            simulation.apply(growing);
            const auto& nodes = growing.instances[0].expansion.nodes;
            require(nodes.size() >= initial + 103 && nodes.size() <= initial + 105, "Spawner did not keep generating boxes");
            require(simulation.bodyCount() == bodies + nodes.size() - initial, "Incremental physics duplicated or missed bodies");
            require(nodes[initial].node.transform->position.y < -2, "Spawned box did not fall below its spawn point");
            require(nodes[initial].node.transform->scale == throne::Vec3{.6f,.6f,.6f}, "Spawned box size changed");
            require(world.scenes.at("scenes/physics.scene.lton").nodes.size() == initial, "Spawning modified the source scene");
            auto restored = throne::expand(world, library);
            require(restored.instances[0].expansion.nodes.size() == initial, "Re-expansion retained spawned boxes");
            require(scripts.evaluate("import^ throne").text.empty(), "Could not import spawning API in console");
            require(scripts.evaluate("let^ p = throne.find(\"Box Spawn Point\")\nif^ p != nil^ { p.spawnBox(throne.vec3(0,1,1)) }").text.find("finite and positive") != std::string::npos,
                    "Invalid spawn size was accepted");
        }
        std::puts("Physics shapes, incremental script spawning, catch-up timing and source restoration passed");
    } catch(const std::exception& error) {
        std::fprintf(stderr,"%s\n",error.what()); return 1;
    }
}
