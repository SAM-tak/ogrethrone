#pragma once
#include "audio/AudioSystem.h"
#include "audio/Reflections.h"
#include "editor/Expansion.h"
#include <array>
#include <functional>
#include <map>
#include <memory>

namespace throne {
/// Scene geometry and raycast simulation, used only by the simulation thread.
class AcousticWorld {
public:
    using Key=std::pair<std::size_t,std::size_t>;
    struct Pose {
        Vec3 position, scale{1,1,1};
        std::array<Vec3,3> axes{{{1,0,0},{0,1,0},{0,0,1}}};
        Vec3 rotate(Vec3 v) const { return axes[0]*v.x+axes[1]*v.y+axes[2]*v.z; }
    };
    struct Source { Key key; Pose pose; bool transmission=true; std::string identity; };
    struct Direct { float occlusion=1; Vec3 transmission{1,1,1}; };
    using Log=std::function<void(const std::string&)>;
    AcousticWorld(AudioSystem& audio,std::filesystem::path root,Log log);
    ~AcousticWorld();
    void syncGeometry(const LevelExpansion& world,const std::vector<std::vector<Pose>>& poses);
    std::map<Key,Direct> simulate(const Pose& listener,const std::vector<Source>& sources);
    /// Polls completed work and schedules at most one background job (10 Hz); never waits.
    std::map<Key,std::shared_ptr<const ReflectionResult>> reflections(const Pose& listener,const std::vector<Source>& sources);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
