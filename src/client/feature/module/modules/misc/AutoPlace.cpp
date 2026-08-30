#include "pch.h"
#include "AutoPlace.h"
#include "client/event/events/TickEvent.h"
#include "client/misc/BlockSolid.h"
#include "client/misc/BoxBuilder.h"
#include "client/misc/CombatTarget.h"
#include "client/misc/MovementSim.h"
#include "client/misc/RealPing.h"
#include "client/misc/SlotLease.h"
#include "client/Necromancer.h"
#include "client/screen/ScreenManager.h"
#include "mc/Addresses.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/game/MinecraftGame.h"
#include "mc/common/client/game/MouseDevice.h"
#include "mc/common/client/game/MouseAction.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/actor/player/Inventory.h"
#include "mc/common/world/actor/player/GameMode.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"
#include "mc/common/world/ItemStack.h"
#include <cmath>

namespace {
    constexpr int switchWaitMs = 50;
    constexpr int actorFlagOnFire = 1;
    constexpr int maxRetakeAttempts = 4;
    const std::string leaseOwner = "AutoPlace";

    int floori(float v) {
        return static_cast<int>(std::floor(v));
    }

    void pushAction(int button, bool down) {
        auto mouse = SDK::MouseDevice::get();
        if (!mouse) return;

        SDK::MouseAction action {};
        action.x = mouse->x;
        action.y = mouse->y;
        action.dx = 0;
        action.dy = 0;
        action.action = static_cast<int8_t>(button);
        action.data = down ? int8_t { 1 } : int8_t { 0 };
        action.pointerId = 0;
        action.forceMotionlessPointer = false;
        mouse->inputs.push_back(action);
    }

    float wrapAngle(float angle) {
        while (angle > 180.f) angle -= 360.f;
        while (angle < -180.f) angle += 360.f;
        return angle;
    }

    bool isTrapBlock(SDK::BlockSource* region, int x, int y, int z, bool& outLava) {
        if (!region) return false;
        auto* block = region->getBlock(BlockPos { x, y, z });
        if (!block || !block->legacyBlock) return false;
        std::string id = block->legacyBlock->name.getString();
        if (id == "lava" || id == "flowing_lava") {
            outLava = true;
            return true;
        }
        if (id == "web" || id == "cobweb") {
            outLava = false;
            return true;
        }
        return false;
    }
}

AutoPlace::AutoPlace()
    : Module("AutoPlace", LocalizeString::get("client.module.autoPlace.name"),
             LocalizeString::get("client.module.autoPlace.desc"), GAME, nokeybind) {
    addSetting("lava", LocalizeString::get("client.module.autoPlace.lava.name"),
               LocalizeString::get("client.module.autoPlace.lava.desc"), lava);
    addSetting("cobweb", LocalizeString::get("client.module.autoPlace.cobweb.name"),
               LocalizeString::get("client.module.autoPlace.cobweb.desc"), cobweb);
    addSetting("ignoreOnFire", LocalizeString::get("client.module.autoPlace.ignoreOnFire.name"),
               LocalizeString::get("client.module.autoPlace.ignoreOnFire.desc"), ignoreOnFire);
    addSetting("retakeLava", LocalizeString::get("client.module.autoPlace.retakeLava.name"),
               LocalizeString::get("client.module.autoPlace.retakeLava.desc"), retakeLava);
    addSliderSetting("retakeDelayMs", LocalizeString::get("client.module.autoPlace.retakeDelayMs.name"),
                     LocalizeString::get("client.module.autoPlace.retakeDelayMs.desc"), retakeDelayMs, FloatValue(0.f),
                     FloatValue(5000.f), FloatValue(100.f), "retakeLava"_istrue);

    rotMode.addEntry(EnumEntry(0, LocalizeString::get("client.module.autoPlace.rotMode.off.name")));
    rotMode.addEntry(EnumEntry(1, LocalizeString::get("client.module.autoPlace.rotMode.smooth.name")));
    rotMode.addEntry(EnumEntry(2, LocalizeString::get("client.module.autoPlace.rotMode.psilent.name")));
    addEnumSetting("rotMode", LocalizeString::get("client.module.autoPlace.rotMode.name"),
                   LocalizeString::get("client.module.autoPlace.rotMode.desc"), rotMode);
    addSliderSetting("smoothSpeed", LocalizeString::get("client.module.autoPlace.smoothSpeed.name"),
                     LocalizeString::get("client.module.autoPlace.smoothSpeed.desc"), smoothSpeed, FloatValue(5.f),
                     FloatValue(180.f), FloatValue(5.f));
    addSliderSetting("reach", LocalizeString::get("client.module.autoPlace.reach.name"),
                     LocalizeString::get("client.module.autoPlace.reach.desc"), reach, FloatValue(2.f),
                     FloatValue(12.f), FloatValue(0.5f));
    addSliderSetting("delayMs", LocalizeString::get("client.module.autoPlace.delayMs.name"),
                     LocalizeString::get("client.module.autoPlace.delayMs.desc"), delayMs, FloatValue(0.f),
                     FloatValue(2000.f), FloatValue(50.f));
    addSetting("returnItem", LocalizeString::get("client.module.autoPlace.returnItem.name"),
               LocalizeString::get("client.module.autoPlace.returnItem.desc"), returnItem);
    addSetting("skipIfTrapped", LocalizeString::get("client.module.autoPlace.skipIfTrapped.name"),
               LocalizeString::get("client.module.autoPlace.skipIfTrapped.desc"), skipIfTrapped);
    addSetting("useMovementSim", LocalizeString::get("client.module.autoPlace.useMovementSim.name"),
               LocalizeString::get("client.module.autoPlace.useMovementSim.desc"), useMovementSim);
    addSliderSetting("maxSimMs", LocalizeString::get("client.module.autoPlace.maxSimMs.name"),
                     LocalizeString::get("client.module.autoPlace.maxSimMs.desc"), maxSimMs, FloatValue(25.f),
                     FloatValue(1000.f), FloatValue(25.f), "useMovementSim"_istrue);
    addSliderSetting("enforcedSimMs", LocalizeString::get("client.module.autoPlace.enforcedSimMs.name"),
                     LocalizeString::get("client.module.autoPlace.enforcedSimMs.desc"), enforcedSimMs,
                     FloatValue(0.f), FloatValue(500.f), FloatValue(25.f), "useMovementSim"_istrue);

    addSetting("players", LocalizeString::get("client.module.autoPlace.players.name"),
               LocalizeString::get("client.module.autoPlace.players.desc"), players);
    addSetting("mobs", LocalizeString::get("client.module.autoPlace.mobs.name"),
               LocalizeString::get("client.module.autoPlace.mobs.desc"), mobs);

    targetMode.addEntry(EnumEntry(0, LocalizeString::get("client.module.autoPlace.targetMode.distance.name")));
    targetMode.addEntry(EnumEntry(1, LocalizeString::get("client.module.autoPlace.targetMode.crosshair.name")));
    targetMode.addEntry(EnumEntry(2, LocalizeString::get("client.module.autoPlace.targetMode.lowestHealth.name")));
    addEnumSetting("targetMode", LocalizeString::get("client.module.autoPlace.targetMode.name"),
                   LocalizeString::get("client.module.autoPlace.targetMode.desc"), targetMode);

    Setting::Condition fovCond(std::vector<Setting::SingleCond> {
        { "targetMode", { 1 }, false },
    });
    addSliderSetting("fov", LocalizeString::get("client.module.autoPlace.fov.name"),
                     LocalizeString::get("client.module.autoPlace.fov.desc"), fov, FloatValue(10.f),
                     FloatValue(180.f), FloatValue(5.f), fovCond);

    listen<TickEvent>(static_cast<EventListenerFunc>(&AutoPlace::onTick));
}

void AutoPlace::onEnable() {
    phase = Phase::Idle;
    retakeAttempts = 0;
    rotSpoofed = false;
}

void AutoPlace::onDisable() {
    if (rotSpoofed) {
        if (auto ci = SDK::ClientInstance::get(); ci && ci->getLocalPlayer()) {
            ci->getLocalPlayer()->getRot() = savedRot;
        }
        rotSpoofed = false;
    }
    SlotLease::release(leaseOwner, true);
    phase = Phase::Idle;
    retakeAttempts = 0;
}

bool AutoPlace::findSlot(SDK::LocalPlayer* lp, bool wantLava, int& outSlot) const {
    if (!lp || !lp->supplies || !lp->supplies->inventory) return false;
    for (int i = 0; i < 9; i++) {
        auto stack = lp->supplies->inventory->getItem(i);
        if (!stack || stack->itemCount <= 0 || !stack->getItem()) continue;
        std::string id = stack->getItem()->namespacedId.getString();
        if (wantLava) {
            if (id == "minecraft:lava_bucket") {
                outSlot = i;
                return true;
            }
        } else {
            if (id == "minecraft:bucket" || id == "minecraft:cobweb" || id == "minecraft:web") {
                outSlot = i;
                return true;
            }
        }
    }
    return false;
}

bool AutoPlace::alreadyTrapped(SDK::BlockSource* region, AABB const& box) {
    int x0 = floori(box.lower.x + 0.01f);
    int x1 = floori(box.higher.x - 0.01f);
    int z0 = floori(box.lower.z + 0.01f);
    int z1 = floori(box.higher.z - 0.01f);
    int y0 = floori(box.lower.y + 0.01f);
    int y1 = floori(box.higher.y - 0.01f);

    for (int x = x0; x <= x1; x++) {
        for (int z = z0; z <= z1; z++) {
            for (int y = y0; y <= y1; y++) {
                bool lavaHit = false;
                if (isTrapBlock(region, x, y, z, lavaHit)) return true;
            }
        }
    }
    return false;
}

Vec2 AutoPlace::rotTowards(Vec3 const& from, Vec3 const& to) {
    Vec3 dir = to - from;
    float len = dir.magnitude();
    if (len < 0.001f) return { 0.f, 0.f };
    dir = dir * (1.f / len);
    Vec2 rot { -std::asin(std::clamp(dir.y, -1.f, 1.f)) * (180.f / pi_f),
               std::atan2(dir.z, dir.x) * (180.f / pi_f) - 90.f };
    rot.x = std::clamp(rot.x, -89.9f, 89.9f);
    rot.y = wrapAngle(rot.y);
    return rot;
}

float AutoPlace::angleBetween(Vec2 const& a, Vec2 const& b) {
    float dx = wrapAngle(b.x - a.x);
    float dy = wrapAngle(b.y - a.y);
    return std::hypot(dx, dy);
}

void AutoPlace::finishAction(bool placed) {
    SlotLease::release(leaseOwner, placed && std::get<BoolValue>(returnItem).value);
    phase = Phase::Idle;
    nextAction = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(std::max(0, static_cast<int>(std::get<FloatValue>(delayMs).value)));
}

bool AutoPlace::resolveCell(SDK::BlockSource* region, CombatTargetResult const& target, float predictTicks, int& outX,
                            int& outY, int& outZ) {
    AABB box = target.box;

    if (std::get<BoolValue>(useMovementSim).value && target.actor) {
        int neededMs = 100 + static_cast<int>(predictTicks * 50.f);
        int maxMs = std::max(25, static_cast<int>(std::get<FloatValue>(maxSimMs).value));
        int enforcedMs = std::max(0, static_cast<int>(std::get<FloatValue>(enforcedSimMs).value));
        int totalMs = std::min(neededMs, maxMs) + enforcedMs;
        int simTicksTotal = std::max(1, (totalMs + 25) / 50);
        Vec3 seedVel = target.actor->getVelocity();
        float turnPerTick = 0.f;
        if (auto seed = MovementSim::simSeed(target.runtimeId)) {
            seedVel = seed->vel;
            turnPerTick = seed->turnPerTick;
        }
        auto sim = MovementSim::predictForward(target.actor, seedVel, simTicksTotal, true, true, turnPerTick);
        if (sim.valid && !sim.hitFloor) {
            for (int extra = 10; extra <= 30 && !sim.hitFloor; extra += 10) {
                auto longer =
                    MovementSim::predictForward(target.actor, seedVel, simTicksTotal + extra, true, true, turnPerTick);
                if (!longer.valid) break;
                sim = std::move(longer);
            }
        }
        if (sim.valid) MovementSim::publishPrediction(target.runtimeId, sim);
        if (sim.valid && !sim.trace.empty()) {
            auto cellFor = [&](AABB const& b, int& cx, int& cy, int& cz) -> bool {
                int feetY = static_cast<int>(std::floor(b.lower.y + 0.001f));
                int bx0 = static_cast<int>(std::floor(b.lower.x + 0.001f));
                int bx1 = static_cast<int>(std::floor(b.higher.x - 0.001f));
                int bz0 = static_cast<int>(std::floor(b.lower.z + 0.001f));
                int bz1 = static_cast<int>(std::floor(b.higher.z - 0.001f));

                float bestCoverage = 0.f;
                bool found = false;
                for (int x = bx0; x <= bx1; x++) {
                    for (int z = bz0; z <= bz1; z++) {
                        if (BlockSolid::isCollidable(region, x, feetY, z)) continue;
                        float overlapX = std::min(b.higher.x, static_cast<float>(x) + 1.f) -
                                         std::max(b.lower.x, static_cast<float>(x));
                        float overlapZ = std::min(b.higher.z, static_cast<float>(z) + 1.f) -
                                         std::max(b.lower.z, static_cast<float>(z));
                        if (overlapX <= 0.f || overlapZ <= 0.f) continue;
                        float coverage = overlapX * overlapZ;
                        if (coverage <= bestCoverage) continue;
                        bestCoverage = coverage;
                        cx = x;
                        cy = feetY;
                        cz = z;
                        found = true;
                    }
                }
                return found;
            };

            int lastIdx = static_cast<int>(sim.trace.size()) - 1;
            int cx = 0;
            int cy = 0;
            int cz = 0;
            int step = 2;
            for (int i = lastIdx; i >= 0; i -= step) {
                if (!cellFor(sim.trace[i].box, cx, cy, cz)) continue;
                for (int j = 0; j <= lastIdx; j++) {
                    auto const& sample = sim.trace[j];
                    int sx = static_cast<int>(std::floor(sample.box.getCenter().x));
                    int sz = static_cast<int>(std::floor(sample.box.getCenter().z));
                    int sy = static_cast<int>(std::floor(sample.box.lower.y + 0.001f));
                    if (sx == cx && sz == cz && std::abs(sy - cy) <= 1) {
                        outX = cx;
                        outY = cy;
                        outZ = cz;
                        return true;
                    }
                }
            }

            if (cellFor(sim.trace[lastIdx].box, outX, outY, outZ)) return true;
            box = sim.trace[lastIdx].box;
        } else if (sim.valid) {
            box = sim.finalBox;
        }
    } else if (predictTicks > 0.f && target.actor) {
        Vec3 vel = target.actor->getVelocity();
        if (auto seed = MovementSim::simSeed(target.runtimeId)) vel = seed->vel;
        int ticks = std::clamp(static_cast<int>(predictTicks), 0, 20);
        Vec3 shift { vel.x * static_cast<float>(ticks), 0.f, vel.z * static_cast<float>(ticks) };
        box.lower = box.lower + shift;
        box.higher = box.higher + shift;
    }

    int feetY = static_cast<int>(std::floor(box.lower.y + 0.001f));
    int x0 = static_cast<int>(std::floor(box.lower.x + 0.001f));
    int x1 = static_cast<int>(std::floor(box.higher.x - 0.001f));
    int z0 = static_cast<int>(std::floor(box.lower.z + 0.001f));
    int z1 = static_cast<int>(std::floor(box.higher.z - 0.001f));

    float bestCoverage = 0.f;
    bool found = false;
    int fallbackX = 0;
    int fallbackZ = 0;
    bool haveFallback = false;
    float fallbackDist = 0.f;
    float cx = (box.lower.x + box.higher.x) * 0.5f;
    float cz = (box.lower.z + box.higher.z) * 0.5f;
    for (int x = x0; x <= x1; x++) {
        for (int z = z0; z <= z1; z++) {
            if (BlockSolid::isCollidable(region, x, feetY, z)) continue;

            float overlapX = std::min(box.higher.x, static_cast<float>(x) + 1.f) -
                             std::max(box.lower.x, static_cast<float>(x));
            float overlapZ = std::min(box.higher.z, static_cast<float>(z) + 1.f) -
                             std::max(box.lower.z, static_cast<float>(z));
            if (overlapX <= 0.f || overlapZ <= 0.f) continue;

            float coverage = overlapX * overlapZ;
            if (!hasPlacementSupport(region, x, feetY, z)) coverage *= 0.25f;
            if (coverage > bestCoverage) {
                bestCoverage = coverage;
                outX = x;
                outY = feetY;
                outZ = z;
                found = true;
            }

            if (!found) {
                float dx = static_cast<float>(x) + 0.5f - cx;
                float dz = static_cast<float>(z) + 0.5f - cz;
                float dist = dx * dx + dz * dz;
                if (!haveFallback || dist < fallbackDist) {
                    haveFallback = true;
                    fallbackDist = dist;
                    fallbackX = x;
                    fallbackZ = z;
                }
            }
        }
    }
    if (!found && haveFallback) {
        outX = fallbackX;
        outY = feetY;
        outZ = fallbackZ;
        return true;
    }
    return found;
}

bool AutoPlace::hasPlacementSupport(SDK::BlockSource* region, int x, int y, int z) {
    static constexpr int offX[6] = { 0, 0, 0, 0, -1, 1 };
    static constexpr int offY[6] = { -1, 1, 0, 0, 0, 0 };
    static constexpr int offZ[6] = { 0, 0, -1, 1, 0, 0 };
    for (int f = 0; f < 6; f++) {
        if (BlockSolid::isCollidable(region, x + offX[f], y + offY[f], z + offZ[f])) return true;
    }
    return false;
}

void AutoPlace::onTick(Event&) {
    SlotLease::pump();

    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->supplies || !lp->supplies->inventory || !lp->gameMode || !lp->aabbShape) {
        finishAction(false);
        return;
    }
    if (Necromancer::get().getScreenManager().getActiveScreen().has_value()) return;
    auto region = ci->getRegion();
    if (!region) return;
    if (!Signatures::GameMode_buildBlock.result) return;

    auto now = std::chrono::steady_clock::now();
    if (now < nextAction) return;
    if (phase == Phase::WaitForSwitch && now < switchTime) return;
    if (phase == Phase::Idle && SlotLease::busyForOthers(leaseOwner)) return;

    if (phase == Phase::Retake) {
        tickRetake(lp, region, now);
        return;
    }

    CombatTargetQuery q;
    q.players = std::get<BoolValue>(players).value;
    q.mobs = std::get<BoolValue>(mobs).value;
    q.mode = targetMode.getSelectedKey();
    q.range = std::get<FloatValue>(reach).value;
    q.fov = std::get<FloatValue>(fov).value;
    auto target = CombatTarget::find(lp, q);
    if (!target) {
        if (phase == Phase::WaitForSwitch) finishAction(false);
        return;
    }

    if (std::get<BoolValue>(skipIfTrapped).value && alreadyTrapped(region, target->box)) {
        if (phase == Phase::WaitForSwitch) finishAction(false);
        return;
    }

    bool wantLava = false;
    int slot = -1;
    bool lavaUsable = std::get<BoolValue>(lava).value;
    if (lavaUsable && std::get<BoolValue>(ignoreOnFire).value && target->actor &&
        target->actor->getStatusFlag(actorFlagOnFire)) {
        lavaUsable = false;
    }
    if (lavaUsable && findSlot(lp, true, slot)) {
        wantLava = true;
    } else if (std::get<BoolValue>(cobweb).value && findSlot(lp, false, slot)) {
        wantLava = false;
    } else {
        if (phase == Phase::WaitForSwitch) finishAction(false);
        return;
    }

    float predictTicks = static_cast<float>(RealPing::get()) / 50.f;

    int cellX = 0;
    int cellY = 0;
    int cellZ = 0;
    if (!resolveCell(region, *target, predictTicks, cellX, cellY, cellZ)) {
        if (phase == Phase::WaitForSwitch) finishAction(false);
        return;
    }

    BlockPos support { cellX, cellY - 1, cellZ };
    uint8_t face = 1;
    if (!BlockSolid::isCollidable(region, support.x, support.y, support.z)) {
        static constexpr int offX[6] = { 0, 0, 0, 0, -1, 1 };
        static constexpr int offY[6] = { -1, 1, 0, 0, 0, 0 };
        static constexpr int offZ[6] = { 0, 0, -1, 1, 0, 0 };
        static constexpr uint8_t opposite[6] = { 1, 0, 3, 2, 5, 4 };
        static constexpr int order[6] = { 2, 3, 4, 5, 1 };

        bool found = false;
        for (int i = 0; i < 5 && !found; i++) {
            int f = order[i];
            int sx = cellX + offX[f];
            int sy = cellY + offY[f];
            int sz = cellZ + offZ[f];
            if (!BlockSolid::isCollidable(region, sx, sy, sz)) continue;
            support = BlockPos { sx, sy, sz };
            face = opposite[f];
            found = true;
        }
        if (!found) {
            if (phase == Phase::WaitForSwitch) finishAction(false);
            return;
        }
    }

    AABB selfBox = lp->aabbShape->boundingBox;
    Vec3 eye { (selfBox.lower.x + selfBox.higher.x) * 0.5f, selfBox.higher.y - 0.18f,
               (selfBox.lower.z + selfBox.higher.z) * 0.5f };
    Vec3 clickPos { static_cast<float>(cellX) + 0.5f, static_cast<float>(cellY) + 0.5f,
                    static_cast<float>(cellZ) + 0.5f };

    int mode = rotMode.getSelectedKey();
    Vec2 rot = lp->getRot();
    if (mode == 1 && phase != Phase::WaitForSwitch) {
        Vec2 desired = rotTowards(eye, clickPos);
        float err = angleBetween(rot, desired);
        if (err > 4.f) {
            float step = std::clamp(std::get<FloatValue>(smoothSpeed).value, 5.f, 180.f);
            float t = std::min(1.f, step / err);
            Vec2 delta { (desired.x - rot.x) * t, wrapAngle(desired.y - rot.y) * t };
            lp->applyTurnDelta({ -delta.x, delta.y });
            return;
        }
    }

    if (phase == Phase::Idle) {
        int holdMs = switchWaitMs + 200;
        if (!SlotLease::acquire(leaseOwner, SlotLease::Priority::Placing, holdMs)) return;
        if (lp->supplies->selectedSlot != slot) {
            lp->supplies->selectedSlot = slot;
            switchTime = now + std::chrono::milliseconds(switchWaitMs);
            phase = Phase::WaitForSwitch;
            return;
        }
    }

    if (!SlotLease::ownedBy(leaseOwner)) {
        phase = Phase::Idle;
        return;
    }
    if (lp->supplies->selectedSlot != slot) lp->supplies->selectedSlot = slot;

    auto held = lp->supplies->inventory->getItem(lp->supplies->selectedSlot);
    if (!held || held->itemCount <= 0 || !held->getItem()) {
        finishAction(false);
        return;
    }

    Vec2 rotBeforePlace = lp->getRot();
    if (mode == 2) {
        Vec2 spoof = rotTowards(eye, clickPos);
        lp->getRot() = spoof;
    }

    BoxBuilder::PlacementBypass bypass;
    bool placed = reinterpret_cast<bool (*)(SDK::GameMode*, BlockPos const*, uint8_t, bool)>(
        Signatures::GameMode_buildBlock.result)(lp->gameMode, &support, face, true);

    if (mode == 2) lp->getRot() = rotBeforePlace;

    if (placed && wantLava && std::get<BoolValue>(retakeLava).value) {
        phase = Phase::Retake;
        retakeCell = BlockPos { cellX, cellY, cellZ };
        retakeStartedAt = now;
        retakeNextTry = now + std::chrono::milliseconds(
                                  std::max(0, static_cast<int>(std::get<FloatValue>(retakeDelayMs).value)));
        retakeAttempts = 0;
        return;
    }

    finishAction(placed);
}

void AutoPlace::restoreSpoofedRot(SDK::LocalPlayer* lp, std::chrono::steady_clock::time_point now) {
    if (!rotSpoofed || !lp) return;

    auto mouse = SDK::MouseDevice::get();
    bool pending = mouse && !mouse->inputs.empty();
    auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - rotSpoofedAt).count();
    if (!pending || heldMs > 150) {
        lp->getRot() = savedRot;
        rotSpoofed = false;
    }
}

void AutoPlace::retakeFinish(SDK::LocalPlayer* lp, bool scooped) {
    if (rotSpoofed && lp) {
        lp->getRot() = savedRot;
        rotSpoofed = false;
    }
    SlotLease::release(leaseOwner, scooped || std::get<BoolValue>(returnItem).value);
    phase = Phase::Idle;
    nextAction = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(std::max(0, static_cast<int>(std::get<FloatValue>(delayMs).value)));
}

void AutoPlace::tickRetake(SDK::LocalPlayer* lp, SDK::BlockSource* region, std::chrono::steady_clock::time_point now) {
    if (!SlotLease::ownedBy(leaseOwner)) {
        phase = Phase::Idle;
        return;
    }
    SlotLease::acquire(leaseOwner, SlotLease::Priority::Placing, 500);

    restoreSpoofedRot(lp, now);

    if (!BlockSolid::isLiquidAt(region, retakeCell.x, retakeCell.y, retakeCell.z)) {
        if (retakeAttempts > 0) {
            retakeFinish(lp, true);
            return;
        }
        int appearMs = std::max(1500, static_cast<int>(std::get<FloatValue>(retakeDelayMs).value) + 1000);
        if (now - retakeStartedAt > std::chrono::milliseconds(appearMs)) {
            retakeFinish(lp, false);
            return;
        }
        return;
    }

    if (now < retakeNextTry || rotSpoofed) return;

    int bucketSlot = -1;
    for (int i = 0; i < 9; i++) {
        auto stack = lp->supplies->inventory->getItem(i);
        if (!stack || stack->itemCount <= 0 || !stack->getItem()) continue;
        if (stack->getItem()->namespacedId.getString() == "minecraft:bucket") {
            bucketSlot = i;
            break;
        }
    }
    if (bucketSlot < 0) {
        retakeFinish(lp, false);
        return;
    }

    if (lp->supplies->selectedSlot != bucketSlot) lp->supplies->selectedSlot = bucketSlot;

    Vec3 eye = lp->getPos();
    Vec3 clickPos { static_cast<float>(retakeCell.x) + 0.5f, static_cast<float>(retakeCell.y) + 0.5f,
                    static_cast<float>(retakeCell.z) + 0.5f };
    if (!rotSpoofed) {
        savedRot = lp->getRot();
        rotSpoofedAt = now;
        lp->getRot() = rotTowards(eye, clickPos);
        rotSpoofed = true;
    }
    pushAction(2, true);
    pushAction(2, false);

    retakeAttempts++;
    retakeNextTry = now + std::chrono::milliseconds(
                               std::max(100, static_cast<int>(std::get<FloatValue>(retakeDelayMs).value)));
    if (retakeAttempts >= maxRetakeAttempts) retakeFinish(lp, false);
}
