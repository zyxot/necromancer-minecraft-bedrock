#pragma once
#include "client/feature/module/Module.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace SDK {
    class Player;
    class LocalPlayer;
    class Packet;
}

class SpearSwap : public Module {
public:
    SpearSwap();

    void onUpdate(Event& evG);
    void onSendPacket(Event& evG);
    void onPacketReceive(Event& evG);
    void onLeaveGame(Event& evG);
    void onDisable() override;

private:
    ValueType ignoreCooldown = BoolValue(false);
    ValueType delayBeforeSwitch = FloatValue(100.f);
    ValueType switchBackDelay = FloatValue(100.f);

    enum class Phase {
        Idle,
        PreSwitch,
        Swapping,
        Clicked,
    };

    struct CooldownEntry {
        std::string category;
        std::chrono::steady_clock::time_point endsAt {};
    };

    Phase phase = Phase::Idle;
    int savedSlot = -1;
    int pendingSpearSlot = -1;
    std::chrono::steady_clock::time_point switchedAt {};

    std::vector<CooldownEntry> cooldowns;
    std::chrono::steady_clock::time_point lastSwapAt {};

    bool isSpearWithLunge(SDK::ItemStack* stack);
    bool isOnCooldownNow();
    int findSpearSlot(SDK::Player* lp);
    void ingestCooldownPacket(SDK::Packet* packet);
    void finishPendingSwap(SDK::Player* lp);
    void clearPendingSwap();
};
