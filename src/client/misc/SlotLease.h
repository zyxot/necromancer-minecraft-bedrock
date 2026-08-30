#pragma once
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/actor/player/Inventory.h"
#include <chrono>
#include <cstdint>
#include <string>

namespace SlotLease {
    enum class Priority : int {
        Building = 10,
        Placing = 20,
        Throwing = 30,
    };

    struct State {
        std::string owner;
        int priority = 0;
        int restoreSlot = -1;
        bool held = false;
        std::chrono::steady_clock::time_point expiresAt {};
    };

    inline State& state() {
        static State s {};
        return s;
    }

    inline void release(std::string const& owner, bool restore) {
        auto& s = state();
        if (!s.held || s.owner != owner) return;

        if (restore && s.restoreSlot >= 0 && s.restoreSlot < 9) {
            auto ci = SDK::ClientInstance::get();
            auto lp = ci ? ci->getLocalPlayer() : nullptr;
            if (lp && lp->supplies) lp->supplies->selectedSlot = s.restoreSlot;
        }
        s = {};
    }

    inline bool expired(std::chrono::steady_clock::time_point now) {
        auto& s = state();
        return s.held && now >= s.expiresAt;
    }

    inline void pump() {
        auto& s = state();
        if (!s.held) return;
        if (std::chrono::steady_clock::now() < s.expiresAt) return;
        release(s.owner, true);
    }

    inline bool acquire(std::string const& owner, Priority priority, int holdMs) {
        auto& s = state();
        auto now = std::chrono::steady_clock::now();
        if (s.held && now >= s.expiresAt) release(s.owner, true);

        int want = static_cast<int>(priority);
        if (s.held && s.owner != owner) {
            if (want <= s.priority) return false;
            release(s.owner, false);
        }

        auto ci = SDK::ClientInstance::get();
        auto lp = ci ? ci->getLocalPlayer() : nullptr;
        if (!lp || !lp->supplies) return false;

        if (!s.held) {
            s.owner = owner;
            s.priority = want;
            s.restoreSlot = lp->supplies->selectedSlot;
            s.held = true;
        }
        s.priority = want;
        s.expiresAt = now + std::chrono::milliseconds(holdMs);
        return true;
    }

    inline bool ownedBy(std::string const& owner) {
        auto& s = state();
        return s.held && s.owner == owner;
    }

    inline bool busyForOthers(std::string const& owner) {
        auto& s = state();
        if (!s.held || s.owner == owner) return false;
        return std::chrono::steady_clock::now() < s.expiresAt;
    }

    inline int restoreSlotFor(std::string const& owner) {
        auto& s = state();
        return (s.held && s.owner == owner) ? s.restoreSlot : -1;
    }
}
