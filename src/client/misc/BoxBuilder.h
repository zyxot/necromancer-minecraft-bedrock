#pragma once
#include "client/misc/BlockSolid.h"
#include "mc/Addresses.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/actor/player/GameMode.h"
#include "mc/common/world/actor/player/Inventory.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/ItemStack.h"
#include <cmath>
#include <unordered_set>
#include <vector>

namespace BoxBuilder {
    inline int& placementBypassDepth() {
        static int depth = 0;
        return depth;
    }

    struct PlacementBypass {
        PlacementBypass() { placementBypassDepth()++; }
        ~PlacementBypass() { placementBypassDepth()--; }
    };

    inline bool placementBypassActive() { return placementBypassDepth() > 0; }

    struct Cell {
        int x;
        int y;
        int z;
    };

    inline bool isUnsafeBlock(std::string const& id) {
        static const std::unordered_set<std::string> unsafe = {
            "minecraft:web",         "minecraft:cobweb",          "minecraft:fire",         "minecraft:soul_fire",
            "minecraft:lava",        "minecraft:flowing_lava",    "minecraft:water",        "minecraft:flowing_water",
            "minecraft:magma",       "minecraft:campfire",        "minecraft:soul_campfire", "minecraft:cactus",
            "minecraft:powder_snow", "minecraft:sweet_berry_bush", "minecraft:tnt",         "minecraft:tnt_minecart",
        };
        return unsafe.contains(id);
    }

    // Functional blocks worth more in the inventory than as cage walls, plus partial
    // shapes that do not seal a box.
    inline bool isWastedBoxBlock(std::string const& id) {
        static const std::unordered_set<std::string> keep = {
            "minecraft:chest",          "minecraft:trapped_chest",   "minecraft:ender_chest",
            "minecraft:barrel",         "minecraft:hopper",          "minecraft:dispenser",
            "minecraft:dropper",        "minecraft:furnace",         "minecraft:blast_furnace",
            "minecraft:smoker",         "minecraft:brewing_stand",   "minecraft:crafter",
            "minecraft:lectern",        "minecraft:jukebox",         "minecraft:chiseled_bookshelf",
            "minecraft:beacon",         "minecraft:crafting_table",  "minecraft:cartography_table",
            "minecraft:smithing_table", "minecraft:fletching_table", "minecraft:stonecutter",
            "minecraft:stonecutter_block", "minecraft:grindstone",   "minecraft:loom",
            "minecraft:enchanting_table", "minecraft:anvil",         "minecraft:chipped_anvil",
            "minecraft:damaged_anvil",  "minecraft:bed",
        };
        if (keep.contains(id)) return true;
        if (id.find("shulker_box") != std::string::npos) return true;
        if (id.find("_door") != std::string::npos) return true;
        if (id.find("_bed") != std::string::npos) return true;
        if (id.find("carpet") != std::string::npos) return true;
        return false;
    }

    inline bool findBlockSlot(SDK::LocalPlayer* lp, bool anySlot, int& outSlot) {
        if (!lp || !lp->supplies || !lp->supplies->inventory) return false;
        int sel = lp->supplies->selectedSlot;
        auto check = [&](int slot) {
            if (slot < 0 || slot >= 9) return false;
            auto stack = lp->supplies->inventory->getItem(slot);
            if (!stack || stack->itemCount <= 0 || !stack->block) return false;
            auto* item = stack->getItem();
            if (!item) return false;
            std::string id = item->namespacedId.getString();
            return !isUnsafeBlock(id) && !isWastedBoxBlock(id);
        };
        if (!anySlot) {
            if (check(sel)) {
                outSlot = sel;
                return true;
            }
            return false;
        }
        for (int i = 0; i < 9; i++) {
            if (check(i)) {
                outSlot = i;
                return true;
            }
        }
        return false;
    }

    struct Footprint {
        int x0;
        int x1;
        int z0;
        int z1;
        int feetY;
    };

    inline Footprint footprintOf(AABB const& box) {
        Footprint f {};
        f.x0 = static_cast<int>(std::floor(box.lower.x + 0.001f));
        f.x1 = static_cast<int>(std::floor(box.higher.x - 0.001f));
        f.z0 = static_cast<int>(std::floor(box.lower.z + 0.001f));
        f.z1 = static_cast<int>(std::floor(box.higher.z - 0.001f));
        f.feetY = static_cast<int>(std::floor(box.lower.y + 0.001f));
        if (f.x1 < f.x0) f.x1 = f.x0;
        if (f.z1 < f.z0) f.z1 = f.z0;
        return f;
    }

    inline void collectRing(SDK::BlockSource* region, int x0, int x1, int z0, int z1, int feetY, int height,
                            bool corners, std::vector<Cell>& out) {
        for (int layer = 0; layer < height; layer++) {
            int y = feetY + layer;
            for (int x = x0 - 1; x <= x1 + 1; x++) {
                for (int z = z0 - 1; z <= z1 + 1; z++) {
                    bool onEdge = (x == x0 - 1 || x == x1 + 1 || z == z0 - 1 || z == z1 + 1);
                    if (!onEdge) continue;
                    bool isCorner = (x == x0 - 1 || x == x1 + 1) && (z == z0 - 1 || z == z1 + 1);
                    if (isCorner && !corners) continue;
                    if (BlockSolid::isCollidable(region, x, y, z)) continue;
                    out.push_back({ x, y, z });
                }
            }
        }
    }

    inline void collectRoof(SDK::BlockSource* region, int x0, int x1, int z0, int z1, int feetY, int height,
                            std::vector<Cell>& out) {
        int y = feetY + height;
        for (int x = x0; x <= x1; x++) {
            for (int z = z0; z <= z1; z++) {
                if (BlockSolid::isCollidable(region, x, y, z)) continue;
                out.push_back({ x, y, z });
            }
        }
    }

    inline bool hasAnySupport(SDK::BlockSource* region, Cell const& cell) {
        static constexpr int offX[6] = { 0, 0, 0, 0, -1, 1 };
        static constexpr int offY[6] = { -1, 1, 0, 0, 0, 0 };
        static constexpr int offZ[6] = { 0, 0, -1, 1, 0, 0 };
        for (int f = 0; f < 6; f++) {
            if (BlockSolid::isCollidable(region, cell.x + offX[f], cell.y + offY[f], cell.z + offZ[f])) return true;
        }
        return false;
    }

    inline int build(SDK::LocalPlayer* lp, SDK::BlockSource* region, std::vector<Cell> const& cells, bool anySlot,
                     int maxPerTick) {
        if (!lp || !lp->gameMode || !Signatures::GameMode_buildBlock.result) return 0;

        PlacementBypass bypass;

        static constexpr int offX[6] = { 0, 0, 0, 0, -1, 1 };
        static constexpr int offY[6] = { -1, 1, 0, 0, 0, 0 };
        static constexpr int offZ[6] = { 0, 0, -1, 1, 0, 0 };
        static constexpr uint8_t opposite[6] = { 1, 0, 3, 2, 5, 4 };
        static constexpr int order[6] = { 0, 2, 3, 4, 5, 1 };

        int placedCount = 0;
        for (auto const& cell : cells) {
            if (placedCount >= maxPerTick) break;
            if (BlockSolid::isCollidable(region, cell.x, cell.y, cell.z)) continue;
            if (!hasAnySupport(region, cell)) continue;

            int slot = -1;
            if (!findBlockSlot(lp, anySlot, slot)) break;
            if (lp->supplies->selectedSlot != slot) lp->supplies->selectedSlot = slot;

            for (int i = 0; i < 6; i++) {
                int f = order[i];
                if (opposite[f] == 0) continue;
                BlockPos support { cell.x + offX[f], cell.y + offY[f], cell.z + offZ[f] };
                if (!BlockSolid::isCollidable(region, support.x, support.y, support.z)) continue;

                if (reinterpret_cast<bool (*)(SDK::GameMode*, BlockPos const*, uint8_t, bool)>(
                        Signatures::GameMode_buildBlock.result)(lp->gameMode, &support, opposite[f], true)) {
                    placedCount++;
                    break;
                }
            }
        }
        return placedCount;
    }
}
