#pragma once
#include "client/feature/module/Module.h"
#include <array>
#include <cstdint>

class NoSlowDown : public Module {
public:
    NoSlowDown();

    void onEnable() override;
    void onDisable() override;
    void onTick(Event& evG);

private:
    ValueType web = BoolValue(true);
    ValueType itemUse = BoolValue(true);

    std::array<uint8_t, 8> webOriginal {};
    std::array<uint8_t, 8> itemOriginal {};
    bool webPatched = false;
    bool itemPatched = false;

    static bool patchBytes(uintptr_t address, uint8_t const* bytes, size_t len);
    void updatePatches(bool active);
};
