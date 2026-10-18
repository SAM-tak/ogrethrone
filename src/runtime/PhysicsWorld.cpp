#include "PhysicsWorld.h"
#include "Box3DMath.h"

namespace throne {
PhysicsWorld::PhysicsWorld() {
    auto worldDef = b3DefaultWorldDef();
    worldDef.workerCount = tasks_.size() + 1;
    worldDef.userTaskContext = this;
    worldDef.enqueueTask = [](b3TaskCallback* task, void* context, void* user, const char*) -> void* {
        auto& self = *static_cast<PhysicsWorld*>(user);
        if (self.serial_ || !self.tasks_.size()) { task(context); return nullptr; }
        return new std::future<void>(self.tasks_.submit([task, context] { task(context); }));
    };
    worldDef.finishTask = [](void* token, void* user) {
        std::unique_ptr<std::future<void>> future(static_cast<std::future<void>*>(token));
        static_cast<PhysicsWorld*>(user)->tasks_.finish(*future);
    };
    world_ = b3CreateWorld(&worldDef);

    auto groundDef = b3DefaultBodyDef();
    groundDef.position = {0.0f, -0.5f, 0.0f};
    const auto ground = b3CreateBody(world_, &groundDef);
    const auto groundHull = b3MakeBoxHull(10.0f, 0.5f, 10.0f);
    auto shapeDef = b3DefaultShapeDef();
    b3CreateHullShape(ground, &shapeDef, &groundHull.base);

    auto bodyDef = b3DefaultBodyDef();
    bodyDef.type = b3_dynamicBody;
    bodyDef.position = {0.0f, 4.0f, 0.0f};
    box_ = b3CreateBody(world_, &bodyDef);
    const auto boxHull = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
    b3CreateHullShape(box_, &shapeDef, &boxHull.base);
}

PhysicsWorld::~PhysicsWorld() { b3DestroyWorld(world_); }
void PhysicsWorld::step() { b3World_Step(world_, 1.0f / 60.0f, 4); }
Vec3 PhysicsWorld::position() const { return fromB3Pos(b3Body_GetPosition(box_)); }
}
