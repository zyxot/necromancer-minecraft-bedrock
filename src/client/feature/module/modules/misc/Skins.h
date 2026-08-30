#pragma once
#include "../../Module.h"

class Skins : public Module {
public:
    Skins();

    void onEnable() override;
    void onDisable() override;
    bool isToggleable() override { return false; }
    bool showToggle() override { return false; }
    bool shouldPersistEnabled() override { return false; }
    void loadConfig(SettingGroup& resolvedGroup) override;
};
