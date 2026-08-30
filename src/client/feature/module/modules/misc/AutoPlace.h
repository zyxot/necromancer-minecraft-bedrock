#pragma once
#include "client/feature/module/Module.h"
#include "client/misc/CombatTarget.h"
#include <chrono>

namespace SDK {
    class LocalPlayer;
    class BlockSource;
}

class AutoPlace : public Module {
public:
    AutoPlace();

    void onTick(Event& evG);
    void onEnable() override;
    void onDisable() override;

private:
    ValueType lava = BoolValue(true);
    ValueType cobweb = BoolValue(true);
    ValueType ignoreOnFire = BoolValue(true);
    ValueType retakeLava = BoolValue(true);
    ValueType retakeDelayMs = FloatValue(1500.f);
    EnumData rotMode;
    ValueType smoothSpeed = FloatValue(60.f);
    ValueType reach = FloatValue(5.f);
    ValueType delayMs = FloatValue(400.f);
    ValueType returnItem = BoolValue(true);
    ValueType skipIfTrapped = BoolValue(true);
    ValueType useMovementSim = BoolValue(true);
    ValueType maxSimMs = FloatValue(250.f);
    ValueType enforcedSimMs = FloatValue(100.f);

    ValueType players = BoolValue(true);
    ValueType mobs = BoolValue(false);
    EnumData targetMode;
    ValueType fov = FloatValue(90.f);

    enum class Phase {
        Idle,
        WaitForSwitch,
        Retake,
    };
    Phase phase = Phase::Idle;
    std::chrono::steady_clock::time_point switchTime {};
    std::chrono::steady_clock::time_point nextAction {};
    BlockPos retakeCell {};
    std::chrono::steady_clock::time_point retakeStartedAt {};
    std::chrono::steady_clock::time_point retakeNextTry {};
    int retakeAttempts = 0;
    Vec2 savedRot {};
    bool rotSpoofed = false;
    std::chrono::steady_clock::time_point rotSpoofedAt {};

    bool findSlot(SDK::LocalPlayer* lp, bool wantLava, int& outSlot) const;
    void finishAction(bool placed);
    void tickRetake(SDK::LocalPlayer* lp, SDK::BlockSource* region, std::chrono::steady_clock::time_point now);
    void retakeFinish(SDK::LocalPlayer* lp, bool scooped);
    void restoreSpoofedRot(SDK::LocalPlayer* lp, std::chrono::steady_clock::time_point now);
    bool resolveCell(SDK::BlockSource* region, CombatTargetResult const& target, float predictTicks, int& outX,
                     int& outY, int& outZ);
    static bool hasPlacementSupport(SDK::BlockSource* region, int x, int y, int z);
    static bool alreadyTrapped(SDK::BlockSource* region, AABB const& box);
    static Vec2 rotTowards(Vec3 const& from, Vec3 const& to);
    static float angleBetween(Vec2 const& a, Vec2 const& b);
};
