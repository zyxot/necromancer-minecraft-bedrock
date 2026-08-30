#pragma once
#include "client/feature/module/Module.h"
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace SDK {
    class ContainerScreenController;
    class ItemStack;
}

class ChestStealer : public Module {
public:
    ChestStealer();

    void onRenderLayer(Event& evG);
    void onTick(Event& evG);
    void onEnable() override;
    void onDisable() override;

    bool hasCustomItem(std::string const& id) const;
    void addCustomItem(std::string const& id);
    void removeCustomItem(std::string const& id);
    int customItemCount(std::string const& id) const;
    void setCustomItemCount(std::string const& id, int count);
    void writeCustomItems(std::map<std::string, int> const& items);
    std::vector<std::string> customItemList() const;

    static std::vector<std::string> const& limitSettingNames();
    float maxValue(std::string const& settingName) const;
    void bumpMax(std::string const& settingName, int delta);
    ValueType const* findLimitValue(std::string const& settingName) const;

private:
    ValueType time = FloatValue(1.f);
    ValueType closeAfterLoot = BoolValue(false);
    ValueType enhanced = BoolValue(false);
    ValueType autoOrganize = BoolValue(false);

    ValueType maxBows = FloatValue(2.f);
    ValueType maxSwords = FloatValue(1.f);
    ValueType maxHelmets = FloatValue(1.f);
    ValueType maxChestplates = FloatValue(1.f);
    ValueType maxLeggings = FloatValue(1.f);
    ValueType maxBoots = FloatValue(1.f);
    ValueType maxGapples = FloatValue(5.f);
    ValueType maxBlocks = FloatValue(5.f);
    ValueType maxArrows = FloatValue(5.f);
    ValueType maxFood = FloatValue(5.f);
    ValueType maxEggs = FloatValue(2.f);
    ValueType maxLavaBuckets = FloatValue(1.f);
    ValueType customItems = TextValue(L"");
    ValueType customOpenPicker = ButtonValue();

    SDK::ContainerScreenController* controller = nullptr;
    std::chrono::steady_clock::time_point lastSeen {};
    std::chrono::steady_clock::time_point lastAction {};
    std::chrono::steady_clock::time_point sessionStart {};
    bool containerScreen = false;

    enum class Phase { Scan,
                       Steal,
                       Organize,
                       Done };
    Phase phase = Phase::Scan;

    std::vector<int> plan;
    size_t planCursor = 0;
    int lootedStacks = 0;
    int verifySlot = -1;
    int verifyCount = 0;
    int verifyAttempts = 0;
    bool exitRequested = false;
    int stableScans = 0;
    int totalMoves = 0;

    void resetSession();
    SDK::ItemStack* readSlot(const std::string& collection, int slot);
    std::chrono::steady_clock::duration perActionDelay() const;
    bool processAction(std::chrono::steady_clock::time_point now);
    bool buildPlan(std::chrono::steady_clock::time_point now);
    bool processSteal();
    bool processOrganize();
    bool tryMove(const std::string& srcColl, int srcIdx, const std::string& dstColl, int dstIdx);
    void finish();
};