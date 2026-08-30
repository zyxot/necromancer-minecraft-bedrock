#pragma once
#include "client/feature/module/Module.h"
#include <array>
#include <cstdint>

class SafeWalk : public Module {
public:
    SafeWalk();

    void onEnable() override;
    void onDisable() override;
    void onTick(Event& evG);

private:
    std::array<uint8_t, 1> gateAOriginal {};
    std::array<uint8_t, 6> gateBOriginal {};
    bool gateAPatched = false;
    bool gateBPatched = false;

    static bool patchBytes(uintptr_t address, uint8_t const* bytes, size_t len);
    void updatePatches(bool want);
};
