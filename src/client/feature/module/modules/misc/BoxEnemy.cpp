#include "pch.h"
#include "BoxEnemy.h"
#include "client/event/events/TickEvent.h"
#include "client/misc/BoxBuilder.h"
#include "client/misc/CombatTarget.h"
#include "client/misc/MovementSim.h"
#include "client/misc/RealPing.h"
#include "client/misc/SlotLease.h"
#include "client/Necromancer.h"
#include "client/screen/ScreenManager.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/level/BlockSource.h"
#include <cmath>

namespace {
    const std::string leaseOwner = "BoxEnemy";
}

BoxEnemy::BoxEnemy()
    : Module("BoxEnemy", LocalizeString::get("client.module.boxEnemy.name"),
             LocalizeString::get("client.module.boxEnemy.desc"), GAME, nokeybind) {
    addSetting("useAnyBlock", LocalizeString::get("client.module.boxEnemy.useAnyBlock.name"),
               LocalizeString::get("client.module.boxEnemy.useAnyBlock.desc"), useAnyBlock);
    addSetting("corners", LocalizeString::get("client.module.boxEnemy.corners.name"),
               LocalizeString::get("client.module.boxEnemy.corners.desc"), corners);
    addSetting("topBlock", LocalizeString::get("client.module.boxEnemy.topBlock.name"),
               LocalizeString::get("client.module.boxEnemy.topBlock.desc"), topBlock);
    addSetting("stepBlock", LocalizeString::get("client.module.boxEnemy.stepBlock.name"),
               LocalizeString::get("client.module.boxEnemy.stepBlock.desc"), stepBlock);
    addSetting("useMovementSim", LocalizeString::get("client.module.boxEnemy.useMovementSim.name"),
               LocalizeString::get("client.module.boxEnemy.useMovementSim.desc"), useMovementSim);
    addSliderSetting("maxSimMs", LocalizeString::get("client.module.boxEnemy.maxSimMs.name"),
                     LocalizeString::get("client.module.boxEnemy.maxSimMs.desc"), maxSimMs, FloatValue(25.f),
                     FloatValue(1000.f), FloatValue(25.f), "useMovementSim"_istrue);
    addSliderSetting("enforcedSimMs", LocalizeString::get("client.module.boxEnemy.enforcedSimMs.name"),
                     LocalizeString::get("client.module.boxEnemy.enforcedSimMs.desc"), enforcedSimMs,
                     FloatValue(0.f), FloatValue(500.f), FloatValue(25.f), "useMovementSim"_istrue);
    addSliderSetting("blocksPerTick", LocalizeString::get("client.module.boxEnemy.blocksPerTick.name"),
                     LocalizeString::get("client.module.boxEnemy.blocksPerTick.desc"), blocksPerTick, FloatValue(1.f),
                     FloatValue(8.f), FloatValue(1.f));
    addSetting("autoDisable", LocalizeString::get("client.module.boxEnemy.autoDisable.name"),
               LocalizeString::get("client.module.boxEnemy.autoDisable.desc"), autoDisable);

    addSetting("players", LocalizeString::get("client.module.boxEnemy.players.name"),
               LocalizeString::get("client.module.boxEnemy.players.desc"), players);
    addSetting("mobs", LocalizeString::get("client.module.boxEnemy.mobs.name"),
               LocalizeString::get("client.module.boxEnemy.mobs.desc"), mobs);

    targetMode.addEntry(EnumEntry(0, LocalizeString::get("client.module.boxEnemy.targetMode.distance.name")));
    targetMode.addEntry(EnumEntry(1, LocalizeString::get("client.module.boxEnemy.targetMode.crosshair.name")));
    targetMode.addEntry(EnumEntry(2, LocalizeString::get("client.module.boxEnemy.targetMode.lowestHealth.name")));
    addEnumSetting("targetMode", LocalizeString::get("client.module.boxEnemy.targetMode.name"),
                   LocalizeString::get("client.module.boxEnemy.targetMode.desc"), targetMode);
    addSliderSetting("enemyRange", LocalizeString::get("client.module.boxEnemy.enemyRange.name"),
                     LocalizeString::get("client.module.boxEnemy.enemyRange.desc"), enemyRange, FloatValue(2.f),
                     FloatValue(8.f), FloatValue(0.5f));

    Setting::Condition fovCond(std::vector<Setting::SingleCond> {
        { "targetMode", { 1 }, false },
    });
    addSliderSetting("fov", LocalizeString::get("client.module.boxEnemy.fov.name"),
                     LocalizeString::get("client.module.boxEnemy.fov.desc"), fov, FloatValue(10.f), FloatValue(180.f),
                     FloatValue(5.f), fovCond);

    listen<TickEvent>(static_cast<EventListenerFunc>(&BoxEnemy::onTick));
}

void BoxEnemy::onEnable() {
    idleTicks = 0;
}

void BoxEnemy::onDisable() {
    SlotLease::release(leaseOwner, true);
    idleTicks = 0;
}

void BoxEnemy::onTick(Event&) {
    SlotLease::pump();

    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->gameMode) return;
    if (Necromancer::get().getScreenManager().getActiveScreen().has_value()) return;
    auto region = ci->getRegion();
    if (!region) return;

    CombatTargetQuery q;
    q.players = std::get<BoolValue>(players).value;
    q.mobs = std::get<BoolValue>(mobs).value;
    q.mode = targetMode.getSelectedKey();
    q.range = std::get<FloatValue>(enemyRange).value;
    q.fov = std::get<FloatValue>(fov).value;

    auto target = CombatTarget::find(lp, q);
    if (!target) {
        SlotLease::release(leaseOwner, true);
        if (std::get<BoolValue>(autoDisable).value && ++idleTicks >= 40) setEnabled(false);
        return;
    }

    AABB box = target->box;
    Vec3 moveDir { 0.f, 0.f, 0.f };
    if (target->actor) {
        moveDir = target->actor->getVelocity();
        if (auto seed = MovementSim::simSeed(target->runtimeId)) moveDir = seed->vel;
    }

    if (std::get<BoolValue>(useMovementSim).value && target->actor) {
        auto curFp = BoxBuilder::footprintOf(box);
        int spanX = curFp.x1 - curFp.x0 + 3;
        int spanZ = curFp.z1 - curFp.z0 + 3;
        int inner = (curFp.x1 - curFp.x0 + 1) * (curFp.z1 - curFp.z0 + 1);
        int ringEstimate = (spanX * spanZ - inner) * wallHeight;
        if (!std::get<BoolValue>(corners).value) ringEstimate = ringEstimate * 3 / 4;
        if (std::get<BoolValue>(stepBlock).value) ringEstimate += 1;
        if (std::get<BoolValue>(topBlock).value) ringEstimate += inner;
        int perTick = std::max(1, static_cast<int>(std::get<FloatValue>(blocksPerTick).value));
        int buildTicksMs = ((ringEstimate + perTick - 1) / perTick) * 50;

        int neededMs = buildTicksMs + 100 + static_cast<int>(RealPing::get());
        int maxMs = std::max(25, static_cast<int>(std::get<FloatValue>(maxSimMs).value));
        int enforcedMs = std::max(0, static_cast<int>(std::get<FloatValue>(enforcedSimMs).value));
        int totalMs = std::min(neededMs, maxMs) + enforcedMs;
        int simTicksTotal = std::max(1, (totalMs + 25) / 50);
        Vec3 seedVel = target->actor->getVelocity();
        float turnPerTick = 0.f;
        if (auto seed = MovementSim::simSeed(target->runtimeId)) {
            seedVel = seed->vel;
            turnPerTick = seed->turnPerTick;
        }
        auto sim = MovementSim::predictForward(target->actor, seedVel, simTicksTotal, true, true, turnPerTick);
        if (sim.valid && !sim.hitFloor) {
            for (int extra = 10; extra <= 30 && !sim.hitFloor; extra += 10) {
                auto longer =
                    MovementSim::predictForward(target->actor, seedVel, simTicksTotal + extra, true, true, turnPerTick);
                if (!longer.valid) break;
                sim = std::move(longer);
            }
        }
        if (sim.valid) MovementSim::publishPrediction(target->runtimeId, sim);
        if (sim.valid) box = sim.finalBox;
    }

    auto fp = BoxBuilder::footprintOf(box);
    int x0 = fp.x0;
    int x1 = fp.x1;
    int z0 = fp.z0;
    int z1 = fp.z1;
    int feetY = fp.feetY;

    std::vector<BoxBuilder::Cell> queue;
    BoxBuilder::collectRing(region, x0, x1, z0, z1, feetY, wallHeight, std::get<BoolValue>(corners).value, queue);

    float moveMag = std::hypot(moveDir.x, moveDir.z);
    if (moveMag > 0.01f && queue.size() > 1) {
        float nx = moveDir.x / moveMag;
        float nz = moveDir.z / moveMag;
        float cx = (static_cast<float>(x0) + static_cast<float>(x1) + 1.f) * 0.5f;
        float cz = (static_cast<float>(z0) + static_cast<float>(z1) + 1.f) * 0.5f;
        std::stable_sort(queue.begin(), queue.end(), [&](BoxBuilder::Cell const& a, BoxBuilder::Cell const& b) {
            float da = (static_cast<float>(a.x) + 0.5f - cx) * nx + (static_cast<float>(a.z) + 0.5f - cz) * nz;
            float db = (static_cast<float>(b.x) + 0.5f - cx) * nx + (static_cast<float>(b.z) + 0.5f - cz) * nz;
            return da > db;
        });
    }

    bool wallsPending = !queue.empty();
    if (std::get<BoolValue>(stepBlock).value && !wallsPending) {
        // One extra block on top of the wall, on the side closest to you — the step
        // you jump on to finish the cage from above.
        Vec3 eye = lp->getPos();
        int bestX = 0;
        int bestZ = 0;
        float bestDist = 1e9f;
        bool found = false;
        for (int x = x0 - 1; x <= x1 + 1; x++) {
            for (int z = z0 - 1; z <= z1 + 1; z++) {
                bool onEdge = (x == x0 - 1 || x == x1 + 1 || z == z0 - 1 || z == z1 + 1);
                if (!onEdge) continue;
                bool isCorner = (x == x0 - 1 || x == x1 + 1) && (z == z0 - 1 || z == z1 + 1);
                if (isCorner && !std::get<BoolValue>(corners).value) continue;
                float d = std::hypot(static_cast<float>(x) + 0.5f - eye.x, static_cast<float>(z) + 0.5f - eye.z);
                if (d < bestDist) {
                    bestDist = d;
                    bestX = x;
                    bestZ = z;
                    found = true;
                }
            }
        }
        if (found && !BlockSolid::isCollidable(region, bestX, feetY + wallHeight, bestZ)) {
            queue.push_back({ bestX, feetY + wallHeight, bestZ });
        }
        wallsPending = !queue.empty();
    }

    if (std::get<BoolValue>(topBlock).value && !wallsPending) {
        BoxBuilder::collectRoof(region, x0, x1, z0, z1, feetY, wallHeight, queue);
    }

    if (queue.empty()) {
        SlotLease::release(leaseOwner, true);
        if (std::get<BoolValue>(autoDisable).value && ++idleTicks >= 10) setEnabled(false);
        return;
    }

    int probeSlot = -1;
    if (!BoxBuilder::findBlockSlot(lp, std::get<BoolValue>(useAnyBlock).value, probeSlot)) {
        SlotLease::release(leaseOwner, true);
        if (std::get<BoolValue>(autoDisable).value && ++idleTicks >= 40) setEnabled(false);
        return;
    }

    if (!SlotLease::acquire(leaseOwner, SlotLease::Priority::Building, 250)) return;

    idleTicks = 0;
    int placed = BoxBuilder::build(lp, region, queue, std::get<BoolValue>(useAnyBlock).value,
                                   std::max(1, static_cast<int>(std::get<FloatValue>(blocksPerTick).value)));
    if (placed == 0) {
        SlotLease::release(leaseOwner, true);
        if (std::get<BoolValue>(autoDisable).value && ++idleTicks >= 40) setEnabled(false);
    }
}
