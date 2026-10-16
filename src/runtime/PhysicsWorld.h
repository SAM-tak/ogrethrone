#pragma once

#include <box3d/box3d.h>

namespace throne {
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void step();
    b3Pos position() const;

private:
    b3WorldId world_;
    b3BodyId box_;
};
}
