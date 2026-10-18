#include "runtime/PhysicsWorld.h"
extern "C" {
#include <lhat.h>
}
#include <array>
#include <cmath>
#include <cstdio>

int main() {
    auto* machine = lhat_machine_new();
    if (!machine) return 1;
    lhat_machine_dispose(machine);

    std::array<throne::Vec3, 180> first;
    {
        throne::PhysicsWorld world;
        for (auto& position : first) {
            world.step();
            position = world.position();
        }
    }
    throne::PhysicsWorld second;
    second.setSerial(true);
    for (const auto& expected : first) {
        second.step();
        if (second.position() != expected) {
            std::fputs("Serial and parallel simulation diverged\n", stderr);
            return 1;
        }
    }
    const auto final = second.position();
    if (!std::isfinite(final.y) || std::abs(final.y - 0.5) > 0.1 || first.front().y <= final.y) {
        std::fputs("Box did not fall and settle on the ground\n", stderr);
        return 1;
    }
    std::printf("Box3D replay matched 180 steps; settled y=%.6f; L^ VM initialized\n", double(final.y));
}
