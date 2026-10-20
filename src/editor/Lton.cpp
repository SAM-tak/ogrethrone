#include "editor/Lton.h"
#include <stdlib/lton.h>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace throne::lton {
namespace {
/// Returns the double whose shortest decimal form matches @p value's, so the
/// text says 0.32 rather than the float's exact binary expansion.
LhatValue real(float value) {
    char text[32];
    *std::to_chars(text, text + sizeof text - 1, value).ptr = '\0';
    return lhat_real(std::strtod(text, nullptr));
}

/// Returns a new machine, throwing when there is no memory.
LhatMachine* newMachine() {
    auto* machine = lhat_machine_new();
    if (!machine) throw std::bad_alloc();
    return machine;
}
}

Writer::Writer() : machine_(newMachine()) {}
Writer::~Writer() { lhat_machine_dispose(machine_); }

LhatValue Writer::table() {
    LhatValue result;
    if (!lhat_machine_make_table(machine_, &result)) throw std::bad_alloc();
    return result;
}

LhatValue Writer::string(std::string_view text) {
    LhatValue result;
    if (!lhat_machine_make_string(machine_, text.data(), text.size(), &result)) throw std::bad_alloc();
    return result;
}

void Writer::set(LhatValue table, LhatValue key, LhatValue value) {
    bool refused = false;
    if (!lhat_machine_table_set(machine_, reinterpret_cast<LhatTable*>(lhat_as_object(table)), key, value, &refused) ||
        refused)
        throw std::bad_alloc();
}

void Writer::push(LhatValue table, LhatValue value) {
    const auto length = lhat_table_length(reinterpret_cast<const LhatTable*>(lhat_as_object(table)));
    set(table, lhat_integer(static_cast<LhatInteger>(length)), value);
}

LhatValue Writer::vector(const float* values) {
    auto result = table();
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(values[i])) throw std::runtime_error("Non-finite value cannot be saved");
        push(result, real(values[i]));
    }
    return result;
}

void Writer::save(LhatValue root, const std::filesystem::path& path) {
    char* text = nullptr;
    std::size_t length = 0;
    if (lhatstdlib_lton_stringify(root, &text, &length) != LHAT_LTON_OK)
        throw std::runtime_error("Could not serialize " + path.filename().u8string());
    const std::unique_ptr<char, decltype(&lhat_free)> owned(text, &lhat_free);
    writeAtomically(path, {text, length});
}

Document::Document(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw Invalid("Could not open " + path.u8string());
    const std::string text((std::istreambuf_iterator<char>(stream)), {});
    program_ = lhat_program_new(true, nullptr, nullptr);
    machine_ = lhat_machine_new();
    if (!program_ || !machine_) {
        release();
        throw std::bad_alloc();
    }
    LhatValue value;
    const auto name = path.filename().u8string();
    const auto status =
        lhatstdlib_lton_parse(machine_, program_, name.c_str(), text.data(), text.size(), &value);
    if (status != LHAT_LTON_OK || !lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) {
        std::string message = name + ": ";
        message += status == LHAT_LTON_REJECTED  ? lhat_program_load_failure(program_)
                   : status == LHAT_LTON_FAULTED ? "failed while being evaluated"
                                                 : "not an LTON table";
        release();
        throw Invalid(message);
    }
    root_ = reinterpret_cast<const LhatTable*>(lhat_as_object(value));
}

Document::~Document() { release(); }

void Document::release() {
    if (machine_) lhat_machine_dispose(machine_);
    if (program_) lhat_program_free(program_);
    machine_ = nullptr;
    program_ = nullptr;
}

void Document::expect(std::string_view format, LhatInteger version) const {
    if (string(field(root(), "format")) != format || integer(field(root(), "version")) != version)
        throw Invalid("Not an " + std::string(format) + " version " + std::to_string(version) + " file");
}

void invalid() { throw Invalid("Invalid or unsupported OgreThrone file"); }

const LhatTable& table(LhatValue value) {
    if (!lhat_is_object_kind(value, LHAT_OBJECT_TABLE)) invalid();
    return *reinterpret_cast<const LhatTable*>(lhat_as_object(value));
}

LhatValue field(const LhatTable& table, std::string_view key) {
    return lhat_table_get_bytes(&table, key.data(), key.size());
}

LhatValue at(const LhatTable& table, std::size_t index) {
    return lhat_table_get(&table, lhat_integer(static_cast<LhatInteger>(index)));
}

std::size_t sequence(const LhatTable& table) {
    if (lhat_table_count(&table) != lhat_table_length(&table)) invalid();
    return lhat_table_length(&table);
}

std::string_view string(LhatValue value) {
    if (!lhat_is_object_kind(value, LHAT_OBJECT_STRING)) invalid();
    const auto* text = reinterpret_cast<const LhatString*>(lhat_as_object(value));
    return {text->text, text->length};
}

double number(LhatValue value) {
    if (lhat_is_integer(value)) return static_cast<double>(lhat_as_integer(value));
    if (lhat_is_real(value)) return lhat_as_real(value);
    invalid();
}

LhatInteger integer(LhatValue value) {
    if (!lhat_is_integer(value)) invalid();
    return lhat_as_integer(value);
}

void vector(LhatValue value, float* values) {
    const auto& components = table(value);
    if (sequence(components) != 3) invalid();
    for (std::size_t i = 0; i < 3; ++i) {
        const double component = number(at(components, i));
        if (!std::isfinite(component) || std::abs(component) > 1.0e6) invalid();
        values[i] = static_cast<float>(component);
    }
}

void vectorOrNumber(LhatValue value, float* values) {
    if (!lhat_is_number(value)) return vector(value, values);
    const double n = number(value);
    if (!std::isfinite(n) || std::abs(n) > 1.0e6) invalid();
    values[0] = values[1] = values[2] = static_cast<float>(n);
}

std::string relativePath(LhatValue value) {
    const std::string text(string(value));
    const auto path = std::filesystem::u8path(text).lexically_normal();
    if (text.size() > 255 || text.find('\0') != std::string::npos || path.is_absolute() || path.has_root_name() ||
        (!path.empty() && *path.begin() == ".."))
        invalid();
    return text;
}

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
            throw std::runtime_error("Could not replace " + path.u8string());
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
}
