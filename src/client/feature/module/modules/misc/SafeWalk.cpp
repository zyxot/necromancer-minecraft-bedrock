#include "pch.h"
#include "SafeWalk.h"
#include "client/event/events/TickEvent.h"
#include "mc/Addresses.h"
#include <Windows.h>

namespace {
    // gateA ends in `jnz clamp` (75 xx). Turning it into `jmp` (EB xx) makes the clamp run
    // regardless of the sneak flag.
    constexpr size_t gateAJumpOffset = 6;
    constexpr uint8_t gateAForceJump[1] = { 0xEB };

    // gateB is `mov [rax+55h],r10b / test rcx,rcx / jz skip`. The jz is a 6-byte near jump
    // at +7; NOPing it lets execution fall into the clamp.
    constexpr size_t gateBJumpOffset = 7;
    constexpr uint8_t gateBNop[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
}

SafeWalk::SafeWalk()
    : Module("SafeWalk", LocalizeString::get("client.module.safeWalk.name"),
             LocalizeString::get("client.module.safeWalk.desc"), GAME, nokeybind) {
    listen<TickEvent>(static_cast<EventListenerFunc>(&SafeWalk::onTick));
}

bool SafeWalk::patchBytes(uintptr_t address, uint8_t const* bytes, size_t len) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(reinterpret_cast<void*>(address), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(address), len, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), len);
    return true;
}

void SafeWalk::updatePatches(bool want) {
    uintptr_t gateA = Signatures::SneakMovement_gateA.result;
    uintptr_t gateB = Signatures::SneakMovement_gateB.result;

    if (gateA != 0 && want != gateAPatched) {
        uintptr_t site = gateA + gateAJumpOffset;
        if (want) {
            memcpy(gateAOriginal.data(), reinterpret_cast<void const*>(site), sizeof(gateAForceJump));
            if (patchBytes(site, gateAForceJump, sizeof(gateAForceJump))) gateAPatched = true;
        } else {
            patchBytes(site, gateAOriginal.data(), sizeof(gateAForceJump));
            gateAPatched = false;
        }
    }

    if (gateB != 0 && want != gateBPatched) {
        uintptr_t site = gateB + gateBJumpOffset;
        if (want) {
            memcpy(gateBOriginal.data(), reinterpret_cast<void const*>(site), sizeof(gateBNop));
            if (patchBytes(site, gateBNop, sizeof(gateBNop))) gateBPatched = true;
        } else {
            patchBytes(site, gateBOriginal.data(), sizeof(gateBNop));
            gateBPatched = false;
        }
    }
}

void SafeWalk::onTick(Event&) {
    updatePatches(true);
}

void SafeWalk::onEnable() {
    updatePatches(true);
}

void SafeWalk::onDisable() {
    updatePatches(false);
}
