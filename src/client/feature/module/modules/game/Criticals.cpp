#include "pch.h"
#include "Criticals.h"

#include <client/event/events/SendPacketEvent.h>
#include <mc/Addresses.h>
#include <mc/common/client/game/ClientInstance.h>
#include <mc/common/client/player/LocalPlayer.h>
#include <mc/common/world/actor/Actor.h>
#include <mc/common/network/Packet.h>

namespace {
    constexpr uint64_t jumpDownBit = 1ull << 3;
    constexpr uint64_t sprintDownBit = 1ull << 4;
    constexpr uint64_t jumpingBit = 1ull << 6;
    constexpr uint64_t wantUpBit = 1ull << 16;
    constexpr uint64_t sprintingBit = 1ull << 20;
    constexpr uint64_t startSprintBit = 1ull << 25;
    constexpr uint64_t stopSprintBit = 1ull << 26;

    constexpr float gravityTick1 = -0.07840000092983246f;
    constexpr float gravityTick2 = -0.1552319973707199f;
    constexpr float jumpOffsets[4] = { 0.f, 0.2000000476837159f, 0.12160003185271847f, 0.f };
    constexpr float miniOffsets[4] = { 0.f, 0.02f, 0.01f, 0.f };

    constexpr size_t offPosDeltaY = Signatures::FieldOffset::PlayerAuthInputPacket::posDeltaY;
    constexpr size_t offInputData = Signatures::FieldOffset::PlayerAuthInputPacket::inputData;
}

Criticals::Criticals()
    : Module("Criticals", LocalizeString::get("client.module.criticals.name"),
             LocalizeString::get("client.module.criticals.desc"), GAME, nokeybind) {
    addSetting("useVelocity", LocalizeString::get("client.module.criticals.useVelocity.name"),
               LocalizeString::get("client.module.criticals.useVelocity.desc"), useVelocity);
    addSetting("positionChange", LocalizeString::get("client.module.criticals.positionChange.name"),
               LocalizeString::get("client.module.criticals.positionChange.desc"), positionChange);
    Setting::Condition posCond(std::vector<Setting::SingleCond> {
        { "positionChange", { 1 }, false },
    });
    addSetting("biggerPositionChange", LocalizeString::get("client.module.criticals.biggerPositionChange.name"),
               LocalizeString::get("client.module.criticals.biggerPositionChange.desc"), biggerPositionChange, posCond);
    addSliderSetting("changePercent", LocalizeString::get("client.module.criticals.changePercent.name"),
                     LocalizeString::get("client.module.criticals.changePercent.desc"), changePercent, FloatValue(0.f),
                     FloatValue(2.f), FloatValue(0.01f), posCond);
    addSetting("sendJumping", LocalizeString::get("client.module.criticals.sendJumping.name"),
               LocalizeString::get("client.module.criticals.sendJumping.desc"), sendJumping);
    addSetting("disableSprint", LocalizeString::get("client.module.criticals.disableSprint.name"),
               LocalizeString::get("client.module.criticals.disableSprint.desc"), disableSprint);

    this->listen<SendPacketEvent>(&Criticals::onSendPacket);
}

void Criticals::onEnable() {
    animState = AnimState::Start;
}

void Criticals::onSendPacket(Event& evG) {
    auto& ev = reinterpret_cast<SendPacketEvent&>(evG);
    auto* packet = ev.getPacket();
    if (!packet || ev.isCancelled()) return;
    if (packet->getID() != SDK::PacketID::PLAYER_AUTH_INPUT) return;

    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !lp->stateVector) return;

    auto base = reinterpret_cast<uintptr_t>(packet);
    float* posDeltaY = reinterpret_cast<float*>(base + offPosDeltaY);
    uint64_t* inputData = reinterpret_cast<uint64_t*>(base + offInputData);

    if (std::get<BoolValue>(disableSprint).value) {
        *inputData &= ~(sprintDownBit | sprintingBit | startSprintBit | stopSprintBit);
    }

    if (fabsf(lp->stateVector->pos.y - lp->stateVector->posOld.y) > 0.0001f) return;

    bool sendJump = std::get<BoolValue>(sendJumping).value;
    bool useVel = std::get<BoolValue>(useVelocity).value;
    bool posChange = std::get<BoolValue>(positionChange).value;
    bool bigChange = std::get<BoolValue>(biggerPositionChange).value;
    float percent = std::get<FloatValue>(changePercent).value;

    if (sendJump) *inputData |= jumpDownBit | jumpingBit | wantUpBit;

    switch (animState) {
        case AnimState::Start:
            if (useVel) *posDeltaY = gravityTick1;
            animState = AnimState::MidAir;
            break;
        case AnimState::MidAir:
            if (posChange) *posDeltaY += (bigChange ? jumpOffsets[1] : miniOffsets[1]) * percent;
            if (useVel) *posDeltaY = gravityTick1;
            animState = AnimState::MidAir2;
            break;
        case AnimState::MidAir2:
            if (sendJump) *inputData &= ~(jumpDownBit | jumpingBit | wantUpBit);
            if (posChange) *posDeltaY += (bigChange ? jumpOffsets[2] : miniOffsets[2]) * percent;
            if (useVel) *posDeltaY = gravityTick2;
            animState = AnimState::Landing;
            break;
        case AnimState::Landing:
            if (useVel) *posDeltaY = gravityTick1;
            animState = AnimState::Finished;
            break;
        case AnimState::Finished:
            if (useVel) *posDeltaY = gravityTick1;
            animState = AnimState::Start;
            break;
    }
}
