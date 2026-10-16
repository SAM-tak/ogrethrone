#include "script/ScriptRuntime.h"
#include <lhat.h>
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
constexpr const char* mainPath = "scripts/main.lh";
/// Loop turns one frame may take before it is treated as an endless loop.
constexpr std::int64_t frameBudget = 10'000'000;

using Machine = std::unique_ptr<LhatMachine, decltype(&lhat_machine_dispose)>;
using Program = std::unique_ptr<LhatProgram, decltype(&lhat_program_free)>;

/// One scripted object: its Behaviour instance and the throne.Object handle passed to it.
struct Behaviour {
    std::string path;
    LhatValue instance, handle;
    bool start, update;
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

/// Returns the component storage of one of an object's vector properties (0-3).
float* property(SceneObject& object, int which) {
    switch (which) {
    case 0: return object.position.data();
    case 1: return object.rotation.data();
    case 2: return object.scale.data();
    default: return object.colour.data();
    }
}
}

struct ScriptRuntime::State {
    SceneDocument& scene;
    std::filesystem::path root;
    Log log;
    Program program{nullptr, &lhat_program_free};
    // Declared after the program so that it is disposed first.
    Machine machine{nullptr, &lhat_machine_dispose};
    const LhatHostValueTag* vec3 = nullptr;
    const LhatHostDataTag* object = nullptr;
    // Values the host holds are not roots, so everything kept between calls
    // also lives in this table, which is reachable from L^.
    LhatValue roots = lhat_nil();
    LhatInteger rootCount = 0;
    LhatValue main = lhat_nil();
    bool mainStart = false, mainUpdate = false, started = false;
    std::vector<Behaviour> behaviours;
    double time = 0;

    State(SceneDocument& scene, std::filesystem::path root, Log log)
        : scene(scene), root(std::move(root)), log(std::move(log)) {}

    /// Keeps @p value alive for the session and returns it.
    LhatValue keep(LhatValue value) {
        bool refused = false;
        if (!lhat_machine_table_set(machine.get(), reinterpret_cast<LhatTable*>(lhat_as_object(roots)),
                                    lhat_integer(rootCount++), value, &refused) ||
            refused)
            throw ScriptError("Out of memory");
        return value;
    }

    /// Returns a throne.Object handle for @p id.
    LhatValue handle(std::uint32_t id) {
        LhatValue result;
        void* pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        if (!lhat_machine_make_hostdata(machine.get(), object, pointer, &result)) throw ScriptError("Out of memory");
        return result;
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

    /// Returns the scene object a throne.Object handle names, or panics and answers null.
    SceneObject* target(LhatMachine* running, LhatValue value) {
        const auto id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(value, object)));
        if (auto* found = scene.find(id)) return found;
        lhat_machine_panic_text(running, "throne.Object refers to a deleted object; check valid() first");
        return nullptr;
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

/// Object.id().
THRONE_HOST(objectId, {
    if (const auto* object = s.target(machine, arguments[0])) {
        answers[0] = lhat_integer(object->id);
        *count = 1;
    }
})
/// Object.name().
THRONE_HOST(objectName, {
    if (const auto* object = s.target(machine, arguments[0]))
        if (lhat_machine_make_string(machine, object->name.data(), object->name.size(), &answers[0])) *count = 1;
})
/// Object.valid(): whether the object still exists.
THRONE_HOST(objectValid, {
    const auto id = reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(arguments[0], s.object));
    answers[0] = lhat_bool(s.scene.find(static_cast<std::uint32_t>(id)) != nullptr);
    *count = 1;
})

/// Reads vector property @p Which of the receiver.
template <int Which>
THRONE_HOST(objectGet, {
    if (auto* object = s.target(machine, arguments[0])) s.answer(answers, count, property(*object, Which));
})
/// Writes vector property @p Which of the receiver, keeping scale and colour in range.
template <int Which>
THRONE_HOST(objectSet, {
    auto* object = s.target(machine, arguments[0]);
    if (!object) return;
    auto value = vec3(s, arguments[1]);
    for (float* component : {&value.x, &value.y, &value.z}) {
        if (!std::isfinite(*component)) {
            lhat_machine_panic_text(machine, "throne.Object property must be finite");
            return;
        }
        *component = std::clamp(*component, Which == 2 ? 0.001f : Which == 3 ? 0.0f : -1.0e6f,
                                Which == 3 ? 1.0f : 1.0e6f);
    }
    std::memcpy(property(*object, Which), value.data(), sizeof value);
})

/// throne.find(name): the first object with that name, or nil^.
THRONE_HOST(find, {
    const auto name = text(arguments[0]);
    answers[0] = lhat_nil();
    *count = 1;
    for (const auto& object : s.scene.objects)
        if (object.name == name) {
            void* pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(object.id));
            if (!lhat_machine_make_hostdata(machine, s.object, pointer, &answers[0])) answers[0] = lhat_nil();
            return;
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

/// Registers the throne module: Vec3, Object and the free functions.
bool registerThrone(LhatProgram* program, State& s) {
    s.vec3 = lhat_register_hostvalue_type(program, moduleName, "Vec3", sizeof(Vec3));
    s.object = lhat_register_hostdata_type(program, moduleName, "Object");
    if (!s.vec3 || !s.object) return false;
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
        {"Object", "id", "f^self^ -> number^;", objectId},
        {"Object", "name", "f^self^ -> string^;", objectName},
        {"Object", "valid", "f^self^ -> bool^;", objectValid},
        {"Object", "position", "f^self^ -> throne.Vec3;", objectGet<0>},
        {"Object", "rotation", "f^self^ -> throne.Vec3;", objectGet<1>},
        {"Object", "scale", "f^self^ -> throne.Vec3;", objectGet<2>},
        {"Object", "colour", "f^self^ -> throne.Vec3;", objectGet<3>},
        {"Object", "setPosition", "p^self^, throne.Vec3;", objectSet<0>},
        {"Object", "setRotation", "p^self^, throne.Vec3;", objectSet<1>},
        {"Object", "setScale", "p^self^, throne.Vec3;", objectSet<2>},
        {"Object", "setColour", "p^self^, throne.Vec3;", objectSet<3>},
        {nullptr, "vec3", "f^number^, number^, number^ -> throne.Vec3;", vecNew},
        {nullptr, "find", "f^string^ -> throne.Object|nil^;", find},
        {nullptr, "log", "p^string^;", log},
        {nullptr, "time", "f^ -> number^;", time},
    };
    for (const auto& entry : entries) {
        const bool ok =
            !entry.type ? lhat_register_func(program, moduleName, entry.name, entry.signature, entry.call, &s)
            : entry.type == std::string_view("Vec3")
                ? lhat_register_hostvalue_member(program, moduleName, "Vec3", entry.name, entry.signature, entry.call, &s)
                : lhat_register_member(program, moduleName, "Object", entry.name, entry.signature, entry.call, &s);
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
}

ScriptRuntime::ScriptRuntime(SceneDocument& scene, const std::filesystem::path& root, Log log)
    : state_(std::make_unique<State>(scene, root, std::move(log))) {
    auto& s = *state_;
    s.program.reset(lhat_program_new(true, loadScript, &s));
    if (!s.program || !lhatstdlib_math_register(s.program.get()) || !registerThrone(s.program.get(), s))
        throw ScriptError("Could not set up the L^ program");
    auto* program = s.program.get();

    std::string errors;
    const auto contract = [&](const std::string& path, const char* problem) {
        errors += path + ": " + problem + "\n";
    };
    // Each unit is checked once; objects sharing a script share its unit.
    const LhatUnit* mainUnit = nullptr;
    if (std::filesystem::exists(s.root / mainPath)) {
        mainUnit = lhat_program_check(program, mainPath);
        if (mainUnit && lhat_unit_ok(mainUnit)) {
            if (!lhat_unit_module_name(mainUnit)) contract(mainPath, "must declare module^");
            s.mainStart = publishes(mainUnit, "start");
            s.mainUpdate = publishes(mainUnit, "update");
            if (s.mainStart && !lhat_unit_export_conforms(mainUnit, "start", "p^;"))
                contract(mainPath, "start must be p^;");
            if (s.mainUpdate && !lhat_unit_export_conforms(mainUnit, "update", "p^number^;"))
                contract(mainPath, "update must be p^number^;");
        }
    }
    std::map<std::string, const LhatUnit*> units;
    for (const auto& object : scene.objects) {
        if (object.script.empty() || units.count(object.script)) continue;
        const auto* unit = lhat_program_check(program, object.script.c_str());
        units[object.script] = unit;
        if (!unit || !lhat_unit_ok(unit)) continue;
        const auto& path = object.script;
        if (!lhat_unit_module_name(unit)) contract(path, "must declare module^");
        if (!publishes(unit, "Behaviour")) {
            contract(path, "must publish Behaviour = def^{ ... }");
            continue;
        }
        if (!lhat_unit_export_conforms(unit, "Behaviour", "t^{ new : f^ -> any^; }"))
            contract(path, "Behaviour must be a def^");
        if (lhat_unit_export_conforms(unit, "Behaviour", "t^{ self^{ start : any^ } }") &&
            !lhat_unit_export_conforms(unit, "Behaviour", "t^{ self^{ start : p^self^, throne.Object; } }"))
            contract(path, "Behaviour.start must be p^self^, throne.Object;");
        if (lhat_unit_export_conforms(unit, "Behaviour", "t^{ self^{ update : any^ } }") &&
            !lhat_unit_export_conforms(unit, "Behaviour", "t^{ self^{ update : p^self^, throne.Object, number^; } }"))
            contract(path, "Behaviour.update must be p^self^, throne.Object, number^;");
    }
    diagnostics(program, errors);
    if (!errors.empty()) throw ScriptError(errors);
    if (!lhat_program_compile(program)) {
        const auto failure = lhat_program_compile_failure(program, nullptr);
        std::string message(lhat_compile_message_write(program, &failure, nullptr, 0) + 1, '\0');
        lhat_compile_message_write(program, &failure, message.data(), message.size());
        message.pop_back();
        throw ScriptError(message);
    }

    s.machine.reset(lhat_machine_new());
    if (!s.machine || !lhat_machine_make_table(s.machine.get(), &s.roots) ||
        !lhat_machine_set_global(s.machine.get(), "throneRuntimeRoots", s.roots))
        throw ScriptError("Out of memory");
    lhat_program_install(program, s.machine.get());
    lhat_machine_set_budget(s.machine.get(), frameBudget);

    if (mainUnit) s.main = s.keep(s.check(lhat_run(s.machine.get(), lhat_unit_proto(mainUnit)), mainPath));
    std::map<std::string, LhatValue> definitions;
    for (const auto& [path, unit] : units) {
        const auto exports = s.check(lhat_run(s.machine.get(), lhat_unit_proto(unit)), path);
        definitions[path] = s.keep(lhat_table_get_bytes(reinterpret_cast<const LhatTable*>(lhat_as_object(exports)),
                                                        "Behaviour", 9));
    }
    for (const auto& object : scene.objects) {
        if (object.script.empty()) continue;
        const auto* unit = units[object.script];
        Behaviour behaviour{object.script, lhat_nil(), s.keep(s.handle(object.id)),
                            lhat_unit_export_conforms(unit, "Behaviour", "t^{ self^{ start : any^ } }"),
                            lhat_unit_export_conforms(unit, "Behaviour", "t^{ self^{ update : any^ } }")};
        behaviour.instance =
            s.keep(s.check(lhat_machine_call_member(s.machine.get(), definitions[object.script], "new", 3, nullptr, 0),
                           object.script + " Behaviour.new"));
        s.behaviours.push_back(std::move(behaviour));
    }
}

ScriptRuntime::~ScriptRuntime() = default;

void ScriptRuntime::update(float deltaSeconds) {
    auto& s = *state_;
    if (!s.started) {
        s.started = true;
        if (s.mainStart) s.call(s.main, "start", nullptr, 0, mainPath);
        for (const auto& behaviour : s.behaviours)
            if (behaviour.start) s.call(behaviour.instance, "start", &behaviour.handle, 1, behaviour.path);
    }
    s.time += deltaSeconds;
    const LhatValue dt = lhat_real(deltaSeconds);
    if (s.mainUpdate) s.call(s.main, "update", &dt, 1, mainPath);
    for (const auto& behaviour : s.behaviours) {
        const LhatValue arguments[2] = {behaviour.handle, dt};
        if (behaviour.update) s.call(behaviour.instance, "update", arguments, 2, behaviour.path);
    }
}
}
