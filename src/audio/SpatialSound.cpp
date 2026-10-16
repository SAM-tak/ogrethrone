#include "SpatialSound.h"
#include <soloud_wav.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace throne {
namespace {
constexpr unsigned blockSize = SAMPLE_GRANULARITY;
void check(IPLerror result) {
    if (result != IPL_STATUS_SUCCESS) throw std::runtime_error("Steam Audio effect creation failed");
}
}

struct SpatialSound::Data {
    IPLContext context = nullptr;
    IPLHRTF hrtf = nullptr;
    SoLoud::Wav wave;
    IPLVector3 direction{};
    IPLDirectEffectParams direct{};
    ~Data() {
        iplHRTFRelease(&hrtf);
        iplContextRelease(&context);
    }
};

class SpatialSound::Instance : public SoLoud::AudioSourceInstance {
public:
    explicit Instance(std::shared_ptr<Data> data) : data_(std::move(data)) {
        IPLAudioSettings audio{static_cast<int>(data_->wave.mBaseSamplerate), blockSize};
        IPLDirectEffectSettings direct{1};
        check(iplDirectEffectCreate(data_->context, &audio, &direct, &direct_));
        IPLBinauralEffectSettings binaural{data_->hrtf};
        const auto result = iplBinauralEffectCreate(data_->context, &audio, &binaural, &binaural_);
        if (result != IPL_STATUS_SUCCESS) {
            iplDirectEffectRelease(&direct_);
            check(result);
        }
    }
    ~Instance() override {
        iplBinauralEffectRelease(&binaural_);
        iplDirectEffectRelease(&direct_);
    }
    unsigned getAudio(float* output, unsigned count, unsigned stride) override {
        unsigned written = 0;
        while (written < count && !hasEnded()) {
            if (pending_ == blockSize) renderBlock();
            const auto n = (std::min)(count - written, blockSize - pending_);
            for (unsigned channel = 0; channel < 2; ++channel)
                std::copy_n(stereo_[channel].data() + pending_, n, output + channel * stride + written);
            pending_ += n;
            written += n;
        }
        return written;
    }
    bool hasEnded() override {
        return !(mFlags & LOOPING) && offset_ >= data_->wave.mSampleCount &&
               pending_ == blockSize && tail_ == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
    }
    SoLoud::result rewind() override {
        offset_ = 0;
        pending_ = blockSize;
        tail_ = IPL_AUDIOEFFECTSTATE_TAILREMAINING;
        iplDirectEffectReset(direct_);
        iplBinauralEffectReset(binaural_);
        return SoLoud::SO_NO_ERROR;
    }

private:
    void renderBlock() {
        float* channels[2] = {stereo_[0].data(), stereo_[1].data()};
        IPLAudioBuffer output{2, blockSize, channels};
        if (offset_ >= data_->wave.mSampleCount && !(mFlags & LOOPING)) {
            tail_ = iplBinauralEffectGetTail(binaural_, &output);
        } else {
            mono_.fill(0);
            for (unsigned i = 0; i < blockSize; ++i) {
                if (offset_ == data_->wave.mSampleCount) {
                    if (!(mFlags & LOOPING)) break;
                    offset_ = 0;
                }
                mono_[i] = data_->wave.mData[offset_++];
            }
            float* mono = mono_.data();
            IPLAudioBuffer input{1, blockSize, &mono};
            iplDirectEffectApply(direct_, &data_->direct, &input, &input);
            IPLBinauralEffectParams params{};
            params.direction = data_->direction;
            params.hrtf = data_->hrtf;
            params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
            params.spatialBlend = 1.0f;
            tail_ = iplBinauralEffectApply(binaural_, &params, &input, &output);
        }
        pending_ = 0;
    }
    std::shared_ptr<Data> data_;
    IPLDirectEffect direct_ = nullptr;
    IPLBinauralEffect binaural_ = nullptr;
    std::array<float, blockSize> mono_{};
    std::array<std::array<float, blockSize>, 2> stereo_{};
    unsigned offset_ = 0;
    unsigned pending_ = blockSize;
    IPLAudioEffectState tail_ = IPL_AUDIOEFFECTSTATE_TAILREMAINING;
};

SpatialSound::SpatialSound(AudioSystem& audio, const char* filename, IPLVector3 direction,
                           IPLDirectEffectParams direct) : data_(std::make_shared<Data>()) {
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!std::isfinite(length) || length <= 0)
        throw std::invalid_argument("Spatial sound direction must be finite and nonzero");
    const auto result = data_->wave.load(filename);
    if (result != SoLoud::SO_NO_ERROR)
        throw std::runtime_error(std::string("Cannot decode audio: ") + filename);
    if (data_->wave.mChannels != 1 || data_->wave.mSampleCount == 0)
        throw std::invalid_argument("SpatialSound requires a nonempty mono clip; use SoLoud for stereo BGM/UI");
    data_->context = iplContextRetain(audio.steam());
    data_->direction = {direction.x / length, direction.y / length, direction.z / length};
    data_->direct = direct;
    mChannels = 2;
    mBaseSamplerate = data_->wave.mBaseSamplerate;
    IPLAudioSettings settings{static_cast<int>(mBaseSamplerate), blockSize};
    IPLHRTFSettings hrtf{};
    hrtf.type = IPL_HRTFTYPE_DEFAULT;
    hrtf.volume = 1.0f;
    check(iplHRTFCreate(data_->context, &settings, &hrtf, &data_->hrtf));
}

SpatialSound::~SpatialSound() { stop(); }
SoLoud::AudioSourceInstance* SpatialSound::createInstance() { return new Instance(data_); }
}
