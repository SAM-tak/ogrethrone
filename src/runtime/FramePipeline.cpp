#include "runtime/FramePipeline.h"
#include "runtime/TaskPool.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <set>
#include <stdexcept>

namespace throne {
bool isEngineStage(const std::string& name) {
    const auto in = [&](const auto& list) { return std::find(list.begin(), list.end(), name) != list.end(); };
    return in(simulationStages) || in(renderStages) || name == "SyncSimulation" || name == "SyncRender";
}

std::vector<std::string> defaultStages() { return {simulationStages.begin(), simulationStages.end()}; }

void validateStages(const std::vector<std::string>& stages) {
    std::set<std::string> names;
    std::vector<std::string> engine;
    for (const auto& name : stages) {
        if (name.empty() || name.size() > 255 || name.find('\0') != std::string::npos || !names.insert(name).second)
            throw std::runtime_error("Execution stage names must be nonempty and unique");
        if (isEngineStage(name)) engine.push_back(name);
    }
    if (engine != defaultStages() || stages.front() != simulationStages.front())
        throw std::runtime_error("FetchInput comes first; engine and BehaviourSystem stages cannot be removed or reordered");
}

FrameTimes runFrame(const std::vector<std::string>& stages, bool serial, int steps,
                    const std::function<void(const std::string&)>& run) {
    using Clock = std::chrono::steady_clock;
    const auto since = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    validateStages(stages);
    FrameTimes times;
    const auto loopEnd = std::find(stages.begin(), stages.end(), behaviourSystems[2]);
    const auto simulate = [&] {
        const auto start = Clock::now();
        run(stages.front());
        for (int i = 0; i < steps; ++i)
            for (auto stage = stages.begin() + 1; stage != loopEnd; ++stage) run(*stage);
        for (auto stage = loopEnd; stage != stages.end(); ++stage) run(*stage);
        times.simulationMs = since(start);
    };
    const auto render = [&] { for (const char* stage : renderStages) run(stage); };
    if (serial) {
        simulate();
        run("SyncSimulation");
        render();
    } else {
        run("SyncSimulation");
        if (std::thread::hardware_concurrency() < 2) {
            render();
            simulate();
        } else {
            static TaskPool driver(1);
            auto worker = driver.submit(simulate);
            std::exception_ptr failure;
            try { render(); } catch (...) { failure = std::current_exception(); }
            const auto wait = Clock::now();
            try { worker.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
            times.waitMs = since(wait);
            if (failure) std::rethrow_exception(failure);
        }
    }
    run("SyncRender");
    return times;
}
}
