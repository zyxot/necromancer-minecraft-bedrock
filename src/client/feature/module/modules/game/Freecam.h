#pragma once
#include "client/feature/module/Module.h"

class Freecam : public Module {
public:
    Freecam();

    void onTick(Event& evG);
    void onUpdate(Event& evG);
    void onBeforeMove(Event& evG);
    void onSendPacket(Event& evG);
    void onClick(Event& evG);
    void onBuildBlock(Event& evG);
    void onRenderLevel(Event& evG);
    void onPacketReceive(Event& evG);
    void onLeaveGame(Event& evG);

    void onEnable() override;
    void onDisable() override;

private:
    void restore();
    void teardown(bool putBack);

    ValueType speed = FloatValue(5.5f);
    ValueType blockInteractions = BoolValue(true);

    bool active = false;

    Vec3 restorePos {};
    Vec3 restorePosOld {};
    Vec3 restoreVelocity {};
    Vec2 restoreRot {};
    AABB restoreBox {};
    Vec3 boxAnchor {};
    Vec3 smoothVel {};
    bool hadBox = false;
    bool fakelagWasEnabled = false;
};
