#include "SpatialSound.h"
#include "SteamAudioMath.h"
#include <soloud_wav.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#include <climits>

namespace throne {
namespace {
constexpr unsigned blockSize = SAMPLE_GRANULARITY;
static_assert(std::atomic<float>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free && std::atomic<int>::is_always_lock_free,
              "Spatial parameter publication must not block the audio callback");
static_assert(std::atomic<const ReflectionResult*>::is_always_lock_free,"IR publication must be lock free");
void check(IPLerror result) {
    if (result != IPL_STATUS_SUCCESS) throw std::runtime_error("Steam Audio effect creation failed");
}
}

struct SpatialSound::Data {
    IPLContext context = nullptr;
    IPLHRTF hrtf = nullptr;
    std::shared_ptr<SoLoud::Wav> wave;
    std::vector<float> resampled;
    unsigned sampleCount() const { return resampled.empty()?wave->mSampleCount:static_cast<unsigned>(resampled.size()); }
    const float* samples() const { return resampled.empty()?wave->mData:resampled.data(); }
    std::atomic<unsigned> version{0};
    std::array<std::atomic<float>,22> parameters{};
    std::atomic<int> flags{0}, transmissionType{0};
    // Readers announce entry before loading the pointer. Only the simulation writer
    // owns/reclaims snapshots; the audio callback performs atomic loads and native DSP.
    std::atomic<unsigned> readers{0};
    std::atomic<const ReflectionResult*> reflection{nullptr};
    std::shared_ptr<const ReflectionResult> current;
    std::vector<std::shared_ptr<const ReflectionResult>> retired;
    IPLDirectEffectParams direct{};
    ~Data() {
        iplHRTFRelease(&hrtf);
        iplContextRelease(&context);
    }
};

class SpatialSound::Instance : public SoLoud::AudioSourceInstance {
public:
    explicit Instance(std::shared_ptr<Data> data) : data_(std::move(data)) {
        IPLAudioSettings audio{reflectionRate, blockSize};
        try {
        IPLDirectEffectSettings direct{1};
        check(iplDirectEffectCreate(data_->context, &audio, &direct, &direct_));
        IPLBinauralEffectSettings binaural{data_->hrtf};
        check(iplBinauralEffectCreate(data_->context, &audio, &binaural, &binaural_));
        IPLReflectionEffectSettings reflections{IPL_REFLECTIONEFFECTTYPE_CONVOLUTION,reflectionSamples,reflectionChannels};
        check(iplReflectionEffectCreate(data_->context,&audio,&reflections,&reflection_));
        IPLAmbisonicsDecodeEffectSettings decode{}; decode.speakerLayout.type=IPL_SPEAKERLAYOUTTYPE_STEREO;
        decode.hrtf=data_->hrtf; decode.maxOrder=reflectionOrder;
        check(iplAmbisonicsDecodeEffectCreate(data_->context,&audio,&decode,&decode_));
        } catch(...) {
            iplAmbisonicsDecodeEffectRelease(&decode_); iplReflectionEffectRelease(&reflection_);
            iplBinauralEffectRelease(&binaural_);
            iplDirectEffectRelease(&direct_);
            throw;
        }
    }
    ~Instance() override {
        iplAmbisonicsDecodeEffectRelease(&decode_); iplReflectionEffectRelease(&reflection_);
        iplBinauralEffectRelease(&binaural_);
        iplDirectEffectRelease(&direct_);
    }
    unsigned getAudio(float* output, unsigned count, unsigned stride) override {
        struct Read {
            Data& data; explicit Read(Data& d):data(d) { data.readers.fetch_add(1); }
            ~Read() { data.readers.fetch_sub(1); }
        } read(*data_);
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
        return !(mFlags & LOOPING) && offset_ >= data_->sampleCount() &&
               pending_ == blockSize && tail_ == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE &&
               reflectionTail_==IPL_AUDIOEFFECTSTATE_TAILCOMPLETE && decodeTail_==IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
    }
    SoLoud::result rewind() override {
        offset_ = 0;
        pending_ = blockSize;
        tail_ = IPL_AUDIOEFFECTSTATE_TAILREMAINING;
        iplDirectEffectReset(direct_);
        iplBinauralEffectReset(binaural_);
        iplReflectionEffectReset(reflection_); iplAmbisonicsDecodeEffectReset(decode_);
        reflectionTail_=decodeTail_=IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
        return SoLoud::SO_NO_ERROR;
    }

private:
    void renderBlock() {
        const auto before=data_->version.load();
        if(!(before&1)) {
            std::array<float,22> next;
            for(unsigned i=0;i<next.size();++i) next[i]=data_->parameters[i].load();
            const auto flags=data_->flags.load(), type=data_->transmissionType.load();
            if(before==data_->version.load()) parameters_=next,flags_=flags,transmissionType_=type;
        }
        float* channels[2] = {stereo_[0].data(), stereo_[1].data()};
        IPLAudioBuffer output{2, blockSize, channels};
        const bool ended=offset_ >= data_->sampleCount() && !(mFlags & LOOPING);
        mono_.fill(0);
        if (ended) {
            tail_ = iplBinauralEffectGetTail(binaural_, &output);
        } else {
            for (unsigned i = 0; i < blockSize; ++i) {
                if (offset_ == data_->sampleCount()) {
                    if (!(mFlags & LOOPING)) break;
                    offset_ = 0;
                }
                mono_[i] = data_->samples()[offset_++];
            }
            wetInput_=mono_; // Reflected sound must not inherit direct-path occlusion/EQ.
            float* mono = mono_.data();
            IPLAudioBuffer input{1, blockSize, &mono};
            IPLDirectEffectParams direct{};
            direct.flags=static_cast<IPLDirectEffectFlags>(flags_);
            direct.transmissionType=static_cast<IPLTransmissionType>(transmissionType_);
            direct.distanceAttenuation=parameters_[3];
            for(unsigned i=0;i<3;++i) direct.airAbsorption[i]=parameters_[4+i];
            direct.directivity=parameters_[7]; direct.occlusion=parameters_[8];
            for(unsigned i=0;i<3;++i) direct.transmission[i]=parameters_[9+i];
            iplDirectEffectApply(direct_, &direct, &input, &input);
            IPLBinauralEffectParams params{};
            params.direction = {parameters_[0],parameters_[1],parameters_[2]};
            params.hrtf = data_->hrtf;
            params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
            params.spatialBlend = 1.0f;
            tail_ = iplBinauralEffectApply(binaural_, &params, &input, &output);
        }
        float* ambisonicChannels[reflectionChannels];
        for(unsigned c=0;c<reflectionChannels;++c) ambisonicChannels[c]=ambisonic_[c].data();
        IPLAudioBuffer ambisonic{reflectionChannels,blockSize,ambisonicChannels};
        float* wetChannels[2]={wet_[0].data(),wet_[1].data()};
        IPLAudioBuffer wet{2,blockSize,wetChannels};
        const auto* result=data_->reflection.load();
        const bool inputActive=!ended && result && parameters_[12]>0;
        const bool renderWet=inputActive || reflectionTail_==IPL_AUDIOEFFECTSTATE_TAILREMAINING;
        if(inputActive) {
            auto params=result->parameters;
            float* mono=wetInput_.data(); IPLAudioBuffer input{1,blockSize,&mono};
            reflectionTail_=iplReflectionEffectApply(reflection_,&params,&input,&ambisonic,nullptr);
        } else if(renderWet) reflectionTail_=iplReflectionEffectGetTail(reflection_,&ambisonic,nullptr);
        if(renderWet) {
            IPLAmbisonicsDecodeEffectParams params{}; params.order=reflectionOrder;
            params.hrtf=data_->hrtf; params.binaural=IPL_TRUE;
            params.orientation={{parameters_[13],parameters_[14],parameters_[15]},
                                {parameters_[16],parameters_[17],parameters_[18]},
                                {parameters_[19],parameters_[20],parameters_[21]},{0,0,0}};
            decodeTail_=iplAmbisonicsDecodeEffectApply(decode_,&params,&ambisonic,&wet);
        } else if(decodeTail_==IPL_AUDIOEFFECTSTATE_TAILREMAINING)
            decodeTail_=iplAmbisonicsDecodeEffectGetTail(decode_,&wet);
        else for(auto& channel:wet_) channel.fill(0);
        for(unsigned c=0;c<2;++c) for(unsigned i=0;i<blockSize;++i) stereo_[c][i]+=wet_[c][i]*parameters_[12];
        pending_ = 0;
    }
    std::shared_ptr<Data> data_;
    IPLDirectEffect direct_ = nullptr;
    IPLBinauralEffect binaural_ = nullptr;
    IPLReflectionEffect reflection_=nullptr;
    IPLAmbisonicsDecodeEffect decode_=nullptr;
    std::array<float,blockSize> wetInput_{};
    std::array<std::array<float,blockSize>,reflectionChannels> ambisonic_{};
    std::array<std::array<float,blockSize>,2> wet_{};
    std::array<float, blockSize> mono_{};
    std::array<std::array<float, blockSize>, 2> stereo_{};
    std::array<float,22> parameters_{{0,0,-1,1,1,1,1,1,1,1,1,1,0,1,0,0,0,1,0,0,0,-1}};
    int flags_=IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION,transmissionType_=IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    unsigned offset_ = 0;
    unsigned pending_ = blockSize;
    IPLAudioEffectState tail_ = IPL_AUDIOEFFECTSTATE_TAILREMAINING;
    IPLAudioEffectState reflectionTail_=IPL_AUDIOEFFECTSTATE_TAILCOMPLETE,decodeTail_=IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
};

namespace {
std::shared_ptr<SoLoud::Wav> loadClip(const char* filename) {
    auto clip=std::make_shared<SoLoud::Wav>();
    if(clip->load(filename)!=SoLoud::SO_NO_ERROR) throw std::runtime_error(std::string("Cannot decode audio: ")+filename);
    return clip;
}
}
SpatialSound::SpatialSound(AudioSystem& audio, const char* filename, Vec3 direction,
                           IPLDirectEffectParams direct) : SpatialSound(audio,loadClip(filename),direction,direct) {}

SpatialSound::SpatialSound(AudioSystem& audio, std::shared_ptr<SoLoud::Wav> clip, Vec3 direction,
                           IPLDirectEffectParams direct) : data_(std::make_shared<Data>()) {
    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!std::isfinite(length) || length <= 0 || !clip)
        throw std::invalid_argument("Spatial sound direction must be finite and nonzero");
    data_->wave=std::move(clip);
    if (data_->wave->mChannels != 1 || data_->wave->mSampleCount == 0)
        throw std::invalid_argument("SpatialSound requires a nonempty mono clip; use SoLoud for stereo BGM/UI");
    data_->context = iplContextRetain(audio.steam());
    // Native reflection IRs use one engine rate, independent of an imported clip's rate.
    if(data_->wave->mBaseSamplerate!=reflectionRate) {
        const double ratio=double(data_->wave->mBaseSamplerate)/reflectionRate;
        const double count=std::ceil(data_->wave->mSampleCount/ratio);
        if(!std::isfinite(count) || count<1 || count>UINT_MAX) throw std::runtime_error("Invalid spatial clip sample rate/length");
        data_->resampled.resize(static_cast<std::size_t>(count));
        for(std::size_t i=0;i<data_->resampled.size();++i) {
            const double sample=i*ratio; const auto a=(std::min)(static_cast<unsigned>(sample),data_->wave->mSampleCount-1);
            const auto b=(std::min)(a+1,data_->wave->mSampleCount-1);
            data_->resampled[i]=data_->wave->mData[a]+float(sample-a)*(data_->wave->mData[b]-data_->wave->mData[a]);
        }
    }
    data_->direct = direct;
    update(direction, (direct.flags & IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION) ? direct.distanceAttenuation : 1.0f);
    mChannels = 2;
    mBaseSamplerate = reflectionRate;
    data_->parameters[13].store(1); data_->parameters[17].store(1); data_->parameters[21].store(-1);
    IPLAudioSettings settings{static_cast<int>(mBaseSamplerate), blockSize};
    IPLHRTFSettings hrtf{};
    hrtf.type = IPL_HRTFTYPE_DEFAULT;
    hrtf.volume = 1.0f;
    check(iplHRTFCreate(data_->context, &settings, &hrtf, &data_->hrtf));
}
void SpatialSound::updateReflections(std::shared_ptr<const ReflectionResult> result,float gain,IPLCoordinateSpace3 listener) {
    if(!std::isfinite(gain) || gain<0 || gain>10) throw std::invalid_argument("Invalid reflection gain");
    const std::array<float,9> axes{{listener.right.x,listener.right.y,listener.right.z,listener.up.x,listener.up.y,listener.up.z,
                                  listener.ahead.x,listener.ahead.y,listener.ahead.z}};
    for(float value:axes) if(!std::isfinite(value)) throw std::invalid_argument("Invalid reflection listener orientation");
    if(data_->readers.load()==0) data_->retired.clear();
    // Bound retained native simulations even if a callback runs continuously. Keep
    // rendering the preceding IR until a later control update can publish safely.
    if(data_->current!=result && data_->retired.size()<2) {
        if(data_->current) data_->retired.push_back(std::move(data_->current));
        data_->current=std::move(result); data_->reflection.store(data_->current.get());
    }
    data_->version.fetch_add(1);
    data_->parameters[12].store(gain);
    for(unsigned i=0;i<9;++i) data_->parameters[13+i].store(axes[i]);
    data_->version.fetch_add(1);
}

void SpatialSound::update(Vec3 direction, float gain) {
    auto direct=data_->direct;
    direct.flags=static_cast<IPLDirectEffectFlags>(direct.flags | IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);
    direct.distanceAttenuation=gain; update(direction,direct);
}
void SpatialSound::update(Vec3 direction, const IPLDirectEffectParams& direct) {
    const float length=std::sqrt(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
    if(!std::isfinite(length)) throw std::invalid_argument("Invalid spatial direction");
    direction=length>1.0e-6f ? direction*(1/length) : Vec3{0,0,-1};
    const std::array<float,12> next{{direction.x,direction.y,direction.z,direct.distanceAttenuation,
        direct.airAbsorption[0],direct.airAbsorption[1],direct.airAbsorption[2],direct.directivity,direct.occlusion,
        direct.transmission[0],direct.transmission[1],direct.transmission[2]}};
    for(unsigned i=3;i<next.size();++i) if(!std::isfinite(next[i]) || next[i]<0 || next[i]>1)
        throw std::invalid_argument("Direct-path coefficients must be finite and between 0 and 1");
    data_->direct=direct; // Simulation-owned copy for the direction/distance convenience overload.
    data_->version.fetch_add(1);
    for(unsigned i=0;i<next.size();++i) data_->parameters[i].store(next[i]);
    data_->flags.store(static_cast<int>(direct.flags));
    data_->transmissionType.store(static_cast<int>(direct.transmissionType));
    data_->version.fetch_add(1);
}
SpatialSound::~SpatialSound() { stop(); }
SoLoud::AudioSourceInstance* SpatialSound::createInstance() { return new Instance(data_); }
}
