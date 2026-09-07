#include "pch.h"
#include "GiveCommand.h"

GiveCommand::GiveCommand()
    : Command("give", L"spawn items", "$ <itemName> <count> [itemData] [NBT]") {}

bool GiveCommand::execute(std::string const label, std::vector<std::string> args) {
    (void)label;

    if (args.size() < 2) return false;

    // The previous port attempted to mutate the ItemStack returned for an
    // empty inventory slot and called an unverified ItemStackBase vtable entry.
    // On current Bedrock that can point at shared/invalid slot state and crash
    // the client immediately. Keep the command registered, but fail safely
    // until item construction + inventory transactions are ported properly.
    message("Give is temporarily disabled in this build while the current inventory API is being ported safely.", true);
    return true;
}
