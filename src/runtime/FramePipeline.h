#pragma once

#include <functional>
#include <string>
#include <vector>

namespace throne {
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
