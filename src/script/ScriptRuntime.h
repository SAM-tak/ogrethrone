#pragma once

#include "editor/SceneDocument.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace throne {
/// A script was rejected before Play, failed while running, or exceeded its frame budget.
/// The message lists every diagnostic, or the failure with its L^ traceback.
struct ScriptError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Runs a scene's L^ scripts for one Play session (TDDja/02).
///
/// Construction checks and compiles `scripts/main.lh` (when present) and every
/// object's script, then creates one `Behaviour` instance per scripted object.
/// Scripts see the `throne` module (Vec3, Object, find, log, time) and `std.math`.
/// Each session owns a fresh L^ program and machine, so edited scripts take
/// effect the next time Play starts.
class ScriptRuntime {
public:
    /// Receives text the scripts pass to throne.log.
    using Log = std::function<void(const std::string&)>;

    /// @param scene Document the scripts read and modify; must outlive the runtime.
    ///        Scripts never add or remove objects.
    /// @param root Project directory that script paths are relative to.
    /// @param log Destination of throne.log.
    /// @throws ScriptError when any script is missing, rejected or breaks the contract.
    ScriptRuntime(SceneDocument& scene, const std::filesystem::path& root, Log log);
    /// Releases the L^ machine and program.
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    /// Runs one frame: every `start` on the first call, then every `update`,
    /// main.lh first and then objects in document order.
    /// @throws ScriptError on a run-time failure or a frame over budget; the
    ///         runtime must not be used afterwards.
    void update(float deltaSeconds);

    /// Session state shared with the host functions; opaque outside ScriptRuntime.cpp.
    struct State;

private:
    std::unique_ptr<State> state_;
};
}
