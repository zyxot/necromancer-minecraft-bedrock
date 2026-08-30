#pragma once
#include "client/feature/module/Module.h"
#include "client/misc/BoxBuilder.h"
#include <vector>

class WallBuilder : public Module {
public:
    WallBuilder();

    void onTick(Event& evG);
    void onEnable() override;
    void onDisable() override;

private:
    static constexpr int wallHeight = 3;

    void collectWallCells(SDK::LocalPlayer* lp, SDK::BlockSource* region, AABB const& enemyBox, Vec3 const& eye,
                          std::vector<BoxBuilder::Cell>& out);

    ValueType useAnyBlock = BoolValue(true);
    ValueType useMovementSim = BoolValue(true);
    ValueType maxSimMs = FloatValue(250.f);
    ValueType enforcedSimMs = FloatValue(100.f);
    ValueType blocksPerTick = FloatValue(4.f);
    ValueType autoDisable = BoolValue(true);

    ValueType players = BoolValue(true);
    ValueType mobs = BoolValue(true);
    EnumData targetMode;
    ValueType enemyRange = FloatValue(6.f);
    ValueType fov = FloatValue(90.f);

    int idleTicks = 0;
};
