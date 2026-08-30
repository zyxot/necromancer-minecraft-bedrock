#include "pch.h"
#include "Freecam.h"

#include <client/event/events/BeforeMoveEvent.h>
#include <client/event/events/BuildBlockEvent.h>
#include <client/event/events/ClickEvent.h>
#include <client/event/events/LeaveGameEvent.h>
#include <client/event/events/PacketReceiveEvent.h>
#include <client/event/events/RenderLevelEvent.h>
#include <client/event/events/SendPacketEvent.h>
#include <client/event/events/TickEvent.h>
#include <client/event/events/UpdateEvent.h>
#include <client/feature/module/modules/visual/AntiObs.h>
#include <client/input/Keyboard.h>
#include <client/screen/ScreenManager.h>
#include <mc/common/client/game/ClientInstance.h>
#include <mc/common/client/game/MinecraftGame.h>
#include <mc/common/client/player/LocalPlayer.h>
#include <mc/common/network/Packet.h>
#include <mc/Addresses.h>
#include <util/DrawUtil3D.h>
#include <util/Util.h>

namespace {
    int mappedKey(const char* name, int fallback) {
        int mapped = Necromancer::getKeyboard().getMappedKey(name);
        return mapped == 0 ? fallback : mapped;
    }
}

Freecam::Freecam()
    : Module("Freecam", LocalizeString::get("client.module.freecam.name"),
             LocalizeString::get("client.module.freecam.desc"), GAME) {
    addSliderSetting("speed", LocalizeString::get("client.module.freecam.speed.name"),
                     LocalizeString::get("client.module.freecam.speed.desc"), speed, FloatValue(0.1f),
                     FloatValue(10.f), FloatValue(0.1f));

    addSetting("blockInteractions", LocalizeString::get("client.module.freecam.blockInteractions.name"),
               LocalizeString::get("client.module.freecam.blockInteractions.desc"), blockInteractions);

    listen<TickEvent>((EventListenerFunc)&Freecam::onTick);
    listen<UpdateEvent>((EventListenerFunc)&Freecam::onUpdate);
    listen<BeforeMoveEvent>((EventListenerFunc)&Freecam::onBeforeMove);
    listen<SendPacketEvent>((EventListenerFunc)&Freecam::onSendPacket, false, -100);
    listen<ClickEvent>((EventListenerFunc)&Freecam::onClick);
    listen<BuildBlockEvent>((EventListenerFunc)&Freecam::onBuildBlock);
    listen<RenderLevelEvent>((EventListenerFunc)&Freecam::onRenderLevel);
    listen<PacketReceiveEvent>((EventListenerFunc)&Freecam::onPacketReceive);
    listen<LeaveGameEvent>((EventListenerFunc)&Freecam::onLeaveGame, true);
}

void Freecam::onEnable() {
    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->stateVector) {
        setEnabled(false);
        return;
    }

    restorePos = lp->stateVector->pos;
    restorePosOld = lp->stateVector->posOld;
    restoreVelocity = lp->stateVector->velocity;
    restoreRot = lp->getRot();

    hadBox = lp->aabbShape != nullptr;
    if (hadBox) {
        restoreBox = lp->aabbShape->boundingBox;
        boxAnchor = restoreBox.lower - restorePos;
    }

    smoothVel = {};

    active = true;

    if (auto fakelag = Necromancer::get().getModuleManager().find("Fakelag")) {
        fakelagWasEnabled = fakelag->isEnabled();
        fakelag->setBlocked(true);
    }

    Necromancer::getNotifications().push(
        L"Freecam on: your position is held on the server, movement keeps flowing");
}

void Freecam::onDisable() {
    teardown(true);
}

void Freecam::teardown(bool putBack) {
    if (putBack) restore();

    if (auto fakelag = Necromancer::get().getModuleManager().find("Fakelag")) {
        fakelag->setBlocked(false);
        if (fakelagWasEnabled) fakelag->setEnabled(true);
    }

    fakelagWasEnabled = false;
    active = false;
    hadBox = false;
}

void Freecam::onLeaveGame(Event&) {
    if (!active) return;

    teardown(false);
}

void Freecam::restore() {
    if (!active) return;

    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->stateVector) return;

    lp->stateVector->pos = restorePos;
    lp->stateVector->posOld = restorePosOld;
    lp->stateVector->velocity = restoreVelocity;
    lp->getRot() = restoreRot;

    if (lp->actorRotation) {
        lp->actorRotation->rotation = restoreRot;
        lp->actorRotation->rotationOld = restoreRot;
    }

    if (hadBox && lp->aabbShape) lp->aabbShape->boundingBox = restoreBox;
}

void Freecam::onTick(Event&) {}

void Freecam::onUpdate(Event&) {
    if (!active) return;

    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->stateVector) return;

    Vec3 cur = lp->stateVector->pos;

    const bool inUI = Necromancer::get().getScreenManager().getActiveScreen()
                   || (ci->minecraftGame && !ci->minecraftGame->isCursorGrabbed());

    float forward = 0.f;
    float strafe = 0.f;
    float vertical = 0.f;

    if (!inUI) {
        auto& keyboard = Necromancer::getKeyboard();
        if (keyboard.isKeyDownHardware(mappedKey("forward", 'W'))) forward += 1.f;
        if (keyboard.isKeyDownHardware(mappedKey("back", 'S'))) forward -= 1.f;
        if (keyboard.isKeyDownHardware(mappedKey("right", 'D'))) strafe += 1.f;
        if (keyboard.isKeyDownHardware(mappedKey("left", 'A'))) strafe -= 1.f;
        if (keyboard.isKeyDownHardware(mappedKey("jump", VK_SPACE))) vertical += 1.f;
        if (keyboard.isKeyDownHardware(mappedKey("sneak", VK_SHIFT))) vertical -= 1.f;
    }

    const float yaw = (lp->getRot().y + 90.f) * (pi_f / 180.f);
    const float c = cosf(yaw);
    const float s = sinf(yaw);

    Vec3 wish { (forward * c) - (strafe * s), vertical, (forward * s) + (strafe * c) };

    const float horizontal = sqrtf((wish.x * wish.x) + (wish.z * wish.z));
    if (horizontal > 1.f) {
        wish.x /= horizontal;
        wish.z /= horizontal;
    }

    float dt = Necromancer::getRenderer().getDeltaTime() / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    const float k = 1.f - expf(-dt * 12.f);
    smoothVel.x += (wish.x - smoothVel.x) * k;
    smoothVel.y += (wish.y - smoothVel.y) * k;
    smoothVel.z += (wish.z - smoothVel.z) * k;

    const float blocksPerSec = std::get<FloatValue>(speed).value * 2.f;

    lp->stateVector->posOld = cur;
    cur.x += smoothVel.x * blocksPerSec * dt;
    cur.y += smoothVel.y * blocksPerSec * dt;
    cur.z += smoothVel.z * blocksPerSec * dt;
    lp->stateVector->pos = cur;
    lp->stateVector->velocity = {};

    if (lp->aabbShape && hadBox) {
        const float w = restoreBox.higher.x - restoreBox.lower.x;
        const float h = restoreBox.higher.y - restoreBox.lower.y;
        const float d = restoreBox.higher.z - restoreBox.lower.z;
        lp->aabbShape->boundingBox.lower = cur + boxAnchor;
        lp->aabbShape->boundingBox.higher = cur + boxAnchor + Vec3 { w, h, d };
    }
}

void Freecam::onBeforeMove(Event& evG) {
    if (!active) return;

    reinterpret_cast<BeforeMoveEvent&>(evG).setCancelled(true);
}

void Freecam::onSendPacket(Event& evG) {
    if (!active) return;

    auto& ev = reinterpret_cast<SendPacketEvent&>(evG);
    auto packet = ev.getPacket();
    if (!packet || ev.isCancelled()) return;

    if (packet->getID() != SDK::PacketID::PLAYER_AUTH_INPUT) return;

    auto base = reinterpret_cast<uintptr_t>(packet);
    using AuthInput = Signatures::FieldOffset::PlayerAuthInputPacket;

    auto transaction = *reinterpret_cast<uintptr_t*>(base + AuthInput::itemUseTransaction);
    auto stackRequest = *reinterpret_cast<uintptr_t*>(base + AuthInput::itemStackRequest);
    if (transaction || stackRequest) return;

    *reinterpret_cast<Vec3*>(base + AuthInput::pos) = restorePos;
    *reinterpret_cast<Vec3*>(base + AuthInput::posDelta) = {};
}

void Freecam::onPacketReceive(Event& evG) {
    if (!active) return;

    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    auto packet = ev.getPacket();
    if (!packet) return;

    auto id = packet->getID();
    if (id != SDK::PacketID::CORRECT_PLAYER_MOVE_PREDICTION && id != SDK::PacketID::MOVE_PLAYER) return;

    if (id == SDK::PacketID::MOVE_PLAYER) {
        auto ci = SDK::ClientInstance::get();
        auto lp = ci ? ci->getLocalPlayer() : nullptr;
        if (!lp) return;
        auto runtimeId =
            *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(packet) + Signatures::FieldOffset::MovePlayerPacket::runtimeId);
        if (runtimeId != lp->getRuntimeID()) return;
    }

    teardown(false);
    Necromancer::getNotifications().push(L"Freecam off: the server pulled you back, so it kept its position");
}

void Freecam::onClick(Event& evG) {
    if (!active || !std::get<BoolValue>(blockInteractions).value) return;

    auto& ev = reinterpret_cast<ClickEvent&>(evG);
    auto type = ev.getClickType();
    if (type == ClickEvent::ClickType::Left || type == ClickEvent::ClickType::Right) {
        ev.setCancelled(true);
    }
}

void Freecam::onBuildBlock(Event& evG) {
    if (!active || !std::get<BoolValue>(blockInteractions).value) return;

    reinterpret_cast<BuildBlockEvent&>(evG).setCancelled(true);
}

void Freecam::onRenderLevel(Event&) {
    if (!active || !hadBox) return;
    if (AntiObs::isActive()) return;

    auto ci = SDK::ClientInstance::get();
    if (!ci || !ci->levelRenderer || !SDK::ScreenContext::instance3d) return;

    d2d::Color color(0.35f, 0.9f, 1.f, 0.28f);
    d2d::Color outline(color);
    outline.a = 0.85f;

    MCDrawUtil3D dc(ci->levelRenderer, SDK::ScreenContext::instance3d,
                    SDK::MaterialPtr::getSelectionOverlayMaterial());
    dc.fillBox(restoreBox, color);
    dc.flush();
    dc.drawThickBox(restoreBox, 0.08f, outline);
    dc.flush();
}
