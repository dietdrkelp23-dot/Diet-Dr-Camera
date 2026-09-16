#pragma once

namespace DietDrCamera
{
    // A default-valued snapshot still belongs to the child. Persist this marker
    // so a later enable cannot overwrite it after the parent changes.
    template <class Profile>
    bool NeedsParentSeed(const Profile& child)
    {
        return !child.parentSeeded && child == Profile{};
    }

    template <class Profile>
    void SeedOverrideFromParent(Profile& child, const Profile& parent)
    {
        if (NeedsParentSeed(child)) child = parent;
        child.parentSeeded = true;
    }

    template <class Table, class Profile>
    void ReadOverrideSeed(const Table& table, Profile& profile)
    {
        // An active legacy override already owns its values, even defaults.
        profile.parentSeeded = table["parent_seeded"].value_or(
            profile.parentSeeded || table["enabled"].value_or(false));
    }

    template <class Table, class Profile>
    void WriteOverrideSeed(Table& table, const Profile& profile)
    {
        if (profile.parentSeeded) table.insert("parent_seeded", true);
    }
}
