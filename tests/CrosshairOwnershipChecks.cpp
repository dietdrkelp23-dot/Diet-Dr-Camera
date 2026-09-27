#include "UI/CrosshairOwnership.h"
#include <stdexcept>
#include <iostream>

int main()
{
    DietDrCamera::CrosshairOwnership ownership;
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    require(!ownership.IsOverriding() && !ownership.EchoTrajectory(), "fresh HUD echoed an unpublished pose");
    require(!ownership.ClearTrajectory(), "melee lock restored an unowned HUD transform");
    ownership.ClaimTrajectory();
    require(ownership.IsOverriding() && ownership.EchoTrajectory(), "tracing lost HUD ownership");
    require(ownership.ClearTrajectory(), "target lock failed to restore the tracing transform");
    require(!ownership.EchoTrajectory() && !ownership.IsOverriding(), "target lock replayed a stale tracing pose");
    require(!ownership.ClearTrajectory(), "repeated locked frames restored the HUD twice");
    ownership.ClaimTrajectory();
    require(ownership.ClearTrajectory(), "tracing could not resume after target lock");
    std::cout << "Crosshair ownership handoff checks passed\n";
}
