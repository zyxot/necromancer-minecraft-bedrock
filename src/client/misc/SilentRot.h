#pragma once
#include "util/LMath.h"
#include <chrono>
#include <mutex>

namespace SilentRot {
    struct Request {
        Vec2 rot {};
        bool active = false;
        bool interactOnly = false;
        std::chrono::steady_clock::time_point expiresAt {};
    };

    inline std::mutex& lock() {
        static std::mutex m;
        return m;
    }

    inline Request& request() {
        static Request r {};
        return r;
    }

    inline void set(Vec2 const& rot, bool interactOnly, int holdMs) {
        std::lock_guard lk(lock());
        auto& r = request();
        r.rot = rot;
        r.active = true;
        r.interactOnly = interactOnly;
        r.expiresAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
    }

    inline void clear() {
        std::lock_guard lk(lock());
        request() = {};
    }

    inline bool get(Vec2& outRot, bool& outInteractOnly) {
        std::lock_guard lk(lock());
        auto& r = request();
        if (!r.active) return false;
        if (std::chrono::steady_clock::now() >= r.expiresAt) {
            r = {};
            return false;
        }
        outRot = r.rot;
        outInteractOnly = r.interactOnly;
        return true;
    }

    inline bool peek(Vec2& outRot) {
        bool interactOnly = false;
        return get(outRot, interactOnly);
    }
}
