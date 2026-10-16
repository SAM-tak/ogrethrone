#pragma once

#include <lhat.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace throne::lton {
/// A file was not valid LTON, or not the expected kind of document.
struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Builds plain data tables and serializes them with lhatstdlib_lton_stringify.
///
/// No L^ code runs while a document is built, so the collector cannot run and
/// the values need no rooting (see stdlib/lton.h).
class Writer {
public:
    Writer();
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Returns a new empty table.
    LhatValue table();
    /// Returns a string value holding @p text.
    LhatValue string(std::string_view text);
    /// Stores @p value under @p key in @p table.
    void set(LhatValue table, LhatValue key, LhatValue value);
    /// Stores @p value under the string key @p key.
    void set(LhatValue table, std::string_view key, LhatValue value) { set(table, string(key), value); }
    /// Appends @p value to the positional part of @p table.
    void push(LhatValue table, LhatValue value);
    /// Returns a positional table of three finite components, written by their shortest decimal form.
    LhatValue vector(const float* values);
    /// Serializes @p root and replaces @p path with it atomically.
    void save(LhatValue root, const std::filesystem::path& path);

private:
    LhatMachine* machine_;
};

/// A parsed LTON file. Values read from root() stay valid while the document lives.
class Document {
public:
    /// Reads @p path. The text is evaluated as the body of an f^, so it cannot
    /// call anything with effects.
    /// @throws Invalid when the file cannot be read or is not LTON.
    explicit Document(const std::filesystem::path& path);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    /// The top-level table.
    const LhatTable& root() const { return *root_; }
    /// Throws Invalid unless root() has `format = format` and `version = version`.
    void expect(std::string_view format, LhatInteger version) const;

private:
    /// Frees the machine and program; safe to call more than once.
    void release();

    LhatProgram* program_ = nullptr;
    LhatMachine* machine_ = nullptr;
    const LhatTable* root_ = nullptr;
};

/// Throws Invalid with a message naming the file kind.
[[noreturn]] void invalid();
/// Returns @p value as a table, or throws Invalid.
const LhatTable& table(LhatValue value);
/// Returns the value stored under the string key @p key (nil^ when absent).
LhatValue field(const LhatTable& table, std::string_view key);
/// Returns the positional element @p index.
LhatValue at(const LhatTable& table, std::size_t index);
/// Number of positional elements, requiring that the table has nothing else.
std::size_t sequence(const LhatTable& table);
/// Returns @p value as a string, or throws Invalid.
std::string_view string(LhatValue value);
/// Returns @p value as a number (integer or real), or throws Invalid.
double number(LhatValue value);
/// Returns @p value as an integer, or throws Invalid.
LhatInteger integer(LhatValue value);
/// Reads a three-element positional table of finite numbers within +-1e6.
void vector(LhatValue value, float* values);
/// Returns a project-relative path: a string of at most 255 bytes that is
/// relative, has no `..` and no NUL. Throws Invalid otherwise.
std::string relativePath(LhatValue value);

/// Writes @p text to a temporary sibling of @p path and then replaces @p path.
void writeAtomically(const std::filesystem::path& path, std::string_view text);
}
