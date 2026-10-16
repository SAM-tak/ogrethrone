#include "PhysicsWorld.h"

namespace throne {
PhysicsWorld::PhysicsWorld() {
    auto worldDef = b3DefaultWorldDef();
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
b3Pos PhysicsWorld::position() const { return b3Body_GetPosition(box_); }
}
