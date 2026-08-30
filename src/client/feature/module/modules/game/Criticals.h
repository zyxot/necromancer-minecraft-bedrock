#pragma once
#include "client/feature/module/Module.h"

class Criticals : public Module {
public:
    Criticals();

    void onSendPacket(Event& evG);
    void onEnable() override;

private:
    ValueType useVelocity = BoolValue(true);
    ValueType positionChange = BoolValue(true);
    ValueType biggerPositionChange = BoolValue(false);
    ValueType changePercent = FloatValue(1.5f);
    ValueType sendJumping = BoolValue(true);
    ValueType disableSprint = BoolValue(true);

    enum class AnimState {
        Start,
        MidAir,
        MidAir2,
        Landing,
        Finished,
    };

    AnimState animState = AnimState::Start;
};
