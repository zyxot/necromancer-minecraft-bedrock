#pragma once
#include "client/feature/module/Module.h"

class FastStop : public Module {
public:
    FastStop();

    void onTick(Event& evG);
    void onDisable() override;

private:
    ValueType onlyOnGround = BoolValue(true);

    bool wasMoving = false;
};
