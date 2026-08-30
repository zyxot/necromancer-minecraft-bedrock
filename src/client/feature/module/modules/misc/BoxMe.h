#pragma once
#include "client/feature/module/Module.h"
#include "client/misc/BoxBuilder.h"
#include <vector>

namespace SDK {
    class BlockSource;
    class LocalPlayer;
}

class BoxMe : public Module {
public:
    BoxMe();

    void onTick(Event& evG);
    void onEnable() override;
    void onDisable() override;

private:
    ValueType useAnyBlock = BoolValue(true);
    ValueType buildBackwards = BoolValue(false);
    ValueType corners = BoolValue(true);
    ValueType topBlock = BoolValue(false);
    ValueType movementLead = BoolValue(true);
    ValueType distance = FloatValue(1.f);
    ValueType blocksPerTick = FloatValue(4.f);
    ValueType autoDisable = BoolValue(true);

    int idleTicks = 0;

    void collectQueue(SDK::LocalPlayer* lp, SDK::BlockSource* region, std::vector<BoxBuilder::Cell>& out) const;
};
