#include "pch.h"
#include "SkinsCommand.h"
#include "../CommandManager.h"
#include "client/Necromancer.h"
#include "client/misc/SkinPackManager.h"

SkinsCommand::SkinsCommand()
    : Command("skins", LocalizeString::get("client.commands.skins.desc"), "skins [sync|open|path]") {
    SkinPack::ensureFolder();
}

bool SkinsCommand::execute(std::string const label, std::vector<std::string> args) {
    std::string sub = args.empty() ? "help" : args[0];
    for (char& c : sub) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    if (sub == "sync") {
        std::string report;
        bool ok = SkinPack::sync(report);
        message(util::StrToWStr(report), !ok);
        return true;
    }

    if (sub == "open") {
        SkinPack::openFolder();
        message(LocalizeString::get("client.commands.skins.opened.name").value());
        return true;
    }

    if (sub == "path") {
        std::filesystem::path mojang = SkinPack::findMojangDir();
        message(util::StrToWStr(mojang.empty() ? "Minecraft data folder not found - launch the game normally and retry"
                                               : mojang.string()));
        return true;
    }

    message(util::StrToWStr("Skins now has a menu: ClickGUI > Misc > Skins\n"
                            "skins open - open " + SkinPack::getSkinsFolder().string() + "\n"
                            "skins sync - build the skin pack from every PNG in that folder\n"
                            "skins path - show where the Minecraft data folder was found"));
    return true;
}
