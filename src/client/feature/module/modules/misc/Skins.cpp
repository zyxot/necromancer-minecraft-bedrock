#include "pch.h"
#include "Skins.h"

#include "client/screen/ScreenManager.h"
#include "client/screen/screens/SkinsScreen.h"

Skins::Skins()
    : Module("Skins", LocalizeString::get("client.module.skins.name"), LocalizeString::get("client.module.skins.desc"),
             GAME, nokeybind) {
}

void Skins::onEnable() {
    Necromancer::getScreenManager().showScreen<SkinsScreen>();
}

void Skins::onDisable() {
}

void Skins::loadConfig(SettingGroup& resolvedGroup) {
    resolvedGroup.forEach([&](std::shared_ptr<Setting> set) {
        if (set->name() == "enabled" || set->name() == "key") return;

        this->settings->forEach([&](std::shared_ptr<Setting> modSet) {
            if (modSet->name() != set->name()) return;

            std::visit(
                [&](auto&& obj) {
                    *modSet->value = obj;
                    modSet->update();
                },
                set->resolvedValue);
        });
    });

    std::get<BoolValue>(enabled).value = false;
}
