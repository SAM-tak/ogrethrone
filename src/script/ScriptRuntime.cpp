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
#include <type_traits>
#include <tuple>
#include <limits>
#include <set>
#include <vector>

namespace throne {
namespace {
constexpr const char* moduleName = "throne";
constexpr const char* mainPath = "main.lh";
constexpr const char* lifecyclePath = "@throne/behaviour-lifecycle.lh";
/// Loop turns one frame may take before it is treated as an endless loop.
constexpr std::int64_t frameBudget = 10'000'000;

using Machine = std::unique_ptr<LhatMachine, decltype(&lhat_machine_dispose)>;
using Program = std::unique_ptr<LhatProgram, decltype(&lhat_program_free)>;

/// A method keeps its own suspended invocation; other methods remain independent.
struct Invocation {
    LhatValue coroutine = lhat_nil();
    LhatInteger root = -1;
    std::string origin, waiting;
    std::uint64_t readyFrame = 0, lastCall = 0;
};

/// One attachment: its VM instance and its SceneNode or System callback argument.
struct Instance {
    std::string what;
    LhatValue instance, system;
    bool start, update;
    std::string stage;
    bool started = false;
    int scene = -1;
    std::size_t node = Expansion::npos, attachment = 0;
    NodePath owner;
    NodeId identity = 0;
    Behaviour config;
    BehaviourDefaults defaults;
    std::array<bool,3> methods{};
    std::array<Invocation,4> calls;
    LhatInteger instanceRoot = -1, handleRoot = -1;
    bool alive = true;
    std::size_t order = 0;
};

/// A behaviour waiting to be instantiated, with where it came from.
struct Pending {
    /// Index of the system's scene instance in LevelExpansion::instances, or -1 for a level system.
    int scene;
    /// The prefab instance the system came from; its NodeRefs are relative to it.
    NodePath prefix;
    std::size_t system;
    const Behaviour* behaviour;
    std::string what;
    std::size_t node = Expansion::npos;
    std::string stage;
    std::string symbol = "Behaviour";
};

double number(LhatValue value);
/// Reads finite script defaults before applying attachment field overrides.
BehaviourDefaults priorityDefaults(const LhatTable* table) {
    BehaviourDefaults defaults;
    const auto read=[&](const std::string& key)->std::optional<double> {
        const auto value=lhat_table_get_bytes(table,key.data(),key.size());
        if(lhat_is_nil(value)) return {};
        if((!lhat_is_integer(value) && !lhat_is_real(value)) || !std::isfinite(number(value)))
            throw ScriptError(key+" must be a finite priority");
        return number(value);
    };
    defaults.priority=read("priority").value_or(0);
    for(int i=0;i<3;++i) defaults.phasePriorities[i]=read(std::string(behaviourMethods[i])+"Priority");
    return defaults;
}

/// Maps a typed host handle to its optional node component.
template <typename T> struct ComponentBinding;
#define THRONE_BIND_COMPONENT(Type, field) \
    template <> struct ComponentBinding<Type> { \
        static constexpr const char* name = #Type; \
        static constexpr auto member = &SceneNode::field; \
    };
THRONE_BIND_COMPONENT(Transform, transform)
THRONE_BIND_COMPONENT(BoxPrimitive, box)
THRONE_BIND_COMPONENT(SpherePrimitive, sphere)
THRONE_BIND_COMPONENT(CapsulePrimitive, capsule)
THRONE_BIND_COMPONENT(PlanePrimitive, plane)
THRONE_BIND_COMPONENT(BoxCollider, boxCollider)
THRONE_BIND_COMPONENT(SphereCollider, sphereCollider)
THRONE_BIND_COMPONENT(CapsuleCollider, capsuleCollider)
THRONE_BIND_COMPONENT(PlaneCollider, planeCollider)
THRONE_BIND_COMPONENT(RigidBody, rigidBody)
THRONE_BIND_COMPONENT(PbsMaterial, material)
template <> struct ComponentBinding<LightComponent> {
    static constexpr const char* name = "Light";
    static constexpr auto member = &SceneNode::light;
};
template <> struct ComponentBinding<ParticleSystemComponent> {
    static constexpr const char* name = "ParticleSystem";
    static constexpr auto member = &SceneNode::particles;
};
#undef THRONE_BIND_COMPONENT

/// Returns a number argument as a double, accepting integers and reals.
double number(LhatValue value) {
    return lhat_is_integer(value) ? static_cast<double>(lhat_as_integer(value)) : lhat_as_real(value);
}

/// Returns the bytes of a string argument.
std::string_view text(LhatValue value) {
    const auto* string = reinterpret_cast<const LhatString*>(lhat_as_object(value));
    return {string->text, string->length};
}

/// Returns the storage of one of a Transform's vector properties (0 position, 1 rotation, 2 scale).
float* property(Transform& transform, int which) {
    switch (which) {
    case 0: return transform.position.data();
    case 1: return transform.rotation.data();
    default: return transform.scale.data();
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
    const LhatHostDataTag *sceneNode = nullptr, *system = nullptr, *transform = nullptr, *box = nullptr;
    std::map<std::string, const LhatHostDataTag*> componentTags;
    // Values the host holds are not roots, so everything kept between calls
    // also lives in the machine's host root table.
    LhatTable* roots = nullptr;
    LhatInteger rootCount = 0;
    /// main.lh, run on the first frame (Game scope only).
    const LhatProto* mainProto = nullptr;
    /// main.lh's coroutine while it is suspended between frames.
    LhatValue game = lhat_nil();
    /// Compiled L^ dispatches the coroutine's native dispose member.
    LhatValue disposeCoroutine = lhat_nil();
    bool started = false, finished = false;
    std::vector<std::unique_ptr<Instance>> instances;
    /// Systems in run order; a throne.System handle is an index into this.
    std::vector<const System*> systems;
    std::vector<ExecutionStage> stages;
    std::vector<System> stageSystems;
    std::vector<LhatValue> systemHandles;
    std::map<std::string,const LhatUnit*> units;
    std::map<std::string,LhatValue> definitions;
    std::map<std::string,std::vector<Instance*>> queries;
    std::map<std::string,bool> dirtyQueries;
    /// Three phase orders and the common order; waits only refilter these lists.
    std::array<std::vector<Instance*>,4> ordered;
    bool orderDirty = true;
    bool registryDirty = true;
    std::uint64_t frame = 0;
    int currentStage = -1;
    std::vector<LhatInteger> freeRoots;
    double time = 0;

    State(const World& world, LevelExpansion& expanded, std::filesystem::path root, Log log)
        : world(world), expanded(expanded), root(std::move(root)), log(std::move(log)) {}

    /// Runs coroutine finalizers while host state and the machine pointer are still valid.
    ~State() {
        if(!machine) return;
        for(auto& entry:instances) {
            try { cancel(*entry); }
            catch(const std::exception& e) { if(log) log(std::string("Behaviour cleanup: ")+e.what()); }
        }
        prompt.reset();
        lhat_machine_dispose(machine.get());
        machine.release();
    }

    /// Keeps @p value alive for the session and returns it.
    LhatValue keep(LhatValue value) {
        bool refused = false;
        if (!lhat_machine_table_set(machine.get(), roots, lhat_integer(rootCount++), value, &refused) ||
            refused)
            throw ScriptError("Out of memory");
        return value;
    }

    /// Pins a lifetime-managed value; completed or removed invocations return their slot.
    LhatInteger pin(LhatValue value) {
        const auto slot=freeRoots.empty()?rootCount++:freeRoots.back();
        if(!freeRoots.empty()) freeRoots.pop_back();
        bool refused=false;
        if(!lhat_machine_table_set(machine.get(),roots,lhat_integer(slot),value,&refused) || refused)
            throw ScriptError("Out of memory");
        return slot;
    }
    /// Releases one host root without retaining dead Behaviour/coroutine objects.
    void unpin(LhatInteger& slot) {
        if(slot<0) return;
        bool refused=false;
        lhat_machine_table_set(machine.get(),roots,lhat_integer(slot),lhat_nil(),&refused);
        freeRoots.push_back(slot); slot=-1;
    }
    /// Reconciles additions/removals at the next frame boundary.
    void reconcile();
    /// Rebuilds and sorts a dirty system query, preserving hierarchy order for ties.
    void rebuildQuery(const std::string& system);
    /// Disposes pending calls when an attachment is removed or its script changes.
    void cancel(Instance& instance);
    /// Executes or resumes matching methods in this system's context.
    void execute(Instance& instance,int phase,LhatValue dt);

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

    /// Returns the node a handle of @p tag (throne.SceneNode unless said) names, or nullptr.
    SceneNode* resolve(LhatValue value, const LhatHostDataTag* tag = nullptr) {
        const auto bits = reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(value, tag ? tag : sceneNode));
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

    /// Returns the component @p member of the node a component handle of @p tag
    /// names, or panics and answers null when the node or the component is gone.
    template <typename Component>
    Component* component(LhatMachine* running, LhatValue value, const LhatHostDataTag* tag,
                         std::optional<Component> SceneNode::*member) {
        if (auto* node = resolve(value, tag); node && node->*member) return &*(node->*member);
        lhat_machine_panic_text(running, "the component's node, or the component itself, is gone");
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
    if (std::string_view(path) == lifecyclePath) {
        constexpr char source[] = "module^ ogrethrone_internal.lifecycle\npublic^let^ dispose=p^task:c^{p^ -> * -> *} { task.dispose() }\n";
        auto* copy = static_cast<char*>(lhat_alloc(sizeof(source)));
        if (!copy) return nullptr;
        std::memcpy(copy, source, sizeof(source));
        *length = sizeof(source)-1;
        return copy;
    }
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
/// SceneNode.spawnBox(size): appends a dynamic box child to the Play expansion only.
THRONE_HOST(nodeSpawnBox, {
    const auto* parent = s.target(machine, arguments[0]);
    if (!parent) return;
    const auto size = vec3(s, arguments[1]);
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || !std::isfinite(size.z) ||
        size.x <= 0 || size.y <= 0 || size.z <= 0) {
        lhat_machine_panic_text(machine, "spawnBox size must be finite and positive");
        return;
    }
    const auto bits = reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(arguments[0], s.sceneNode));
    const auto scene = static_cast<std::size_t>(bits >> 32) - 1;
    const auto index = static_cast<std::size_t>(bits & 0xFFFFFFFFu);
    auto& nodes = s.expanded.instances[scene].expansion.nodes;
    SceneNode box;
    box.id = newNodeId();
    box.parent = parent->id;
    box.name = "Spawned Box " + std::to_string(nodes.size());
    box.transform = Transform{};
    box.transform->scale = size;
    box.box = BoxPrimitive{};
    box.boxCollider = BoxCollider{};
    box.rigidBody = RigidBody{};
    box.rigidBody->type = BodyType::Dynamic;
    auto path = nodes[index].path;
    path.push_back(box.id);
    const auto added = nodes.size();
    nodes.push_back({std::move(path), index, std::move(box)});
    answers[0] = s.handle(machine, s.sceneNode, State::nodeBits(scene, added));
    *count = 1;
})
/// SceneNode.valid(): whether the node is still in a loaded scene.
THRONE_HOST(nodeValid, {
    answers[0] = lhat_bool(s.resolve(arguments[0]) != nullptr);
    *count = 1;
})
/// SceneNode.transform() and SceneNode.box(): a handle on the component, or nil^ when
/// the node has none. The handle names the node, as a SceneNode handle does.
template <typename Component, std::optional<Component> SceneNode::*Member>
THRONE_HOST(nodeComponent, {
    auto* node = s.target(machine, arguments[0]);
    if (!node) return;
    const auto* tag = s.componentTags.at(ComponentBinding<Component>::name);
    answers[0] = node->*Member ? s.handle(machine, tag, reinterpret_cast<std::uintptr_t>(
                                                           lhat_hostdata_pointer(arguments[0], s.sceneNode)))
                               : lhat_nil();
    *count = 1;
})
/// SceneNode.component(type): a typed component handle, or nil when absent.
THRONE_HOST(typedComponent, {
    auto* node = s.target(machine, arguments[0]);
    if (!node) return;
    if (!lhat_is_object_kind(arguments[1], LHAT_OBJECT_TYPE)) {
        lhat_machine_panic_text(machine, "Expected a component type");
        return;
    }
    const auto* type = reinterpret_cast<const LhatRuntimeType*>(lhat_as_object(arguments[1]));
    if (type->kind != LHAT_TYPE_RT_HOSTDATA) {
        lhat_machine_panic_text(machine, "Expected a built-in component type");
        return;
    }
    bool matched = false;
    const auto get = [&](auto member) {
        using Component = typename std::decay_t<decltype(node->*member)>::value_type;
        const auto* tag = s.componentTags.at(ComponentBinding<Component>::name);
        if (type->hostdata_tag != tag) return;
        matched = true;
        answers[0] = node->*member ? s.handle(machine, tag, reinterpret_cast<std::uintptr_t>(
            lhat_hostdata_pointer(arguments[0], s.sceneNode))) : lhat_nil();
        *count = 1;
    };
    get(&SceneNode::transform); get(&SceneNode::box); get(&SceneNode::sphere);
    get(&SceneNode::capsule); get(&SceneNode::plane); get(&SceneNode::boxCollider);
    get(&SceneNode::sphereCollider); get(&SceneNode::capsuleCollider); get(&SceneNode::planeCollider);
    get(&SceneNode::rigidBody); get(&SceneNode::material); get(&SceneNode::light); get(&SceneNode::particles);
    if (!matched) lhat_machine_panic_text(machine, "Expected a built-in component type");
})
/// Reads Vec3 @p value into @p out, clamped to [@p low, @p high]; panics when not finite.
bool finite(LhatMachine* machine, Vec3 value, float low, float high, float* out) {
    for (float* component : {&value.x, &value.y, &value.z}) {
        if (!std::isfinite(*component)) {
            lhat_machine_panic_text(machine, "a component property must be finite");
            return false;
        }
        *component = std::clamp(*component, low, high);
    }
    std::memcpy(out, value.data(), sizeof value);
    return true;
}
/// Reads vector property @p Which of a Transform.
template <int Which>
THRONE_HOST(transformGet, {
    if (auto* transform = s.component(machine, arguments[0], s.transform, &SceneNode::transform))
        s.answer(answers, count, property(*transform, Which));
})
/// Writes vector property @p Which of a Transform, keeping scale above zero.
template <int Which>
THRONE_HOST(transformSet, {
    if (auto* transform = s.component(machine, arguments[0], s.transform, &SceneNode::transform))
        finite(machine, vec3(s, arguments[1]), Which == 2 ? 0.001f : -1.0e6f, 1.0e6f, property(*transform, Which));
})
/// BoxPrimitive.colour().
THRONE_HOST(boxColour, {
    if (auto* box = s.component(machine, arguments[0], s.box, &SceneNode::box)) s.answer(answers, count, box->colour.data());
})
/// BoxPrimitive.setColour(v), each component kept in [0, 1].
THRONE_HOST(boxSetColour, {
    if (auto* box = s.component(machine, arguments[0], s.box, &SceneNode::box))
        finite(machine, vec3(s, arguments[1]), 0.0f, 1.0f, box->colour.data());
})

/// Reads a vector property of a typed built-in component.
template <typename T, auto Member>
THRONE_HOST(componentVector, {
    if (auto* c=s.component(machine,arguments[0],s.componentTags.at(ComponentBinding<T>::name),ComponentBinding<T>::member))
        s.answer(answers,count,(c->*Member).data());
})
/// Writes a finite vector property of a typed built-in component.
template <typename T, auto Member>
THRONE_HOST(componentSetVector, {
    if (auto* c=s.component(machine,arguments[0],s.componentTags.at(ComponentBinding<T>::name),ComponentBinding<T>::member))
        finite(machine,vec3(s,arguments[1]),-1.0e6f,1.0e6f,(c->*Member).data());
})
/// Reads a scalar shape property.
template <typename T, auto Member>
THRONE_HOST(componentScalar, {
    if (auto* c=s.component(machine,arguments[0],s.componentTags.at(ComponentBinding<T>::name),ComponentBinding<T>::member)) {
        answers[0]=lhat_real(c->*Member); *count=1;
    }
})
/// Reads the colour of a non-box primitive.
template <typename T>
THRONE_HOST(primitiveColourGet, {
    if (auto* c=s.component(machine,arguments[0],s.componentTags.at(ComponentBinding<T>::name),ComponentBinding<T>::member))
        s.answer(answers,count,c->colour.data());
})
/// Writes a primitive's colour, keeping channels in [0,1].
template <typename T>
THRONE_HOST(primitiveColourSet, {
    if (auto* c=s.component(machine,arguments[0],s.componentTags.at(ComponentBinding<T>::name),ComponentBinding<T>::member))
        finite(machine,vec3(s,arguments[1]),0,1,c->colour.data());
})
/// RigidBody.type(): the Box3D motion mode.
THRONE_HOST(bodyMotionType, {
    if(auto* c=s.component(machine,arguments[0],s.componentTags.at("RigidBody"),&SceneNode::rigidBody)) {
        const char* type=c->type==BodyType::Static?"Static":c->type==BodyType::Kinematic?"Kinematic":"Dynamic";
        if(lhat_machine_make_string(machine,type,std::strlen(type),&answers[0])) *count=1;
    }
})

/// System.name().
THRONE_HOST(systemName, {
    const auto index = reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(arguments[0], s.system));
    const auto& name = s.systems.at(index)->name;
    if (lhat_machine_make_string(machine, name.data(), name.size(), &answers[0])) *count = 1;
})

/// throne.system(name): finds a serial user execution stage.
THRONE_HOST(findSystem, {
    answers[0] = lhat_nil();
    *count = 1;
    const auto name = text(arguments[0]);
    for (const auto& stage : s.stages) {
        if (stage.name != name || !stage.parallel.empty() || name == "FetchInput" || name == "ApplyPhysics" ||
            name == "SyncRender" || name == "SyncSimulation") continue;
        for (std::size_t i = 0; i < s.systems.size(); ++i)
            if (s.systems[i]->name == name) {
                answers[0] = s.handle(machine, s.system, i);
                return;
            }
    }
})
/// Resolves a zero-based attachment index for the Behaviour mutation API.
Behaviour* attachment(State& s,LhatMachine* machine,const LhatValue* arguments) {
    auto* node=s.target(machine,arguments[0]);
    if(!node) return nullptr;
    const auto index=number(arguments[1]);
    if(!std::isfinite(index) || index<0 || std::floor(index)!=index || index>=node->behaviour.size()) {
        lhat_machine_panic_text(machine,"Invalid Behaviour index"); return nullptr;
    }
    return &node->behaviour[static_cast<std::size_t>(index)];
}
/// Returns the active serial system, including the destination of a resumed coroutine.
THRONE_HOST(currentSystem, {
    answers[0]=lhat_nil(); *count=1;
    if(s.currentStage<0) return;
    const auto& name=s.stages[s.currentStage].name;
    for(std::size_t i=0;i<s.systems.size();++i) if(s.systems[i]->name==name) {
        answers[0]=s.systemHandles[i]; return;
    }
})
/// Sets or clears the instance's common priority at the next frame boundary.
THRONE_HOST(setBehaviourPriority, {
    auto* b=attachment(s,machine,arguments); if(!b) return;
    const auto previous=b->priority;
    if(lhat_is_nil(arguments[2])) b->priority.reset();
    else {
        const auto value=number(arguments[2]);
        if(!std::isfinite(value)) { lhat_machine_panic_text(machine,"Priority must be finite"); return; }
        b->priority=value;
    }
    s.registryDirty|=previous!=b->priority;
})
/// Sets or clears an instance's override for one BehaviourSystem.
THRONE_HOST(setBehaviourPhasePriority, {
    auto* b=attachment(s,machine,arguments); if(!b) return;
    const auto index=reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(arguments[2],s.system));
    const int phase=index<s.systems.size()?behaviourPhase(s.systems[index]->name):-1;
    if(phase<0) { lhat_machine_panic_text(machine,"Expected a BehaviourSystem"); return; }
    const auto previous=b->phasePriorities[phase];
    if(lhat_is_nil(arguments[3])) b->phasePriorities[phase].reset();
    else {
        const auto value=number(arguments[3]);
        if(!std::isfinite(value)) { lhat_machine_panic_text(machine,"Priority must be finite"); return; }
        b->phasePriorities[phase]=value;
    }
    s.registryDirty|=previous!=b->phasePriorities[phase];
})
/// Disables/enables callbacks and suspended invocations without repeating start.
THRONE_HOST(setBehaviourEnabled, {
    auto* b=attachment(s,machine,arguments); if(!b) return;
    const bool enabled=lhat_as_bool(arguments[2]);
    s.registryDirty|=b->enabled!=enabled; b->enabled=enabled;
})
/// Adds an attachment; it is checked and instantiated at the next frame boundary.
THRONE_HOST(addBehaviour, {
    auto* node=s.target(machine,arguments[0]); if(!node) return;
    const auto path=std::filesystem::u8path(std::string(text(arguments[1]))).lexically_normal();
    if(path.empty() || path.is_absolute() || path.has_root_name() || *path.begin()=="..") {
        lhat_machine_panic_text(machine,"Behaviour script must be project-relative"); return;
    }
    answers[0]=lhat_integer(static_cast<LhatInteger>(node->behaviour.size())); *count=1;
    node->behaviour.push_back({path.generic_u8string(),{}}); s.registryDirty=true;
})
/// Removes an attachment and cancels its pending invocations at the next frame boundary.
THRONE_HOST(removeBehaviour, {
    auto* b=attachment(s,machine,arguments); if(!b) return;
    auto* node=s.target(machine,arguments[0]);
    node->behaviour.erase(node->behaviour.begin()+static_cast<std::size_t>(number(arguments[1])));
    s.registryDirty=true;
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

/// print(...): its arguments as text, separated by spaces, as one log line (like the L^ CLI's).
void print(LhatMachine*, void* context, const LhatValue* arguments, std::size_t count, LhatValue*, int*) {
    std::string line;
    for (std::size_t i = 0; i < count; ++i) {
        std::string text(lhat_value_text(arguments[i], nullptr, 0), '\0');
        lhat_value_text(arguments[i], text.data(), text.size() + 1);
        line += (i ? " " : "") + text;
    }
    state(context).log(line);
}

/// throne.query(names...): a stable snapshot of nodes having every named component.
void query(LhatMachine* machine, void* context, const LhatValue* arguments, std::size_t argumentCount,
           LhatValue* answers, int* count) {
    auto& s = state(context);
    LhatValue list;
    if (!lhat_machine_make_table(machine, &list)) return;
    // Root the result while allocating its handles.
    const auto slot = s.rootCount++;
    bool refusedRoot = false;
    if (!lhat_machine_table_set(machine, s.roots, lhat_integer(slot), list, &refusedRoot) || refusedRoot) return;
    struct RootGuard {
        State& s;
        LhatInteger slot;
        ~RootGuard() {
            bool refused = false;
            lhat_machine_table_set(s.machine.get(), s.roots, lhat_integer(slot), lhat_nil(), &refused);
            --s.rootCount;
        }
    } guard{s, slot};
    auto* table = reinterpret_cast<LhatTable*>(lhat_as_object(list));
    LhatInteger index = 0;
    for (std::size_t scene = 0; scene < s.expanded.instances.size(); ++scene) {
        const auto& nodes = s.expanded.instances[scene].expansion.nodes;
        for (const auto n : s.expanded.instances[scene].expansion.hierarchy()) {
            const auto& node = nodes[n].node;
            bool matches = true;
            for (std::size_t i = 0; i < argumentCount; ++i) {
                const auto name = text(arguments[i]);
                bool present=false;
                if(name=="Transform") present=bool(node.transform);
                if(name=="BoxPrimitive") present=bool(node.box);
                if(name=="SpherePrimitive") present=bool(node.sphere);
                if(name=="CapsulePrimitive") present=bool(node.capsule);
                if(name=="PlanePrimitive") present=bool(node.plane);
                if(name=="BoxCollider") present=bool(node.boxCollider);
                if(name=="SphereCollider") present=bool(node.sphereCollider);
                if(name=="CapsuleCollider") present=bool(node.capsuleCollider);
                if(name=="PlaneCollider") present=bool(node.planeCollider);
                if(name=="RigidBody") present=bool(node.rigidBody);
                if(name=="PbsMaterial") present=bool(node.material);
                if(name=="Light") present=bool(node.light);
                if(name=="ParticleSystem") present=bool(node.particles);
                matches=matches && present;
            }
            if (!matches) continue;
            const auto handle = s.handle(machine, s.sceneNode, State::nodeBits(scene, n));
            bool refused = false;
            if (lhat_is_nil(handle) || !lhat_machine_table_set(machine, table, lhat_integer(index++), handle, &refused) || refused) {
                lhat_machine_panic_text(machine, "Out of memory building query");
                return;
            }
        }
    }
    answers[0] = list;
    *count = 1;
}

/// Registers the throne module: Vec3, SceneNode, System and the free functions.
bool registerThrone(LhatProgram* program, State& s) {
    s.vec3 = lhat_register_hostvalue_type(program, moduleName, "Vec3", sizeof(Vec3));
    s.sceneNode = lhat_register_hostdata_type(program, moduleName, "SceneNode");
    s.system = lhat_register_hostdata_type(program, moduleName, "System");
    s.transform = lhat_register_hostdata_type(program, moduleName, "Transform");
    s.box = lhat_register_hostdata_type(program, moduleName, "BoxPrimitive");
    if (!s.vec3 || !s.sceneNode || !s.system || !s.transform || !s.box) return false;
    s.componentTags["Transform"]=s.transform;
    s.componentTags["BoxPrimitive"]=s.box;
    for(const char* name:{"SpherePrimitive","CapsulePrimitive","PlanePrimitive","BoxCollider","SphereCollider","CapsuleCollider","PlaneCollider","RigidBody","PbsMaterial","Light","ParticleSystem"}) {
        auto* tag=lhat_register_hostdata_type(program,moduleName,name);
        if(!tag) return false;
        s.componentTags[name]=tag;
    }
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
        {"SceneNode", "spawnBox", "p^self^, throne.Vec3 -> throne.SceneNode;", nodeSpawnBox},
        {"SceneNode", "transform", "f^self^ -> throne.Transform|nil^;", nodeComponent<Transform, &SceneNode::transform>},
        {"SceneNode", "component", "f^self^, type^ -> ARG0.T0|nil^;", typedComponent},
        {"SceneNode", "setBehaviourPriority", "p^self^, number^, number^|nil^;", setBehaviourPriority},
        {"SceneNode", "setBehaviourPhasePriority", "p^self^, number^, throne.System, number^|nil^;", setBehaviourPhasePriority},
        {"SceneNode", "setBehaviourEnabled", "p^self^, number^, bool^;", setBehaviourEnabled},
        {"SceneNode", "addBehaviour", "p^self^, string^ -> number^;", addBehaviour},
        {"SceneNode", "removeBehaviour", "p^self^, number^;", removeBehaviour},
        {"SceneNode", "box", "f^self^ -> throne.BoxPrimitive|nil^;", nodeComponent<BoxPrimitive, &SceneNode::box>},
        {"Transform", "position", "f^self^ -> throne.Vec3;", transformGet<0>},
        {"Transform", "rotation", "f^self^ -> throne.Vec3;", transformGet<1>},
        {"Transform", "scale", "f^self^ -> throne.Vec3;", transformGet<2>},
        {"Transform", "setPosition", "p^self^, throne.Vec3;", transformSet<0>},
        {"Transform", "setRotation", "p^self^, throne.Vec3;", transformSet<1>},
        {"Transform", "setScale", "p^self^, throne.Vec3;", transformSet<2>},
        {"BoxPrimitive", "colour", "f^self^ -> throne.Vec3;", boxColour},
        {"BoxPrimitive", "setColour", "p^self^, throne.Vec3;", boxSetColour},
        {"SceneNode", "sphere", "f^self^ -> throne.SpherePrimitive|nil^;", nodeComponent<SpherePrimitive, &SceneNode::sphere>},
        {"SpherePrimitive", "colour", "f^self^ -> throne.Vec3;", primitiveColourGet<SpherePrimitive>},
        {"SpherePrimitive", "setColour", "p^self^, throne.Vec3;", primitiveColourSet<SpherePrimitive>},
        {"SpherePrimitive", "radius", "f^self^ -> number^;", componentScalar<SpherePrimitive, &SpherePrimitive::radius>},
        {"SceneNode", "capsule", "f^self^ -> throne.CapsulePrimitive|nil^;", nodeComponent<CapsulePrimitive, &SceneNode::capsule>},
        {"CapsulePrimitive", "colour", "f^self^ -> throne.Vec3;", primitiveColourGet<CapsulePrimitive>},
        {"CapsulePrimitive", "setColour", "p^self^, throne.Vec3;", primitiveColourSet<CapsulePrimitive>},
        {"CapsulePrimitive", "radius", "f^self^ -> number^;", componentScalar<CapsulePrimitive, &CapsulePrimitive::radius>},
        {"CapsulePrimitive", "height", "f^self^ -> number^;", componentScalar<CapsulePrimitive, &CapsulePrimitive::height>},
        {"SceneNode", "plane", "f^self^ -> throne.PlanePrimitive|nil^;", nodeComponent<PlanePrimitive, &SceneNode::plane>},
        {"PlanePrimitive", "colour", "f^self^ -> throne.Vec3;", primitiveColourGet<PlanePrimitive>},
        {"PlanePrimitive", "setColour", "p^self^, throne.Vec3;", primitiveColourSet<PlanePrimitive>},
        {"PlanePrimitive", "width", "f^self^ -> number^;", componentScalar<PlanePrimitive, &PlanePrimitive::width>},
        {"PlanePrimitive", "depth", "f^self^ -> number^;", componentScalar<PlanePrimitive, &PlanePrimitive::depth>},
        {"SceneNode", "boxCollider", "f^self^ -> throne.BoxCollider|nil^;", nodeComponent<BoxCollider, &SceneNode::boxCollider>},
        {"BoxCollider", "centre", "f^self^ -> throne.Vec3;", componentVector<BoxCollider, &Collider::centre>},
        {"BoxCollider", "size", "f^self^ -> throne.Vec3;", componentVector<BoxCollider, &BoxCollider::size>},
        {"SceneNode", "sphereCollider", "f^self^ -> throne.SphereCollider|nil^;", nodeComponent<SphereCollider, &SceneNode::sphereCollider>},
        {"SphereCollider", "centre", "f^self^ -> throne.Vec3;", componentVector<SphereCollider, &Collider::centre>},
        {"SphereCollider", "radius", "f^self^ -> number^;", componentScalar<SphereCollider, &SphereCollider::radius>},
        {"SceneNode", "capsuleCollider", "f^self^ -> throne.CapsuleCollider|nil^;", nodeComponent<CapsuleCollider, &SceneNode::capsuleCollider>},
        {"CapsuleCollider", "centre", "f^self^ -> throne.Vec3;", componentVector<CapsuleCollider, &Collider::centre>},
        {"CapsuleCollider", "radius", "f^self^ -> number^;", componentScalar<CapsuleCollider, &CapsuleCollider::radius>},
        {"CapsuleCollider", "height", "f^self^ -> number^;", componentScalar<CapsuleCollider, &CapsuleCollider::height>},
        {"SceneNode", "planeCollider", "f^self^ -> throne.PlaneCollider|nil^;", nodeComponent<PlaneCollider, &SceneNode::planeCollider>},
        {"PlaneCollider", "centre", "f^self^ -> throne.Vec3;", componentVector<PlaneCollider, &Collider::centre>},
        {"PlaneCollider", "width", "f^self^ -> number^;", componentScalar<PlaneCollider, &PlaneCollider::width>},
        {"PlaneCollider", "depth", "f^self^ -> number^;", componentScalar<PlaneCollider, &PlaneCollider::depth>},
        {"SceneNode", "particleSystem", "f^self^ -> throne.ParticleSystem|nil^;", nodeComponent<ParticleSystemComponent, &SceneNode::particles>},
        {"SceneNode", "material", "f^self^ -> throne.PbsMaterial|nil^;", nodeComponent<PbsMaterial, &SceneNode::material>},
        {"PbsMaterial", "roughness", "f^self^ -> number^;", componentScalar<PbsMaterial, &PbsMaterial::roughness>},
        {"PbsMaterial", "metalness", "f^self^ -> number^;", componentScalar<PbsMaterial, &PbsMaterial::metalness>},
        {"SceneNode", "light", "f^self^ -> throne.Light|nil^;", nodeComponent<LightComponent, &SceneNode::light>},
        {"Light", "colour", "f^self^ -> throne.Vec3;", primitiveColourGet<LightComponent>},
        {"Light", "setColour", "p^self^, throne.Vec3;", primitiveColourSet<LightComponent>},
        {"Light", "power", "f^self^ -> number^;", componentScalar<LightComponent, &LightComponent::power>},
        {"Light", "radius", "f^self^ -> number^;", componentScalar<LightComponent, &LightComponent::radius>},
        {"SceneNode", "rigidBody", "f^self^ -> throne.RigidBody|nil^;", nodeComponent<RigidBody, &SceneNode::rigidBody>},
        {"RigidBody", "type", "f^self^ -> string^;", bodyMotionType},
        {"RigidBody", "linearVelocity", "f^self^ -> throne.Vec3;", componentVector<RigidBody, &RigidBody::linearVelocity>},
        {"RigidBody", "setLinearVelocity", "p^self^, throne.Vec3;", componentSetVector<RigidBody, &RigidBody::linearVelocity>},
        {"RigidBody", "angularVelocity", "f^self^ -> throne.Vec3;", componentVector<RigidBody, &RigidBody::angularVelocity>},
        {"RigidBody", "setAngularVelocity", "p^self^, throne.Vec3;", componentSetVector<RigidBody, &RigidBody::angularVelocity>},
        {"System", "name", "f^self^ -> string^;", systemName},
        {nullptr, "vec3", "f^number^, number^, number^ -> throne.Vec3;", vecNew},
        {nullptr, "query", "f^...:string^ -> t^{throne.SceneNode[]};", query},
        {nullptr, "system", "f^string^ -> throne.System|nil^;", findSystem},
        {nullptr, "currentSystem", "f^ -> throne.System|nil^;", currentSystem},
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
    // @export marks the Behaviour fields the Inspector shows and a system stores.
    // print is a global, as the L^ CLI has it; also reachable as L^.print.
    return lhat_register_annotation(program, moduleName, "export", LHAT_ANNOTATION_FIELD) &&
           lhat_register_global(program, "print", "f^...->nil^;", print, &s) &&
           lhat_bind_initial(program, "print", "L^.print");
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
bool behaviourHas(const LhatUnit* unit, const std::string& name, const char* type, const std::string& symbol = "Behaviour") {
    return lhat_unit_export_conforms(unit, symbol.c_str(), ("t^{ self^{ " + name + " : " + type + " } }").c_str());
}

/// Whether field @p name of @p unit's Behaviour is written with @export.
bool exported(const LhatUnit* unit, const std::string& name, const std::string& symbol = "Behaviour") {
    for (std::size_t i = 0, count = lhat_unit_annotation_count(unit, symbol.c_str(), name.c_str()); i < count; ++i) {
        const auto annotation = lhat_unit_annotation(unit, symbol.c_str(), name.c_str(), i);
        if (std::string_view(annotation.name, annotation.name_length) == "export") return true;
    }
    return false;
}

/// Creates @p s's program with everything scripts may name registered.
void createProgram(State& s) {
    s.program.reset(lhat_program_new(true, loadScript, &s));
    if (!s.program || !lhatstdlib_math_register(s.program.get()) || !registerThrone(s.program.get(), s))
        throw ScriptError("Could not set up the L^ program");
    const auto* lifecycle = lhat_program_check(s.program.get(), lifecyclePath);
    if (!lifecycle || !lhat_unit_ok(lifecycle)) {
        std::string errors; diagnostics(s.program.get(),errors);
        throw ScriptError("Could not check Behaviour lifecycle helpers: "+errors);
    }
}

/// Checks @p path as a behaviour script, adding contract problems to @p errors.
const LhatUnit* checkBehaviour(State& s, const std::string& path, std::string& errors, int node = -1, const std::string& symbol = "Behaviour") {
    const auto* unit = lhat_program_check(s.program.get(), path.c_str());
    if (!unit || !lhat_unit_ok(unit)) return unit;
    const auto problem = [&](const char* text) { errors += path + ": " + text + "\n"; };
    if (!lhat_unit_module_name(unit)) problem("must declare module^");
    if (!publishes(unit, symbol)) {
        errors += path + ": must publish " + symbol + " = def^{ ... }\n";
        return unit;
    }
    if (!lhat_unit_export_conforms(unit, symbol.c_str(), "t^{ new : f^ -> any^; }"))
        errors += path + ": " + symbol + " must be a def^\n";
    for(int method=0;method<4;++method) {
        const std::string name=method==3?"start":behaviourMethods[method];
        if(!behaviourHas(unit,name,"any^",symbol)) continue;
        bool valid=false;
        for(const char* receiver:{"throne.SceneNode","throne.System"}) {
            if((node==1 && std::string(receiver)!="throne.SceneNode") || (node==0 && std::string(receiver)!="throne.System")) continue;
            const std::string args=std::string("self^, ")+receiver+(method==3?"":", number^");
            valid|=behaviourHas(unit,name,("p^"+args+";").c_str(),symbol);
            valid|=behaviourHas(unit,name,("f^"+args+" -> any^;").c_str(),symbol);
            valid|=behaviourHas(unit,name,("p^"+args+" -> any^;").c_str(),symbol);
        }
        if(!valid) errors+=path+": "+symbol+"."+name+" must be a function or procedure accepting self, "+(node==0?"System":"SceneNode")+(method==3?"":" and dt")+"\n";
    }
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
    const auto* lifecycle=lhat_program_check(s.program.get(),lifecyclePath);
    const auto exports=s.check(lhat_run(s.machine.get(),lhat_unit_proto(lifecycle)),lifecyclePath);
    s.disposeCoroutine=s.keep(lhat_table_get_bytes(reinterpret_cast<const LhatTable*>(lhat_as_object(exports)),"dispose",7));
}

/// Runs @p unit and returns its published Behaviour definition, kept alive.
LhatValue definition(State& s, const LhatUnit* unit, const std::string& path, const std::string& symbol = "Behaviour") {
    const auto exports = s.check(lhat_run(s.machine.get(), lhat_unit_proto(unit)), path);
    return s.keep(lhat_table_get_bytes(reinterpret_cast<const LhatTable*>(lhat_as_object(exports)), symbol.data(), symbol.size()));
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

static BehaviourInfo inspectScript(const std::filesystem::path& root, const std::string& script, std::string symbol) {
    World none;
    LevelExpansion nothing;
    ScriptRuntime::State s(none, nothing, root, {});
    createProgram(s);
    std::string errors;
    if (symbol.empty()) symbol = "Behaviour";
    if (symbol != "Behaviour") throw ScriptError("Expected a Behaviour script");
    const auto* unit = checkBehaviour(s, script, errors, -1, symbol);
    diagnostics(s.program.get(), errors);
    if (!errors.empty()) throw ScriptError(errors);
    compile(s);
    const auto* type = lhat_unit_export_type(unit, symbol.c_str());
    if (!type || !type->instance) throw ScriptError(script + ": " + symbol + " must be a def^");
    // The defaults are what a fresh instance holds.
    startMachine(s);
    const auto instance = s.keep(s.check(
        lhat_machine_call_member(s.machine.get(), definition(s, unit, script, symbol), "new", 3, nullptr, 0), script + " new"));
    const auto* table = reinterpret_cast<const LhatTable*>(lhat_as_object(instance));
    BehaviourInfo info;
    auto& result=info.fields;
    info.defaults=priorityDefaults(table);
    info.start=behaviourHas(unit,"start","any^",symbol);
    for(int phase=0;phase<3;++phase) info.methods[phase]=behaviourHas(unit,behaviourMethods[phase],"any^",symbol);
    for (std::size_t i = 0; i < type->instance->member_count; ++i) {
        const auto& member = type->instance->members[i];
        if (!member.type) continue;
        const std::string name(member.name->text, member.name->length);
        if (!exported(unit, name, symbol)) continue;  // The script's own, not the Inspector's.
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
    return info;
}

std::vector<FieldInfo> ScriptRuntime::fields(const std::filesystem::path& root,const std::string& script,std::string symbol) {
    return inspectScript(root,script,std::move(symbol)).fields;
}
BehaviourInfo ScriptRuntime::inspectBehaviour(const std::filesystem::path& root,const std::string& script) {
    return inspectScript(root,script,"Behaviour");
}

namespace {
/// Instantiates one attachment with isolated state and retained, releasable VM roots.
void instantiate(State& s,const Pending& item) {
        const auto& path = item.behaviour->script;
        const auto key = path + "#" + item.symbol;
        const auto* unit = s.units.at(key);
        const auto instance = s.check(
            lhat_machine_call_member(s.machine.get(), s.definitions.at(key), "new", 3, nullptr, 0), item.what + " new");
        auto entry=std::make_unique<Instance>();
        entry->instanceRoot=s.pin(instance);
        entry->defaults=priorityDefaults(reinterpret_cast<const LhatTable*>(lhat_as_object(instance)));
        auto* table = reinterpret_cast<LhatTable*>(lhat_as_object(instance));
        for (const auto& [name, value] : item.behaviour->data) {
            if (!behaviourHas(unit, name, fieldType(value), item.symbol) ||
                !exported(unit, name, item.symbol)) {
                s.log(item.what + ": field " + name + " ignored (the script has no " +
                      std::string("@export ") + fieldType(value) +
                      " field of that name)");
                continue;
            }
            LhatValue written = lhat_nil();
            if (const auto* number = std::get_if<double>(&value)) written = lhat_real(*number);
            else if (const auto* flag = std::get_if<bool>(&value)) written = lhat_bool(*flag);
            else if (const auto* string = std::get_if<std::string>(&value)) {
                if (!lhat_machine_make_string(s.machine.get(), string->data(), string->size(), &written))
                    throw ScriptError("Out of memory");
            } else {
                // A reference without a scene is relative to the system's own scene instance and
                // prefab instance; one naming a scene instance starts from that scene's nodes.
                const auto& ref = std::get<NodeRef>(value);
                int scene = item.scene;
                NodePath path = item.prefix;
                if (ref.scene) {
                    scene = -1;
                    path.clear();
                    for (std::size_t i = 0; i < s.expanded.instances.size(); ++i)
                        if (s.expanded.instances[i].id == ref.scene) scene = static_cast<int>(i);
                }
                path.push_back(ref.node);
                path.insert(path.end(), ref.inner.begin(), ref.inner.end());
                if (scene >= 0 && ref.node)
                    if (const auto at = s.expanded.instances[scene].expansion.find(path); at != Expansion::npos)
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
        entry->what=item.what; entry->instance=instance;
        entry->system=item.node==Expansion::npos?s.systemHandles[item.system]:s.handle(s.machine.get(),s.sceneNode,State::nodeBits(item.scene,item.node));
        if(item.node!=Expansion::npos) entry->handleRoot=s.pin(entry->system);
        entry->start=behaviourHas(unit,"start","any^",item.symbol);
        for(int phase=0;phase<3;++phase) entry->methods[phase]=behaviourHas(unit,behaviourMethods[phase],"any^",item.symbol);
        entry->update=entry->methods[0]; entry->stage=item.stage;
        entry->scene=item.scene; entry->node=item.node;
        entry->attachment=item.node==Expansion::npos?0:static_cast<std::size_t>(item.behaviour-s.expanded.instances[item.scene].expansion.nodes[item.node].node.behaviour.data());
        if(item.node!=Expansion::npos) entry->owner=s.expanded.instances[item.scene].expansion.nodes[item.node].path;
        entry->identity=item.behaviour->id; entry->config=*item.behaviour;
        for(int phase=0;phase<3;++phase)
            if(!std::isfinite(effectivePriority(entry->config,entry->defaults,phase))) throw ScriptError("Priority must be finite");
        entry->order=s.instances.size();
        s.instances.push_back(std::move(entry));
}
}

void ScriptRuntime::State::cancel(Instance& instance) {
    for(auto& call:instance.calls) {
        if(!lhat_is_nil(call.coroutine) && !lhat_machine_coroutine_done(call.coroutine))
            check(lhat_machine_call(machine.get(),disposeCoroutine,&call.coroutine,1),instance.what+" dispose");
        call.coroutine=lhat_nil(); unpin(call.root);
    }
    unpin(instance.instanceRoot); unpin(instance.handleRoot);
}

void ScriptRuntime::State::reconcile() {
    if(!registryDirty) return;
    registryDirty=false;
    using Key=std::tuple<int,NodePath,NodeId>;
    std::map<Key,Instance*> existing;
    std::size_t order=0;
    for(auto& entry:instances) {
        entry->alive=entry->node==Expansion::npos;
        if(entry->alive) entry->order=order++;
        else existing[{entry->scene,entry->owner,entry->identity}]=entry.get();
    }
    for(std::size_t scene=0;scene<expanded.instances.size();++scene) {
        auto& expansion=expanded.instances[scene].expansion;
        for(const auto n:expansion.hierarchy()) {
            auto& node=expansion.nodes[n];
            std::set<NodeId> identities;
            for(std::size_t attachment=0;attachment<node.node.behaviour.size();++attachment) {
                const auto& b=node.node.behaviour[attachment];
                if(!b.id || !identities.insert(b.id).second) throw ScriptError("Duplicate Behaviour attachment identity");
                auto* entry=existing[{static_cast<int>(scene),node.path,b.id}];
                if(entry && entry->config.script!=b.script) entry=nullptr;
                if(!entry) {
                    const auto key=b.script+"#Behaviour";
                    if(!definitions.count(key)) {
                        std::string errors;
                        const auto* unit=checkBehaviour(*this,b.script,errors,1);
                        diagnostics(program.get(),errors);
                        if(!errors.empty()) throw ScriptError(errors);
                        compile(*this); lhat_program_install(program.get(),machine.get());
                        units[key]=unit; definitions[key]=definition(*this,unit,b.script);
                    }
                    auto prefix=node.path;
                    if(!prefix.empty() && !node.root) prefix.pop_back();
                    instantiate(*this,{static_cast<int>(scene),prefix,0,&b,node.node.name+" "+b.script,n,""});
                    entry=instances.back().get();
                }
                entry->alive=true; entry->config=b;
                entry->node=n; entry->attachment=attachment; entry->order=order++;
                for(int phase=0;phase<3;++phase)
                    if(!std::isfinite(effectivePriority(b,entry->defaults,phase))) throw ScriptError("Priority must be finite");
            }
        }
    }
    for(auto& entry:instances) if(!entry->alive) cancel(*entry);
    instances.erase(std::remove_if(instances.begin(),instances.end(),[](const auto& entry) { return !entry->alive; }),instances.end());
    orderDirty=true;
    for(const auto& stage:stages) dirtyQueries[stage.name]=true;
}

void ScriptRuntime::State::rebuildQuery(const std::string& system) {
    if(orderDirty) {
        for(int phase=0;phase<4;++phase) {
            auto& order=ordered[phase]; order.clear();
            for(auto& entry:instances)
                if(entry->alive && entry->config.enabled) order.push_back(entry.get());
            const auto priority=[&](const Instance* entry) {
                return phase<3?effectivePriority(entry->config,entry->defaults,phase):entry->config.priority.value_or(entry->defaults.priority);
            };
            std::sort(order.begin(),order.end(),[&](const Instance* a,const Instance* b) {
                const auto pa=priority(a),pb=priority(b);
                return pa!=pb?pa<pb:a->order<b->order;
            });
        }
        orderDirty=false;
    }
    if(!dirtyQueries[system]) return;
    auto& query=queries[system]; query.clear();
    const int phase=behaviourPhase(system);
    for(auto* entry:ordered[phase>=0?phase:3]) {
        bool matches=entry->node==Expansion::npos ? entry->stage==system : phase>=0 && entry->methods[phase];
        matches|=phase==0 && entry->node!=Expansion::npos && entry->start && !entry->started &&
            std::none_of(entry->methods.begin(),entry->methods.end(),[](bool present) { return present; });
        for(const auto& call:entry->calls) matches|=!lhat_is_nil(call.coroutine) && call.waiting==system;
        if(matches) query.push_back(entry);
    }
    dirtyQueries[system]=false;
}

void ScriptRuntime::State::execute(Instance& entry,int phase,LhatValue dt) {
    const auto& system=stages[currentStage].name;
    const auto suspend=[&](Invocation& call,LhatValue yielded) {
        std::string destination=call.origin;
        if(!lhat_is_nil(yielded)) {
            if(!lhat_is_object_kind(yielded,LHAT_OBJECT_HOSTDATA) ||
               reinterpret_cast<const LhatHostData*>(lhat_as_object(yielded))->tag!=this->system)
                throw ScriptError(entry.what+": yield must be nil^ or a throne.System");
            const auto index=reinterpret_cast<std::uintptr_t>(lhat_hostdata_pointer(yielded,this->system));
            if(index>=systems.size()) throw ScriptError("Invalid System wait target");
            destination=systems[index]->name;
        }
        const auto at=std::find_if(stages.begin(),stages.end(),[&](const auto& stage) { return stage.name==destination; });
        if(at==stages.end() || !at->parallel.empty() || destination=="FetchInput" || destination=="ApplyPhysics" ||
           destination=="SyncRender" || destination=="SyncSimulation")
            throw ScriptError(entry.what+": coroutine requires a serial script System");
        dirtyQueries[call.waiting]=true; dirtyQueries[destination]=true;
        call.waiting=destination;
        call.readyFrame=frame+((std::distance(stages.begin(),at)>currentStage)?0:1);
    };
    const auto resume=[&](Invocation& call) {
        const auto result=check(lhat_machine_resume(machine.get(),call.coroutine,nullptr,0),entry.what+" resume");
        call.lastCall=frame;
        if(lhat_machine_coroutine_done(call.coroutine)) {
            dirtyQueries[call.waiting]=true;
            call.coroutine=lhat_nil(); call.waiting.clear(); unpin(call.root);
        } else suspend(call,result);
    };
    const auto invoke=[&](int method) {
        auto& call=entry.calls[method]; call.origin=system; call.lastCall=frame;
        const char* name=method==3?"start":behaviourMethods[method];
        const LhatValue args[]={entry.system,dt};
        const auto result=check(lhat_machine_call_member(machine.get(),entry.instance,name,std::strlen(name),args,method==3?1:2),entry.what+" "+name);
        if(lhat_is_object_kind(result,LHAT_OBJECT_COROUTINE)) {
            call.coroutine=result; call.root=pin(result); resume(call);
        }
    };
    if(!entry.started) {
        entry.started=true;
        dirtyQueries[behaviourSystems[0]]=true;
        if(entry.start) invoke(3);
    }
    // Fixed method order resolves ties between a phase callback and a resumed call of the same instance.
    for(int method:{3,0,1,2}) {
        auto& call=entry.calls[method];
        if(!lhat_is_nil(call.coroutine)) {
            if(call.waiting==system && call.readyFrame<=frame) resume(call);
        } else if(method<3 && call.lastCall!=frame && entry.methods[method] &&
                  (entry.node==Expansion::npos ? method==0 && system==entry.stage : method==phase)) invoke(method);
    }
}

void ScriptRuntime::refreshBehaviours() { state_->registryDirty=true; }

std::vector<BehaviourMatch> ScriptRuntime::behaviourMatches(const std::string& system) const {
    std::vector<BehaviourMatch> result;
    state_->rebuildQuery(system);
    const auto found=state_->queries.find(system);
    if(found==state_->queries.end()) return result;
    const auto phase=behaviourPhase(system);
    for(const auto* entry:found->second) {
        if(entry->node==Expansion::npos) continue;
        bool waiting=false;
        for(const auto& call:entry->calls) waiting|=!lhat_is_nil(call.coroutine) && call.waiting==system;
        result.push_back({entry->scene,entry->node,entry->attachment,
            phase>=0?effectivePriority(entry->config,entry->defaults,phase):entry->config.priority.value_or(entry->defaults.priority),
            entry->config.script,waiting});
    }
    return result;
}

ScriptRuntime::ScriptRuntime(const World& world, LevelExpansion& expanded, const std::filesystem::path& root, Log log,
                             PlayScope scope)
    : state_(std::make_unique<State>(world, expanded, root, std::move(log))) {
    auto& s = *state_;
    try { s.stages = schedule(world, expanded); }
    catch (const std::exception& error) { throw ScriptError(error.what()); }
    createProgram(s);
    auto* program = s.program.get();

    std::string errors;
    // main.lh is a script; an await^ at its top level makes it a coroutine (TDDja/02 4.1).
    const LhatUnit* mainUnit = nullptr;
    if (scope == PlayScope::Game && std::filesystem::exists(s.root / mainPath))
        mainUnit = lhat_program_check(program, mainPath);

    // Run order: scene systems scene by scene, then level systems.
    std::vector<Pending> pending;
    const auto collect = [&](int scene, const NodePath& prefix, const System& system, const std::string& owner) {
        s.systems.push_back(&system);
        for (const auto& behaviour : system.behaviour)
            pending.push_back({scene, prefix, s.systems.size() - 1, &behaviour,
                               owner + " " + system.name + " " + behaviour.script, Expansion::npos, system.name, "Behaviour"});
    };
    // Scene and prefab systems come expanded, one copy per instance (TDDja/06 2).
    for (std::size_t i = 0; i < expanded.instances.size(); ++i)
        for (const auto& system : expanded.instances[i].expansion.systems)
            collect(static_cast<int>(i), system.instance, system.system, "scene " + std::to_string(expanded.instances[i].id));
    for (const auto& system : world.level.systems) collect(-1, {}, system, "level");

    for (std::size_t scene = 0; scene < expanded.instances.size(); ++scene) {
        const auto& nodes = expanded.instances[scene].expansion.nodes;
        for (const auto n : expanded.instances[scene].expansion.hierarchy()) {
            auto prefix = nodes[n].path;
            if (!prefix.empty() && !nodes[n].root) prefix.pop_back();
            for (const auto& behaviour : nodes[n].node.behaviour)
                pending.push_back({static_cast<int>(scene), prefix, 0, &behaviour,
                                   nodes[n].node.name + " " + behaviour.script, n, ""});
        }
    }
    std::map<std::string, const LhatUnit*> units;
    for (const auto& item : pending)
        units[item.behaviour->script + "#" + item.symbol] = checkBehaviour(s, item.behaviour->script, errors,
            item.node != Expansion::npos, item.symbol);
    for (const auto& item : pending) {
        if (item.node!=Expansion::npos) continue;
        const auto found = std::find_if(s.stages.begin(), s.stages.end(), [&](const ExecutionStage& stage) {
            return stage.name == item.stage && stage.parallel.empty();
        });
        if (found == s.stages.end() || behaviourPhase(item.stage)>=0 || item.stage == "FetchInput" || item.stage == "ApplyPhysics" ||
            item.stage == "SyncRender" || item.stage == "SyncSimulation")
            errors += item.what + ": unknown or engine-owned System " + item.stage + "\n";
    }
    diagnostics(program, errors);
    if (!errors.empty()) throw ScriptError(errors);
    compile(s);
    startMachine(s);

    if (mainUnit) s.mainProto = lhat_unit_proto(mainUnit);
    std::map<std::string, LhatValue> definitions;
    for (const auto& [key, unit] : units) {
        const auto split = key.rfind('#');
        definitions[key] = definition(s, unit, key.substr(0, split), key.substr(split + 1));
    }
    s.stageSystems.reserve(s.stages.size());
    for (const auto& stage : s.stages) {
        s.stageSystems.push_back({0, stage.name, {}});
        s.systems.push_back(&s.stageSystems.back());
    }
    std::vector<LhatValue> systemHandles;
    for (std::size_t i = 0; i < s.systems.size(); ++i) systemHandles.push_back(s.keep(s.handle(s.machine.get(), s.system, i)));

    s.units=units;
    s.definitions=definitions;
    s.systemHandles=systemHandles;
    for(const auto& item:pending) instantiate(s,item);
    s.registryDirty=false;
    for(const auto& stage:s.stages) s.dirtyQueries[stage.name]=true;
    for(const auto& stage:s.stages) s.rebuildQuery(stage.name);
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

std::vector<ExecutionStage> ScriptRuntime::schedule(const World& world, const LevelExpansion& expanded) {
    auto stages = world.level.stages;
    const auto add = [&](const System& system) {
        if (system.name == "OGRE" || system.name == "Box3D" || system.name == "Animator") return;
        if (std::none_of(stages.begin(), stages.end(), [&](const ExecutionStage& stage) { return stage.name == system.name; })) {
            const auto at = std::find_if(stages.begin(), stages.end(), [](const ExecutionStage& stage) { return stage.name == behaviourSystems[0]; });
            stages.insert(at, {system.name, {}});
        }
    };
    for (const auto& scene : expanded.instances)
        for (const auto& system : scene.expansion.systems) add(system.system);
    for (const auto& system : world.level.systems) add(system);
    validateStages(stages);
    return stages;
}

void ScriptRuntime::update(float deltaSeconds) { update(deltaSeconds, {}); }

void ScriptRuntime::update(float deltaSeconds, const std::function<void(const std::string&)>& engine) {
    auto& s = *state_;
    ++s.frame;
    s.reconcile();
    if (!s.started) {
        s.started = true;
        if (s.mainProto) {
            // A script without await^ runs to its end here; one with await^ answers its coroutine.
            const auto result = s.check(lhat_run(s.machine.get(), s.mainProto), mainPath);
            if (lhat_is_object_kind(result, LHAT_OBJECT_COROUTINE)) s.game = s.keep(result);
            else s.finished = true;
        }

    }
    s.time += deltaSeconds;
    const LhatValue dt = lhat_real(deltaSeconds);
    if (!lhat_is_nil(s.game) && !s.finished) {
        s.check(lhat_machine_resume(s.machine.get(), s.game, nullptr, 0), mainPath);
        s.finished = lhat_machine_coroutine_done(s.game);
    }
    runStages(s.stages, s.world.level.serialExecution || !engine, [&](const std::string& stage) {
        if (engine) engine(stage);
        if (stage == "Box3D" || stage == "Animator" || stage == "OGRE") return;
        const auto found=std::find_if(s.stages.begin(),s.stages.end(),[&](const auto& item) { return item.name==stage; });
        s.currentStage=static_cast<int>(std::distance(s.stages.begin(),found));
        s.rebuildQuery(stage);
        const auto& query=s.queries[stage];
        for(auto* instance:query) s.execute(*instance,behaviourPhase(stage),dt);
        s.currentStage=-1;
    });
}
}
