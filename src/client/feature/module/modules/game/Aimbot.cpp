#include "pch.h"
#include "Aimbot.h"
#include "Backtrack.h"
#include "Freelook.h"
#include "client/event/events/ClickEvent.h"
#include "client/event/events/AfterMoveEvent.h"
#include "client/event/events/BeforeMoveEvent.h"
#include "client/event/events/SendPacketEvent.h"
#include "client/event/events/TurnDeltaEvent.h"
#include "client/event/events/TickEvent.h"
#include "client/misc/EntityCache.h"
#include "client/misc/PlayerListManager.h"
#include "client/misc/RenderFrameState.h"
#include "client/misc/TargetManager.h"
#include "client/misc/WallCheck.h"
#include "client/screen/ScreenManager.h"
#include "mc/Addresses.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/client/renderer/GameRenderer.h"
#include "mc/common/world/actor/player/GameMode.h"
#include "mc/common/world/actor/player/Player.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/Dimension.h"
#include <array>
#include <cfloat>

namespace {
    enum HitboxSlot : int {
        SlotHead = 0,
        SlotNeck,
        SlotChest,
        SlotBody,
        SlotStomach,
        SlotLegs,
        SlotFeet,
        SlotAuto,
    };

    constexpr int anchorCount = 7;
    constexpr float anchorHeight[anchorCount] = { 0.9f, 0.8f, 0.7f, 0.55f, 0.42f, 0.22f, 0.05f };

    Vec3 lookDir(Vec2 const& rot) {
        float yaw = (rot.y + 90.f) * (pi_f / 180.f);
        float pitch = rot.x * -(pi_f / 180.f);
        return { cosf(yaw) * cosf(pitch), sinf(pitch), sinf(yaw) * cosf(pitch) };
    }

    float wrapAngle(float angle) {
        while (angle > 180.f) angle -= 360.f;
        while (angle < -180.f) angle += 360.f;
        return angle;
    }

    Vec3 poseAwareEye(SDK::Actor* actor, Vec3 const& position) {
        AABB bounds = actor->getBoundingBox();
        float height = bounds.higher.y - bounds.lower.y;
        float eyeOffset = actor->getPos().y - bounds.lower.y;
        bounds.rebase(position - Vec3 { 0.f, eyeOffset, 0.f } + Vec3 { 0.f, height * 0.5f, 0.f });

        Vec3 eye = position;
        if (NecromancerMath::aequals(bounds.lower.y, eye.y)) {
            eye.y = bounds.lower.y + height * 0.85f;
        }
        return eye;
    }

    bool isAimableMob(SDK::Actor* entt) {
        if (entt->isItem()) return false;
        static const std::unordered_set<std::string> skip = {
            "minecraft:armor_stand",   "minecraft:boat",           "minecraft:chest_boat",
            "minecraft:minecart",      "minecraft:hopper_minecart", "minecraft:chest_minecart",
            "minecraft:tnt_minecart",  "minecraft:command_block_minecart", "minecraft:tnt",
            "minecraft:leash_knot",    "minecraft:painting",       "minecraft:lightning_bolt",
            "minecraft:xp_orb",        "minecraft:arrow",          "minecraft:thrown_trident",
            "minecraft:snowball",      "minecraft:egg",            "minecraft:ender_pearl",
            "minecraft:fishing_hook",  "minecraft:fireball",       "minecraft:small_fireball",
            "minecraft:wither_skull",  "minecraft:shulker_bullet", "minecraft:falling_block",
            "minecraft:area_effect_cloud", "minecraft:dragon_fireball", "minecraft:fireworks_rocket",
            "minecraft:eye_of_ender_signal", "minecraft:llama_spit", "minecraft:wind_charge_projectile",
            "minecraft:breeze_wind_charge_projectile", "minecraft:ominous_item_spawner",
        };
        thread_local std::unordered_map<uint32_t, bool> verdictByType;
        uint32_t typeId = entt->getEntityTypeID();
        auto it = verdictByType.find(typeId);
        if (it != verdictByType.end()) return it->second;
        bool aimable = !skip.contains(entt->getEntityTypeName());
        verdictByType.emplace(typeId, aimable);
        return aimable;
    }

    constexpr int lateralCount = 5;
    constexpr float lateralOffset[lateralCount][2] = {
        { 0.5f, 0.5f }, { 0.18f, 0.5f }, { 0.82f, 0.5f }, { 0.5f, 0.18f }, { 0.5f, 0.82f },
    };

    constexpr float lateralBias = 0.35f;

    constexpr float surfaceInset = 0.05f;

    constexpr float snapSpeedThreshold = 90.f;

    Vec3 closestFrac(AABB const& bb, Vec3 const& eye) {
        auto axis = [](float low, float high, float from) {
            float span = high - low;
            if (span <= 0.0001f) return 0.5f;
            float inset = std::min(surfaceInset, span * 0.25f);
            return (std::clamp(from, low + inset, high - inset) - low) / span;
        };
        return { axis(bb.lower.x, bb.higher.x, eye.x), axis(bb.lower.y, bb.higher.y, eye.y),
                 axis(bb.lower.z, bb.higher.z, eye.z) };
    }

    Vec3 fracForSlot(int slot) {
        int clamped = std::clamp(slot, 0, anchorCount - 1);
        return { 0.5f, anchorHeight[clamped], 0.5f };
    }

    Vec3 boxPoint(AABB const& bb, Vec3 const& frac) {
        return { bb.lower.x + (bb.higher.x - bb.lower.x) * frac.x,
                 bb.lower.y + (bb.higher.y - bb.lower.y) * frac.y,
                 bb.lower.z + (bb.higher.z - bb.lower.z) * frac.z };
    }

    std::optional<Vec3> resolveAutoFrac(AABB const& bb, Vec3 const& eye, SDK::BlockSource* region) {
        auto reachable = [&](Vec3 const& frac) {
            return !region || WallCheck::isVisible(region, eye, boxPoint(bb, frac));
        };

        Vec3 nearest = closestFrac(bb, eye);
        if (reachable(nearest)) return nearest;

        struct Ranked {
            Vec3 frac;
            float cost;
        };

        float width = std::max(bb.higher.x - bb.lower.x, bb.higher.z - bb.lower.z);

        std::array<Ranked, anchorCount * lateralCount> ranked {};
        int count = 0;
        for (int h = 0; h < anchorCount; ++h) {
            for (int l = 0; l < lateralCount; ++l) {
                Vec3 frac { lateralOffset[l][0], anchorHeight[h], lateralOffset[l][1] };
                float lateralDrift = std::abs(frac.x - 0.5f) + std::abs(frac.z - 0.5f);
                float cost = eye.distance(boxPoint(bb, frac)) + lateralDrift * width * lateralBias;
                ranked[count++] = { frac, cost };
            }
        }
        std::sort(ranked.begin(), ranked.end(), [](Ranked const& a, Ranked const& b) { return a.cost < b.cost; });

        for (auto const& entry : ranked) {
            if (reachable(entry.frac)) return entry.frac;
        }
        return std::nullopt;
    }
}

Aimbot::Aimbot()
    : Module("Aimbot", LocalizeString::get("client.module.aimbot.name"),
             LocalizeString::get("client.module.aimbot.desc"), GAME, nokeybind) {
    aimMode.addEntry(EnumEntry(aim_normal, LocalizeString::get("client.module.aimbot.aimMode.normal.name"),
                               LocalizeString::get("client.module.aimbot.aimMode.normal.desc")));
    aimMode.addEntry(EnumEntry(aim_psilent, LocalizeString::get("client.module.aimbot.aimMode.psilent.name"),
                               LocalizeString::get("client.module.aimbot.aimMode.psilent.desc")));
    aimMode.addEntry(EnumEntry(aim_silent, LocalizeString::get("client.module.aimbot.aimMode.silent.name"),
                               LocalizeString::get("client.module.aimbot.aimMode.silent.desc")));
    addEnumSetting("aimMode", LocalizeString::get("client.module.aimbot.aimMode.name"),
                   LocalizeString::get("client.module.aimbot.aimMode.desc"), aimMode);

    Setting::Condition psilentCond(std::vector<Setting::SingleCond> {
        { "aimMode", { aim_psilent }, false },
    });
    addSetting("psilentSprintHits", LocalizeString::get("client.module.aimbot.psilentSprintHits.name"),
               LocalizeString::get("client.module.aimbot.psilentSprintHits.desc"), psilentSprintHits, psilentCond);

    Setting::Condition silentCond(std::vector<Setting::SingleCond> {
        { "aimMode", { aim_normal, aim_silent }, false },
    });
    addSliderSetting("smoothSpeed", LocalizeString::get("client.module.aimbot.smoothSpeed.name"),
                     LocalizeString::get("client.module.aimbot.smoothSpeed.desc"), smoothSpeed, FloatValue(0.5f),
                     FloatValue(100.f), FloatValue(0.5f), silentCond);
    addSetting("lockOn", LocalizeString::get("client.module.aimbot.lockOn.name"),
               LocalizeString::get("client.module.aimbot.lockOn.desc"), lockOn, silentCond);
    addSetting("silentRotSmooth", LocalizeString::get("client.module.aimbot.silentRotSmooth.name"),
               LocalizeString::get("client.module.aimbot.silentRotSmooth.desc"), silentRotSmooth,
               Setting::Condition(std::vector<Setting::SingleCond> {
                   { "aimMode", { aim_silent }, false },
               }));

    addSetting("allBodyParts", LocalizeString::get("client.module.aimbot.allBodyParts.name"),
               LocalizeString::get("client.module.aimbot.allBodyParts.desc"), allBodyParts);

    addSliderSetting("range", LocalizeString::get("client.module.aimbot.range.name"),
                     LocalizeString::get("client.module.aimbot.range.desc"), range, FloatValue(1.f), FloatValue(15.f),
                     FloatValue(0.5f));

    targetMode.addEntry(EnumEntry(0, LocalizeString::get("client.module.aimbot.targetMode.distance.name"),
                                  LocalizeString::get("client.module.aimbot.targetMode.distance.desc")));
    targetMode.addEntry(EnumEntry(1, LocalizeString::get("client.module.aimbot.targetMode.crosshair.name"),
                                  LocalizeString::get("client.module.aimbot.targetMode.crosshair.desc")));
    targetMode.addEntry(EnumEntry(2, LocalizeString::get("client.module.aimbot.targetMode.lowestHealth.name"),
                                  LocalizeString::get("client.module.aimbot.targetMode.lowestHealth.desc")));
    addEnumSetting("targetMode", LocalizeString::get("client.module.aimbot.targetMode.name"),
                   LocalizeString::get("client.module.aimbot.targetMode.desc"), targetMode);

    Setting::Condition fovCond(std::vector<Setting::SingleCond> {
        { "targetMode", { 1 }, false },
    });
    addSliderSetting("fov", LocalizeString::get("client.module.aimbot.fov.name"),
                     LocalizeString::get("client.module.aimbot.fov.desc"), fov, FloatValue(10.f), FloatValue(180.f),
                     FloatValue(5.f), fovCond);
    addSetting("fovColor", LocalizeString::get("client.module.aimbot.fovColor.name"),
               LocalizeString::get("client.module.aimbot.fovColor.desc"), fovColor, fovCond);
    addSliderSetting("fovWidth", LocalizeString::get("client.module.aimbot.fovWidth.name"),
                     LocalizeString::get("client.module.aimbot.fovWidth.desc"), fovWidth, FloatValue(0.5f),
                     FloatValue(5.f), FloatValue(0.5f), fovCond);

    auto addHitboxEntries = [](EnumData& e) {
        e.addEntry(EnumEntry(SlotHead, LocalizeString::get("client.module.aimbot.hitbox.head.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.head.desc")));
        e.addEntry(EnumEntry(SlotNeck, LocalizeString::get("client.module.aimbot.hitbox.neck.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.neck.desc")));
        e.addEntry(EnumEntry(SlotChest, LocalizeString::get("client.module.aimbot.hitbox.chest.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.chest.desc")));
        e.addEntry(EnumEntry(SlotBody, LocalizeString::get("client.module.aimbot.hitbox.body.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.body.desc")));
        e.addEntry(EnumEntry(SlotStomach, LocalizeString::get("client.module.aimbot.hitbox.stomach.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.stomach.desc")));
        e.addEntry(EnumEntry(SlotLegs, LocalizeString::get("client.module.aimbot.hitbox.legs.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.legs.desc")));
        e.addEntry(EnumEntry(SlotFeet, LocalizeString::get("client.module.aimbot.hitbox.feet.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.feet.desc")));
        e.addEntry(EnumEntry(SlotAuto, LocalizeString::get("client.module.aimbot.hitbox.auto.name"),
                             LocalizeString::get("client.module.aimbot.hitbox.auto.desc")));
    };

    addHitboxEntries(hitbox);
    hitbox.setSelectedKey(SlotAuto);
    addEnumSetting("hitbox", LocalizeString::get("client.module.aimbot.hitbox.name"),
                   LocalizeString::get("client.module.aimbot.hitbox.desc"), hitbox)
        ->defaultValue = EnumValue(SlotAuto);
    {
        Setting* hb = nullptr;
        settings->forEach([&](std::shared_ptr<Setting> s) {
            if (!hb && s->name() == "hitbox") hb = s.get();
        });
        if (hb) hb->multiSelect = true;
    }

    addSetting("backtrackTarget", LocalizeString::get("client.module.aimbot.backtrackTarget.name"),
               LocalizeString::get("client.module.aimbot.backtrackTarget.desc"), backtrackTarget, "players"_istrue);
    addSetting("lagRecordsOnly", LocalizeString::get("client.module.aimbot.lagRecordsOnly.name"),
               LocalizeString::get("client.module.aimbot.lagRecordsOnly.desc"), lagRecordsOnly,
               Setting::Condition(std::vector<Setting::SingleCond> {
                   { "players", { 1 }, false },
                   { "backtrackTarget", { 1 }, false },
               }));
    addSetting("backtrackHitboxSame", LocalizeString::get("client.module.aimbot.backtrackHitboxSame.name"),
               LocalizeString::get("client.module.aimbot.backtrackHitboxSame.desc"), backtrackHitboxSame,
               "backtrackTarget"_istrue);
    addHitboxEntries(backtrackHitbox);
    backtrackHitbox.setSelectedKey(SlotAuto);
    addEnumSetting("backtrackHitbox", LocalizeString::get("client.module.aimbot.backtrackHitbox.name"),
                   LocalizeString::get("client.module.aimbot.backtrackHitbox.desc"), backtrackHitbox,
                   Setting::Condition(std::vector<Setting::SingleCond> {
                       { "backtrackTarget", { 1 }, false },
                       { "backtrackHitboxSame", { 0 }, false },
                   }))
        ->defaultValue = EnumValue(SlotAuto);

    addSetting("players", LocalizeString::get("client.module.aimbot.players.name"),
               LocalizeString::get("client.module.aimbot.players.desc"), players);
    addSetting("mobs", LocalizeString::get("client.module.aimbot.mobs.name"),
               LocalizeString::get("client.module.aimbot.mobs.desc"), mobs);
    addSetting("priority", LocalizeString::get("client.module.aimbot.priority.name"),
               LocalizeString::get("client.module.aimbot.priority.desc"), prioritizeTags);
    addSetting("ignoreFriends", LocalizeString::get("client.module.aimbot.ignoreFriends.name"),
               LocalizeString::get("client.module.aimbot.ignoreFriends.desc"), ignoreFriends);
    addSetting("ignoreInvulnerable", LocalizeString::get("client.module.aimbot.ignoreInvulnerable.name"),
               LocalizeString::get("client.module.aimbot.ignoreInvulnerable.desc"), ignoreInvulnerable);
    addSetting("wallCheck", LocalizeString::get("client.module.aimbot.wallCheck.name"),
               LocalizeString::get("client.module.aimbot.wallCheck.desc"), wallCheck);
    addSetting("hitBehindWall", LocalizeString::get("client.module.aimbot.hitBehindWall.name"),
               LocalizeString::get("client.module.aimbot.hitBehindWall.desc"), hitBehindWall);

    listen<UpdateEvent>(&Aimbot::onUpdate);
    listen<UpdatePlayerCameraEvent>((EventListenerFunc)&Aimbot::onCameraUpdate, false, 1);
    listen<TurnDeltaEvent>((EventListenerFunc)&Aimbot::onTurnDelta);
    listen<CinematicCameraEvent>((EventListenerFunc)&Aimbot::onCinematicCamera, false, -100);
    listen<ClickEvent>(&Aimbot::onClick, false, 10);
    listen<AfterMoveEvent>(&Aimbot::onAfterMove, false, 10);
    listen<BeforeMoveEvent>(&Aimbot::onBeforeMove, false, 10);
    listen<SendPacketEvent>(&Aimbot::onSendPacket, false, 20);
    listen<AfterMoveEvent>(&Aimbot::onAfterMovePartBurst, false, 12);
    Eventing::get().listen<RenderOverlayEvent, &Aimbot::onRenderOverlay>(this);
    listen<RendererCleanupEvent>(&Aimbot::onRendererCleanup, true);
}

void Aimbot::afterLoadConfig() {
    int selected = hitbox.getSelectedKey();
    if (selected < 0 || selected > SlotAuto) hitbox.setSelectedKey(SlotAuto);
}

int Aimbot::selectedHitboxes(bool ghost, int* outSlots, int maxSlots) {
    if (maxSlots < 1) return 0;

    if (ghost && !std::get<BoolValue>(backtrackHitboxSame).value) {
        int slot = backtrackHitbox.getSelectedKey();
        if (slot < 0 || slot > SlotAuto) slot = SlotAuto;
        outSlots[0] = slot;
        return 1;
    }

    int selected = hitbox.getSelectedKey();
    if (selected < 0x100) {
        int slot = selected;
        if (slot < 0 || slot > SlotAuto) slot = SlotAuto;
        outSlots[0] = slot;
        return 1;
    }

    int mask = selected & 0xFF;
    if (mask == 0 || (mask & (1 << SlotAuto))) {
        outSlots[0] = SlotAuto;
        return 1;
    }

    int count = 0;
    for (int slot = 0; slot < anchorCount && count < maxSlots; slot++) {
        if (mask & (1 << slot)) outSlots[count++] = slot;
    }
    if (count == 0) {
        outSlots[0] = SlotBody;
        return 1;
    }
    return count;
}

Vec3 Aimbot::bestFracForSelection(AABB const& bounds, Vec3 const& eye, SDK::BlockSource* region, bool ghost) {
    constexpr int maxSlots = 8;
    int slots[maxSlots];
    int count = selectedHitboxes(ghost, slots, maxSlots);
    if (count < 1) return closestFrac(bounds, eye);

    if (count == 1 && slots[0] == SlotAuto) {
        auto frac = resolveAutoFrac(bounds, eye, region);
        return frac ? *frac : closestFrac(bounds, eye);
    }

    if (count == 1) return fracForSlot(slots[0]);

    int bestSlot = slots[0];
    float bestDist = FLT_MAX;
    for (int i = 0; i < count; i++) {
        Vec3 point = boxPoint(bounds, fracForSlot(slots[i]));
        float dist = eye.distance(point);
        if (region && WallCheck::isVisible(region, eye, point)) dist -= 100.f;
        if (dist < bestDist) {
            bestDist = dist;
            bestSlot = slots[i];
        }
    }
    return fracForSlot(bestSlot);
}

Backtrack* Aimbot::resolveBacktrack() {
    if (!backtrackResolved) {
        auto mod = Necromancer::getModuleManager().find("Backtrack");
        backtrackModule = mod ? static_cast<Backtrack*>(mod.get()) : nullptr;
        backtrackResolved = true;
    }
    return backtrackModule;
}

void Aimbot::onDisable() {
    std::lock_guard controllerLock { controllerMutex };
    TargetManager::clearTarget(currentTargetId);
    currentTargetId = 0;
    currentTargetGhost = false;
    currentTargetObstructed = false;
    currentTargetBox = {};
    currentTargetRecordAgeMs = -1.f;
    pendingAttack = {};
    partBurst.clear();
    haveCurrentAimFrac = false;
    psilentActive = false;
    publishPSilentLock(0, AABB {}, Vec3 {}, false);
    psilentTouchArmed.store(false, std::memory_order_release);
    commandActive = false;
    silentRotActive = false;
    injectingTurn.store(false, std::memory_order_release);
    userInput.store(0.f, std::memory_order_release);
    lastFrame = {};
    lastCorrectionFrame = UINT64_MAX;
    candidates.clear();
    std::lock_guard lock { aimMutex };
    desiredTargetId = 0;
}

void Aimbot::onUpdate(Event&) {
    std::lock_guard controllerLock { controllerMutex };
    auto ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraft || !ci->minecraftGame || !ci->minecraftGame->isCursorGrabbed() ||
        Necromancer::get().getScreenManager().getActiveScreen()) {
        TargetManager::clearTarget(currentTargetId);
        currentTargetId = 0;
        currentTargetGhost = false;
        currentTargetObstructed = false;
        currentTargetBox = {};
        currentTargetRecordAgeMs = -1.f;
        pendingAttack = {};
        partBurst.clear();
        haveCurrentAimFrac = false;
        psilentActive = false;
        publishPSilentLock(0, AABB {}, Vec3 {}, false);
        commandActive = false;
        silentRotActive = false;
            lastFrame = {};
        lastCorrectionFrame = UINT64_MAX;
        std::lock_guard lock { aimMutex };
        desiredTargetId = 0;
        return;
    }

    auto lp = ci->getLocalPlayer();
    if (!lp) {
        TargetManager::clearTarget(currentTargetId);
        currentTargetId = 0;
        currentTargetGhost = false;
        currentTargetObstructed = false;
        currentTargetBox = {};
        currentTargetRecordAgeMs = -1.f;
        pendingAttack = {};
        partBurst.clear();
        haveCurrentAimFrac = false;
        psilentActive = false;
        publishPSilentLock(0, AABB {}, Vec3 {}, false);
        commandActive = false;
        silentRotActive = false;
            lastFrame = {};
        lastCorrectionFrame = UINT64_MAX;
        std::lock_guard lock { aimMutex };
        desiredTargetId = 0;
        return;
    }

    bool doPlayers = std::get<BoolValue>(players);
    bool doMobs = std::get<BoolValue>(mobs);
    bool doPriority = std::get<BoolValue>(prioritizeTags);
    bool doIgnoreFriends = std::get<BoolValue>(ignoreFriends);
    bool doWallCheck = std::get<BoolValue>(wallCheck);
    bool doHitBehindWall = std::get<BoolValue>(hitBehindWall);
    bool enforceWallCheck = doWallCheck && !doHitBehindWall;
    bool doLockOn = std::get<BoolValue>(lockOn);
    float maxRange = std::get<FloatValue>(range).value;
    int targetModeKey = targetMode.getSelectedKey();
    float fovDeg = std::get<FloatValue>(fov).value;

    uint64_t retainedId;
    {
        std::lock_guard lock { aimMutex };
        retainedId = desiredTargetId;
    }

    Vec3 eye = poseAwareEye(lp, lp->getPos());
    Vec3 forward = lookDir(commandActive ? commandedRot : lp->getRot());

    SDK::BlockSource* region = (doWallCheck || doHitBehindWall) ? ci->getRegion() : nullptr;
    if (region) WallCheck::beginPass();

    candidates.clear();

    bool doGhost = std::get<BoolValue>(backtrackTarget) && doPlayers;
    Backtrack* bt = doGhost ? resolveBacktrack() : nullptr;
    // Lag records only: mobs never have records, so requiring one excludes them entirely.
    bool recordsOnly = doGhost && std::get<BoolValue>(lagRecordsOnly).value;
    bool skipInvulnerable = std::get<BoolValue>(ignoreInvulnerable).value;

    auto snap = EntityCache::get().snapshot();
    for (auto const& view : snap->views) {
        SDK::Actor* entt = view.actor;
        if (!entt || entt == lp || !entt->aabbShape) continue;
        bool isPlayer = view.isPlayer;
        if (isPlayer ? !doPlayers : (!doMobs || !isAimableMob(entt))) continue;
        if (view.invisible) continue;

        if (!view.hasHealth || view.health <= 0.f) continue;
        float health = view.health;
        if (doIgnoreFriends && isPlayer &&
            PlayerListManager::get().isFriend(reinterpret_cast<SDK::Player*>(entt)->playerName))
            continue;

        int priority = 0;
        if (doPriority && isPlayer) {
            priority = PlayerListManager::get().getPriority(reinterpret_cast<SDK::Player*>(entt)->playerName);
        }

        // A target inside its damage-immunity window cannot take a hit; the 10-tick
        // (0.5s) i-frame counter is invulnerableTime. Attacking through it is wasted
        // and looks like a miss, so optionally skip them until it drains.
        if (skipInvulnerable && entt->invulnerableTime > 5) continue;

        std::vector<Backtrack::GhostRecord> ghostRecords;
        bool haveGhostRecords = bt && isPlayer && bt->getGhostRecords(view.runtimeId, ghostRecords);
        if (recordsOnly && !haveGhostRecords) continue;

        auto consider = [&](AABB const& bounds, bool isGhost, float recordAgeMs) {
            float distance = eye.distance(bounds.getCenter());
            if (distance > maxRange) return;

            bool sameTarget = retainedId != 0 && view.runtimeId == retainedId && isGhost == currentTargetGhost;
            bool lockedFrac = doLockOn && sameTarget && haveCurrentAimFrac;

            Vec3 probeFrac = lockedFrac ? currentAimFrac : bestFracForSelection(bounds, eye, nullptr, isGhost);
            Vec3 aimPoint = boxPoint(bounds, probeFrac);
            Vec3 direction = (aimPoint - eye).normalized();
            float angle =
                std::acos(std::clamp(forward.x * direction.x + forward.y * direction.y + forward.z * direction.z,
                                     -1.f, 1.f)) *
                (180.f / pi_f);

            float fovLimit = doLockOn && sameTarget ? fovDeg + 8.f : fovDeg;
            bool isRetained = doLockOn && sameTarget && (targetModeKey != 1 || angle <= fovLimit);

            float score = distance;
            if (targetModeKey == 1) {
                if (angle > fovLimit && !isRetained) return;
                score = angle;
            } else if (targetModeKey == 2) {
                score = health;
            }

            candidates.push_back({ entt, score, priority, isRetained, isGhost, bounds, probeFrac, recordAgeMs });
        };

        if (!recordsOnly) consider(entt->getBoundingBox(), false, -1.f);
        if (haveGhostRecords) {
            for (auto const& record : ghostRecords) consider(record.box, true, record.ageMs);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](TargetCandidate const& a, TargetCandidate const& b) {
        if (a.retained != b.retained) return a.retained;
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.score < b.score;
    });

    SDK::Actor* target = nullptr;
    Vec3 resolvedFrac { 0.5f, 0.55f, 0.5f };
    bool targetIsGhost = false;
    bool targetObstructed = false;
    AABB targetGhostBox {};
    float targetRecordAgeMs = -1.f;

    for (auto const& candidate : candidates) {
        AABB const& bounds = candidate.bounds;

        if (doLockOn && candidate.retained && haveCurrentAimFrac &&
            (!region || !enforceWallCheck || WallCheck::isVisible(region, eye, boxPoint(bounds, candidate.frac)))) {
            resolvedFrac = candidate.frac;
        } else {
            constexpr int maxSlots = 8;
            int slots[maxSlots];
            int slotCount = selectedHitboxes(candidate.isGhost, slots, maxSlots);
            bool isAuto = slotCount == 1 && slots[0] == SlotAuto;

            if (isAuto) {
                auto frac = resolveAutoFrac(bounds, eye, enforceWallCheck ? region : nullptr);
                if (!frac) continue;
                resolvedFrac = *frac;
            } else {
                resolvedFrac = bestFracForSelection(bounds, eye, nullptr, candidate.isGhost);
                if (region && enforceWallCheck &&
                    !WallCheck::isVisible(region, eye, boxPoint(bounds, resolvedFrac))) {
                    bool found = false;
                    for (int i = 0; i < slotCount && !found; i++) {
                        if (WallCheck::isVisible(region, eye, boxPoint(bounds, fracForSlot(slots[i])))) {
                            resolvedFrac = fracForSlot(slots[i]);
                            found = true;
                        }
                    }
                    if (!found) continue;
                }
            }
        }

        target = candidate.actor;
        targetIsGhost = candidate.isGhost;
        targetObstructed = region && !WallCheck::isVisible(region, eye, boxPoint(bounds, resolvedFrac));
        targetGhostBox = candidate.bounds;
        targetRecordAgeMs = candidate.recordAgeMs;
        break;
    }

    if (!target) {
        TargetManager::clearTarget(currentTargetId);
        currentTargetId = 0;
        currentTargetGhost = false;
        currentTargetObstructed = false;
        currentTargetBox = {};
        currentTargetRecordAgeMs = -1.f;
        pendingAttack = {};
        partBurst.clear();
        haveCurrentAimFrac = false;
        psilentActive = false;
        publishPSilentLock(0, AABB {}, Vec3 {}, false);
        commandActive = false;
        silentRotActive = false;
            lastFrame = {};
        lastCorrectionFrame = UINT64_MAX;
        std::lock_guard lock { aimMutex };
        desiredTargetId = 0;
        return;
    }

    uint64_t targetId = target->getRuntimeID();
    {
        std::lock_guard lock { aimMutex };
        desiredTargetId = targetId;
        desiredAimFrac = resolvedFrac;
        desiredIsGhost = targetIsGhost;
        desiredGhostBox = targetGhostBox;
    }

    TargetManager::setTarget(target);
    currentTargetId = targetId;
    currentTargetGhost = targetIsGhost;
    currentTargetObstructed = targetObstructed;
    currentTargetBox = targetGhostBox;
    currentTargetRecordAgeMs = targetRecordAgeMs;
    currentAimFrac = resolvedFrac;
    haveCurrentAimFrac = true;
}

void Aimbot::onCameraUpdate(Event&) {
    std::lock_guard controllerLock { controllerMutex };
    auto ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraft || !ci->minecraftGame || !ci->minecraftGame->isCursorGrabbed() ||
        Necromancer::get().getScreenManager().getActiveScreen())
        return;

    auto lp = ci->getLocalPlayer();
    if (!lp) return;

    uint64_t targetId;
    Vec3 aimFrac;
    bool wantGhost;
    AABB selectedGhost {};
    {
        std::lock_guard lock { aimMutex };
        targetId = desiredTargetId;
        aimFrac = desiredAimFrac;
        wantGhost = desiredIsGhost;
        selectedGhost = desiredGhostBox;
    }
    if (targetId == 0) {
        commandActive = false;
        silentRotActive = false;
            lastFrame = {};
        lastCorrectionFrame = UINT64_MAX;
        userInput.store(0.f, std::memory_order_release);
        return;
    }

    SDK::Actor* target = EntityCache::get().findByRuntimeID(targetId);
    if (!target) {
        commandActive = false;
        silentRotActive = false;
            lastFrame = {};
        lastCorrectionFrame = UINT64_MAX;
        return;
    }

    float partialTick = ci->minecraft->timer ? std::clamp(ci->minecraft->timer->alpha, 0.f, 1.f) : 1.f;
    Vec3 currentPos = target->getPos();
    Vec3 oldPos = target->getPosOld();
    Vec3 interpolatedPos {
        std::lerp(oldPos.x, currentPos.x, partialTick),
        std::lerp(oldPos.y, currentPos.y, partialTick),
        std::lerp(oldPos.z, currentPos.z, partialTick),
    };

    AABB bounds = target->getBoundingBox();
    Vec3 offset = interpolatedPos - currentPos;
    bounds.lower = bounds.lower + offset;
    bounds.higher = bounds.higher + offset;

    bool usingGhost = false;
    if (wantGhost) {
        bounds = selectedGhost;
        usingGhost = true;
    }

    Vec3 localPos = lp->getPos();
    Vec3 localOld = lp->getPosOld();
    Vec3 interpolatedLocalPos {
        std::lerp(localOld.x, localPos.x, partialTick),
        std::lerp(localOld.y, localPos.y, partialTick),
        std::lerp(localOld.z, localPos.z, partialTick),
    };
    Vec3 eye = poseAwareEye(lp, interpolatedLocalPos);

    Vec3 aimPoint = boxPoint(bounds, aimFrac);
    Vec3 direction = aimPoint - eye;
    float distance = direction.magnitude();
    if (distance < 0.01f) return;
    direction = direction * (1.f / distance);

    Vec2 desired {
        -std::asin(std::clamp(direction.y, -1.f, 1.f)) * (180.f / pi_f),
        std::atan2(direction.z, direction.x) * (180.f / pi_f) - 90.f,
    };
    desired.x = std::clamp(desired.x, -89.9f, 89.9f);
    desired.y = wrapAngle(desired.y);

    if (aimMode.getSelectedKey() == aim_psilent) {
        psilentActive = true;
        // Publish for the game thread. bounds/aimPoint here are already the
        // interpolated ones used for the shot, and ghosts are never valid PSilent
        // targets because the touch data has to match the live model.
        publishPSilentLock(targetId, bounds, aimPoint, !usingGhost);
        commandActive = false;
        silentRotActive = false;
        lastFrame = {};
        lastCorrectionFrame = UINT64_MAX;
        return;
    }
    psilentActive = false;
    publishPSilentLock(0, AABB {}, Vec3 {}, false);

    bool silentMode = aimMode.getSelectedKey() == aim_silent;

    if (!freelookResolved) {
        auto mod = Necromancer::getModuleManager().find("Freelook");
        freelookModule = mod ? static_cast<Freelook*>(mod.get()) : nullptr;
        freelookResolved = true;
    }

    Freelook* freelook = (freelookModule && freelookModule->isEnabled()) ? freelookModule : nullptr;

    uint64_t frameId = RenderFrameState::get().captureCount();
    if (frameId == lastCorrectionFrame) return;
    lastCorrectionFrame = frameId;

    auto now = std::chrono::steady_clock::now();
    bool hadPreviousFrame = lastFrame != std::chrono::steady_clock::time_point {};
    float dt = hadPreviousFrame
        ? std::clamp(std::chrono::duration<float>(now - lastFrame).count(), 0.001f, 0.25f)
        : 0.f;
    lastFrame = now;

    if (!commandActive) {
        commandedRot = silentMode ? Vec2 { desired.x, wrapAngle(desired.y) }
                                  : (freelook ? freelook->getPinnedRot() : lp->getRot());
        commandActive = true;
        if (silentMode) {
            std::lock_guard lock { aimMutex };
            desiredSilentRot = commandedRot;
        }
    }

    Vec2 error { desired.x - commandedRot.x, wrapAngle(desired.y - commandedRot.y) };
    float errorMagnitude = std::hypot(error.x, error.y);
    bool gripping = std::get<BoolValue>(lockOn) && errorMagnitude <= 5.f;
    float response = std::clamp(std::get<FloatValue>(smoothSpeed).value, 0.5f, 100.f);

    if (silentMode) {
        // Silent never touches the local camera, so the turn-delta injector below
        // is skipped entirely; only the server-facing rotation state advances.
        if (std::get<BoolValue>(silentRotSmooth)) {
            if (response < snapSpeedThreshold && !gripping && errorMagnitude <= 0.3f) {
                silentRotActive = true;
                return;
            }
            if (!hadPreviousFrame) return;

            float alpha = response >= snapSpeedThreshold
                ? std::lerp(1.f - std::exp(-response * dt), 1.f,
                            std::clamp((response - snapSpeedThreshold) / (100.f - snapSpeedThreshold), 0.f, 1.f))
                : 1.f - std::exp(-response * dt);
            Vec2 correction { error.x * alpha, error.y * alpha };
            commandedRot.x = std::clamp(commandedRot.x + correction.x, -89.9f, 89.9f);
            commandedRot.y = wrapAngle(commandedRot.y + correction.y);
        } else {
            commandedRot = Vec2 { desired.x, wrapAngle(desired.y) };
        }

        silentRotActive = true;
        std::lock_guard lock { aimMutex };
        desiredSilentRot = commandedRot;
        return;
    }

    if (response < snapSpeedThreshold && !gripping && errorMagnitude <= 0.3f) return;

    if (!hadPreviousFrame) return;

    float input = userInput.load(std::memory_order_acquire);
    input *= std::exp(-10.f * dt);
    userInput.store(input, std::memory_order_release);

    float alpha;
    if (response >= snapSpeedThreshold) {
        float snapBlend = std::clamp((response - snapSpeedThreshold) / (100.f - snapSpeedThreshold), 0.f, 1.f);
        float eased = 1.f - std::exp(-response * dt);
        alpha = std::lerp(eased, 1.f, snapBlend);
    } else {
        alpha = 1.f - std::exp(-response * dt);
    }
    float assist = gripping || response >= snapSpeedThreshold ? 1.f : 1.f - std::clamp(input, 0.f, 1.f) * 0.85f;
    float blend = std::clamp(alpha * assist, 0.f, 1.f);
    Vec2 correction { error.x * blend, error.y * blend };

    if (std::abs(correction.x) < 0.001f && std::abs(correction.y) < 0.001f) return;

    commandedRot.x = std::clamp(commandedRot.x + correction.x, -89.9f, 89.9f);
    commandedRot.y = wrapAngle(commandedRot.y + correction.y);

    if (freelook) {
        freelook->setPinnedRot(commandedRot);
        return;
    }

    pendingTurnDelta = Vec2 { -correction.x, correction.y };
    injectingTurn.store(true, std::memory_order_release);
    lp->applyTurnDelta(pendingTurnDelta);
    injectingTurn.store(false, std::memory_order_release);
}

void Aimbot::onTurnDelta(Event& evGeneric) {
    std::lock_guard controllerLock { controllerMutex };
    auto& ev = reinterpret_cast<TurnDeltaEvent&>(evGeneric);
    Vec2& delta = ev.getDelta();

    if (injectingTurn.load(std::memory_order_acquire)) {
        delta = pendingTurnDelta;
        return;
    }

    float magnitude = std::hypot(delta.x, delta.y);
    if (magnitude < 0.02f) return;

    if (commandActive) {
        commandedRot.x = std::clamp(commandedRot.x - delta.x, -89.9f, 89.9f);
        commandedRot.y = wrapAngle(commandedRot.y + delta.y);
    }

    userInput.store(std::clamp(magnitude * 2.f, 0.25f, 1.f), std::memory_order_release);
}

void Aimbot::onCinematicCamera(Event& evGeneric) {
    std::lock_guard controllerLock { controllerMutex };
    if (!injectingTurn.load(std::memory_order_acquire)) return;
    reinterpret_cast<CinematicCameraEvent&>(evGeneric).setValue(false);
}

void Aimbot::onClick(Event& evGeneric) {
    auto& ev = reinterpret_cast<ClickEvent&>(evGeneric);
    if (ev.getClickType() != ClickEvent::ClickType::Left || !ev.isDown()) return;

    std::lock_guard controllerLock { controllerMutex };
    bool allowWallAttack = std::get<BoolValue>(hitBehindWall);
    bool psilent = aimMode.getSelectedKey() == aim_psilent && psilentActive;
    bool redirectGhost = currentTargetGhost;
    bool redirectWall = allowWallAttack && currentTargetObstructed;
    if (!currentTargetId || (!redirectGhost && !redirectWall && !psilent) || pendingAttack.active) return;
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) return;

    auto ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraftGame || !ci->minecraftGame->isCursorGrabbed() ||
        Necromancer::get().getScreenManager().getActiveScreen())
        return;

    auto* target = EntityCache::get().findByRuntimeID(currentTargetId);
    if (!target) return;

    pendingAttack.runtimeID = currentTargetId;
    pendingAttack.box = currentTargetBox;
    pendingAttack.hitPoint = boxPoint(currentTargetBox, currentAimFrac);
    pendingAttack.recordAgeMs = currentTargetRecordAgeMs;
    pendingAttack.ghost = currentTargetGhost;
    pendingAttack.active = true;
    ev.setCancelled(true);
}

void Aimbot::onAfterMove(Event&) {
    PendingAttack attack {};
    {
        std::lock_guard controllerLock { controllerMutex };
        if (!pendingAttack.active) return;
        attack = pendingAttack;
        pendingAttack = {};
        partBurst.clear();
    }

    auto ci = SDK::ClientInstance::get();
    auto* lp = ci ? ci->getLocalPlayer() : nullptr;
    auto* target = EntityCache::get().findByRuntimeID(attack.runtimeID);
    if (!lp || !lp->gameMode || !target) return;
    if (auto hp = target->getHealth(); !hp || *hp <= 0.f) return;

    if (attack.ghost) {
        auto* bt = resolveBacktrack();
        if (!bt || !bt->queueGhostAttack(attack.runtimeID, attack.box, attack.hitPoint, attack.recordAgeMs)) return;
    } else {
        if (!Signatures::GameMode_attack.result) return;
        if (auto* bt = resolveBacktrack()) bt->allowDirectAttack(attack.runtimeID);
        using GameModeAttackFn = __int64 (*)(void*, SDK::Actor*, char, Vec3*);
        Vec3 clickPos = attack.hitPoint;
        reinterpret_cast<GameModeAttackFn>(Signatures::GameMode_attack.result)(lp->gameMode, target, 0, &clickPos);
    }

    TargetManager::setTarget(target);
}

namespace {
    float wrapYaw(float a) {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    }

    Vec2 rotationTo(Vec3 const& from, Vec3 const& to) {
        Vec3 dir = to - from;
        float len = dir.magnitude();
        if (len < 0.0001f) return Vec2 { 0.f, 0.f };
        dir = dir * (1.f / len);
        float pitch = std::clamp(-std::asin(std::clamp(dir.y, -1.f, 1.f)) * (180.f / pi_f), -89.9f, 89.9f);
        float yaw = wrapYaw(std::atan2(dir.z, dir.x) * (180.f / pi_f) - 90.f);
        return Vec2 { pitch, yaw };
    }
}

void Aimbot::publishPSilentLock(uint64_t runtimeID, AABB const& box, Vec3 const& hitPoint, bool valid) {
    uint32_t start = psilentSeq.load(std::memory_order_relaxed);
    psilentSeq.store(start + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    psilentShared.runtimeID = runtimeID;
    psilentShared.box = box;
    psilentShared.hitPoint = hitPoint;
    psilentShared.valid = valid;
    std::atomic_thread_fence(std::memory_order_release);
    psilentSeq.store(start + 2, std::memory_order_release);
}

bool Aimbot::loadPSilentLock(PSilentLock& out) const {
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = psilentSeq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        out = psilentShared;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (psilentSeq.load(std::memory_order_acquire) == before) return out.valid;
    }
    return false;
}

bool Aimbot::observeAttack(SDK::Packet* packet, uint64_t& outTarget) {
    if (!packet || packet->getID() != SDK::PacketID::PLAYER_AUTH_INPUT) return false;

    auto base = reinterpret_cast<uintptr_t>(packet);
    using AuthInput = Signatures::FieldOffset::PlayerAuthInputPacket;

    auto txn = *reinterpret_cast<uintptr_t*>(base + AuthInput::itemUseTransaction);
    if (!txn) return false;
    using Txn = Signatures::FieldOffset::ItemUseTransaction;
    if (*reinterpret_cast<uint32_t*>(txn + Txn::actionType) != Txn::actionAttack) return false;

    outTarget = *reinterpret_cast<uint64_t*>(txn + Txn::targetRuntimeId);
    return outTarget != 0;
}

void Aimbot::queuePartBurst(uint64_t runtimeID, AABB const& box, Vec3 const& aimPoint, float recordAgeMs,
                            bool ghost) {
    auto ci = SDK::ClientInstance::get();
    auto* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp) return;

    Vec3 eye = poseAwareEye(lp, lp->getPos());
    if (eye.distance(aimPoint) > 8.f) return;

    auto* target = EntityCache::get().findByRuntimeID(runtimeID);
    if (!target) return;
    if (auto hp = target->getHealth(); !hp || *hp <= 0.f) return;

    int startSlot = nextBurstSlot;
    for (int i = 0; i < anchorCount; i++) {
        int slot = (startSlot + i) % anchorCount;
        Vec3 frac = fracForSlot(slot);
        Vec3 point = boxPoint(box, frac);
        if ((point - aimPoint).magnitude() < 0.001f) continue;

        PartBurst burst;
        burst.runtimeID = runtimeID;
        burst.box = box;
        burst.hitPoint = boxPoint(box, frac);
        burst.recordAgeMs = recordAgeMs;
        burst.ghost = ghost;
        burst.fireAt = burstClock + i + 1;
        burst.live = true;
        partBurst.push_back(std::move(burst));
    }
    nextBurstSlot = (startSlot + anchorCount) % anchorCount;
    while (partBurst.size() > 48) partBurst.pop_front();
}

void Aimbot::processPartBurst() {
    if (partBurst.empty()) return;

    burstClock++;

    auto ci = SDK::ClientInstance::get();
    auto* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->gameMode) {
        partBurst.clear();
        return;
    }

    auto* bt = resolveBacktrack();
    std::optional<Vec2> armedDir;

    for (auto it = partBurst.begin(); it != partBurst.end();) {
        if (it->fireAt > burstClock) {
            ++it;
            continue;
        }

        auto* target = EntityCache::get().findByRuntimeID(it->runtimeID);
        if (!target || !target->aabbShape) {
            it = partBurst.erase(it);
            continue;
        }
        if (auto hp = target->getHealth(); !hp || *hp <= 0.f) {
            it = partBurst.erase(it);
            continue;
        }

        Vec3 clickPos = it->hitPoint;

        if (it->ghost) {
            if (bt && bt->queueGhostAttack(it->runtimeID, it->box, clickPos, it->recordAgeMs, true)) {
                it = partBurst.erase(it);
                continue;
            }
            it = partBurst.erase(it);
            continue;
        }

        if (!Signatures::GameMode_attack.result) {
            it = partBurst.erase(it);
            continue;
        }

        if (bt) bt->allowDirectAttack(it->runtimeID);

        Vec3 eye = poseAwareEye(lp, lp->getPos());
        auto* input = lp->getMoveInputComponent();
        if (input && !armedDir) {
            Vec3 dir = clickPos - eye;
            float len = dir.magnitude();
            if (len > 0.0001f) {
                dir = dir * (1.f / len);
                float pitch = std::clamp(-std::asin(std::clamp(dir.y, -1.f, 1.f)) * (180.f / pi_f), -89.9f, 89.9f);
                float yaw = std::atan2(dir.z, dir.x) * (180.f / pi_f) - 90.f;
                while (yaw > 180.f) yaw -= 360.f;
                while (yaw < -180.f) yaw += 360.f;
                armedDir = Vec2 { pitch, yaw };
                input->interactDir = *armedDir;
            }
        }

        using GameModeAttackFn = __int64 (*)(void*, SDK::Actor*, char, Vec3*);
        reinterpret_cast<GameModeAttackFn>(Signatures::GameMode_attack.result)(lp->gameMode, target, 0, &clickPos);

        it = partBurst.erase(it);
    }
}

void Aimbot::onBeforeMove(Event& evGeneric) {
    if (aimMode.getSelectedKey() != aim_psilent) return;

    auto& ev = reinterpret_cast<BeforeMoveEvent&>(evGeneric);
    SDK::MoveInputComponent* input = ev.getMoveInputHandler();
    if (!input) return;

    PSilentLock lock {};
    if (!loadPSilentLock(lock)) {
        psilentTouchArmed.store(false, std::memory_order_release);
        return;
    }
    Vec3 hitPoint = lock.hitPoint;

    auto ci = SDK::ClientInstance::get();
    auto* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp) {
        psilentTouchArmed.store(false, std::memory_order_release);
        return;
    }

    Vec2 touchDir = rotationTo(poseAwareEye(lp, lp->getPos()), hitPoint);
    input->interactDir = touchDir;
    psilentTouchDir = touchDir;
    psilentTouchArmed.store(true, std::memory_order_release);
}

void Aimbot::onSendPacket(Event& evGeneric) {
    auto& ev = reinterpret_cast<SendPacketEvent&>(evGeneric);
    auto* packet = ev.getPacket();
    if (!packet || ev.isCancelled()) return;

    int mode = aimMode.getSelectedKey();
    if (mode == aim_normal || mode == aim_silent) {
        if (std::get<BoolValue>(allBodyParts).value) {
            uint64_t attackTarget = 0;
            if (observeAttack(packet, attackTarget)) {
                queuePartBurst(attackTarget, currentTargetBox, boxPoint(currentTargetBox, currentAimFrac),
                               currentTargetRecordAgeMs, currentTargetGhost);
            }
        }
    }

    if (packet->getID() != SDK::PacketID::PLAYER_AUTH_INPUT) return;

    if (aimMode.getSelectedKey() == aim_silent) {
        if (silentRotActive) {
            Vec2 silentRot {};
            {
                std::lock_guard lock { aimMutex };
                silentRot = desiredSilentRot;
            }
            auto base = reinterpret_cast<uintptr_t>(packet);
            using AuthInput = Signatures::FieldOffset::PlayerAuthInputPacket;
            auto* rot = reinterpret_cast<float*>(base + AuthInput::rot);
            rot[0] = silentRot.x;
            rot[1] = silentRot.y;
            *reinterpret_cast<float*>(base + AuthInput::yHeadRot) = silentRot.y;
        }
        return;
    }

    if (aimMode.getSelectedKey() != aim_psilent) return;
    if (!psilentTouchArmed.load(std::memory_order_acquire)) return;

    PSilentLock lock {};
    if (!loadPSilentLock(lock)) return;
    uint64_t lockId = lock.runtimeID;
    Vec3 hitPoint = lock.hitPoint;

    auto base = reinterpret_cast<uintptr_t>(packet);

    using AuthInput = Signatures::FieldOffset::PlayerAuthInputPacket;
    using Txn = Signatures::FieldOffset::ItemUseTransaction;

    auto txn = *reinterpret_cast<uintptr_t*>(base + AuthInput::itemUseTransaction);
    if (!txn) return;
    if (*reinterpret_cast<uint32_t*>(txn + Txn::actionType) != Txn::actionAttack) return;
    if (*reinterpret_cast<uint64_t*>(txn + Txn::targetRuntimeId) != lockId) return;

    auto ci = SDK::ClientInstance::get();
    auto* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp) return;

    if (std::get<BoolValue>(psilentSprintHits).value) {
        auto* input = lp->getMoveInputComponent();
        bool sprinting = input && (input->sprinting || input->rawInputState.sprintDown || input->inputState.sprintDown);
        if (sprinting) {
            constexpr uint64_t sprintDownBit = 1ull << 4;
            constexpr uint64_t sprintingBit = 1ull << 20;
            constexpr uint64_t startSprintBit = 1ull << 25;
            constexpr uint64_t stopSprintBit = 1ull << 26;
            auto* inputData = reinterpret_cast<uint64_t*>(base + AuthInput::inputData);
            *inputData |= sprintDownBit | sprintingBit | startSprintBit;
            *inputData &= ~stopSprintBit;
        }
    }

    Vec3 eye = poseAwareEye(lp, lp->getPos());

    auto* interact = reinterpret_cast<float*>(base + AuthInput::interactRotation);
    interact[0] = psilentTouchDir.x;
    interact[1] = psilentTouchDir.y;

    auto* fromPos = reinterpret_cast<float*>(txn + Txn::fromPos);
    fromPos[0] = eye.x;
    fromPos[1] = eye.y;
    fromPos[2] = eye.z;
    auto* clickPos = reinterpret_cast<float*>(txn + Txn::clickPos);
    clickPos[0] = hitPoint.x;
    clickPos[1] = hitPoint.y;
    clickPos[2] = hitPoint.z;

    if (std::get<BoolValue>(allBodyParts).value) {
        queuePartBurst(lockId, lock.box, hitPoint, -1.f, false);
    }
}

bool Aimbot::isPSilent() {
    return isEnabled() && aimMode.getSelectedKey() == aim_psilent;
}

void Aimbot::onAfterMovePartBurst(Event&) {
    processPartBurst();
}

bool Aimbot::getPSilentLock(uint64_t& outRuntimeID, AABB& outBox, Vec3& outHitPoint) {
    if (!isPSilent()) return false;
    PSilentLock lock {};
    if (!loadPSilentLock(lock)) return false;
    outRuntimeID = lock.runtimeID;
    outBox = lock.box;
    outHitPoint = lock.hitPoint;
    return true;
}

void Aimbot::onRenderOverlay(RenderOverlayEvent& ev) {
    if (targetMode.getSelectedKey() != 1) return;

    auto ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraftGame || !ci->minecraftGame->gameRenderer || !ci->minecraftGame->isCursorGrabbed() ||
        Necromancer::get().getScreenManager().getActiveScreen())
        return;

    auto* ctx = ev.getDeviceContext();
    if (!ctx) return;

    auto pixelSize = ctx->GetPixelSize();
    if (pixelSize.width == 0 || pixelSize.height == 0) return;

    float halfAngle = std::clamp(std::get<FloatValue>(fov).value, 0.f, 180.f);
    if (halfAngle < 0.01f) return;

    auto* gameRenderer = ci->minecraftGame->gameRenderer;
    float focal = gameRenderer->lastProjectionMatrix._m[1][1];
    if (!std::isfinite(focal) || focal <= 0.001f) return;

    float radius;
    if (halfAngle >= 89.9f) {
        radius = static_cast<float>(std::max(pixelSize.width, pixelSize.height));
    } else {
        radius = focal * std::tan(halfAngle * (pi_f / 180.f)) * static_cast<float>(pixelSize.height) * 0.5f;
    }
    if (!std::isfinite(radius) || radius < 1.f) return;

    float maxRadius = static_cast<float>(std::max(pixelSize.width, pixelSize.height));
    radius = std::min(radius, maxRadius);

    if (!ringBrush) {
        if (FAILED(ctx->CreateSolidColorBrush({ 1.f, 1.f, 1.f, 1.f }, ringBrush.GetAddressOf()))) return;
    }

    ringBrush->SetColor(d2d::Color(std::get<ColorValue>(fovColor).getMainColor()).get());
    D2D1_ELLIPSE ellipse { { static_cast<float>(pixelSize.width) * 0.5f, static_cast<float>(pixelSize.height) * 0.5f },
                           radius, radius };
    ctx->DrawEllipse(ellipse, ringBrush.Get(), std::get<FloatValue>(fovWidth).value);
}

void Aimbot::onRendererCleanup(Event&) {
    ringBrush = nullptr;
}
