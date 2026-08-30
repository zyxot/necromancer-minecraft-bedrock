#pragma once
#include "client/misc/LatencySpoof.h"
#include <atomic>
#include <cstdint>

namespace RealPing {
    inline std::atomic<uint32_t> lastRaw { 0 };

    inline void recordRaw(uint32_t ping) {
        lastRaw.store(ping, std::memory_order_relaxed);
    }

    inline uint32_t get() {
        uint32_t raw = lastRaw.load(std::memory_order_relaxed);
        uint32_t spoof = LatencySpoof::getLatency();
        return raw > spoof ? raw - spoof : 0;
    }
}
