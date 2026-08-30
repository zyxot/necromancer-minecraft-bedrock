#include "pch.h"
#include "NoSlowDown.h"
#include "client/event/events/TickEvent.h"
#include "mc/Addresses.h"
#include <Windows.h>

namespace {
    constexpr uint8_t webStub[1] = { 0xC3 };
    constexpr uint8_t itemStub[8] = { 0x41, 0xC7, 0x01, 0x00, 0x00, 0x00, 0x00, 0xC3 };
}

NoSlowDown::NoSlowDown()
    : Module("NoSlowDown", LocalizeString::get("client.module.noSlowDown.name"),
             LocalizeString::get("client.module.noSlowDown.desc"), GAME, nokeybind) {
    addSetting("web", LocalizeString::get("client.module.noSlowDown.web.name"),
               LocalizeString::get("client.module.noSlowDown.web.desc"), web);
    addSetting("itemUse", LocalizeString::get("client.module.noSlowDown.itemUse.name"),
               LocalizeString::get("client.module.noSlowDown.itemUse.desc"), itemUse);

    listen<TickEvent>(static_cast<EventListenerFunc>(&NoSlowDown::onTick));
}

bool NoSlowDown::patchBytes(uintptr_t address, uint8_t const* bytes, size_t len) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(reinterpret_cast<void*>(address), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(address), len, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), len);
    return true;
}

void NoSlowDown::updatePatches(bool active) {
    uintptr_t webFn = Signatures::BlockMovementSlowdown_apply.result;
    uintptr_t itemFn = Signatures::ItemUseSlowdown_apply.result;

    bool wantWeb = std::get<BoolValue>(web).value && active;
    bool wantItem = std::get<BoolValue>(itemUse).value && active;

    if (webFn != 0 && wantWeb != webPatched) {
        if (wantWeb) {
            memcpy(webOriginal.data(), reinterpret_cast<void const*>(webFn), sizeof(webStub));
            if (patchBytes(webFn, webStub, sizeof(webStub))) webPatched = true;
        } else {
            patchBytes(webFn, webOriginal.data(), sizeof(webStub));
            webPatched = false;
        }
    }
    if (itemFn != 0 && wantItem != itemPatched) {
        if (wantItem) {
            memcpy(itemOriginal.data(), reinterpret_cast<void const*>(itemFn), sizeof(itemStub));
            if (patchBytes(itemFn, itemStub, sizeof(itemStub))) itemPatched = true;
        } else {
            patchBytes(itemFn, itemOriginal.data(), sizeof(itemStub));
            itemPatched = false;
        }
    }
}

void NoSlowDown::onTick(Event&) {
    updatePatches(true);
}

void NoSlowDown::onEnable() {
    updatePatches(true);
}

void NoSlowDown::onDisable() {
    updatePatches(false);
}
