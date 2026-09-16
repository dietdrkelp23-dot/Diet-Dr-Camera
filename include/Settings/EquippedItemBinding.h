#pragma once

#include "Settings/ItemBindingIdentity.h"

namespace RE { class Actor; class TESForm; }

namespace DietDrCamera::ItemBindings
{
    [[nodiscard]] EquippedItem DescribeEquipped(RE::Actor* actor, bool leftHand, bool includeNames = false);
    [[nodiscard]] EquippedItem DescribeForm(RE::TESForm* form);
    [[nodiscard]] FormIdentity Identify(RE::TESForm* form);
}
