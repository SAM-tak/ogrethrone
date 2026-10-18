#pragma once

#include "editor/Expansion.h"
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
    /// The whole game: main.lh as well. main.lh is a script; with await^ at its top
    /// level it is a coroutine, resumed once per frame before the runners.
    Game,
};

/// Runs a world's L^ node Behaviours, direct runner attachments and Systems.
/// Each Play checks and compiles its scripts, then constructs isolated Behaviour
/// and Component instances for every expanded scene/prefab instance. All VM calls
/// stay on the calling thread; native engine callbacks may run in the parallel group.
class ScriptRuntime {
public:
    /// Receives text the scripts pass to throne.log, and warnings about field values.
    using Log = std::function<void(const std::string&)>;

    /// @param world The level (for its own runners); must outlive the runtime.
    /// @param expanded The world's scene instances, expanded; the scripts read and
    ///        modify these nodes, never the world itself, so re-expanding the world
    ///        undoes a session. Must outlive the runtime; spawned nodes are appended, never removed.
    /// @param root Project directory that script paths are relative to.
    /// @param log Destination of throne.log and of field warnings.
    /// @param scope Whether main.lh runs as well as the runners.
    /// @throws ScriptError when any script is missing, rejected or breaks the contract.
    ScriptRuntime(const World& world, LevelExpansion& expanded, const std::filesystem::path& root, Log log,
                  PlayScope scope);
    /// Releases the L^ machine and program.
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    /// Runs main.lh (Game scope) and the level schedule. Within a user stage,
    /// direct attachments precede node attachments in scene-instance/hierarchy order.
    /// A start runs once, before its attachment's first update in its selected stage.
    /// @throws ScriptError on a run-time failure or a frame over budget.
    void update(float deltaSeconds);
    /// Runs engine callbacks at the same barriers as script stages; OGRE stays on this thread.
    void update(float deltaSeconds, const std::function<void(const std::string&)>& engine);
    /// Effective level schedule, including direct runners declared in scenes or prefabs.
    static std::vector<ExecutionStage> schedule(const World& world, const LevelExpansion& expanded);
    /// True once main.lh has run to its end (Game scope only).
    bool finished() const;

    /// What one console line gave.
    struct ConsoleAnswer {
        /// The input is not whole yet; the next line continues it.
        bool more = false;
        /// Diagnostics or the run error, then the answer when there is one; may be empty.
        std::string text;
    };
    /// Feeds @p line to this session's L^ prompt (TDDja/02): the names an input
    /// binds stay for the next, and an input runs on the session's machine with
    /// the frame budget, between frames. A refused or failed input is reported,
    /// never thrown.
    ConsoleAnswer evaluate(const std::string& line);
    /// Throws away a console input being continued.
    void dropInput();

    /// Returns the host API scripts can use (the `throne` module and `std.math`) as
    /// the JSON the L^ language server reads from `lhat-host.json`.
    static std::string hostApi();
    /// Returns editable fields of Behaviour, System or Component, in name order.
    /// Behaviour/System fields require @export; Component data fields do not.
    /// @throws ScriptError when the script is missing, rejected or publishes no Behaviour.
    static std::vector<FieldInfo> fields(const std::filesystem::path& root, const std::string& script,
                                         std::string symbol = {});

    /// Session state shared with the host functions; opaque outside ScriptRuntime.cpp.
    struct State;

private:
    std::unique_ptr<State> state_;
};
}
