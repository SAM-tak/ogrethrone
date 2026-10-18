#include "script/ScriptRuntime.h"
#include <lhat.h>
#include <lhat/prompt.h>
#include <stdlib/math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>
#include <vector>

namespace throne {
namespace {
constexpr const char* moduleName = "throne";
constexpr const char* mainPath = "main.lh";
/// Loop turns one frame may take before it is treated as an endless loop.
constexpr std::int64_t frameBudget = 10'000'000;

using Machine = std::unique_ptr<LhatMachine, decltype(&lhat_machine_dispose)>;
using Program = std::unique_ptr<LhatProgram, decltype(&lhat_program_free)>;

/// One behaviour of one runner: its instance and the throne.Runner handle passed to it.
struct Instance {
    std::string what;
    LhatValue instance, runner;
    bool start, update;
};

/// A behaviour waiting to be instantiated, with where it came from.
struct Pending {
    /// Index of the runner's scene instance in LevelExpansion::instances, or -1 for a level runner.
    int scene;
    /// The prefab instance the runner came from; its NodeRefs are relative to it.
    NodePath prefix;
    std::size_t runner;
    const Behaviour* behaviour;
    std::string what;
};

/// Returns a number argument as a double, accepting integers and reals.
double number(LhatValue value) {
    return lhat_is_integer(value) ? static_cast<double>(lhat_as_integer(value)) : lhat_as_real(value);
}

/// Returns the bytes of a string argument.
std::string_view text(LhatValue value) {
    const auto* string = reinterpret_cast<const LhatString*>(lhat_as_object(value));
    return {string->text, string->length};
}

/// Returns the component storage of one of a node's vector properties (0-3).
float* property(SceneNode& node, int which) {
    switch (which) {
    case 0: return node.position.data();
    case 1: return node.rotation.data();
    case 2: return node.scale.data();
    default: return node.colour.data();
    }
}

/// The L^ type a field value has to fit, as conformance text.
const char* fieldType(const FieldValue& value) {
    switch (value.index()) {
    case 0: return "number^";
    case 1: return "bool^";
    case 2: return "string^";
    default: return "throne.SceneNode|nil^";
    }
}
}

struct ScriptRuntime::State {
    const World& world;
    LevelExpansion& expanded;
    std::filesystem::path root;
    Log log;
    Program program{nullptr, &lhat_program_free};
    // Declared after the program so that it is disposed first.
    Machine machine{nullptr, &lhat_machine_dispose};
    /// The console's prompt, made on first use; disposed before the machine.
    std::unique_ptr<LhatPrompt, decltype(&lhat_prompt_dispose)> prompt{nullptr, &lhat_prompt_dispose};
    const LhatHostValueTag* vec3 = nullptr;
    const LhatHostDataTag *sceneNode = nullptr, *runner = nullptr;
    // Values the host holds are not roots, so everything kept between calls
    // also lives in the machine's host root table.
    LhatTable* roots = nullptr;
    LhatInteger rootCount = 0;
    /// main.lh, run on the first frame (Game scope only).
    const LhatProto* mainProto = nullptr;
    /// main.lh's coroutine while it is suspended between frames.
    LhatValue game = lhat_nil();
    bool started = false, finished = false;
    std::vector<Instance> instances;
    /// Runners in run order; a throne.Runner handle is an index into this.
    std::vector<const Runner*> runners;
    double time = 0;

    State(const World& world, LevelExpansion& expanded, std::filesystem::path root, Log log)
        : world(world), expanded(expanded), root(std::move(root)), log(std::move(log)) {}

    /// Keeps @p value alive for the session and returns it.
    LhatValue keep(LhatValue value) {
        bool refused = false;
        if (!lhat_machine_table_set(machine.get(), roots, lhat_integer(rootCount++), value, &refused) ||
            refused)
            throw ScriptError("Out of memory");
        return value;
    }

    /// Returns a handle of @p tag holding @p bits.
    LhatValue handle(LhatMachine* on, const LhatHostDataTag* tag, std::uintptr_t bits) {
        LhatValue result;
        if (!lhat_machine_make_hostdata(on, tag, reinterpret_cast<void*>(bits), &result)) return lhat_nil();
        return result;
    }

    /// A throne.SceneNode handle: the scene's index plus one, and the node ID.
    static std::uintptr_t nodeBits(std::size_t scene, std::size_t node) {
        return (static_cast<std::uintptr_t>(scene + 1) << 32) | node;
    }

    /// Returns the node a throne.SceneNode handle names, or nullptr.
    SceneNode* resolve(LhatValue value) {
        const auto bits = reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(value, sceneNode));
        const auto scene = static_cast<std::size_t>(bits >> 32);
        if (scene == 0 || scene > expanded.instances.size()) return nullptr;
        auto& nodes = expanded.instances[scene - 1].expansion.nodes;
        const auto node = static_cast<std::size_t>(bits & 0xFFFFFFFFu);
        return node < nodes.size() ? &nodes[node].node : nullptr;
    }

    /// Returns the node a handle names, or panics and answers null.
    SceneNode* target(LhatMachine* running, LhatValue value) {
        if (auto* node = resolve(value)) return node;
        lhat_machine_panic_text(running, "throne.SceneNode refers to a node that is gone; check valid() first");
        return nullptr;
    }

    /// Runs @p result's call to completion or throws with what went wrong.
    LhatValue check(LhatRunResult result, const std::string& what) {
        if (result.status == LHAT_RUN_OK) return result.value;
        if (result.status == LHAT_RUN_SUSPENDED)
            throw ScriptError(what + ": exceeded the per-frame budget (endless loop?)");
        std::string message = what + ": " + lhat_run_status_message(result.status);
        if (result.status == LHAT_RUN_PANIC) {
            std::string value(lhat_value_text(result.value, nullptr, 0) + 1, '\0');
            lhat_value_text(result.value, value.data(), value.size());
            value.pop_back();
            message += ": " + value;
        }
        std::string trace(lhat_machine_traceback(machine.get(), nullptr, 0) + 1, '\0');
        lhat_machine_traceback(machine.get(), trace.data(), trace.size());
        trace.pop_back();
        if (!trace.empty()) message += "\n" + trace;
        throw ScriptError(message);
    }

    /// Calls @p name on @p receiver with @p arguments.
    void call(LhatValue receiver, const char* name, const LhatValue* arguments, std::size_t count,
              const std::string& what) {
        check(lhat_machine_call_member(machine.get(), receiver, name, std::strlen(name), arguments, count),
              what + " " + name);
    }

    /// Answers with @p value as a throne.Vec3.
    void answer(LhatValue* answers, int* count, const float* value) {
        if (lhat_make_hostvalue(machine.get(), vec3, value, &answers[0])) *count = 1;
    }
};

namespace {
using State = ScriptRuntime::State;

/// Reads a script for the L^ loader, refusing paths outside the project root.
char* loadScript(void* context, const char* path, std::size_t* length) {
    const auto& state = *static_cast<const State*>(context);
    const auto relative = std::filesystem::u8path(path).lexically_normal();
    if (relative.is_absolute() || relative.has_root_name() || (!relative.empty() && *relative.begin() == ".."))
        return nullptr;
    std::ifstream stream(state.root / relative, std::ios::binary);
    if (!stream) return nullptr;
    const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    auto* copy = static_cast<char*>(lhat_alloc(bytes.size() + 1));
    if (!copy) return nullptr;
    std::memcpy(copy, bytes.c_str(), bytes.size() + 1);
    *length = bytes.size();
    return copy;
}

/// The State behind a host call.
State& state(void* context) { return *static_cast<State*>(context); }

/// Reads a throne.Vec3 argument.
Vec3 vec3(State& s, LhatValue value) {
    Vec3 result;
    std::memcpy(result.data(), lhat_hostvalue_data(value, s.vec3), sizeof result);
    return result;
}

/// Defines a host function whose body sees `s` (the State), `machine`, `arguments`,
/// `answers` and `count`. The body is variadic because it may contain commas.
#define THRONE_HOST(name, ...)                                                                             \
    void name(LhatMachine* machine, void* context, const LhatValue* arguments, std::size_t, LhatValue* answers, \
              int* count) {                                                                                \
        auto& s = state(context);                                                                          \
        (void)machine, (void)arguments, (void)answers, (void)count, (void)s;                               \
        __VA_ARGS__                                                                                        \
    }

/// throne.vec3(x, y, z).
THRONE_HOST(vecNew, {
    const float value[3] = {static_cast<float>(number(arguments[0])), static_cast<float>(number(arguments[1])),
                            static_cast<float>(number(arguments[2]))};
    s.answer(answers, count, value);
})
/// Vec3 + Vec3.
THRONE_HOST(vecAdd, { s.answer(answers, count, (vec3(s, arguments[0]) + vec3(s, arguments[1])).data()); })
/// Vec3 - Vec3.
THRONE_HOST(vecSub, { s.answer(answers, count, (vec3(s, arguments[0]) - vec3(s, arguments[1])).data()); })
/// -Vec3.
THRONE_HOST(vecNeg, { s.answer(answers, count, (vec3(s, arguments[0]) * -1.0f).data()); })
/// Vec3 * number.
THRONE_HOST(vecScale, {
    s.answer(answers, count, (vec3(s, arguments[0]) * static_cast<float>(number(arguments[1]))).data());
})
/// Vec3 / number.
THRONE_HOST(vecDivide, {
    s.answer(answers, count, (vec3(s, arguments[0]) * static_cast<float>(1 / number(arguments[1]))).data());
})
/// Vec3.dot(other).
THRONE_HOST(vecDot, {
    const auto a = vec3(s, arguments[0]), b = vec3(s, arguments[1]);
    answers[0] = lhat_real(double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z);
    *count = 1;
})
/// Vec3.cross(other).
THRONE_HOST(vecCross, {
    const auto a = vec3(s, arguments[0]), b = vec3(s, arguments[1]);
    const Vec3 result{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    s.answer(answers, count, result.data());
})
/// Vec3.length().
THRONE_HOST(vecLength, {
    const auto a = vec3(s, arguments[0]);
    answers[0] = lhat_real(std::sqrt(double(a.x) * a.x + double(a.y) * a.y + double(a.z) * a.z));
    *count = 1;
})
/// Vec3.normalized(); a zero vector stays zero.
THRONE_HOST(vecNormalized, {
    const auto a = vec3(s, arguments[0]);
    const float length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    s.answer(answers, count, (length > 0 ? a * (1 / length) : a).data());
})

/// SceneNode.id().
THRONE_HOST(nodeId, {
    if (const auto* node = s.target(machine, arguments[0])) {
        answers[0] = lhat_integer(static_cast<LhatInteger>(node->id));
        *count = 1;
    }
})
/// SceneNode.name().
THRONE_HOST(nodeName, {
    if (const auto* node = s.target(machine, arguments[0]))
        if (lhat_machine_make_string(machine, node->name.data(), node->name.size(), &answers[0])) *count = 1;
})
/// SceneNode.valid(): whether the node is still in a loaded scene.
THRONE_HOST(nodeValid, {
    answers[0] = lhat_bool(s.resolve(arguments[0]) != nullptr);
    *count = 1;
})
/// Reads local vector property @p Which of the receiver.
template <int Which>
THRONE_HOST(nodeGet, {
    if (auto* node = s.target(machine, arguments[0])) s.answer(answers, count, property(*node, Which));
})
/// Writes local vector property @p Which of the receiver, keeping scale and colour in range.
template <int Which>
THRONE_HOST(nodeSet, {
    auto* node = s.target(machine, arguments[0]);
    if (!node) return;
    auto value = vec3(s, arguments[1]);
    for (float* component : {&value.x, &value.y, &value.z}) {
        if (!std::isfinite(*component)) {
            lhat_machine_panic_text(machine, "throne.SceneNode property must be finite");
            return;
        }
        *component = std::clamp(*component, Which == 2 ? 0.001f : Which == 3 ? 0.0f : -1.0e6f,
                                Which == 3 ? 1.0f : 1.0e6f);
    }
    std::memcpy(property(*node, Which), value.data(), sizeof value);
})

/// Runner.name().
THRONE_HOST(runnerName, {
    const auto index = reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(arguments[0], s.runner));
    const auto& name = s.runners.at(index)->name;
    if (lhat_machine_make_string(machine, name.data(), name.size(), &answers[0])) *count = 1;
})

/// throne.find(name): the first scene node with that name, scene by scene, or nil^.
THRONE_HOST(find, {
    const auto name = text(arguments[0]);
    answers[0] = lhat_nil();
    *count = 1;
    for (std::size_t scene = 0; scene < s.expanded.instances.size(); ++scene) {
        const auto& nodes = s.expanded.instances[scene].expansion.nodes;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].node.name == name) {
                answers[0] = s.handle(machine, s.sceneNode, State::nodeBits(scene, i));
                return;
            }
    }
})
/// throne.log(text).
THRONE_HOST(log, { s.log(std::string(text(arguments[0]))); })
/// throne.time(): seconds since Play started.
THRONE_HOST(time, {
    answers[0] = lhat_real(s.time);
    *count = 1;
})
#undef THRONE_HOST

/// Registers the throne module: Vec3, SceneNode, Runner and the free functions.
bool registerThrone(LhatProgram* program, State& s) {
    s.vec3 = lhat_register_hostvalue_type(program, moduleName, "Vec3", sizeof(Vec3));
    s.sceneNode = lhat_register_hostdata_type(program, moduleName, "SceneNode");
    s.runner = lhat_register_hostdata_type(program, moduleName, "Runner");
    if (!s.vec3 || !s.sceneNode || !s.runner) return false;
    const char* axes[] = {"x", "y", "z"};
    for (std::size_t i = 0; i < 3; ++i)
        if (!lhat_register_hostvalue_field(program, moduleName, "Vec3", axes[i], i * sizeof(float), LHAT_HVFIELD_F32))
            return false;
    struct Entry {
        const char *type, *name, *signature;
        LhatHostFn call;
    };
    // Getters are f^ (they change nothing); setters and log have effects, so they are p^.
    const Entry entries[] = {
        {"Vec3", "+", "f^self^, throne.Vec3 -> throne.Vec3;", vecAdd},
        {"Vec3", "-", "f^self^, throne.Vec3 -> throne.Vec3;", vecSub},
        {"Vec3", "-", "f^self^ -> throne.Vec3;", vecNeg},
        {"Vec3", "*", "f^self^, number^ -> throne.Vec3;", vecScale},
        {"Vec3", "/", "f^self^, number^ -> throne.Vec3;", vecDivide},
        {"Vec3", "dot", "f^self^, throne.Vec3 -> number^;", vecDot},
        {"Vec3", "cross", "f^self^, throne.Vec3 -> throne.Vec3;", vecCross},
        {"Vec3", "length", "f^self^ -> number^;", vecLength},
        {"Vec3", "normalized", "f^self^ -> throne.Vec3;", vecNormalized},
        {"SceneNode", "id", "f^self^ -> number^;", nodeId},
        {"SceneNode", "name", "f^self^ -> string^;", nodeName},
        {"SceneNode", "valid", "f^self^ -> bool^;", nodeValid},
        {"SceneNode", "position", "f^self^ -> throne.Vec3;", nodeGet<0>},
        {"SceneNode", "rotation", "f^self^ -> throne.Vec3;", nodeGet<1>},
        {"SceneNode", "scale", "f^self^ -> throne.Vec3;", nodeGet<2>},
        {"SceneNode", "colour", "f^self^ -> throne.Vec3;", nodeGet<3>},
        {"SceneNode", "setPosition", "p^self^, throne.Vec3;", nodeSet<0>},
        {"SceneNode", "setRotation", "p^self^, throne.Vec3;", nodeSet<1>},
        {"SceneNode", "setScale", "p^self^, throne.Vec3;", nodeSet<2>},
        {"SceneNode", "setColour", "p^self^, throne.Vec3;", nodeSet<3>},
        {"Runner", "name", "f^self^ -> string^;", runnerName},
        {nullptr, "vec3", "f^number^, number^, number^ -> throne.Vec3;", vecNew},
        {nullptr, "find", "f^string^ -> throne.SceneNode|nil^;", find},
        {nullptr, "log", "p^string^;", log},
        {nullptr, "time", "f^ -> number^;", time},
    };
    for (const auto& entry : entries) {
        const std::string_view type = entry.type ? entry.type : "";
        const bool ok = type.empty() ? lhat_register_func(program, moduleName, entry.name, entry.signature, entry.call, &s)
                        : type == "Vec3" ? lhat_register_hostvalue_member(program, moduleName, entry.type, entry.name,
                                                                          entry.signature, entry.call, &s)
                                         : lhat_register_member(program, moduleName, entry.type, entry.name,
                                                                entry.signature, entry.call, &s);
        if (!ok) return false;
    }
    return true;
}

/// Appends every diagnostic the program and its units reported to @p out.
void diagnostics(const LhatProgram* program, std::string& out) {
    for (std::size_t i = 0; i < lhat_program_diagnostic_count(program); ++i) {
        const auto* diagnostic = lhat_program_diagnostic(program, i);
        out += std::string(diagnostic->path ? diagnostic->path : "(program)") + ": " +
               lhat_program_error_message(diagnostic->code) + "\n";
    }
    for (const auto* unit = lhat_program_units(program); unit; unit = lhat_unit_next(unit)) {
        for (std::size_t i = 0; i < lhat_unit_diagnostic_count(unit); ++i) {
            std::string line(lhat_unit_diagnostic_write(unit, i, false, nullptr, 0) + 1, '\0');
            lhat_unit_diagnostic_write(unit, i, false, line.data(), line.size());
            line.pop_back();
            out += line + "\n";
        }
    }
}

/// Whether @p unit publishes @p name. Read from the export list, because the
/// typed descriptor exists only after compilation.
bool publishes(const LhatUnit* unit, std::string_view name) {
    for (std::size_t i = 0; i < lhat_unit_export_count(unit); ++i) {
        const auto exported = lhat_unit_export_name(unit, i);
        if (std::string_view(exported.text, exported.length) == name) return true;
    }
    return false;
}

/// Whether the instances of @p unit's Behaviour have a member @p name of type @p type.
bool behaviourHas(const LhatUnit* unit, const std::string& name, const char* type) {
    return lhat_unit_export_conforms(unit, "Behaviour", ("t^{ self^{ " + name + " : " + type + " } }").c_str());
}

/// Creates @p s's program with everything scripts may name registered.
void createProgram(State& s) {
    s.program.reset(lhat_program_new(true, loadScript, &s));
    if (!s.program || !lhatstdlib_math_register(s.program.get()) || !registerThrone(s.program.get(), s))
        throw ScriptError("Could not set up the L^ program");
}

/// Checks @p path as a behaviour script, adding contract problems to @p errors.
const LhatUnit* checkBehaviour(State& s, const std::string& path, std::string& errors) {
    const auto* unit = lhat_program_check(s.program.get(), path.c_str());
    if (!unit || !lhat_unit_ok(unit)) return unit;
    const auto problem = [&](const char* text) { errors += path + ": " + text + "\n"; };
    if (!lhat_unit_module_name(unit)) problem("must declare module^");
    if (!publishes(unit, "Behaviour")) {
        problem("must publish Behaviour = def^{ ... }");
        return unit;
    }
    if (!lhat_unit_export_conforms(unit, "Behaviour", "t^{ new : f^ -> any^; }")) problem("Behaviour must be a def^");
    if (behaviourHas(unit, "start", "any^") && !behaviourHas(unit, "start", "p^self^, throne.Runner;"))
        problem("Behaviour.start must be p^self^, throne.Runner;");
    if (behaviourHas(unit, "update", "any^") && !behaviourHas(unit, "update", "p^self^, throne.Runner, number^;"))
        problem("Behaviour.update must be p^self^, throne.Runner, number^;");
    return unit;
}

/// Compiles the checked program, throwing with the compiler's message on failure.
void compile(State& s) {
    auto* program = s.program.get();
    if (lhat_program_compile(program)) return;
    const auto failure = lhat_program_compile_failure(program, nullptr);
    std::string message(lhat_compile_message_write(program, &failure, nullptr, 0) + 1, '\0');
    lhat_compile_message_write(program, &failure, message.data(), message.size());
    message.pop_back();
    throw ScriptError(message);
}

/// Creates the machine, the root table and the installed registrations.
void startMachine(State& s) {
    s.machine.reset(lhat_machine_new());
    if (!s.machine) throw ScriptError("Out of memory");
    s.roots = lhat_machine_host_root(s.machine.get());
    lhat_program_install(s.program.get(), s.machine.get());
    lhat_machine_set_budget(s.machine.get(), frameBudget);
}

/// Runs @p unit and returns its published Behaviour definition, kept alive.
LhatValue definition(State& s, const LhatUnit* unit, const std::string& path) {
    const auto exports = s.check(lhat_run(s.machine.get(), lhat_unit_proto(unit)), path);
    return s.keep(lhat_table_get_bytes(reinterpret_cast<const LhatTable*>(lhat_as_object(exports)), "Behaviour", 9));
}
}

std::string ScriptRuntime::hostApi() {
    World none;
    LevelExpansion nothing;
    State s(none, nothing, {}, {});
    createProgram(s);
    std::string json(lhat_program_dump_host_api(s.program.get(), nullptr, 0) + 1, '\0');
    lhat_program_dump_host_api(s.program.get(), json.data(), json.size());
    json.pop_back();
    return json;
}

std::vector<FieldInfo> ScriptRuntime::fields(const std::filesystem::path& root, const std::string& script) {
    World none;
    LevelExpansion nothing;
    State s(none, nothing, root, {});
    createProgram(s);
    std::string errors;
    const auto* unit = checkBehaviour(s, script, errors);
    diagnostics(s.program.get(), errors);
    if (!errors.empty()) throw ScriptError(errors);
    compile(s);
    const auto* type = lhat_unit_export_type(unit, "Behaviour");
    if (!type || !type->instance) throw ScriptError(script + ": Behaviour must be a def^");
    // The defaults are what a fresh instance holds.
    startMachine(s);
    const auto instance = s.keep(s.check(
        lhat_machine_call_member(s.machine.get(), definition(s, unit, script), "new", 3, nullptr, 0), script + " new"));
    const auto* table = reinterpret_cast<const LhatTable*>(lhat_as_object(instance));
    std::vector<FieldInfo> result;
    for (std::size_t i = 0; i < type->instance->member_count; ++i) {
        const auto& member = type->instance->members[i];
        if (!member.type) continue;
        const std::string name(member.name->text, member.name->length);
        const auto value = lhat_table_get_bytes(table, name.data(), name.size());
        bool node = member.type->kind == LHAT_TYPE_RT_HOSTDATA && member.type->hostdata_tag == s.sceneNode;
        for (std::size_t k = 0; member.type->kind == LHAT_TYPE_RT_UNION && k < member.type->part_count; ++k)
            node = node || member.type->parts[k]->hostdata_tag == s.sceneNode;
        if (node) result.push_back({name, FieldKind::SceneNode, NodeRef{}});
        else if (member.type->kind == LHAT_TYPE_RT_NUMBER) result.push_back({name, FieldKind::Number, number(value)});
        else if (member.type->kind == LHAT_TYPE_RT_BOOL) result.push_back({name, FieldKind::Bool, lhat_as_bool(value)});
        else if (member.type->kind == LHAT_TYPE_RT_STRING)
            result.push_back({name, FieldKind::String, std::string(text(value))});
    }
    std::sort(result.begin(), result.end(), [](const FieldInfo& a, const FieldInfo& b) { return a.name < b.name; });
    return result;
}

ScriptRuntime::ScriptRuntime(const World& world, LevelExpansion& expanded, const std::filesystem::path& root, Log log,
                             PlayScope scope)
    : state_(std::make_unique<State>(world, expanded, root, std::move(log))) {
    auto& s = *state_;
    createProgram(s);
    auto* program = s.program.get();

    std::string errors;
    // main.lh is a script; an await^ at its top level makes it a coroutine (TDDja/02 4.1).
    const LhatUnit* mainUnit = nullptr;
    if (scope == PlayScope::Game && std::filesystem::exists(s.root / mainPath))
        mainUnit = lhat_program_check(program, mainPath);

    // Run order: scene runners scene by scene, then level runners.
    std::vector<Pending> pending;
    const auto collect = [&](int scene, const NodePath& prefix, const Runner& runner, const std::string& owner) {
        s.runners.push_back(&runner);
        for (const auto& behaviour : runner.behaviours)
            pending.push_back({scene, prefix, s.runners.size() - 1, &behaviour,
                               owner + " " + runner.name + " " + behaviour.script});
    };
    // Scene and prefab runners come expanded, one copy per instance (TDDja/06 2).
    for (std::size_t i = 0; i < expanded.instances.size(); ++i)
        for (const auto& runner : expanded.instances[i].expansion.runners)
            collect(static_cast<int>(i), runner.instance, runner.runner, "scene " + std::to_string(expanded.instances[i].id));
    for (const auto& runner : world.level.runners) collect(-1, {}, runner, "level");

    std::map<std::string, const LhatUnit*> units;
    for (const auto& item : pending)
        if (!units.count(item.behaviour->script))
            units[item.behaviour->script] = checkBehaviour(s, item.behaviour->script, errors);
    diagnostics(program, errors);
    if (!errors.empty()) throw ScriptError(errors);
    compile(s);
    startMachine(s);

    if (mainUnit) s.mainProto = lhat_unit_proto(mainUnit);
    std::map<std::string, LhatValue> definitions;
    for (const auto& [path, unit] : units) definitions[path] = definition(s, unit, path);
    std::vector<LhatValue> runnerHandles;
    for (std::size_t i = 0; i < s.runners.size(); ++i) runnerHandles.push_back(s.keep(s.handle(s.machine.get(), s.runner, i)));

    for (const auto& item : pending) {
        const auto& path = item.behaviour->script;
        const auto* unit = units[path];
        const auto instance = s.keep(s.check(
            lhat_machine_call_member(s.machine.get(), definitions[path], "new", 3, nullptr, 0), item.what + " new"));
        auto* table = reinterpret_cast<LhatTable*>(lhat_as_object(instance));
        for (const auto& [name, value] : item.behaviour->fields) {
            if (!behaviourHas(unit, name, fieldType(value))) {
                s.log(item.what + ": field " + name + " ignored (the script has no " + fieldType(value) + " field of that name)");
                continue;
            }
            LhatValue written = lhat_nil();
            if (const auto* number = std::get_if<double>(&value)) written = lhat_real(*number);
            else if (const auto* flag = std::get_if<bool>(&value)) written = lhat_bool(*flag);
            else if (const auto* string = std::get_if<std::string>(&value)) {
                if (!lhat_machine_make_string(s.machine.get(), string->data(), string->size(), &written))
                    throw ScriptError("Out of memory");
            } else {
                // A reference without a scene is relative to the runner's own scene instance and
                // prefab instance; one naming a scene instance starts from that scene's nodes.
                const auto& ref = std::get<NodeRef>(value);
                int scene = item.scene;
                NodePath path = item.prefix;
                if (ref.scene) {
                    scene = -1;
                    path.clear();
                    for (std::size_t i = 0; i < expanded.instances.size(); ++i)
                        if (expanded.instances[i].id == ref.scene) scene = static_cast<int>(i);
                }
                path.push_back(ref.node);
                path.insert(path.end(), ref.inner.begin(), ref.inner.end());
                if (scene >= 0 && ref.node)
                    if (const auto at = expanded.instances[scene].expansion.find(path); at != Expansion::npos)
                        written = s.handle(s.machine.get(), s.sceneNode, State::nodeBits(scene, at));
                // A node that is not loaded leaves the script's default in place.
                if (lhat_is_nil(written)) continue;
            }
            LhatValue key;
            bool refused = false;
            if (!lhat_machine_make_string(s.machine.get(), name.data(), name.size(), &key) ||
                !lhat_machine_table_set(s.machine.get(), table, key, written, &refused) || refused)
                throw ScriptError(item.what + ": could not set field " + name);
        }
        s.instances.push_back({item.what, instance, runnerHandles[item.runner], behaviourHas(unit, "start", "any^"),
                               behaviourHas(unit, "update", "any^")});
    }
}

ScriptRuntime::~ScriptRuntime() = default;

bool ScriptRuntime::finished() const { return state_->finished; }

ScriptRuntime::ConsoleAnswer ScriptRuntime::evaluate(const std::string& line) {
    auto& s = *state_;
    if (!s.prompt) s.prompt.reset(lhat_prompt_new(s.program.get(), s.machine.get(), "console"));
    if (!s.prompt) return {false, "Out of memory"};
    const auto input = line + "\n";
    const auto result = lhat_prompt_feed(s.prompt.get(), input.data(), input.size());
    ConsoleAnswer answer;
    answer.more = result.status == LHAT_PROMPT_MORE;
    answer.text.resize(lhat_prompt_report(s.prompt.get(), nullptr, 0));
    lhat_prompt_report(s.prompt.get(), answer.text.data(), answer.text.size() + 1);
    if (result.status == LHAT_PROMPT_RAN && !lhat_is_nil(result.value)) {
        std::string value(lhat_value_write(result.value, nullptr, 0), '\0');
        lhat_value_write(result.value, value.data(), value.size() + 1);
        if (!answer.text.empty() && answer.text.back() != '\n') answer.text += '\n';
        answer.text += value;
    }
    while (!answer.text.empty() && answer.text.back() == '\n') answer.text.pop_back();
    return answer;
}

void ScriptRuntime::dropInput() {
    if (state_->prompt) lhat_prompt_drop(state_->prompt.get());
}

void ScriptRuntime::update(float deltaSeconds) {
    auto& s = *state_;
    if (!s.started) {
        s.started = true;
        if (s.mainProto) {
            // A script without await^ runs to its end here; one with await^ answers its coroutine.
            const auto result = s.check(lhat_run(s.machine.get(), s.mainProto), mainPath);
            if (lhat_is_object_kind(result, LHAT_OBJECT_COROUTINE)) s.game = s.keep(result);
            else s.finished = true;
        }
        for (const auto& instance : s.instances)
            if (instance.start) s.call(instance.instance, "start", &instance.runner, 1, instance.what);
    }
    s.time += deltaSeconds;
    const LhatValue dt = lhat_real(deltaSeconds);
    if (!lhat_is_nil(s.game) && !s.finished) {
        s.check(lhat_machine_resume(s.machine.get(), s.game, nullptr, 0), mainPath);
        s.finished = lhat_machine_coroutine_done(s.game);
    }
    for (const auto& instance : s.instances) {
        const LhatValue arguments[2] = {instance.runner, dt};
        if (instance.update) s.call(instance.instance, "update", arguments, 2, instance.what);
    }
}
}
