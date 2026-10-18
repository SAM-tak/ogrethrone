#include "runtime/PhysicsWorld.h"
#include "runtime/Box3DMath.h"
#include "runtime/TaskPool.h"
#include <box3d/box3d.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace throne {
namespace {
constexpr float radians = 0.017453292519943f;
/// Component-wise scale composition.
Vec3 product(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
/// Converts vectors at the Box3D adapter boundary.
b3Vec3 vector(Vec3 v) { return {v.x, v.y, v.z}; }
/// Converts intrinsic XYZ Euler degrees to a Z*Y*X quaternion.
b3Quat rotation(Vec3 e) {
    const auto axis = [](float angle, b3Vec3 v) {
        const auto half = angle * radians * 0.5f;
        return b3Quat{b3MulSV(std::sin(half), v), std::cos(half)};
    };
    return b3MulQuat(axis(e.z, {0,0,1}), b3MulQuat(axis(e.y, {0,1,0}), axis(e.x, {1,0,0})));
}
/// Converts Box3D rotation back to XYZ Euler degrees.
Vec3 euler(b3Quat q) {
    const auto x=q.v.x, y=q.v.y, z=q.v.z, w=q.s;
    return {std::atan2(2*(w*x+y*z), 1-2*(x*x+y*y))/radians,
            std::asin(std::clamp(2*(w*y-z*x), -1.0f, 1.0f))/radians,
            std::atan2(2*(w*z+x*y), 1-2*(y*y+z*z))/radians};
}
/// World placement, matching scene-node translation/orientation/scale inheritance.
struct Pose { Vec3 position, scale{1,1,1}; b3Quat rotation = {{0,0,0},1}; };
/// Appends a local transform to its parent's placement.
Pose compose(const Pose& parent, const Transform& local) {
    return {parent.position + fromB3Pos(b3RotateVector(parent.rotation, vector(product(parent.scale, local.position)))),
            product(parent.scale, local.scale), b3MulQuat(parent.rotation, rotation(local.rotation))};
}
/// Resolves every node's world placement without depending on OGRE.
std::vector<Pose> poses(const LevelExpansion::Instance& instance) {
    std::vector<Pose> result;
    const auto base = compose(Pose{}, instance.transform);
    for (const auto& node : instance.expansion.nodes) {
        const auto parent = node.parent == Expansion::npos ? base : result.at(node.parent);
        result.push_back(node.node.transform ? compose(parent, *node.node.transform) : parent);
    }
    return result;
}
/// Resolves a node's parent's current placement, including a level scene transform.
Pose parentPose(const LevelExpansion::Instance& instance, std::size_t at) {
    std::vector<std::size_t> chain;
    for (auto p = instance.expansion.nodes[at].parent; p != Expansion::npos; p = instance.expansion.nodes[p].parent)
        chain.push_back(p);
    auto parent = compose(Pose{}, instance.transform);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (const auto& t = instance.expansion.nodes[*it].node.transform) parent = compose(parent, *t);
    return parent;
}
/// Detects a user/script teleport without waking resting bodies due to Euler roundoff.
bool moved(const Pose& a, const Pose& b) {
    const auto d = a.position - b.position;
    const auto dot = a.rotation.s*b.rotation.s + b3Dot(a.rotation.v,b.rotation.v);
    return d.x*d.x+d.y*d.y+d.z*d.z > 1.0e-10f || std::abs(dot) < 0.999999f;
}
/// Converts the public motion mode to the physics engine's native type.
b3BodyType bodyType(BodyType type) {
    return type == BodyType::Static ? b3_staticBody : type == BodyType::Kinematic ? b3_kinematicBody : b3_dynamicBody;
}
/// Builds a Box3D material from a component.
b3ShapeDef shapeDef(const Collider& c) {
    auto def = b3DefaultShapeDef();
    def.density = c.density;
    def.baseMaterial.friction = c.friction;
    def.baseMaterial.restitution = c.restitution;
    def.isSensor = c.sensor;
    return def;
}
}

struct PhysicsWorld::State {
    TaskPool tasks;
    bool serial = false, pending = false;
    double accumulator = 0;
    b3WorldId world;
    struct Body {
        std::size_t scene, node;
        b3BodyId id;
        Pose input;
        RigidBody settings;
        Transform localInput;
    };
    std::vector<Body> bodies;
    std::vector<std::unique_ptr<b3MeshData, decltype(&b3DestroyMesh)>> meshes;
    /// Installs the native task pool and creates a fresh simulation world.
    State() {
        auto def = b3DefaultWorldDef();
        def.gravity = {0,-9.81f,0};
        def.workerCount = tasks.size() + 1;
        def.userTaskContext = this;
        def.enqueueTask = [](b3TaskCallback* task, void* context, void* user, const char*) -> void* {
            auto& s = *static_cast<State*>(user);
            if (s.serial || !s.tasks.size()) { task(context); return nullptr; }
            return new std::future<void>(s.tasks.submit([task,context] { task(context); }));
        };
        def.finishTask = [](void* token, void* user) {
            std::unique_ptr<std::future<void>> future(static_cast<std::future<void>*>(token));
            static_cast<State*>(user)->tasks.finish(*future);
        };
        world = b3CreateWorld(&def);
        if (!b3World_IsValid(world)) throw std::runtime_error("Could not create Box3D world");
    }
    /// Destroys bodies before their shared collision mesh storage.
    ~State() { b3DestroyWorld(world); }
    /// Adds all shapes of a node to its body, in the body's scaled local space.
    void shapes(b3BodyId body, const SceneNode& node, Vec3 scale) {
        const auto valid = [&](float v) {
            if (!std::isfinite(v) || v < 0.001f) throw std::runtime_error(node.name + ": physics dimensions must be positive and finite");
            return v;
        };
        if (const auto& c = node.boxCollider) {
            const auto size = product(c->size, scale);
            const auto hull = b3MakeOffsetBoxHull(valid(size.x)*0.5f,valid(size.y)*0.5f,valid(size.z)*0.5f,vector(product(c->centre,scale)));
            auto def = shapeDef(*c); b3CreateHullShape(body,&def,&hull.base);
        }
        if (const auto& c = node.sphereCollider) {
            const b3Sphere sphere{vector(product(c->centre,scale)), valid(c->radius * (std::max)({scale.x,scale.y,scale.z}))};
            auto def = shapeDef(*c); b3CreateSphereShape(body,&def,&sphere);
        }
        if (const auto& c = node.capsuleCollider) {
            const auto radius = valid(c->radius * (std::max)(scale.x,scale.z));
            const auto half = (std::max)(0.0f, valid(c->height * scale.y)*0.5f - radius);
            const auto centre = product(c->centre,scale);
            const b3Capsule capsule{vector(centre + Vec3{0,-half,0}),vector(centre + Vec3{0,half,0}),radius};
            auto def = shapeDef(*c); b3CreateCapsuleShape(body,&def,&capsule);
        }
        if (const auto& c = node.planeCollider) {
            if (node.rigidBody && node.rigidBody->type == BodyType::Dynamic)
                throw std::runtime_error(node.name + ": PlaneCollider requires a static or kinematic body");
            const auto x = valid(c->width)*0.5f, z = valid(c->depth)*0.5f;
            b3Vec3 vertices[] = {{-x,0,-z},{x,0,-z},{x,0,z},{-x,0,z}};
            for (auto& v : vertices) v = vector(product(fromB3Pos(v)+c->centre,scale));
            int32_t indices[] = {0,2,1,0,3,2};
            b3MeshDef meshDef{};
            meshDef.vertices=vertices; meshDef.vertexCount=4; meshDef.indices=indices; meshDef.triangleCount=2;
            meshDef.identifyEdges=true;
            auto* mesh = b3CreateMesh(&meshDef,nullptr,0);
            if (!mesh) throw std::runtime_error(node.name + ": could not create PlaneCollider mesh");
            meshes.emplace_back(mesh,&b3DestroyMesh);
            auto def=shapeDef(*c);
            b3CreateMeshShape(body,&def,mesh,{1,1,1});
        }
    }
};

PhysicsWorld::PhysicsWorld() : state_(std::make_unique<State>()) {}
PhysicsWorld::~PhysicsWorld() = default;
void PhysicsWorld::setSerial(bool serial) { state_->serial = serial; }
std::size_t PhysicsWorld::bodyCount() const { return state_->bodies.size(); }

void PhysicsWorld::load(const LevelExpansion& expanded) {
    auto fresh = std::make_unique<State>();
    fresh->serial = state_->serial;
    for (std::size_t scene=0; scene<expanded.instances.size(); ++scene) {
        const auto& instance=expanded.instances[scene];
        const auto world=poses(instance);
        for (const auto at : instance.expansion.hierarchy()) {
            const auto& node=instance.expansion.nodes[at].node;
            if (!node.rigidBody && !node.boxCollider && !node.sphereCollider && !node.capsuleCollider && !node.planeCollider) continue;
            if (node.rigidBody && !node.transform) throw std::runtime_error(node.name + ": RigidBody requires Transform");
            const auto settings=node.rigidBody.value_or(RigidBody{BodyType::Static});
            auto def=b3DefaultBodyDef();
            def.type=bodyType(settings.type); def.position=toB3Pos(world[at].position); def.rotation=world[at].rotation;
            def.linearVelocity=vector(settings.linearVelocity); def.angularVelocity=vector(settings.angularVelocity*radians);
            def.linearDamping=settings.linearDamping; def.angularDamping=settings.angularDamping;
            def.gravityScale=settings.gravityScale; def.isBullet=settings.bullet;
            auto body=b3CreateBody(fresh->world,&def);
            fresh->shapes(body,node,world[at].scale);
            fresh->bodies.push_back({scene,at,body,world[at],settings,node.transform.value_or(Transform{})});
        }
    }
    state_=std::move(fresh);
}

void PhysicsWorld::prepare(const LevelExpansion& expanded) {
    for (auto& body : state_->bodies) {
        const auto& instance=expanded.instances.at(body.scene);
        const auto& node=instance.expansion.nodes.at(body.node).node;
        const auto world=compose(parentPose(instance,body.node),node.transform.value_or(Transform{}));
        if (moved(world,body.input)) b3Body_SetTransform(body.id,toB3Pos(world.position),world.rotation);
        if (world.scale != body.input.scale) {
            std::vector<b3ShapeId> shapes(b3Body_GetShapeCount(body.id));
            b3Body_GetShapes(body.id,shapes.data(),static_cast<int>(shapes.size()));
            for (auto shape:shapes) b3DestroyShape(shape,false);
            state_->shapes(body.id,node,world.scale);
        }
        if (node.rigidBody) {
            const auto& c=*node.rigidBody;
            if (c.linearVelocity != body.settings.linearVelocity) b3Body_SetLinearVelocity(body.id,vector(c.linearVelocity));
            if (c.angularVelocity != body.settings.angularVelocity) b3Body_SetAngularVelocity(body.id,vector(c.angularVelocity*radians));
            if (c.type != body.settings.type) b3Body_SetType(body.id,bodyType(c.type));
            b3Body_SetGravityScale(body.id,c.gravityScale);
            b3Body_SetLinearDamping(body.id,c.linearDamping);
            b3Body_SetAngularDamping(body.id,c.angularDamping);
            body.settings=c;
        }
        body.input=world;
        body.localInput=node.transform.value_or(Transform{});
    }
}

void PhysicsWorld::advance(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds<0) throw std::runtime_error("Physics delta time must be finite and nonnegative");
    state_->accumulator+=(std::min)(double(deltaSeconds),0.25);
    constexpr double step=1.0/60.0;
    while (state_->accumulator+1.0e-9>=step) {
        b3World_Step(state_->world,static_cast<float>(step),4);
        state_->accumulator-=step;
    }
    state_->pending=true;
}

void PhysicsWorld::apply(LevelExpansion& expanded) {
    if (!state_->pending) return;
    // Bodies were collected in hierarchy order, so children see their updated parent.
    for (auto& body : state_->bodies) {
        auto& instance=expanded.instances.at(body.scene);
        auto& node=instance.expansion.nodes.at(body.node).node;
        if (!node.rigidBody || node.rigidBody->type == BodyType::Static) continue;
        const auto parent=parentPose(instance,body.node);
        const bool teleported=node.transform->position!=body.localInput.position || node.transform->rotation!=body.localInput.rotation;
        if(!teleported) {
            const auto position=fromB3Pos(b3Body_GetPosition(body.id));
            const auto q=b3Body_GetRotation(body.id);
            const auto local=fromB3Pos(b3InvRotateVector(parent.rotation,vector(position-parent.position)));
            node.transform->position={local.x/parent.scale.x,local.y/parent.scale.y,local.z/parent.scale.z};
            node.transform->rotation=euler(b3InvMulQuat(parent.rotation,q));
            const auto previousScale=body.input.scale;
            body.input=compose(parent,*node.transform);
            body.input.scale=previousScale; // prepare must still observe scale edits.
            body.localInput=*node.transform;
        }
        const auto linear=fromB3Pos(b3Body_GetLinearVelocity(body.id));
        const auto angular=fromB3Pos(b3Body_GetAngularVelocity(body.id))*(1/radians);
        // Console commands made between frames must survive publication of the old result.
        if(node.rigidBody->linearVelocity==body.settings.linearVelocity) node.rigidBody->linearVelocity=linear;
        if(node.rigidBody->angularVelocity==body.settings.angularVelocity) node.rigidBody->angularVelocity=angular;
        body.settings.linearVelocity=linear;
        body.settings.angularVelocity=angular;
    }
    state_->pending=false;
}
}
