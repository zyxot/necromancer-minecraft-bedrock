#include "pch.h"
#include "GiveCommand.h"

#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/ItemStack.h"
#include "mc/common/world/actor/player/Inventory.h"
#include "mc/common/world/actor/player/PlayerInventory.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace {
constexpr int kInventorySlotCount = 36;
constexpr int kGiveStackSize = 64;

bool parsePositiveInt(std::string const& value, int& out) {
    if (value.empty()) return false;

    int parsed = 0;
    auto const* first = value.data();
    auto const* last = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc {} || ptr != last || parsed <= 0) return false;

    out = parsed;
    return true;
}

bool parseInt(std::string const& value, int& out) {
    if (value.empty()) return false;

    int parsed = 0;
    auto const* first = value.data();
    auto const* last = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc {} || ptr != last) return false;

    out = parsed;
    return true;
}

int findEmptySlot(SDK::Inventory* inventory) {
    if (!inventory) return -1;

    for (int slot = 0; slot < kInventorySlotCount; ++slot) {
        auto* stack = inventory->getItem(slot);
        if (!stack) continue;

        if (stack->getItem() == nullptr || stack->itemCount == 0) {
            return slot;
        }
    }

    return -1;
}

bool reinitStackByName(SDK::ItemStack* stack, std::string const& itemName, int count, int aux) {
    if (!stack || !stack->vtable) return false;

    // ItemStackBase vtable on current Bedrock:
    // 0 = destructor, 1 = reinit(Item), 2 = reinit(Block), 3 = reinit(string_view).
    using ReinitByName = void(__fastcall*)(SDK::ItemStackBase*, std::string_view const&, int, int);
    auto fn = reinterpret_cast<ReinitByName>(stack->vtable[3]);
    if (!fn) return false;

    std::string_view nameView { itemName };
    fn(stack, nameView, count, aux);

    return stack->getItem() != nullptr && stack->itemCount > 0;
}
} // namespace

GiveCommand::GiveCommand()
    : Command("give", L"spawn items", "$ <itemName> <count> [itemData] [NBT]") {}

bool GiveCommand::execute(std::string const label, std::vector<std::string> args) {
    (void)label;

    if (args.size() < 2) return false;

    int fullCount = 0;
    if (!parsePositiveInt(args[1], fullCount)) {
        message("Invalid item count!", true);
        return true;
    }

    int itemData = 0;
    if (args.size() >= 3 && !parseInt(args[2], itemData)) {
        message("Invalid item data!", true);
        return true;
    }

    auto* client = SDK::ClientInstance::get();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player || !player->supplies || !player->supplies->inventory) {
        message("You must be in a world to use this command.", true);
        return true;
    }

    auto* inventory = player->supplies->inventory;

    std::string itemName = args[0];
    if (itemName.find(':') == std::string::npos) {
        itemName.insert(0, "minecraft:");
    }

    int remaining = fullCount;
    int given = 0;

    while (remaining > 0) {
        int slot = findEmptySlot(inventory);
        if (slot < 0) {
            if (given == 0) {
                message("No empty inventory slots!", true);
            } else {
                message("Inventory filled after giving " + std::to_string(given) + " items.", true);
            }
            return true;
        }

        int const amount = (std::min)(remaining, kGiveStackSize);
        auto* targetStack = inventory->getItem(slot);

        if (!reinitStackByName(targetStack, itemName, amount, itemData)) {
            message("Invalid item name: " + itemName, true);
            return true;
        }

        // reinit() updates the ItemStack that already lives in the player's
        // inventory slot. This keeps the command client-side; it does not run
        // Minecraft's server /give command or require operator permissions.
        targetStack->itemCount = static_cast<uint8_t>(amount);

        given += amount;
        remaining -= amount;
    }

    if (args.size() >= 4) {
        // The original Horion command obtains Mojangson from the clipboard.
        // Necromancer currently has no Mojangson parser, so leave the item
        // untouched rather than corrupting its CompoundTag.
        message("Items were given, but NBT/Mojangson is not available in this build.", true);
        return true;
    }

    message("Successfully gave items!");
    return true;
}
