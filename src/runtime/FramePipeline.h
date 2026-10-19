#pragma once

#include <functional>
#include <array>
#include <string>
#include <vector>

namespace throne {
/// Built-in script phases and the methods selected by their cached queries.
inline constexpr std::array<const char*,3> behaviourSystems={
    "UpdateBehaviourSystem","LateUpdateBehaviourSystem","EndOfFrameBehaviourSystem"};
inline constexpr std::array<const char*,3> behaviourMethods={"update","lateupdate","onendofframe"};
/// Returns the built-in behaviour phase index, or -1 for another system.
inline int behaviourPhase(const std::string& name) {
    for(int i=0;i<3;++i) if(name==behaviourSystems[i]) return i;
    return -1;
}
/// One serial stage, or a group whose first member runs on the calling thread.
struct ExecutionStage {
    std::string name;
    std::vector<std::string> parallel;
};

/// The level's default execution order; engine stages retain these relative positions.
std::vector<ExecutionStage> defaultStages();
/// Checks unique names and the required engine barriers; throws on invalid schedules.
void validateStages(const std::vector<ExecutionStage>& stages);
/// Runs a frame, joining every worker before returning or propagating an exception.
/// Serial mode preserves the same barriers and callback inputs.
void runStages(const std::vector<ExecutionStage>& stages, bool serial,
               const std::function<void(const std::string&)>& run);
}
