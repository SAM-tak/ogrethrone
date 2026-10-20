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
/// Simulated time one UpdateLoop pass advances; there is no variable-rate update.
inline constexpr double fixedStep=1.0/60.0;
/// Engine stages of the simulation column, in their required order. Stages before
/// EndOfFrameBehaviourSystem, except FetchInput, form the UpdateLoop.
inline constexpr std::array<const char*,6> simulationStages={
    "FetchInput",behaviourSystems[0],behaviourSystems[1],"Animation","Box3DStep",behaviourSystems[2]};
/// The rendering column; it always runs on the calling (main) thread.
inline constexpr std::array<const char*,3> renderStages={"PreRenderEventSystem","OGRE","PostRenderEventSystem"};

/// Whether @p name is a fixed engine stage or barrier rather than a user System.
bool isEngineStage(const std::string& name);
/// The level's default simulation column.
std::vector<std::string> defaultStages();
/// Checks unique names and the engine stages' order; user stages may sit anywhere after
/// FetchInput. Throws on invalid schedules.
void validateStages(const std::vector<std::string>& stages);

/// Wall times of one frame, in milliseconds.
struct FrameTimes {
    /// The simulation column, from FetchInput to its last stage.
    double simulationMs = 0;
    /// The main thread waiting for simulation after rendering.
    double waitMs = 0;
};
/// Runs one frame of @p stages (the simulation column) with @p steps UpdateLoop passes.
/// Serial: simulation, SyncSimulation, rendering, SyncRender; rendering shows this frame.
/// Otherwise: SyncSimulation, then rendering on this thread while simulation runs on a
/// worker, then SyncRender; rendering shows the previous frame. Every worker is joined
/// before returning or propagating an exception.
FrameTimes runFrame(const std::vector<std::string>& stages, bool serial, int steps,
                    const std::function<void(const std::string&)>& run);
}
