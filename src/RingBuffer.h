#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

template <size_t CapacityPow2>
class StereoRing {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "capacity must be power of two");
public:
    struct Frame { float l{}, r{}; };

    bool push(float l, float r) {
        auto w = write_.load(std::memory_order_relaxed);
        auto next = w + 1;
        auto rd = read_.load(std::memory_order_acquire);
        if (next - rd > CapacityPow2) return false;
        data_[w & (CapacityPow2-1)] = {l,r};
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Frame& out) {
        auto rd = read_.load(std::memory_order_relaxed);
        auto w = write_.load(std::memory_order_acquire);
        if (rd == w) return false;
        out = data_[rd & (CapacityPow2-1)];
        read_.store(rd+1, std::memory_order_release);
        return true;
    }

    size_t available() const {
        auto w = write_.load(std::memory_order_acquire);
        auto r = read_.load(std::memory_order_acquire);
        return static_cast<size_t>(w-r);
    }

    void discard(size_t frames) {
        auto rd = read_.load(std::memory_order_relaxed);
        auto w = write_.load(std::memory_order_acquire);
        auto n = static_cast<uint64_t>(frames);
        if (n > w-rd) n = w-rd;
        read_.store(rd+n, std::memory_order_release);
    }

    void clear() {
        auto w = write_.load(std::memory_order_acquire);
        read_.store(w, std::memory_order_release);
    }

private:
    std::unique_ptr<Frame[]> data_{std::make_unique<Frame[]>(CapacityPow2)};
    std::atomic<uint64_t> write_{0};
    std::atomic<uint64_t> read_{0};
};
