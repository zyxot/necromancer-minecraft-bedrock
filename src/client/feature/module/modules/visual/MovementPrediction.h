#pragma once
#include "client/feature/module/Module.h"

class RenderLevelEvent;

class MovementPrediction : public Module {
public:
    MovementPrediction();

    void onRenderLevel(Event& evG);

private:
    ValueType showPath = BoolValue(true);
    ValueType showHitbox = BoolValue(true);
    ValueType pathColor = ColorValue(1.f, 0.85f, 0.f, 0.75f);
    ValueType hitboxColor = ColorValue(0.f, 1.f, 0.7f, 0.6f);
    ValueType thickness = FloatValue(0.25f);
    EnumData hitboxStyle;
};
