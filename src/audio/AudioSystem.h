#pragma once

#include <phonon.h>
#include <soloud.h>

namespace throne {
// Keep one device-backed AudioSystem alive; SoLoud's miniaudio backend is global.
class AudioSystem {
public:
    explicit AudioSystem(bool offline = false);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    SoLoud::Soloud& mixer() { return mixer_; }
    IPLContext steam() const { return context_; }

private:
    IPLContext context_ = nullptr;
    SoLoud::Soloud mixer_;
};
}
