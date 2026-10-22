#include "editor/Editor.h"
#include <OgreDataStream.h>
#include <OgreImage2.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreRoot.h>
#include <OgreRenderSystem.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpuManager.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace throne {
namespace {
namespace fs = std::filesystem;

/// Drag-and-drop payload naming a Browser entry by its path in the assets folder.
constexpr const char* assetPayload = "THRONE_ASSET";
/// The largest side of an image's thumbnail, in pixels.
constexpr std::uint32_t thumbnailSide = 128;

/// What a Browser file is, by its name (TDDja/15 7): how it is drawn and what it opens into.
enum class AssetKind { Folder, Level, Scene, Prefab, Mixer, Script, Model, Texture, Environment, Audio, Particle, Other };

/// Whether @p name ends with @p suffix, ignoring case.
bool endsWith(const std::string& name, std::string_view suffix) {
    if (name.size() < suffix.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(name[name.size() - suffix.size() + i])) != suffix[i]) return false;
    return true;
}

AssetKind kindOf(const std::string& name, bool folder = false) {
    if (folder) return AssetKind::Folder;
    if (endsWith(name, ".level.lton")) return AssetKind::Level;
    if (endsWith(name, ".scene.lton")) return AssetKind::Scene;
    if (endsWith(name, ".prefab.lton")) return AssetKind::Prefab;
    if (endsWith(name, ".mixer.lton")) return AssetKind::Mixer;
    if (endsWith(name, ".lh")) return AssetKind::Script;
    if (endsWith(name, ".fbx") || endsWith(name, ".mesh")) return AssetKind::Model;
    for (const char* image : {".png", ".jpg", ".jpeg", ".dds", ".tga", ".bmp"})
        if (endsWith(name, image)) return AssetKind::Texture;
    if (endsWith(name, ".hdr")) return AssetKind::Environment;
    for (const char* sound : {".wav", ".ogg", ".mp3", ".flac"})
        if (endsWith(name, sound)) return AssetKind::Audio;
    if (endsWith(name, ".particle2")) return AssetKind::Particle;
    return AssetKind::Other;
}

/// A kind's name, and the short label and colour its icon is drawn with.
struct KindStyle {
    const char *name, *label;
    ImU32 colour;
};
KindStyle styleOf(AssetKind kind) {
    switch (kind) {
    case AssetKind::Folder: return {"Folder", "", IM_COL32(214, 172, 74, 255)};
    case AssetKind::Level: return {"Level", "LVL", IM_COL32(204, 96, 72, 255)};
    case AssetKind::Scene: return {"Scene", "SCN", IM_COL32(80, 150, 214, 255)};
    case AssetKind::Prefab: return {"Prefab", "PFB", IM_COL32(96, 170, 255, 255)};
    case AssetKind::Mixer: return {"Mixer", "MIX", IM_COL32(170, 120, 210, 255)};
    case AssetKind::Script: return {"Script", "L^", IM_COL32(110, 190, 110, 255)};
    case AssetKind::Model: return {"Model", "3D", IM_COL32(220, 140, 60, 255)};
    case AssetKind::Texture: return {"Texture", "IMG", IM_COL32(120, 120, 200, 255)};
    case AssetKind::Environment: return {"Environment", "HDR", IM_COL32(90, 170, 170, 255)};
    case AssetKind::Audio: return {"Audio", "SND", IM_COL32(210, 110, 160, 255)};
    case AssetKind::Particle: return {"Particles", "FX", IM_COL32(230, 200, 90, 255)};
    default: return {"File", "FILE", IM_COL32(130, 130, 130, 255)};
    }
}

/// Draws @p kind's icon filling the square at @p min of side @p size.
void drawIcon(ImDrawList& draw, ImVec2 min, float size, AssetKind kind) {
    const auto style = styleOf(kind);
    const ImVec2 max{min.x + size, min.y + size};
    if (kind == AssetKind::Folder) {
        const float tab = size * .4f, top = min.y + size * .18f;
        draw.AddRectFilled({min.x + size * .06f, top}, {min.x + size * .06f + tab, top + size * .14f}, style.colour, size * .04f);
        draw.AddRectFilled({min.x + size * .06f, top + size * .1f}, {max.x - size * .06f, max.y - size * .14f}, style.colour, size * .06f);
        return;
    }
    draw.AddRectFilled({min.x + size * .14f, min.y + size * .06f}, {max.x - size * .14f, max.y - size * .06f}, IM_COL32(58, 62, 70, 255),
                       size * .06f);
    draw.AddRectFilled({min.x + size * .14f, max.y - size * .38f}, {max.x - size * .14f, max.y - size * .06f}, style.colour, size * .06f,
                       ImDrawFlags_RoundCornersBottom);
    // Small icons (the list's) are only coloured: the label would not fit.
    const auto text = ImGui::CalcTextSize(style.label);
    if (text.x > size * .7f) return;
    draw.AddText({min.x + (size - text.x) * .5f, max.y - size * .22f - text.y * .5f}, IM_COL32_WHITE, style.label);
}

/// @p name shortened with an ellipsis to fit @p width.
std::string fitted(const std::string& name, float width) {
    if (ImGui::CalcTextSize(name.c_str()).x <= width) return name;
    std::string text = name;
    while (!text.empty() && ImGui::CalcTextSize((text + "...").c_str()).x > width) {
        text.pop_back();
        // Never cut a UTF-8 sequence in two.
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80)) text.pop_back();
    }
    return text + "...";
}

/// The part of @p name before its first dot (a file's name without its kind's extension),
/// and the rest.
std::pair<std::string, std::string> splitName(const std::string& name, bool folder) {
    if (folder) return {name, {}};
    const auto dot = name.find('.', 1);
    return dot == std::string::npos ? std::pair{name, std::string()} : std::pair{name.substr(0, dot), name.substr(dot)};
}

/// @p folder joined with @p name, as paths in the assets folder are written.
std::string join(const std::string& folder, const std::string& name) { return folder.empty() ? name : folder + "/" + name; }

/// A path in @p folder named @p stem, numbered from @p first when taken, with @p extension.
std::string freePath(const Project& project, const std::string& folder, std::string stem, const std::string& extension, int first = 1) {
    // Characters a file name cannot hold become '_'.
    for (auto& c : stem)
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || (c >= 0 && c < 32)) c = '_';
    for (int number = first;; ++number) {
        const auto path = join(folder, stem + (number > 1 ? " " + std::to_string(number) : "") + extension);
        if (!fs::exists(project.resolve(path))) return path;
    }
}
}

void Editor::clearThumbnails() {
    auto* textures = root_.getRenderSystem()->getTextureGpuManager();
    for (auto& [path, thumbnail] : thumbnails_) {
        if (thumbnail.reading.valid()) thumbnail.reading.wait();
        if (thumbnail.texture) textures->destroyTexture(thumbnail.texture);
    }
    thumbnails_.clear();
}

Ogre::TextureGpu* Editor::thumbnail(const std::string& path) {
    auto& entry = thumbnails_[path];
    if (entry.texture || entry.failed) return entry.texture;
    if (!entry.reading.valid()) {
        // A few images are read at once; the rest wait for a later frame.
        const auto reading = std::count_if(thumbnails_.begin(), thumbnails_.end(),
                                           [](const auto& each) { return each.second.reading.valid(); });
        if (reading >= 4) return nullptr;
        entry.reading = std::async(std::launch::async, [file = mediaFile(path)]() -> std::shared_ptr<Ogre::Image2> {
            try {
                auto* stream = new std::ifstream(file, std::ios::binary);
                Ogre::DataStreamPtr data(OGRE_NEW Ogre::FileStreamDataStream(stream, true));
                Ogre::Image2 image;
                auto extension = file.extension().u8string();
                if (!extension.empty()) extension.erase(0, 1);
                image.load(data, extension);
                // The smallest mipmap that still covers the thumbnail, or the image scaled down.
                std::uint8_t mip = 0;
                while (mip + 1 < image.getNumMipmaps() && std::max(image.getWidth() >> (mip + 1), image.getHeight() >> (mip + 1)) >= thumbnailSide)
                    ++mip;
                const auto box = image.getData(mip);
                auto small = std::make_shared<Ogre::Image2>();
                small->createEmptyImage(box.width, box.height, 1, Ogre::TextureTypes::Type2D, image.getPixelFormat(), 1);
                small->getData(0).copyFrom(box);
                if (std::max(box.width, box.height) > thumbnailSide && !Ogre::PixelFormatGpuUtils::isCompressed(image.getPixelFormat())) {
                    const float scale = float(thumbnailSide) / float(std::max(box.width, box.height));
                    small->resize(std::max(1u, std::uint32_t(box.width * scale)), std::max(1u, std::uint32_t(box.height * scale)));
                }
                return small;
            } catch (const std::exception&) {
                return nullptr;
            }
        });
        return nullptr;
    }
    if (entry.reading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    const auto image = entry.reading.get();
    if (!image) {
        entry.failed = true;
        return nullptr;
    }
    try {
        auto* textures = root_.getRenderSystem()->getTextureGpuManager();
        auto* texture = textures->createTexture("ThroneThumbnail/" + mediaFile(path).generic_u8string(), Ogre::GpuPageOutStrategy::Discard,
                                                Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2D);
        texture->setResolution(image->getWidth(), image->getHeight());
        // ImGui draws to a target that is not sRGB: the colours are shown as they are stored.
        texture->setPixelFormat(Ogre::PixelFormatGpuUtils::getEquivalentLinear(image->getPixelFormat()));
        texture->setNumMipmaps(1);
        texture->scheduleTransitionTo(Ogre::GpuResidency::Resident);
        image->uploadTo(texture, 0, 0);
        entry.texture = texture;
    } catch (const std::exception&) {
        entry.failed = true;
    }
    return entry.texture;
}

void Editor::rescanAssets() {
    assets_.scan([this](const std::string& line) { log(line); });
    clearThumbnails();
    browserListed_.clear();
}

void Editor::importFiles(const std::vector<fs::path>& files) {
    std::size_t copied = 0;
    for (const auto& file : files) {
        const auto name = file.filename().u8string();
        const auto [stem, extension] = splitName(name, fs::is_directory(file));
        const auto path = freePath(*project_, browserFolder_, stem, extension);
        try {
            fs::copy(file, project_->resolve(path), fs::copy_options::recursive);
            ++copied;
        } catch (const std::exception& error) {
            log("Could not import " + file.u8string() + ": " + error.what());
        }
    }
    if (!copied) return;
    // The copies get their IDs (and .meta files) at once, so they can be referred to.
    rescanAssets();
    log("Imported " + std::to_string(copied) + " item(s) into assets/" + browserFolder_);
}

void Editor::openAsset(const std::string& path) {
    const auto ref = assets_.bind(AssetRef(path));
    switch (kindOf(path)) {
    case AssetKind::Level:
        if (runtime_ || path.size() >= sizeof levelPath_) return;
        std::memcpy(levelPath_, path.c_str(), path.size() + 1);
        open();
        break;
    case AssetKind::Scene:
        if (!runtime_) enter({Context::Kind::Scene, ref});
        break;
    case AssetKind::Prefab:
        if (!runtime_) enter({Context::Kind::Prefab, ref});
        break;
    case AssetKind::Model:
        if (!runtime_) enterModel({ref});
        break;
    case AssetKind::Mixer:
        try {
            openMixer(ref);
        } catch (const std::exception& error) {
            log(error.what());
        }
        break;
    default: platform_.openFile(mediaFile(ref));
    }
}

void Editor::newAsset(const std::string& stem, const std::string& extension,
                      const std::function<void(const fs::path&)>& write) {
    const auto path = freePath(*project_, browserFolder_, stem, extension);
    try {
        write(project_->resolve(path));
        assets_.add(path);
        browserSelection_ = renaming_ = path;
        std::snprintf(renameText_, sizeof renameText_, "%s", splitName(fs::u8path(path).filename().u8string(), false).first.c_str());
        renameFocus_ = true;
        browserListed_.clear();
    } catch (const std::exception& error) {
        log("Could not create " + path + ": " + error.what());
    }
}

void Editor::moveAsset(const std::string& from, const std::string& to) {
    if (from == to) return;
    try {
        assets_.move(from, to);
        // The open level is kept by its path.
        const std::string level = levelPath_;
        if ((level == from || level.rfind(from + "/", 0) == 0) && to.size() + level.size() - from.size() < sizeof levelPath_)
            std::snprintf(levelPath_, sizeof levelPath_, "%s", (to + level.substr(from.size())).c_str());
        if (browserSelection_ == from) browserSelection_ = to;
        if (browserFolder_ == from || browserFolder_.rfind(from + "/", 0) == 0) browserFolder_ = to + browserFolder_.substr(from.size());
        browserListed_.clear();
    } catch (const std::exception& error) {
        log("Could not move " + from + ": " + error.what());
    }
}

bool Editor::place(const std::string& path) {
    if (runtime_) return false;
    const auto ref = assets_.bind(AssetRef(path));
    switch (kindOf(path)) {
    case AssetKind::Prefab: addInstance(ref); return true;
    case AssetKind::Model: addModel(ref); return true;
    case AssetKind::Scene:
        if (content()) return false;
        addScene(ref);
        return true;
    default: return false;
    }
}

void Editor::browserTree(const std::string& relative) {
    std::vector<std::string> folders;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(project_->resolve(relative), error)) {
        const auto name = entry.path().filename().u8string();
        if (entry.is_directory() && !name.empty() && name.front() != '.') folders.push_back(name);
    }
    std::sort(folders.begin(), folders.end());
    for (const auto& name : folders) {
        const auto path = join(relative, name);
        bool leaf = true;
        for (const auto& entry : fs::directory_iterator(project_->resolve(path), error))
            if (entry.is_directory() && entry.path().filename().u8string().front() != '.') leaf = false;
        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (leaf) flags |= ImGuiTreeNodeFlags_Leaf;
        if (browserFolder_ == path) flags |= ImGuiTreeNodeFlags_Selected;
        // The chosen folder's parents open, so it can be seen.
        if (browserFolder_.rfind(path + "/", 0) == 0) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
        const bool open = ImGui::TreeNodeEx(path.c_str(), flags, "%s", name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) browserFolder_ = path;
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload(assetPayload)) {
                const std::string dragged(static_cast<const char*>(payload->Data));
                moveAsset(dragged, join(path, fs::u8path(dragged).filename().u8string()));
            }
            ImGui::EndDragDropTarget();
        }
        if (open) {
            browserTree(path);
            ImGui::TreePop();
        }
    }
}

void Editor::browser() {
    browserFocused_ = false;
    if (ImGui::Begin("Browser")) {
        if (!project_) {
            ImGui::End();
            return;
        }
        browserMin_ = ImGui::GetWindowPos();
        browserMax_ = {browserMin_.x + ImGui::GetWindowWidth(), browserMin_.y + ImGui::GetWindowHeight()};
        browserFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (!fs::is_directory(project_->resolve(browserFolder_))) browserFolder_.clear();
        if (ImGui::BeginTable("##browser", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("##folders")) {
                auto flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (browserFolder_.empty()) flags |= ImGuiTreeNodeFlags_Selected;
                const bool open = ImGui::TreeNodeEx("##assets", flags, "assets");
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) browserFolder_.clear();
                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload(assetPayload)) {
                        const std::string dragged(static_cast<const char*>(payload->Data));
                        moveAsset(dragged, fs::u8path(dragged).filename().u8string());
                    }
                    ImGui::EndDragDropTarget();
                }
                if (open) {
                    browserTree({});
                    ImGui::TreePop();
                }
            }
            ImGui::EndChild();
            ImGui::TableNextColumn();
            browserFiles();
            ImGui::EndTable();
        }
    }
    ImGui::End();
    // Deleting asks first: references to what goes keep their IDs and show as missing.
    if (deleting_) {
        ImGui::OpenPopup("Delete Asset");
        if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete %s?", deleting_->c_str());
            ImGui::TextDisabled("What refers to it will show it as missing. This cannot be undone.");
            if (ImGui::Button("Delete")) {
                try {
                    assets_.remove(*deleting_);
                    if (browserSelection_ == *deleting_) browserSelection_.clear();
                } catch (const std::exception& error) {
                    log("Could not delete " + *deleting_ + ": " + error.what());
                }
                browserListed_.clear();
                deleting_.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                deleting_.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void Editor::browserFiles() {
    // The breadcrumb of the chosen folder, then how the files are shown.
    if (ImGui::SmallButton("assets")) browserFolder_.clear();
    if (!browserFolder_.empty()) {
        std::string walked;
        for (const auto& part : fs::u8path(browserFolder_)) {
            walked = join(walked, part.u8string());
            ImGui::SameLine(0, 2);
            ImGui::TextUnformatted("/");
            ImGui::SameLine(0, 2);
            ImGui::PushID(walked.c_str());
            if (ImGui::SmallButton(part.u8string().c_str())) browserFolder_ = walked;
            ImGui::PopID();
        }
    }
    ImGui::SameLine();
    const float right = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, right - 440));
    ImGui::SetNextItemWidth(140);
    ImGui::InputTextWithHint("##filter", "Filter", browserFilter_, sizeof browserFilter_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::BeginCombo("##kind", browserKind_ ? styleOf(AssetKind(browserKind_)).name : "All Kinds")) {
        if (ImGui::Selectable("All Kinds", browserKind_ == 0)) browserKind_ = 0;
        for (int kind = int(AssetKind::Level); kind <= int(AssetKind::Other); ++kind)
            if (ImGui::Selectable(styleOf(AssetKind(kind)).name, browserKind_ == kind)) browserKind_ = kind;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(browserList_ ? "Grid" : "List")) browserList_ = !browserList_;
    if (!browserList_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderFloat("##size", &browserTile_, 40, 160, "");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) rescanAssets();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scan the assets folder for changes made outside the editor (Ctrl+R)");

    // The chosen folder's entries, folders first; listed again now and then, not every frame.
    const auto now = std::chrono::steady_clock::now();
    if (browserListed_ != browserFolder_ || now - browserListedAt_ > std::chrono::seconds(1)) {
        browserEntries_.clear();
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(project_->resolve(browserFolder_), error)) {
            const auto name = entry.path().filename().u8string();
            if (name.empty() || name.front() == '.' || endsWith(name, ".meta") || endsWith(name, ".tmp")) continue;
            BrowserEntry item{name, join(browserFolder_, name), entry.is_directory()};
            item.time = entry.last_write_time(error);
            if (!item.folder) item.size = entry.file_size(error);
            browserEntries_.push_back(std::move(item));
        }
        std::sort(browserEntries_.begin(), browserEntries_.end(), [](const BrowserEntry& a, const BrowserEntry& b) {
            return a.folder != b.folder ? a.folder : a.name < b.name;
        });
        browserListed_ = browserFolder_;
        browserListedAt_ = now;
    }
    std::string filter = browserFilter_;
    for (auto& c : filter) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::vector<const BrowserEntry*> shownEntries;
    for (const auto& entry : browserEntries_) {
        auto name = entry.name;
        for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        // Folders stay while a kind is chosen, so the way to such files does too.
        const bool kind = !browserKind_ || entry.folder || int(kindOf(entry.name)) == browserKind_;
        if (kind && (filter.empty() || name.find(filter) != std::string::npos)) shownEntries.push_back(&entry);
    }

    if (!ImGui::BeginChild("##files", {0, 0}, ImGuiChildFlags_None)) {
        ImGui::EndChild();
        return;
    }
    // A node dropped on a folder becomes a prefab there, named after it.
    const auto prefabInto = [&](const std::string& folder, const DragNode& dragged) {
        const auto* file = fileOf(dragged.instance);
        const auto* node = file ? file->findNode(dragged.id) : nullptr;
        prefabify(dragged.instance, dragged.id, AssetRef(freePath(*project_, folder, node ? node->name : "Prefab", ".prefab.lton")));
        browserListed_.clear();
    };
    // What one entry does: selection, opening, dragging, dropping and its menu.
    const auto interact = [&](const BrowserEntry& entry) {
        const auto kind = kindOf(entry.name, entry.folder);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) browserSelection_ = entry.path;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (entry.folder) browserFolder_ = entry.path;
            else openAsset(entry.path);
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(assetPayload, entry.path.c_str(), entry.path.size() + 1);
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            // Files dropped on a folder move into it.
            if (entry.folder)
                if (const auto* payload = ImGui::AcceptDragDropPayload(assetPayload)) {
                    const std::string dragged(static_cast<const char*>(payload->Data));
                    moveAsset(dragged, join(entry.path, fs::u8path(dragged).filename().u8string()));
                }
            // A node dropped on a folder becomes a prefab there; on a prefab, it replaces it
            // (or an instance applies its overrides to its own prefab).
            if (!runtime_ && (entry.folder || kind == AssetKind::Prefab))
                if (const auto* payload = ImGui::AcceptDragDropPayload(nodePayload)) {
                    const auto& dragged = *static_cast<const DragNode*>(payload->Data);
                    if (entry.folder) prefabInto(entry.path, dragged);
                    else dropOnPrefab(dragged.instance, dragged.id, assets_.bind(AssetRef(entry.path)));
                }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##entry")) {
            if (ImGui::MenuItem("Open")) entry.folder ? void(browserFolder_ = entry.path) : openAsset(entry.path);
            if (!entry.folder && kind != AssetKind::Script && kind != AssetKind::Other && ImGui::MenuItem("Open in Application"))
                platform_.openFile(mediaFile(entry.path));
            if (ImGui::MenuItem("Rename", "F2")) {
                renaming_ = entry.path;
                std::snprintf(renameText_, sizeof renameText_, "%s", splitName(entry.name, entry.folder).first.c_str());
                renameFocus_ = true;
            }
            if (!entry.folder && ImGui::MenuItem("Duplicate")) {
                const auto [stem, extension] = splitName(entry.name, false);
                const auto path = freePath(*project_, browserFolder_, stem, extension, 2);
                try {
                    assets_.duplicate(entry.path, path);
                    browserSelection_ = path;
                } catch (const std::exception& error) {
                    log("Could not duplicate " + entry.path + ": " + error.what());
                }
                browserListed_.clear();
            }
            if (ImGui::MenuItem("Delete", "Delete")) deleting_ = entry.path;
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(entry.path.c_str());
            if (ImGui::MenuItem("Show in Explorer")) platform_.showInFolder(mediaFile(entry.path));
            ImGui::EndPopup();
        }
    };
    // The name typed over an entry being renamed: Enter renames it, Escape or leaving cancels.
    const auto rename = [&](const BrowserEntry& entry, float width) {
        if (renameFocus_) ImGui::SetKeyboardFocusHere(), renameFocus_ = false;
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputText("##rename", renameText_, sizeof renameText_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            const auto extension = splitName(entry.name, entry.folder).second;
            if (renameText_[0]) moveAsset(entry.path, join(browserFolder_, renameText_ + extension));
            renaming_.clear();
        } else if (ImGui::IsItemDeactivated()) {
            renaming_.clear();
        }
    };
    auto* draw = ImGui::GetWindowDrawList();
    if (browserList_) {
        if (ImGui::BeginTable("##list", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(int(shownEntries.size()));
            while (clipper.Step())
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto& entry = *shownEntries[std::size_t(row)];
                    const auto kind = kindOf(entry.name, entry.folder);
                    ImGui::PushID(entry.path.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const float line = ImGui::GetTextLineHeight();
                    drawIcon(*draw, ImGui::GetCursorScreenPos(), line, kind);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + line + 4);
                    if (renaming_ == entry.path) {
                        rename(entry, -1);
                    } else {
                        ImGui::Selectable(entry.name.c_str(), browserSelection_ == entry.path,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                        interact(entry);
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", styleOf(kind).name);
                    ImGui::TableNextColumn();
                    if (!entry.folder) ImGui::TextDisabled("%.1f KB", double(entry.size) / 1024);
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    } else {
        // Tiles of an icon (or an image's thumbnail) above a name, in rows filling the width.
        const float tile = browserTile_, label = ImGui::GetTextLineHeightWithSpacing(), gap = ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (tile + gap)));
        const int rows = (int(shownEntries.size()) + columns - 1) / columns;
        ImGuiListClipper clipper;
        clipper.Begin(rows, tile + label + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                for (int column = 0; column < columns; ++column) {
                    const auto index = std::size_t(row * columns + column);
                    if (index >= shownEntries.size()) break;
                    const auto& entry = *shownEntries[index];
                    const auto kind = kindOf(entry.name, entry.folder);
                    if (column) ImGui::SameLine();
                    ImGui::PushID(entry.path.c_str());
                    const auto at = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##tile", {tile, tile + label});
                    const bool hovered = ImGui::IsItemHovered();
                    if (renaming_ != entry.path) interact(entry);
                    if (browserSelection_ == entry.path || hovered)
                        draw->AddRectFilled(at, {at.x + tile, at.y + tile + label},
                                            ImGui::GetColorU32(browserSelection_ == entry.path ? ImGuiCol_Header : ImGuiCol_HeaderHovered), 4);
                    const float inset = tile * .08f, picture = tile - inset * 2;
                    auto* image = !entry.folder && kind == AssetKind::Texture ? thumbnail(entry.path) : nullptr;
                    if (image) {
                        // The image keeps its proportions inside the square.
                        const float width = float(image->getWidth()), height = float(image->getHeight());
                        const float scale = picture / std::max(width, height);
                        const ImVec2 min{at.x + inset + (picture - width * scale) * .5f, at.y + inset + (picture - height * scale) * .5f};
                        draw->AddImage(image, min, {min.x + width * scale, min.y + height * scale});
                    } else {
                        drawIcon(*draw, {at.x + inset, at.y + inset}, picture, kind);
                    }
                    if (renaming_ == entry.path) {
                        const auto after = ImGui::GetCursorScreenPos();
                        ImGui::SetCursorScreenPos({at.x, at.y + tile});
                        rename(entry, tile);
                        ImGui::SetCursorScreenPos(after);
                    } else {
                        const auto name = fitted(entry.name, tile - 4);
                        const auto width = ImGui::CalcTextSize(name.c_str()).x;
                        draw->AddText({at.x + (tile - width) * .5f, at.y + tile}, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
                        if (hovered && name != entry.name) ImGui::SetTooltip("%s", entry.name.c_str());
                    }
                    ImGui::PopID();
                }
    }
    // The empty space: its menu makes new files and folders, and files dropped there land here.
    if (ImGui::BeginPopupContextWindow("##folder", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("New Folder")) {
            const auto path = freePath(*project_, browserFolder_, "New Folder", "");
            fs::create_directories(project_->resolve(path));
            browserSelection_ = renaming_ = path;
            std::snprintf(renameText_, sizeof renameText_, "%s", fs::u8path(path).filename().u8string().c_str());
            renameFocus_ = true;
            browserListed_.clear();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Level")) newAsset("New Level", ".level.lton", [](const fs::path& file) { Level().save(file); });
        if (ImGui::MenuItem("New Scene")) newAsset("New Scene", ".scene.lton", [](const fs::path& file) { Scene().save(file); });
        if (ImGui::MenuItem("New Prefab"))
            newAsset("New Prefab", ".prefab.lton", [](const fs::path& file) {
                Prefab prefab;
                prefab.addNode("New Prefab");
                prefab.save(file);
            });
        if (ImGui::MenuItem("New Mixer"))
            newAsset("New Mixer", ".mixer.lton", [](const fs::path& file) { MixerDefinition::defaults().save(file); });
        if (ImGui::MenuItem("New Script"))
            newAsset("new_behaviour", ".lh", [](const fs::path& file) {
                lton::writeAtomically(file, "module^ " + file.stem().u8string() +
                                                "\nimport^ throne\npublic^let^ Behaviour = def^{\n    self^{ },\n"
                                                "    update = p^self^, node:throne.SceneNode, dt:number^ {\n    },\n}\n");
            });
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer")) platform_.showInFolder(project_->resolve(browserFolder_));
        ImGui::EndPopup();
    }
    // F2 renames the selected entry; Delete asks to delete it.
    if (ImGui::IsWindowFocused() && !browserSelection_.empty() && renaming_.empty() && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            renaming_ = browserSelection_;
            const auto name = fs::u8path(browserSelection_).filename().u8string();
            std::snprintf(renameText_, sizeof renameText_, "%s",
                          splitName(name, fs::is_directory(project_->resolve(browserSelection_))).first.c_str());
            renameFocus_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleting_ = browserSelection_;
    }
    ImGui::EndChild();
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(assetPayload)) {
            const std::string dragged(static_cast<const char*>(payload->Data));
            moveAsset(dragged, join(browserFolder_, fs::u8path(dragged).filename().u8string()));
        }
        if (!runtime_)
            if (const auto* payload = ImGui::AcceptDragDropPayload(nodePayload))
                prefabInto(browserFolder_, *static_cast<const DragNode*>(payload->Data));
        ImGui::EndDragDropTarget();
    }
}
}
