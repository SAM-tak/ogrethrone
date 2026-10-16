#pragma once

#include "editor/Level.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace throne {
/// A script was rejected before Play, failed while running, or exceeded its frame budget.
/// The message lists every diagnostic, or the failure with its L^ traceback.
struct ScriptError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// How the inspector edits a behaviour field.
enum class FieldKind { Number, Bool, String, SceneNode };

/// One editable field of a behaviour script (a `self^{ ... }` entry of its `Behaviour`).
struct FieldInfo {
    std::string name;
    FieldKind kind;
    /// The script's default; a SceneNode field defaults to no node.
    FieldValue defaultValue;
};

/// What a Play session runs.
enum class PlayScope {
    /// The open level: its runners and its scenes' runners, without main.lh.
    Level,
    /// The whole game: main.lh as well. main.lh's final form (the game's organizer,
    /// TDDja/02 4.1) is not settled; it currently runs as start/update hooks.
    Game,
};

/// Runs a world's L^ scripts for one Play session (TDDja/02, 05).
///
/// Construction checks and compiles `main.lh` (when present and the scope is Game) and every
/// behaviour script, then creates one `Behaviour` instance per behaviour of each
/// runner and applies the runner's field values to it. Scripts see the `throne`
/// module (Vec3, SceneNode, Runner, find, log, time) and `std.math`. Each session
/// owns a fresh L^ program and machine, so edited scripts take effect the next
/// time Play starts.
class ScriptRuntime {
public:
    /// Receives text the scripts pass to throne.log, and warnings about field values.
    using Log = std::function<void(const std::string&)>;

    /// @param world Level and scenes the scripts read and modify; must outlive the runtime.
    ///        Scripts never add or remove nodes, runners or scenes.
    /// @param root Project directory that script paths are relative to.
    /// @param log Destination of throne.log and of field warnings.
    /// @param scope Whether main.lh runs as well as the runners.
    /// @throws ScriptError when any script is missing, rejected or breaks the contract.
    ScriptRuntime(World& world, const std::filesystem::path& root, Log log, PlayScope scope);
    /// Releases the L^ machine and program.
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    /// Runs one frame: every `start` on the first call, then every `update`, in the
    /// order main.lh, scene runners (scene by scene), level runners.
    /// @throws ScriptError on a run-time failure or a frame over budget; the
    ///         runtime must not be used afterwards.
    void update(float deltaSeconds);

    /// Returns the host API scripts can use (the `throne` module and `std.math`) as
    /// the JSON the L^ language server reads from `lhat-host.json`.
    static std::string hostApi();
    /// Returns the editable fields of the `Behaviour` published by @p script, in
    /// name order. Fields of other types are not listed.
    /// @throws ScriptError when the script is missing, rejected or publishes no Behaviour.
    static std::vector<FieldInfo> fields(const std::filesystem::path& root, const std::string& script);

    /// Session state shared with the host functions; opaque outside ScriptRuntime.cpp.
    struct State;

private:
    std::unique_ptr<State> state_;
};
}
