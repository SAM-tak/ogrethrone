#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>

namespace throne {
/// Hands out Snowflake IDs (https://en.wikipedia.org/wiki/Snowflake_ID): positive
/// 64-bit integers holding, from the top, 41 bits of milliseconds since the
/// generator's epoch, 10 bits of worker number and 12 bits counting within a
/// millisecond. They rise in the order they are made, and the time makes them
/// large numbers, plainly IDs. Each kind of ID keeps its own generator (its own
/// epoch and sequence); the worker number is one per process, picked at random,
/// so editors running at once do not meet.
class Ids {
public:
    /// @param epoch Milliseconds since the Unix epoch that the IDs count from.
    explicit Ids(std::int64_t epoch) : epoch_(epoch) {}

    /// Returns a new ID, larger than any this generator returned before, even if
    /// the clock goes back; past 4096 in one millisecond it borrows the next one.
    /// Not thread safe.
    std::uint64_t next() {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count() - epoch_;
        auto stamp = std::max(static_cast<std::uint64_t>(std::max<std::int64_t>(now, 0)), last_);
        if (stamp != last_) sequence_ = 0;
        else if (++sequence_ > 0xFFFu) sequence_ = 0, ++stamp;
        last_ = stamp;
        return stamp << 22 | worker() << 12 | sequence_;
    }

    /// This process's worker number: 10 bits picked at random once.
    static std::uint64_t worker() {
        static const std::uint64_t value = std::random_device{}() & 0x3FFu;
        return value;
    }

private:
    std::int64_t epoch_;
    std::uint64_t last_ = 0, sequence_ = 0;
};
}
