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

/// Method-query capabilities and priority defaults read from a fresh script instance.
struct BehaviourInfo {
    std::array<bool,3> methods{};
    bool start = false;
    BehaviourDefaults defaults;
    std::vector<FieldInfo> fields;
};

/// One ordered result of a BehaviourSystem query, including coroutine resumptions.
struct BehaviourMatch {
    int scene;
    std::size_t node, attachment;
    double priority;
    std::string script;
    bool waiting = false;
};

/// What a Play session runs.
enum class PlayScope {
    /// The open level: its systems and its scenes' systems, without main.lh.
    Level,
    /// The whole game: main.lh as well. main.lh is a script; with await^ at its top
    /// level it is a coroutine, resumed once per frame before the systems.
    Game,
};

/// Runs a world's L^ node Behaviours, direct system attachments.
/// Each Play checks and compiles its scripts, then constructs isolated Behaviour
/// instances for every expanded scene/prefab instance. VM calls run on one thread at a
/// time: the simulation column's, or the caller's between frames (console input).
class ScriptRuntime {
public:
    /// Receives text the scripts pass to throne.log, and warnings about field values.
    using Log = std::function<void(const std::string&)>;

    /// @param world The level (for its own systems); must outlive the runtime.
    /// @param expanded The world's scene instances, expanded; the scripts read and
    ///        modify these nodes, never the world itself, so re-expanding the world
    ///        undoes a session. Must outlive the runtime; spawned nodes are appended, never removed.
    /// @param root Project directory that script paths are relative to.
    /// @param log Destination of throne.log and of field warnings.
    /// @param scope Whether main.lh runs as well as the systems.
    /// @throws ScriptError when any script is missing, rejected or breaks the contract.
    ScriptRuntime(const World& world, LevelExpansion& expanded, const std::filesystem::path& root, Log log,
                  PlayScope scope);
    /// Releases the L^ machine and program.
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    /// Runs main.lh (Game scope) and one frame of the level schedule: FetchInput, one
    /// UpdateLoop pass per elapsed fixedStep (dt is fixedStep), then the stages from
    /// EndOfFrameBehaviourSystem on (dt is the time those passes advanced). BehaviourSystems
    /// query enabled methods in effective-priority order, with hierarchy/attachment ties.
    /// Each method owns its suspended invocation independently; start runs once.
    /// @throws ScriptError on a run-time failure or a frame over budget.
    FrameTimes update(float deltaSeconds);
    /// Also passes every stage, the render column and barriers included, to @p engine
    /// before its scripts run. The simulation column, scripts included, runs on a worker
    /// thread unless the level is serial; the render column stays on this thread.
    FrameTimes update(float deltaSeconds, const std::function<void(const std::string&)>& engine);
    /// Effective simulation column, including direct systems declared in scenes or prefabs.
    static std::vector<std::string> schedule(const World& world, const LevelExpansion& expanded);
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
    /// Returns editable fields of Behaviour, in name order.
    /// Editable fields require @export.
    /// @throws ScriptError when the script is missing, rejected or publishes no Behaviour.
    static std::vector<FieldInfo> fields(const std::filesystem::path& root, const std::string& script,
                                         std::string symbol = {});
    /// Reflects node callbacks and ordering defaults without starting a Play session.
    static BehaviourInfo inspectBehaviour(const std::filesystem::path& root,const std::string& script);
    /// Returns the cached execution order used by the named BehaviourSystem.
    std::vector<BehaviourMatch> behaviourMatches(const std::string& system) const;
    /// Marks externally edited expanded attachments dirty; script attachment APIs do this automatically.
    void refreshBehaviours();

    /// Session state shared with the host functions; opaque outside ScriptRuntime.cpp.
    struct State;

private:
    std::unique_ptr<State> state_;
};
}
