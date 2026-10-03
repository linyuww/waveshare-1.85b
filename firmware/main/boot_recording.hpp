#pragma once
#include <atomic>
#include <cstdint>

// UI publishes releases independently of the command queue. The worker owns active_.
class BootRecording {
public:
    uint32_t next() { if (++sequence_ == 0) ++sequence_; return sequence_; }
    void finish(uint32_t id) { if (id) finished_.store(id); }
    bool claim(uint32_t id) {
        if (static_cast<int32_t>(id - finished_.load()) <= 0) return false;
        active_ = id;
        return true;
    }
    void cancel() { active_ = 0; }
    void restore(uint32_t id) { active_ = id; }
    bool takeFinish() {
        if (!active_ || static_cast<int32_t>(finished_.load() - active_) < 0) return false;
        active_ = 0;
        return true;
    }
private:
    uint32_t sequence_ = 0;
    uint32_t active_ = 0;
    std::atomic<uint32_t> finished_{0};
};
