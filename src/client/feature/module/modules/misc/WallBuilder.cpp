#include "pch.h"
#include "WallBuilder.h"
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
#include <algorithm>
#include <cmath>

namespace {
    const std::string leaseOwner = "WallBuilder";
}

WallBuilder::WallBuilder()
    : Module("WallBuilder", LocalizeString::get("client.module.wallBuilder.name"),
             LocalizeString::get("client.module.wallBuilder.desc"), GAME, nokeybind) {
    addSetting("useAnyBlock", LocalizeString::get("client.module.wallBuilder.useAnyBlock.name"),
               LocalizeString::get("client.module.wallBuilder.useAnyBlock.desc"), useAnyBlock);
    addSetting("useMovementSim", LocalizeString::get("client.module.wallBuilder.useMovementSim.name"),
               LocalizeString::get("client.module.wallBuilder.useMovementSim.desc"), useMovementSim);
    addSliderSetting("maxSimMs", LocalizeString::get("client.module.wallBuilder.maxSimMs.name"),
                     LocalizeString::get("client.module.wallBuilder.maxSimMs.desc"), maxSimMs, FloatValue(25.f),
                     FloatValue(1000.f), FloatValue(25.f), "useMovementSim"_istrue);
    addSliderSetting("enforcedSimMs", LocalizeString::get("client.module.wallBuilder.enforcedSimMs.name"),
                     LocalizeString::get("client.module.wallBuilder.enforcedSimMs.desc"), enforcedSimMs,
                     FloatValue(0.f), FloatValue(500.f), FloatValue(25.f), "useMovementSim"_istrue);
    addSliderSetting("blocksPerTick", LocalizeString::get("client.module.wallBuilder.blocksPerTick.name"),
                     LocalizeString::get("client.module.wallBuilder.blocksPerTick.desc"), blocksPerTick,
                     FloatValue(1.f), FloatValue(8.f), FloatValue(1.f));
    addSetting("autoDisable", LocalizeString::get("client.module.wallBuilder.autoDisable.name"),
               LocalizeString::get("client.module.wallBuilder.autoDisable.desc"), autoDisable);

    addSetting("players", LocalizeString::get("client.module.wallBuilder.players.name"),
               LocalizeString::get("client.module.wallBuilder.players.desc"), players);
    addSetting("mobs", LocalizeString::get("client.module.wallBuilder.mobs.name"),
               LocalizeString::get("client.module.wallBuilder.mobs.desc"), mobs);

    targetMode.addEntry(EnumEntry(0, LocalizeString::get("client.module.wallBuilder.targetMode.distance.name")));
    targetMode.addEntry(EnumEntry(1, LocalizeString::get("client.module.wallBuilder.targetMode.crosshair.name")));
    targetMode.addEntry(EnumEntry(2, LocalizeString::get("client.module.wallBuilder.targetMode.lowestHealth.name")));
    addEnumSetting("targetMode", LocalizeString::get("client.module.wallBuilder.targetMode.name"),
                   LocalizeString::get("client.module.wallBuilder.targetMode.desc"), targetMode);
    addSliderSetting("enemyRange", LocalizeString::get("client.module.wallBuilder.enemyRange.name"),
                     LocalizeString::get("client.module.wallBuilder.enemyRange.desc"), enemyRange, FloatValue(2.f),
                     FloatValue(12.f), FloatValue(0.5f));

    Setting::Condition fovCond(std::vector<Setting::SingleCond> {
        { "targetMode", { 1 }, false },
    });
    addSliderSetting("fov", LocalizeString::get("client.module.wallBuilder.fov.name"),
                     LocalizeString::get("client.module.wallBuilder.fov.desc"), fov, FloatValue(10.f),
                     FloatValue(180.f), FloatValue(5.f), fovCond);

    listen<TickEvent>(static_cast<EventListenerFunc>(&WallBuilder::onTick));
}

void WallBuilder::onEnable() {
    idleTicks = 0;
}

void WallBuilder::onDisable() {
    SlotLease::release(leaseOwner, true);
    idleTicks = 0;
}

void WallBuilder::collectWallCells(SDK::LocalPlayer* lp, SDK::BlockSource* region, AABB const& enemyBox,
                                   Vec3 const& eye, std::vector<BoxBuilder::Cell>& out) {
    Vec3 enemyCenter = enemyBox.getCenter();
    Vec3 eyeToEnemy = enemyCenter - eye;
    eyeToEnemy.y = 0.f;
    if (eyeToEnemy.magnitude() < 0.5f) return;

    // getPos() is the eye position, not the feet; the wall's base layer has to
    // come from the hitbox or the whole wall floats a block and a half in the
    // air where nothing can support it.
    Vec3 selfFeet = lp->getPos();
    if (lp->aabbShape) selfFeet.y = lp->aabbShape->boundingBox.lower.y;

    Vec3 toEnemy = enemyCenter - selfFeet;
    toEnemy.y = 0.f;
    float selfDist = toEnemy.magnitude();
    if (selfDist < 0.5f) return;
    Vec3 selfDir = toEnemy * (1.f / selfDist);

    float wallDist = selfDist * 0.5f;
    if (wallDist > selfDist - 1.f) wallDist = selfDist - 1.f;
    if (wallDist < 1.f) wallDist = 1.f;

    Vec3 wallCenter = selfFeet + selfDir * wallDist;
    float feetY = std::floor(wallCenter.y + 0.001f);

    Vec3 side { -selfDir.z, 0.f, selfDir.x };

    int cx = static_cast<int>(std::floor(wallCenter.x));
    int cz = static_cast<int>(std::floor(wallCenter.z));
    int ax = static_cast<int>(std::floor(wallCenter.x + side.x + 0.5f));
    int az = static_cast<int>(std::floor(wallCenter.z + side.z + 0.5f));
    int bx = static_cast<int>(std::floor(wallCenter.x - side.x + 0.5f));
    int bz = static_cast<int>(std::floor(wallCenter.z - side.z + 0.5f));
    if ((ax == cx && az == cz) || (bx == cx && bz == cz)) {
        ax = cx + (side.x >= 0.f ? 1 : -1);
        az = cz;
        bx = cx - (side.x >= 0.f ? 1 : -1);
        bz = cz;
    }

    // Cells overlapping a body get rejected by the server anyway; skipping them
    // up front stops those dead placements eating the per-tick budget.
    auto cellIntersects = [](BoxBuilder::Cell const& c, AABB const& b) {
        return static_cast<float>(c.x) + 1.f > b.lower.x && static_cast<float>(c.x) < b.higher.x &&
               static_cast<float>(c.y) + 1.f > b.lower.y && static_cast<float>(c.y) < b.higher.y &&
               static_cast<float>(c.z) + 1.f > b.lower.z && static_cast<float>(c.z) < b.higher.z;
    };
    AABB selfBox = lp->aabbShape ? lp->aabbShape->boundingBox : enemyBox;

    out.reserve(out.size() + wallHeight * 3);
    for (int layer = 0; layer < wallHeight; layer++) {
        int y = static_cast<int>(feetY) + layer;
        BoxBuilder::Cell cells[3] = { { cx, y, cz }, { ax, y, az }, { bx, y, bz } };
        for (auto const& cell : cells) {
            if (cellIntersects(cell, enemyBox)) continue;
            if (cellIntersects(cell, selfBox)) continue;
            if (BlockSolid::isCollidable(region, cell.x, cell.y, cell.z)) continue;
            out.push_back(cell);
        }
    }

    Vec3 wallMid { static_cast<float>(cx) + 0.5f, feetY, static_cast<float>(cz) + 0.5f };
    std::stable_sort(out.begin(), out.end(), [&](BoxBuilder::Cell const& a, BoxBuilder::Cell const& b) {
        return wallMid.distance(Vec3 { static_cast<float>(a.x) + 0.5f, static_cast<float>(a.y),
                                       static_cast<float>(a.z) + 0.5f }) <
               wallMid.distance(Vec3 { static_cast<float>(b.x) + 0.5f, static_cast<float>(b.y),
                                       static_cast<float>(b.z) + 0.5f });
    });
}

void WallBuilder::onTick(Event&) {
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
    if (std::get<BoolValue>(useMovementSim).value && target->actor) {
        int cellEstimate = wallHeight * 3;
        int perTick = std::max(1, static_cast<int>(std::get<FloatValue>(blocksPerTick).value));
        int buildTicksMs = ((cellEstimate + perTick - 1) / perTick) * 50;

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

    std::vector<BoxBuilder::Cell> queue;
    collectWallCells(lp, region, box, lp->getPos(), queue);

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
