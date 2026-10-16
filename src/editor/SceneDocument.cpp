#include "editor/SceneDocument.h"
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace throne {
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
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("format"); writer.String("ogrethrone-scene");
    writer.Key("version"); writer.Uint(1);
    writer.Key("objects"); writer.StartArray();
    for (const auto& object : objects) {
        writer.StartObject();
        writer.Key("id"); writer.Uint(object.id);
        writer.Key("name"); writer.String(object.name.c_str(),
                                           static_cast<rapidjson::SizeType>(object.name.size()));
        writer.Key("type"); writer.String("cube");
        const auto vector = [&](const char* key, const float* values) {
            writer.Key(key); writer.StartArray();
            for (unsigned i = 0; i < 3; ++i) {
                if (!std::isfinite(values[i])) throw std::runtime_error("Non-finite scene value");
                writer.Double(values[i]);
            }
            writer.EndArray();
        };
        vector("position", object.position.data());
        vector("rotation", object.rotation.data());
        vector("scale", object.scale.data());
        vector("colour", object.colour.data());
        writer.EndObject();
    }
    writer.EndArray(); writer.EndObject();
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
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

SceneDocument SceneDocument::load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open scene file");
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    rapidjson::Document json;
    json.Parse(text.data(), text.size());
    const auto invalid = [] { throw std::runtime_error("Invalid or unsupported OgreThrone scene"); };
    if (json.HasParseError() || !json.IsObject() || !json.HasMember("format") ||
        !json["format"].IsString() || std::string(json["format"].GetString()) != "ogrethrone-scene" ||
        !json.HasMember("version") || !json["version"].IsUint() || json["version"].GetUint() != 1 ||
        !json.HasMember("objects") || !json["objects"].IsArray()) invalid();
    SceneDocument result;
    std::set<std::uint32_t> ids;
    for (const auto& entry : json["objects"].GetArray()) {
        if (!entry.IsObject() || !entry.HasMember("id") || !entry["id"].IsUint() ||
            !entry.HasMember("name") || !entry["name"].IsString() ||
            !entry.HasMember("type") || !entry["type"].IsString() ||
            std::string(entry["type"].GetString()) != "cube") invalid();
        SceneObject object;
        object.id = entry["id"].GetUint();
        if (!object.id || object.id == std::numeric_limits<std::uint32_t>::max() ||
            !ids.insert(object.id).second) invalid();
        object.name.assign(entry["name"].GetString(), entry["name"].GetStringLength());
        if (object.name.empty() || object.name.size() > 255 || object.name.find('\0') != std::string::npos)
            invalid();
        const auto vector = [&](const char* key, float* values) {
            if (!entry.HasMember(key) || !entry[key].IsArray() || entry[key].Size() != 3) invalid();
            for (unsigned i = 0; i < 3; ++i) {
                if (!entry[key][i].IsNumber()) invalid();
                const double value = entry[key][i].GetDouble();
                if (!std::isfinite(value) || std::abs(value) > 1.0e6) invalid();
                values[i] = static_cast<float>(value);
            }
        };
        vector("position", object.position.data());
        vector("rotation", object.rotation.data());
        vector("scale", object.scale.data());
        vector("colour", object.colour.data());
        for (unsigned i = 0; i < 3; ++i)
            if (object.scale.data()[i] < 0.001f || object.colour[i] < 0 || object.colour[i] > 1) invalid();
        result.nextId = std::max(result.nextId, object.id + 1);
        result.objects.push_back(std::move(object));
    }
    return result;
}
}
