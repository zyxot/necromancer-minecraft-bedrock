#pragma once
#include "../Command.h"

class SkinsCommand final : public Command {
public:
    SkinsCommand();
    ~SkinsCommand() = default;

    bool execute(std::string const label, std::vector<std::string> args) override;
};
