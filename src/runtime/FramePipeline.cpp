#include "runtime/FramePipeline.h"
#include "runtime/TaskPool.h"
#include <exception>
#include <future>
#include <set>
#include <stdexcept>

namespace throne {
std::vector<ExecutionStage> defaultStages() {
    return {{"FetchInput", {}}, {"ApplyPhysics", {}}, {"Update", {}}, {"SyncRender", {}},
            {"Parallel", {"OGRE", "Box3D", "Animator"}}, {"SyncSimulation", {}}};
}

void validateStages(const std::vector<ExecutionStage>& stages) {
    if (stages.empty() || stages.front().name != "FetchInput")
        throw std::runtime_error("FetchInput must be the first stage");
    std::set<std::string> names;
    std::vector<std::string> engine;
    for (const auto& stage : stages) {
        if (stage.name.empty() || stage.name.size() > 255 || stage.name.find('\0') != std::string::npos || !names.insert(stage.name).second)
            throw std::runtime_error("Execution stage names must be nonempty and unique");
        if (stage.name == "FetchInput" || stage.name == "ApplyPhysics" || stage.name == "SyncRender" ||
            stage.name == "Parallel" || stage.name == "SyncSimulation") engine.push_back(stage.name);
        if (stage.name == "Parallel") {
            if (stage.parallel != std::vector<std::string>{"OGRE", "Box3D", "Animator"})
                throw std::runtime_error("Parallel must contain OGRE, Box3D, Animator in that order");
        } else if (!stage.parallel.empty()) throw std::runtime_error("Only the engine group may run in parallel");
        for (const auto& name : stage.parallel)
            if (!names.insert(name).second) throw std::runtime_error("Duplicate parallel member");
    }
    if (engine != std::vector<std::string>{"FetchInput", "ApplyPhysics", "SyncRender", "Parallel", "SyncSimulation"})
        throw std::runtime_error("Engine execution stages cannot be removed or reordered");
}

void runStages(const std::vector<ExecutionStage>& stages, bool serial,
               const std::function<void(const std::string&)>& run) {
    validateStages(stages);
    for (const auto& stage : stages) {
        if (stage.parallel.empty()) { run(stage.name); continue; }
        if (serial || std::thread::hardware_concurrency() < 3) {
            for (const auto& member : stage.parallel) run(member);
            continue;
        }
        static TaskPool drivers(2);
        std::vector<std::future<void>> workers;
        workers.reserve(stage.parallel.size() - 1);
        std::exception_ptr failure;
        try {
            for (std::size_t i = 1; i < stage.parallel.size(); ++i) {
                const auto member = stage.parallel[i];
                workers.push_back(drivers.submit([&, member] { run(member); }));
            }
            run(stage.parallel.front());
        } catch (...) { failure = std::current_exception(); }
        for (auto& worker : workers) {
            try { worker.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
        }
        if (failure) std::rethrow_exception(failure);
    }
}
}
