#pragma once
#include "client/feature/module/Module.h"
#include <string>

class Spoofer : public Module {
public:
    Spoofer();

    void onEnable() override;
    void onTick(Event& evG);

private:
    ValueType editionFaker = BoolValue(false);
    EnumData edition;
    ValueType spoofDeviceId = BoolValue(false);
    ValueType autoDeviceId = BoolValue(true);
    ValueType newIdEachJoin = BoolValue(false);
    ValueType deviceId = TextValue(L"");

    std::string generatedId;

    static std::string randomDeviceId();
    void rerollId();
    void pushState();
};
