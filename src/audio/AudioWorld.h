#pragma once
#include "audio/AudioSystem.h"
#include "audio/AudioComponents.h"
#include "audio/Mixer.h"
#include "editor/Expansion.h"
#include <functional>
#include <memory>
#include <optional>

namespace throne {
/// A Play session's audio state. All methods run on the simulation thread or between frames.
class AudioWorld {
public:
    using Log = std::function<void(const std::string&)>;
    /// @param root The assets folder clips are found in; @param assets finds clips referred to by
    /// ID (null: by path). Assets must not change while it plays.
    AudioWorld(AudioSystem& audio, std::filesystem::path root, const MixerDefinition& mixer, Log log = {},
               const AssetDatabase* assets = nullptr);
    ~AudioWorld();
    struct ListenerPose {
        Vec3 position, right{1,0,0}, up{0,1,0}, forward{0,0,-1};
    };
    /// An editor listener overrides every scene priority, without modifying scene data.
    void spatialStep(const LevelExpansion& world,std::optional<ListenerPose> listenerOverride = {});
    void step(const LevelExpansion& world);
    void command(std::size_t instance, std::size_t node, AudioCommand command);
    bool playing(std::size_t instance, std::size_t node) const;
    void setPaused(bool paused);
    void setMixer(const MixerDefinition& definition);
    void clear();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
