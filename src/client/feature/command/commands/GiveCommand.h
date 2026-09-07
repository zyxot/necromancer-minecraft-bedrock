#pragma once
#include "../Command.h"

class GiveCommand : public Command {
public:
    GiveCommand();

protected:
    bool execute(std::string const label, std::vector<std::string> args) override;
};
