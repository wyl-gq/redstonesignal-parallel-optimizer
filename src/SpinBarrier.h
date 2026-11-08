#pragma once

#include <atomic>
#include <intrin.h>

class SpinBarrier {
    alignas(64) std::atomic<int> arrived{0};
    alignas(64) std::atomic<int> generation{0};
    int count;

public:
    explicit SpinBarrier(int count) : count(count) {}

    void arrive_and_wait() {
        const int gen = generation.load(std::memory_order_acquire);
        if (arrived.fetch_add(1, std::memory_order_acq_rel) == count - 1) {
            arrived.store(0, std::memory_order_relaxed);
            generation.fetch_add(1, std::memory_order_release);
            return;
        }
        while (generation.load(std::memory_order_acquire) == gen) _mm_pause();
    }
};
