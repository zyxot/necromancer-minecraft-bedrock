#include "pch.h"
#include "BoxMe.h"
#include "client/event/events/TickEvent.h"
#include "client/misc/BoxBuilder.h"
#include "client/misc/MovementSim.h"
#include "client/misc/SlotLease.h"
#include "client/Necromancer.h"
#include "client/screen/ScreenManager.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/level/BlockSource.h"
#include <cmath>

namespace {
    const std::string leaseOwner = "BoxMe";
}

BoxMe::BoxMe()
    : Module("BoxMe", LocalizeString::get("client.module.boxMe.name"),
             LocalizeString::get("client.module.boxMe.desc"), GAME, nokeybind) {
    addSetting("useAnyBlock", LocalizeString::get("client.module.boxMe.useAnyBlock.name"),
               LocalizeString::get("client.module.boxMe.useAnyBlock.desc"), useAnyBlock);
    addSetting("buildBackwards", LocalizeString::get("client.module.boxMe.buildBackwards.name"),
               LocalizeString::get("client.module.boxMe.buildBackwards.desc"), buildBackwards);
    addSetting("corners", LocalizeString::get("client.module.boxMe.corners.name"),
               LocalizeString::get("client.module.boxMe.corners.desc"), corners);
    addSetting("topBlock", LocalizeString::get("client.module.boxMe.topBlock.name"),
               LocalizeString::get("client.module.boxMe.topBlock.desc"), topBlock);
    addSetting("movementLead", LocalizeString::get("client.module.boxMe.movementLead.name"),
               LocalizeString::get("client.module.boxMe.movementLead.desc"), movementLead);
    addSliderSetting("distance", LocalizeString::get("client.module.boxMe.distance.name"),
                     LocalizeString::get("client.module.boxMe.distance.desc"), distance, FloatValue(1.f),
                     FloatValue(2.f), FloatValue(1.f));
    addSliderSetting("blocksPerTick", LocalizeString::get("client.module.boxMe.blocksPerTick.name"),
                     LocalizeString::get("client.module.boxMe.blocksPerTick.desc"), blocksPerTick, FloatValue(1.f),
                     FloatValue(8.f), FloatValue(1.f));
    addSetting("autoDisable", LocalizeString::get("client.module.boxMe.autoDisable.name"),
               LocalizeString::get("client.module.boxMe.autoDisable.desc"), autoDisable);

    listen<TickEvent>(static_cast<EventListenerFunc>(&BoxMe::onTick));
}

void BoxMe::onEnable() {
    idleTicks = 0;
}

void BoxMe::onDisable() {
    SlotLease::release(leaseOwner, true);
    idleTicks = 0;
}

void BoxMe::collectQueue(SDK::LocalPlayer* lp, SDK::BlockSource* region, std::vector<BoxBuilder::Cell>& out) const {
    if (!lp->aabbShape) return;
    AABB box = lp->aabbShape->boundingBox;

    // Building takes ticks. Project the ring onto where we are heading so a sprinting
    // self-box does not end up behind us by the time the last block lands.
    if (std::get<BoolValue>(movementLead).value) {
        Vec3 vel = lp->getVelocity();
        auto sim = MovementSim::predictForward(lp, vel, 2, false);
        if (sim.valid) box = sim.finalBox;
    }

    int dist = std::max(1, std::min(2, static_cast<int>(std::get<FloatValue>(distance).value)));
    auto fp = BoxBuilder::footprintOf(box);
    int inner = dist - 1;
    int x0 = fp.x0 - inner;
    int x1 = fp.x1 + inner;
    int z0 = fp.z0 - inner;
    int z1 = fp.z1 + inner;
    int feetY = fp.feetY;
    bool wantCorners = std::get<BoolValue>(corners).value;

    BoxBuilder::collectRing(region, x0, x1, z0, z1, feetY, 2, wantCorners, out);

    if (!std::get<BoolValue>(buildBackwards).value && !out.empty()) {
        Vec2 rot = lp->getRot();
        float yaw = (rot.y + 90.f) * (pi_f / 180.f);
        float dx = cosf(yaw);
        float dz = sinf(yaw);

        int backX = 0;
        int backZ = 0;
        if (fabsf(dx) >= fabsf(dz)) {
            backX = dx > 0.f ? x0 - 1 : x1 + 1;
        } else {
            backZ = dz > 0.f ? z0 - 1 : z1 + 1;
        }

        std::erase_if(out, [&](BoxBuilder::Cell const& cell) {
            if (backX != 0) return cell.x == backX && cell.z >= z0 && cell.z <= z1;
            return cell.z == backZ && cell.x >= x0 && cell.x <= x1;
        });
    }

    if (std::get<BoolValue>(topBlock).value && out.empty()) {
        BoxBuilder::collectRoof(region, x0, x1, z0, z1, feetY, 2, out);
    }
}

void BoxMe::onTick(Event&) {
    SlotLease::pump();

    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->gameMode) return;
    if (Necromancer::get().getScreenManager().getActiveScreen().has_value()) return;
    auto region = ci->getRegion();
    if (!region) return;

    std::vector<BoxBuilder::Cell> queue;
    collectQueue(lp, region, queue);

    if (queue.empty()) {
        SlotLease::release(leaseOwner, true);
        if (std::get<BoolValue>(autoDisable).value && ++idleTicks >= 10) setEnabled(false);
        return;
    }

    if (!SlotLease::acquire(leaseOwner, SlotLease::Priority::Building, 250)) return;

    idleTicks = 0;
    BoxBuilder::build(lp, region, queue, std::get<BoolValue>(useAnyBlock).value,
                      std::max(1, static_cast<int>(std::get<FloatValue>(blocksPerTick).value)));
}
