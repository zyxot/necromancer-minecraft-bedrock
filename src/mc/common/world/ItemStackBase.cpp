#include "pch.h"
#include "ItemStackBase.h"

std::string SDK::ItemStackBase::getHoverName() {
    if (!Signatures::ItemStackBase_getHoverName.result) return {};

    std::string out;
    reinterpret_cast<std::string*(__fastcall*)(ItemStackBase*, std::string*)>(
        Signatures::ItemStackBase_getHoverName.result)(this, &out);
    return out;
}

std::map<int, int> SDK::ItemStackBase::gatherEnchants() {
    std::map<int, int> out;
    if (!tag) return out;

    auto enchTag = tag->get("ench");
    if (enchTag && enchTag->getId() == static_cast<uint8_t>(TagType::List)) {
        auto list = reinterpret_cast<ListTag*>(enchTag);
        for (auto entry : list->val) {
            if (!entry || entry->getId() != static_cast<uint8_t>(TagType::Compound)) continue;

            auto comp = reinterpret_cast<CompoundTag*>(entry);
            int id = -1;
            int lvl = 0;

            if (auto idTag = comp->get("id")) {
                if (idTag->getId() == static_cast<uint8_t>(TagType::Byte)) {
                    id = reinterpret_cast<ByteTag*>(idTag)->val;
                } else if (idTag->getId() == static_cast<uint8_t>(TagType::Short)) {
                    id = reinterpret_cast<ShortTag*>(idTag)->val;
                }
            }
            if (auto lvlTag = comp->get("lvl")) {
                if (lvlTag->getId() == static_cast<uint8_t>(TagType::Byte)) {
                    lvl = reinterpret_cast<ByteTag*>(lvlTag)->val;
                } else if (lvlTag->getId() == static_cast<uint8_t>(TagType::Short)) {
                    lvl = reinterpret_cast<ShortTag*>(lvlTag)->val;
                }
            }

            if (id >= 0) out[id] = lvl;
        }
    }

    static const std::pair<const char*, int> known[] = {
        {"protection", 0},        {"fire_protection", 1},  {"feather_falling", 2}, {"blast_protection", 3},
        {"projectile_protection", 4}, {"thorns", 5},       {"respiration", 6},     {"depth_strider", 7},
        {"aqua_affinity", 8},     {"sharpness", 9},        {"smite", 10},          {"bane_of_arthropods", 11},
        {"knockback", 12},        {"fire_aspect", 13},     {"looting", 14},        {"efficiency", 15},
        {"silk_touch", 16},       {"unbreaking", 17},      {"fortune", 18},        {"power", 19},
        {"punch", 20},            {"flame", 21},           {"infinity", 22},       {"mending", 26},
    };

    if (auto compsTag = tag->get("components")) {
        if (compsTag->getId() == static_cast<uint8_t>(TagType::Compound)) {
            auto comps = reinterpret_cast<CompoundTag*>(compsTag);
            if (auto enchCompTag = comps->get("minecraft:enchantments")) {
                if (enchCompTag->getId() == static_cast<uint8_t>(TagType::Compound)) {
                    auto enchComp = reinterpret_cast<CompoundTag*>(enchCompTag);
                    CompoundTag* levels = enchComp;
                    if (auto levelsTag = enchComp->get("levels")) {
                        if (levelsTag->getId() == static_cast<uint8_t>(TagType::Compound))
                            levels = reinterpret_cast<CompoundTag*>(levelsTag);
                    }
                    for (auto const& [name, id] : known) {
                        auto t = levels->get(name);
                        if (!t) continue;
                        int lvl = 0;
                        if (t->getId() == static_cast<uint8_t>(TagType::Short)) {
                            lvl = reinterpret_cast<ShortTag*>(t)->val;
                        } else if (t->getId() == static_cast<uint8_t>(TagType::Int)) {
                            lvl = reinterpret_cast<IntTag*>(t)->val;
                        } else if (t->getId() == static_cast<uint8_t>(TagType::Byte)) {
                            lvl = reinterpret_cast<ByteTag*>(t)->val;
                        }
                        if (lvl > 0) out[id] = lvl;
                    }
                }
            }
        }
    }

    return out;
}

int SDK::ItemStackBase::getEnchantValue(int id) {
    auto enchants = gatherEnchants();
    auto it = enchants.find(id);
    return it != enchants.end() ? it->second : 0;
}
