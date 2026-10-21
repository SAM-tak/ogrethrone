#include "audio/BufferedStream.h"
#include <soloud_wavstream.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace throne {
struct BufferedStream::State {
    static constexpr unsigned capacity=65536, block=1024;
    SoLoud::WavStream source;
    std::vector<float> pcm;
    std::vector<unsigned char> boundary;
    std::atomic<std::uint64_t> read{0},write{0};
    std::atomic<bool> stopping{false}, failed{false};
    std::thread producer;
    std::mutex mutex;
    std::condition_variable ready;
    bool primed=false;
    std::exception_ptr failure;
    explicit State(const std::string& filename) {
        if(source.load(filename.c_str())!=SoLoud::SO_NO_ERROR || !source.mSampleCount || source.mChannels<1 || source.mChannels>2)
            throw std::runtime_error("Cannot stream mono/stereo audio: "+filename);
        pcm.resize(capacity*source.mChannels); boundary.resize(capacity);
    }
    ~State() { stop(); }
    void stop() { stopping.store(true); if(producer.joinable()) producer.join(); }
    void prime() {
        stop(); stopping.store(false); failed.store(false); read.store(0); write.store(0); primed=false; failure=nullptr;
        producer=std::thread([this] {
            try {
                std::unique_ptr<SoLoud::AudioSourceInstance> decoder(source.createInstance());
                decoder->init(source,0);
                std::vector<float> data(block*source.mChannels);
                while(!stopping.load()) {
                    const auto w=write.load(std::memory_order_relaxed),r=read.load(std::memory_order_acquire);
                    if(w-r>capacity-block) {
                        { std::lock_guard<std::mutex> lock(mutex); primed=true; }
                        ready.notify_one();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
                    }
                    const unsigned count=decoder->getAudio(data.data(),block,block);
                    if(!count) throw std::runtime_error("Streaming decoder made no progress");
                    const bool end=decoder->hasEnded();
                    for(unsigned i=0;i<count;++i) {
                        const auto at=static_cast<unsigned>((w+i)%capacity);
                        for(unsigned c=0;c<source.mChannels;++c) pcm[c*capacity+at]=data[c*block+i];
                        boundary[at]=(end && i+1==count)?1:0;
                    }
                    write.store(w+count,std::memory_order_release);
                    if(end && decoder->rewind()!=SoLoud::SO_NO_ERROR) throw std::runtime_error("Streaming rewind failed");
                }
            } catch(...) {
                { std::lock_guard<std::mutex> lock(mutex); failure=std::current_exception(); primed=true; }
                failed.store(true);
                ready.notify_one();
            }
        });
        std::unique_lock<std::mutex> lock(mutex); ready.wait(lock,[this]{return primed;});
        if(failure) std::rethrow_exception(failure);
    }
};
class BufferedStream::Instance : public SoLoud::AudioSourceInstance {
public:
    explicit Instance(State& state):state_(state) {}
    unsigned getAudio(float* output,unsigned count,unsigned stride) override {
        const auto r=state_.read.load(std::memory_order_relaxed),w=state_.write.load(std::memory_order_acquire);
        const auto available=static_cast<unsigned>((std::min)(std::uint64_t(count),w-r));
        unsigned written=0;
        for(;written<available && !ended_;++written) {
            const auto at=static_cast<unsigned>((r+written)%State::capacity);
            for(unsigned c=0;c<state_.source.mChannels;++c) output[c*stride+written]=state_.pcm[c*State::capacity+at];
            if(state_.boundary[at] && !(mFlags&LOOPING)) ended_=true;
        }
        state_.read.store(r+written,std::memory_order_release);
        if(!ended_) {
            // An underrun inserts silence without blocking the audio device or consuming PCM.
            for(unsigned c=0;c<state_.source.mChannels;++c) std::fill(output+c*stride+written,output+c*stride+count,0.0f);
            return count;
        }
        return written;
    }
    bool hasEnded() override { return ended_; }
private:
    State& state_;
    bool ended_=false;
};
BufferedStream::BufferedStream(const std::string& filename):state_(std::make_unique<State>(filename)) {
    mChannels=state_->source.mChannels; mBaseSamplerate=state_->source.mBaseSamplerate;
    state_->prime();
}
BufferedStream::~BufferedStream() { stop(); }
bool BufferedStream::failed() const { return state_->failed.load(); }
void BufferedStream::restart() { state_->prime(); }
SoLoud::AudioSourceInstance* BufferedStream::createInstance() { return new Instance(*state_); }
}
