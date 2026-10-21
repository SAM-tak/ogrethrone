#pragma once
#include <soloud.h>
#include <memory>
#include <string>

namespace throne {
/// Bounded streaming decoder. File reads and decoder rewinds run on a producer,
/// never on SoLoud's callback. One voice at a time, as owned by AudioWorld.
class BufferedStream : public SoLoud::AudioSource {
public:
    explicit BufferedStream(const std::string& filename);
    ~BufferedStream() override;
    /// Called only after the preceding voice stopped, on the simulation thread.
    void restart();
    bool failed() const;
    SoLoud::AudioSourceInstance* createInstance() override;
private:
    struct State;
    class Instance;
    std::unique_ptr<State> state_;
};
}
