#include "pch.h"
#include "FastStop.h"
#include "client/event/events/TickEvent.h"
#include "client/misc/BlockSolid.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/level/BlockSource.h"
#include <cmath>

namespace {
    bool hasGroundUnder(SDK::ClientInstance* ci, SDK::LocalPlayer* lp) {
        if (!lp->aabbShape) return false;
        auto region = ci->getRegion();
        if (!region) return false;
        AABB box = lp->aabbShape->boundingBox;
        int y = static_cast<int>(std::floor(box.lower.y + 0.01f)) - 1;
        int x0 = static_cast<int>(std::floor(box.lower.x + 0.01f));
        int z0 = static_cast<int>(std::floor(box.lower.z + 0.01f));
        int x1 = static_cast<int>(std::floor(box.higher.x - 0.01f));
        int z1 = static_cast<int>(std::floor(box.higher.z - 0.01f));
        for (int x = x0; x <= x1; x++) {
            for (int z = z0; z <= z1; z++) {
                if (BlockSolid::isCollidable(region, x, y, z)) return true;
            }
        }
        return false;
    }
}

FastStop::FastStop()
    : Module("FastStop", LocalizeString::get("client.module.fastStop.name"),
             LocalizeString::get("client.module.fastStop.desc"), GAME, nokeybind) {
    addSetting("onlyOnGround", LocalizeString::get("client.module.fastStop.onlyOnGround.name"),
               LocalizeString::get("client.module.fastStop.onlyOnGround.desc"), onlyOnGround);

    listen<TickEvent>(static_cast<EventListenerFunc>(&FastStop::onTick));
}

void FastStop::onTick(Event&) {
    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->stateVector) {
        wasMoving = false;
        return;
    }

    auto* input = lp->getMoveInputComponent();
    if (!input) {
        wasMoving = false;
        return;
    }

    auto const& raw = input->rawInputState;
    bool moving = raw.up || raw.down || raw.left || raw.right || raw.upLeft || raw.upRight || raw.downLeft ||
                  raw.downRight;

    bool was = wasMoving;
    wasMoving = moving;

    if (!was || moving) return;
    if (std::get<BoolValue>(onlyOnGround).value && !hasGroundUnder(ci, lp)) return;

    lp->stateVector->velocity.x = 0.f;
    lp->stateVector->velocity.z = 0.f;
}

void FastStop::onDisable() {
    wasMoving = false;
}
