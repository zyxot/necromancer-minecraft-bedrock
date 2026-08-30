#include "pch.h"
#include "SpearSwap.h"

#include "client/misc/SlotLease.h"
#include "util/Logger.h"
#include <client/screen/ScreenManager.h>
#include <client/event/events/UpdateEvent.h>
#include <client/event/events/SendPacketEvent.h>
#include <client/event/events/PacketReceiveEvent.h>
#include <client/event/events/LeaveGameEvent.h>
#include <mc/Addresses.h>
#include <mc/common/client/game/ClientInstance.h>
#include <mc/common/client/game/MinecraftGame.h>
#include <mc/common/client/game/MouseDevice.h>
#include <mc/common/client/game/MouseAction.h>
#include <mc/common/client/player/LocalPlayer.h>
#include <mc/common/world/Minecraft.h>
#include <mc/common/world/actor/player/Player.h>
#include <mc/common/world/actor/player/PlayerInventory.h>
#include <mc/common/world/actor/player/Inventory.h>
#include <mc/common/world/ItemStack.h>

namespace {
    constexpr uint8_t playerStartItemCooldownPacketId = 176;
    constexpr int lungeEnchantId = 41;
    constexpr int hotbarSlots = 9;
    constexpr float minSwapGapMs = 150.f;
    constexpr float attackHoldMs = 100.f;
    constexpr float leasePadMs = 600.f;
    const std::string leaseOwner = "SpearSwap";
    char const* spearSuffix = "_spear";

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

    void pushLeftClick() {
        pushAction(1, true);
        pushAction(1, false);
    }

    bool isSpearItem(SDK::ItemStack* stack) {
        if (!stack) return false;
        auto* item = stack->getItem();
        if (!item) return false;
        std::string id = item->namespacedId.getString();
        return id == "minecraft:spear" || (id.size() > 13 && id.starts_with("minecraft:") && id.ends_with(spearSuffix));
    }
}

SpearSwap::SpearSwap()
    : Module("SpearSwap", LocalizeString::get("client.module.spearSwap.name"),
             LocalizeString::get("client.module.spearSwap.desc"), GAME, nokeybind) {
    addSetting("ignoreCooldown", LocalizeString::get("client.module.spearSwap.ignoreCooldown.name"),
               LocalizeString::get("client.module.spearSwap.ignoreCooldown.desc"), ignoreCooldown);

    addSliderSetting("delayBeforeSwitch", LocalizeString::get("client.module.spearSwap.delayBeforeSwitch.name"),
                     LocalizeString::get("client.module.spearSwap.delayBeforeSwitch.desc"), delayBeforeSwitch,
                     FloatValue(0.f), FloatValue(2000.f), FloatValue(25.f));
    addSliderSetting("switchBackDelay", LocalizeString::get("client.module.spearSwap.switchBackDelay.name"),
                     LocalizeString::get("client.module.spearSwap.switchBackDelay.desc"), switchBackDelay,
                     FloatValue(0.f), FloatValue(2000.f), FloatValue(25.f));

    this->listen<UpdateEvent>(&SpearSwap::onUpdate);
    this->listen<SendPacketEvent>(&SpearSwap::onSendPacket);
    this->listen<PacketReceiveEvent>(&SpearSwap::onPacketReceive);
    this->listen<LeaveGameEvent>(&SpearSwap::onLeaveGame);
}

void SpearSwap::onDisable() {
    auto ci = SDK::ClientInstance::get();
    finishPendingSwap(ci ? ci->getLocalPlayer() : nullptr);
    cooldowns.clear();
    lastSwapAt = {};
}

void SpearSwap::onLeaveGame(Event&) {
    clearPendingSwap();
    cooldowns.clear();
}

bool SpearSwap::isSpearWithLunge(SDK::ItemStack* stack) {
    if (!isSpearItem(stack)) return false;
    return stack->getEnchantValue(lungeEnchantId) > 0;
}

bool SpearSwap::isOnCooldownNow() {
    if (std::get<BoolValue>(ignoreCooldown)) return false;

    auto now = std::chrono::steady_clock::now();
    std::erase_if(cooldowns, [now](CooldownEntry const& e) { return now >= e.endsAt; });
    return !cooldowns.empty();
}

int SpearSwap::findSpearSlot(SDK::Player* lp) {
    if (!lp->supplies || !lp->supplies->inventory) return -1;
    for (int slot = 0; slot < hotbarSlots; ++slot) {
        if (isSpearWithLunge(lp->supplies->inventory->getItem(slot))) return slot;
    }
    return -1;
}

void SpearSwap::clearPendingSwap() {
    phase = Phase::Idle;
    savedSlot = -1;
    pendingSpearSlot = -1;
    switchedAt = {};
}void SpearSwap::finishPendingSwap(SDK::Player* lp) {
    if (lp && lp->supplies && savedSlot >= 0 && savedSlot < hotbarSlots) {
        lp->supplies->selectedSlot = savedSlot;
    }
    SlotLease::release(leaseOwner, false);
    clearPendingSwap();
}

void SpearSwap::onSendPacket(Event& evGeneric) {
    auto& ev = reinterpret_cast<SendPacketEvent&>(evGeneric);
    ingestCooldownPacket(ev.getPacket());
}

void SpearSwap::onPacketReceive(Event& evGeneric) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evGeneric);
    ingestCooldownPacket(ev.getPacket());
}

void SpearSwap::ingestCooldownPacket(SDK::Packet* packet) {
    if (!packet) return;
    if (static_cast<uint8_t>(packet->getID()) != playerStartItemCooldownPacketId) return;

    struct CooldownPacket {
        uint8_t pad0[0x30];
        std::string categoryName;
        uint32_t durationTicks;
    };

    auto* cd = reinterpret_cast<CooldownPacket*>(packet);
    if (cd->categoryName.empty()) return;

    auto now = std::chrono::steady_clock::now();
    auto endsAt = now + std::chrono::milliseconds(static_cast<long long>(cd->durationTicks) * 50);

    for (auto& entry : cooldowns) {
        if (entry.category == cd->categoryName) {
            entry.endsAt = endsAt;
            return;
        }
    }
    cooldowns.push_back({ cd->categoryName, endsAt });
}

void SpearSwap::onUpdate(Event&) {
    SlotLease::pump();

    auto ci = SDK::ClientInstance::get();
    auto lp = ci ? ci->getLocalPlayer() : nullptr;

    if (!ci || !ci->minecraft || !ci->minecraftGame || !ci->minecraftGame->isCursorGrabbed() ||
        Necromancer::get().getScreenManager().getActiveScreen()) {
        finishPendingSwap(lp);
        return;
    }

    if (!lp || !lp->supplies || !lp->supplies->inventory) {
        finishPendingSwap(lp);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    float preMs = std::clamp(std::get<FloatValue>(delayBeforeSwitch).value, 0.f, 2000.f);
    float backMs = std::clamp(std::get<FloatValue>(switchBackDelay).value, 0.f, 2000.f);

    if (phase == Phase::PreSwitch) {
        if (!SlotLease::ownedBy(leaseOwner)) {
            finishPendingSwap(lp);
            return;
        }
        SlotLease::acquire(leaseOwner, SlotLease::Priority::Throwing,
                           static_cast<int>(preMs + backMs + attackHoldMs + leasePadMs));
        if (std::chrono::duration<float, std::milli>(now - switchedAt).count() < preMs) return;

        auto held = lp->supplies->inventory->getItem(pendingSpearSlot);
        if (!isSpearWithLunge(held)) {
            Logger::Warn("[SpearSwap] spear vanished from slot {}", pendingSpearSlot);
            finishPendingSwap(lp);
            return;
        }
        lp->supplies->selectedSlot = pendingSpearSlot;
        switchedAt = now;
        phase = Phase::Swapping;
        return;
    }

    if (phase == Phase::Swapping) {
        if (!SlotLease::ownedBy(leaseOwner)) {
            finishPendingSwap(lp);
            return;
        }
        SlotLease::acquire(leaseOwner, SlotLease::Priority::Throwing,
                           static_cast<int>(backMs + attackHoldMs + leasePadMs));

        auto held = lp->supplies->inventory->getItem(pendingSpearSlot);
        if (!isSpearWithLunge(held)) {
            Logger::Warn("[SpearSwap] spear vanished from slot {}", pendingSpearSlot);
            finishPendingSwap(lp);
            return;
        }
        if (lp->supplies->selectedSlot != pendingSpearSlot) lp->supplies->selectedSlot = pendingSpearSlot;

        Logger::Info("[SpearSwap] attacking with spear in slot {}", pendingSpearSlot);
        pushLeftClick();
        switchedAt = now;
        phase = Phase::Clicked;
        return;
    }

    if (phase == Phase::Clicked) {
        if (!SlotLease::ownedBy(leaseOwner)) {
            finishPendingSwap(lp);
            return;
        }
        SlotLease::acquire(leaseOwner, SlotLease::Priority::Throwing,
                           static_cast<int>(attackHoldMs + backMs + leasePadMs));

        auto elapsed = std::chrono::duration<float, std::milli>(now - switchedAt).count();
        if (elapsed < attackHoldMs + backMs) return;

        finishPendingSwap(lp);
        lastSwapAt = now;
        return;
    }

    if (isOnCooldownNow()) return;

    if (lastSwapAt != std::chrono::steady_clock::time_point {} &&
        std::chrono::duration<float, std::milli>(now - lastSwapAt).count() < minSwapGapMs) {
        return;
    }

    int spearSlot = findSpearSlot(lp);
    if (spearSlot < 0) {
        static auto lastNoSpear = std::chrono::steady_clock::time_point {};
        auto nowLog = std::chrono::steady_clock::now();
        if (lastNoSpear == std::chrono::steady_clock::time_point {} ||
            nowLog - lastNoSpear > std::chrono::seconds(2)) {
            std::string dump;
            for (int i = 0; i < hotbarSlots; i++) {
                auto stack = lp->supplies->inventory->getItem(i);
                if (!stack || !stack->getItem()) {
                    dump += "<empty>; ";
                    continue;
                }
                dump += stack->getItem()->namespacedId.getString();
                dump += "(e41=" + std::to_string(stack->getEnchantValue(lungeEnchantId)) + "); ";
            }
            Logger::Info("[SpearSwap] no Lunge spear found. hotbar: {}", dump);
            lastNoSpear = nowLog;
        }
        return;
    }
    Logger::Info("[SpearSwap] starting swap to slot {}", spearSlot);

    int originalSlot = lp->supplies->selectedSlot;
    if (originalSlot < 0 || originalSlot >= hotbarSlots) originalSlot = spearSlot;

    if (!SlotLease::acquire(leaseOwner, SlotLease::Priority::Throwing,
                            static_cast<int>(preMs + backMs + attackHoldMs + leasePadMs))) {
        Logger::Info("[SpearSwap] lease busy, skipping");
        return;
    }

    phase = Phase::PreSwitch;
    savedSlot = originalSlot;
    pendingSpearSlot = spearSlot;
    switchedAt = now;
}
