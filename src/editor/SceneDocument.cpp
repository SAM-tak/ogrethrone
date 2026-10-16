#include "editor/SceneDocument.h"
#include <lhat.h>
#include <stdlib/lton.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace throne {
namespace {
constexpr std::string_view sceneFormat = "ogrethrone-scene";
constexpr LhatInteger sceneVersion = 1;

using Machine = std::unique_ptr<LhatMachine, decltype(&lhat_machine_dispose)>;
using Program = std::unique_ptr<LhatProgram, decltype(&lhat_program_free)>;

/// Creates a machine, throwing when there is no memory.
Machine newMachine() {
    Machine machine(lhat_machine_new(), &lhat_machine_dispose);
    if (!machine) throw std::runtime_error("Out of memory");
    return machine;
}

/// Returns the double whose shortest decimal form matches @p value's, so the
/// LTON text says 0.32 rather than the float's exact binary expansion.
LhatValue real(float value) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof text - 1, value).ptr;
    *end = '\0';
    return lhat_real(std::strtod(text, nullptr));
}

/// Builds plain data tables on a machine for lhatstdlib_lton_stringify.
///
/// No L^ code runs while a document is built, so the collector cannot run and
/// the values need no rooting (see stdlib/lton.h).
class Builder {
public:
    Builder() : machine_(newMachine()) {}

    /// Returns a new empty table.
    LhatValue table() {
        LhatValue result;
        if (!lhat_machine_make_table(machine_.get(), &result)) throw std::runtime_error("Out of memory");
        return result;
    }
    /// Returns a string value holding @p text.
    LhatValue string(std::string_view text) {
        LhatValue result;
        if (!lhat_machine_make_string(machine_.get(), text.data(), text.size(), &result))
            throw std::runtime_error("Out of memory");
        return result;
    }
    /// Stores @p value under @p key in @p table.
    void set(LhatValue table, LhatValue key, LhatValue value) {
        bool refused = false;
        if (!lhat_machine_table_set(machine_.get(), reinterpret_cast<LhatTable*>(lhat_as_object(table)), key,
                                    value, &refused) ||
            refused)
            throw std::runtime_error("Could not build scene table");
    }
    /// Stores @p value under the string key @p key.
    void set(LhatValue table, std::string_view key, LhatValue value) { set(table, string(key), value); }
    /// Returns a positional table of the three components.
    LhatValue vector(const float* values) {
        auto result = table();
        for (LhatInteger i = 0; i < 3; ++i) {
            if (!std::isfinite(values[i])) throw std::runtime_error("Non-finite scene value");
            set(result, lhat_integer(i), real(values[i]));
        }
        return result;
    }

private:
    Machine machine_;
};

/// Writes @p text to a temporary sibling of @p path and then replaces @p path.
void writeAtomically(const std::filesystem::path& path, std::string_view text) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not replace scene file");
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

[[noreturn]] void invalid() { throw std::runtime_error("Invalid or unsupported OgreThrone scene"); }

/// Returns @p value as a table, or throws.
const LhatTable& table(LhatValue value) {
    if (!lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) invalid();
    return *reinterpret_cast<const LhatTable*>(lhat_as_object(value));
}

/// Returns the value stored under the string key @p key.
LhatValue field(const LhatTable& table, std::string_view key) {
    return lhat_table_get_bytes(&table, key.data(), key.size());
}

/// Returns @p value as a string, or throws.
std::string_view string(LhatValue value) {
    if (!lhat_is_object_kind(value, LHAT_OBJECT_STRING)) invalid();
    const auto* text = reinterpret_cast<const LhatString*>(lhat_as_object(value));
    return {text->text, text->length};
}

/// Returns @p value as a number, accepting both integers and reals, or throws.
double number(LhatValue value) {
    if (lhat_is_integer(value)) return static_cast<double>(lhat_as_integer(value));
    if (lhat_is_real(value)) return lhat_as_real(value);
    invalid();
}

/// Returns @p value as a scene object ID (a positive integer below UINT32_MAX), or throws.
std::uint32_t id(LhatValue value) {
    if (!lhat_is_integer(value) || lhat_as_integer(value) <= 0 ||
        lhat_as_integer(value) >= std::numeric_limits<std::uint32_t>::max())
        invalid();
    return static_cast<std::uint32_t>(lhat_as_integer(value));
}

/// Reads a three-element positional table of finite numbers within +-1e6 into @p values.
void vector(LhatValue value, float* values) {
    const auto& components = table(value);
    if (lhat_table_length(&components) != 3 || lhat_table_count(&components) != 3) invalid();
    for (LhatInteger i = 0; i < 3; ++i) {
        const double component = number(lhat_table_get(&components, lhat_integer(i)));
        if (!std::isfinite(component) || std::abs(component) > 1.0e6) invalid();
        values[i] = static_cast<float>(component);
    }
}
}

SceneObject& SceneDocument::addCube() {
    if (nextId == std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Scene object ID limit reached");
    SceneObject object;
    object.id = nextId++;
    object.name = "Cube " + std::to_string(object.id);
    objects.push_back(object);
    return objects.back();
}

SceneObject* SceneDocument::find(std::uint32_t id) {
    auto it = std::find_if(objects.begin(), objects.end(),
                           [id](const auto& object) { return object.id == id; });
    return it == objects.end() ? nullptr : &*it;
}

SceneDocument SceneDocument::example() {
    SceneDocument result;
    result.addCube();
    auto& second = result.addCube();
    second.position = {2, 0.75f, -1};
    second.scale = {1, 1.5f, 1};
    second.colour = {0.86f, 0.53f, 0.25f};
    return result;
}

void SceneDocument::save(const std::filesystem::path& path) const {
    Builder build;
    const auto root = build.table();
    build.set(root, "format", build.string(sceneFormat));
    build.set(root, "version", lhat_integer(sceneVersion));
    const auto list = build.table();
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        const auto entry = build.table();
        build.set(entry, "id", lhat_integer(object.id));
        build.set(entry, "name", build.string(object.name));
        build.set(entry, "type", build.string("cube"));
        build.set(entry, "position", build.vector(object.position.data()));
        build.set(entry, "rotation", build.vector(object.rotation.data()));
        build.set(entry, "scale", build.vector(object.scale.data()));
        build.set(entry, "colour", build.vector(object.colour.data()));
        if (!object.script.empty()) build.set(entry, "script", build.string(object.script));
        build.set(list, lhat_integer(static_cast<LhatInteger>(i)), entry);
    }
    build.set(root, "objects", list);

    char* text = nullptr;
    std::size_t length = 0;
    if (lhatstdlib_lton_stringify(root, &text, &length) != LHAT_LTON_OK)
        throw std::runtime_error("Could not serialize scene");
    const std::unique_ptr<char, decltype(&lhat_free)> owned(text, &lhat_free);
    writeAtomically(path, {text, length});
}

SceneDocument SceneDocument::load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open scene file");
    const std::string text((std::istreambuf_iterator<char>(stream)), {});

    // The text is evaluated as the body of an f^, so it cannot call anything with effects.
    Program program(lhat_program_new(true, nullptr, nullptr), &lhat_program_free);
    auto machine = newMachine();
    if (!program) throw std::runtime_error("Out of memory");
    LhatValue value;
    const auto name = path.filename().u8string();
    switch (lhatstdlib_lton_parse(machine.get(), program.get(), name.c_str(), text.data(), text.size(), &value)) {
    case LHAT_LTON_OK: break;
    case LHAT_LTON_REJECTED:
        throw std::runtime_error(std::string("Invalid scene file: ") + lhat_program_load_failure(program.get()));
    case LHAT_LTON_FAULTED: throw std::runtime_error("Scene file failed while being evaluated");
    default: throw std::runtime_error("Could not read scene file");
    }

    const auto& root = table(value);
    if (string(field(root, "format")) != sceneFormat) invalid();
    const auto version = field(root, "version");
    if (!lhat_is_integer(version) || lhat_as_integer(version) != sceneVersion) invalid();
    const auto& list = table(field(root, "objects"));
    if (lhat_table_count(&list) != lhat_table_length(&list)) invalid();

    SceneDocument result;
    std::set<std::uint32_t> ids;
    for (std::size_t i = 0; i < lhat_table_length(&list); ++i) {
        const auto& entry = table(lhat_table_get(&list, lhat_integer(static_cast<LhatInteger>(i))));
        if (string(field(entry, "type")) != "cube") invalid();
        SceneObject object;
        object.id = id(field(entry, "id"));
        if (!ids.insert(object.id).second) invalid();
        object.name = string(field(entry, "name"));
        if (object.name.empty() || object.name.size() > 255 || object.name.find('\0') != std::string::npos)
            invalid();
        vector(field(entry, "position"), object.position.data());
        vector(field(entry, "rotation"), object.rotation.data());
        vector(field(entry, "scale"), object.scale.data());
        vector(field(entry, "colour"), object.colour.data());
        if (const auto script = field(entry, "script"); !lhat_is_nil(script)) {
            object.script = string(script);
            if (object.script.size() > 255 || object.script.find(' ') != std::string::npos) invalid();
        }
        for (unsigned axis = 0; axis < 3; ++axis)
            if (object.scale.data()[axis] < 0.001f || object.colour[axis] < 0 || object.colour[axis] > 1)
                invalid();
        result.nextId = std::max(result.nextId, object.id + 1);
        result.objects.push_back(std::move(object));
    }
    return result;
}
}
