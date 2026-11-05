#pragma once
#include <string>

#include "rt.h"

namespace rt
{
// Runs the game from its PE file (the user's own ra3_1.12.game). Returns the guest exit code.
int launch(const std::string& gameFile);
}
