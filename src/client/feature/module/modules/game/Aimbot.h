#pragma once
#include "client/feature/module/Module.h"
#include "util/LMath.h"
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

namespace SDK {
    class Actor;
    class Packet;
}

class Aimbot : public Module {
public:
    Aimbot();

    void onUpdate(Event& evG);
    void onCameraUpdate(Event& evG);
    void onTurnDelta(Event& evG);
    void onCinematicCamera(Event& evG);
    void onClick(Event& evG);
    void onAfterMove(Event& evG);
    void onBeforeMove(Event& evG);
    void onSendPacket(Event& evG);
    void onAfterMovePartBurst(Event& evG);
    void onRenderOverlay(RenderOverlayEvent& ev);
    void onRendererCleanup(Event& evG);
    void onDisable() override;
    void afterLoadConfig() override;

    [[nodiscard]] bool isPSilent();
    bool getPSilentLock(uint64_t& outRuntimeID, AABB& outBox, Vec3& outHitPoint);

private:
    EnumData aimMode;
    static constexpr int aim_normal = 0;
    static constexpr int aim_psilent = 1;
    static constexpr int aim_silent = 2;

    ValueType players = BoolValue(true);
    ValueType mobs = BoolValue(false);
    ValueType prioritizeTags = BoolValue(true);
    ValueType ignoreFriends = BoolValue(true);
    ValueType wallCheck = BoolValue(true);
    ValueType hitBehindWall = BoolValue(false);
    ValueType backtrackTarget = BoolValue(false);
    ValueType lagRecordsOnly = BoolValue(false);
    ValueType ignoreInvulnerable = BoolValue(false);
    ValueType psilentSprintHits = BoolValue(true);
    ValueType silentRotSmooth = BoolValue(true);
    ValueType allBodyParts = BoolValue(false);

    ValueType smoothSpeed = FloatValue(8.f);
    ValueType lockOn = BoolValue(false);

    ValueType range = FloatValue(5.f);
    EnumData targetMode;
    EnumData hitbox;
    EnumData backtrackHitbox;
    ValueType backtrackHitboxSame = BoolValue(true);
    ValueType fov = FloatValue(90.f);
    ValueType fovColor = ColorValue(1.f, 1.f, 1.f, 0.75f);
    ValueType fovWidth = FloatValue(1.5f);

    struct TargetCandidate {
        SDK::Actor* actor;
        float score;
        int priority;
        bool retained;
        bool isGhost;
        AABB bounds;
        Vec3 frac;
        float recordAgeMs;
    };

    std::vector<TargetCandidate> candidates;
    class Freelook* freelookModule = nullptr;
    bool freelookResolved = false;
    uint64_t currentTargetId = 0;
    bool currentTargetGhost = false;
    bool currentTargetObstructed = false;
    AABB currentTargetBox {};
    float currentTargetRecordAgeMs = -1.f;
    Vec3 currentAimFrac { 0.5f, 0.55f, 0.5f };
    bool haveCurrentAimFrac = false;
    uint64_t desiredTargetId = 0;
    Vec3 desiredAimFrac { 0.5f, 0.55f, 0.5f };
    bool desiredIsGhost = false;
    AABB desiredGhostBox {};
    Vec2 desiredSilentRot {};
    bool silentRotActive = false;
    bool commandActive = false;
    Vec2 commandedRot {};
    Vec2 pendingTurnDelta {};
    std::atomic<float> userInput = 0.f;
    std::chrono::steady_clock::time_point lastFrame {};
    uint64_t lastCorrectionFrame = UINT64_MAX;
    std::atomic_bool injectingTurn = false;
    std::recursive_mutex controllerMutex;
    std::mutex aimMutex;
    class Backtrack* backtrackModule = nullptr;
    bool backtrackResolved = false;
    ComPtr<ID2D1SolidColorBrush> ringBrush;

    struct PendingAttack {
        uint64_t runtimeID = 0;
        AABB box {};
        Vec3 hitPoint {};
        float recordAgeMs = -1.f;
        bool ghost = false;
        bool active = false;
    };

    PendingAttack pendingAttack {};
    bool psilentActive = false;
    std::atomic_bool psilentTouchArmed = false;
    Vec2 psilentTouchDir {};

    // Published by the render thread for the game thread to read without taking
    // controllerMutex. Taking that lock from BeforeMove/SendPacket stalls the game
    // thread behind the render thread's whole target scan, which shows up as
    // seconds-long stutter even with nobody in range.
    struct PSilentLock {
        uint64_t runtimeID = 0;
        Vec3 hitPoint {};
        AABB box {};
        bool valid = false;
    };

    std::atomic<uint32_t> psilentSeq { 0 };
    PSilentLock psilentShared {};

    void publishPSilentLock(uint64_t runtimeID, AABB const& box, Vec3 const& hitPoint, bool valid);
    bool loadPSilentLock(PSilentLock& out) const;

    Backtrack* resolveBacktrack();
    int selectedHitboxes(bool ghost, int* outSlots, int maxSlots);
    Vec3 bestFracForSelection(AABB const& bounds, Vec3 const& eye, SDK::BlockSource* region, bool ghost);

    struct PartBurst {
        uint64_t runtimeID = 0;
        AABB box {};
        Vec3 hitPoint {};
        float recordAgeMs = -1.f;
        bool ghost = false;
        int fireAt = 0;
        bool live = false;
    };

    std::deque<PartBurst> partBurst;
    int burstClock = 0;
    int nextBurstSlot = 0;

    void queuePartBurst(uint64_t runtimeID, AABB const& box, Vec3 const& aimPoint, float recordAgeMs, bool ghost);
    void processPartBurst();
    bool observeAttack(SDK::Packet* packet, uint64_t& outTarget);
};
