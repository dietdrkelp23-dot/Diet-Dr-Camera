void CheckMagicHitShakePresets(SettingsManager& s)
{
    using Noise = SettingsManager::NoiseProfile;
    using Category = SettingsManager::BindingCategory;
    using Scope = ItemBindings::Scope;
    auto resolve = [&](std::string_view key, bool fp, const ItemBindings::EquippedItem& item = {}) {
        return s.ResolveHitShakeProfile(key, fp, MeleeWeaponType::Unarmed, item);
    };
    const HitShake::Tuning tp{1.5f, .25f, .75f, .5f, true}, fp{2, .75f, .25f, .875f, true};
    s.ResetAllToVanilla(); s.indoorMode = false;
    std::vector<std::string> keys{"transformations.vampire_lord.fire_and_forget"};
    for (const auto school : HitShake::kMagicSchools) for (bool staff : {false, true})
        for (bool sneak : {false, true}) for (const auto cast : {"fire_and_forget", "ritual"}) {
            const auto base = std::string(staff ? "staves." : "magic.") + std::string(school) + (sneak ? ".sneak." : ".") + cast;
            keys.push_back(base);
            if (!staff) for (const auto hand : {"left", "both", "right"}) keys.push_back(base + ".hand." + hand);
        }
    for (const auto& key : keys)
        Require(!resolve(key, false) && !resolve(key, true), "Untouched projectile spell enables Hit Shake");
    for (const auto& key : keys) {
        s.stateNoise[key].enabled = true; s.stateNoise[key].hitShake = tp;
        s.stateFirstPerson[key].noise.hitShake = fp;
    }
    const auto verifyKeys = [&] {
        for (const auto& key : keys) for (bool first : {false, true}) {
            auto* p = resolve(key, first);
            Require(p && p->hitShake == (first ? fp : tp) && p->amp == 0, "Magic school/hand/ritual/POV tuning fails persistence");
            Require(s.IsHitShakeLocationKey(key), "Magic location hides Hit Shake controls");
        }
    };
    verifyKeys(); RoundTrip(s); verifyKeys();

    s.ResetAllToVanilla(); s.indoorMode = false;
    const std::string base = "magic.destruction.fire_and_forget", hand = base + ".hand.left";
    s.stateNoise[base].enabled = true; s.stateNoise[base].hitShake = tp;
    s.stateFirstPerson[base].noise.hitShake = fp;
    for (bool first : {false, true}) Require(resolve(hand, first)->hitShake == (first ? fp : tp), "Unconfigured hand does not inherit its school");
    s.stateNoise[hand].enabled = true; s.stateNoise[hand].hitShake.authored = true;
    s.stateFirstPerson[hand].noise.hitShake.authored = true;
    RoundTrip(s);
    Require(resolve(hand, false)->hitShake.strength == 0 && resolve(hand, true)->hitShake.strength == 0, "Explicit hand zero borrows school strength");
    Require(!resolve("magic.destruction.concentration", false), "Concentration borrows projectile strength");
    s.stateNoiseIndoor[hand].enabled = true; s.stateNoiseIndoor[hand].hitShake = tp;
    s.indoorMode = true;
    Require(resolve(hand, false)->hitShake == tp, "Indoor hand tuning is skipped");
    SettingsManager::LocationOverride room;
    room.stateNoise[hand].enabled = true; room.stateNoise[hand].hitShake.authored = true;
    room.fpState[hand].noise.hitShake.authored = true;
    s.locationOverrides.push_back(room);
    RoundTrip(s); s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
    Require(resolve(hand, false)->hitShake.strength == 0 && resolve(hand, true)->hitShake.strength == 0, "Named location loses magic hand zero");

    const ItemBindings::EquippedItem item{{"Magic.esp", 0x123}, {"Magic.esp", 0x100}, "frost"};
    for (bool staff : {false, true}) {
        s.ResetAllToVanilla(); s.indoorMode = false;
        const auto category = staff ? Category::Staff : Category::Spell;
        const std::string key = staff ? "staves.destruction.sneak.ritual" : "magic.destruction.sneak.fire_and_forget.hand.left";
        for (bool first : {false, true}) for (Scope scope : {Scope::BaseItem, Scope::ExactForm, Scope::Enchantment}) {
            SettingsManager::WeaponBinding b;
            b.category = category; b.fpOnly = first; b.bindingScope = scope; b.settingsSeeded = true;
            b.castType = SettingsManager::SpellCastType::FireAndForget;
            b.pluginName = "Magic.esp"; b.formID = scope == Scope::ExactForm ? 0x123 : 0x100;
            b.enchantmentKey = scope == Scope::Enchantment ? "frost" : "";
            for (const int slot : {0, 3}) {
                b.noiseEnabled[slot] = true;
                b.noiseProfiles[slot].parentSeeded = true; b.noiseProfiles[slot].hitShake = tp;
                b.noiseProfilesIndoor[slot] = b.noiseProfiles[slot]; b.noiseProfilesIndoor[slot].hitShake.texture = .125f;
                b.fpProfiles[slot].noise.hitShake = fp;
            }
            if (!staff) for (int h = 0; h < 3; ++h) {
                b.handNoiseEnabled[h][3] = true;
                b.handNoise[h][3].parentSeeded = true;
                b.handNoise[h][3].hitShake = {static_cast<float>(h), .375f, .625f, .875f, true};
                b.handNoiseIndoor[h][3] = b.handNoise[h][3]; b.handNoiseIndoor[h][3].hitShake.bounce = .125f;
            }
            s.weaponBindings.push_back(std::move(b));
        }
        const auto verifyBindings = [&] {
            for (bool first : {false, true}) for (int match = 0; match < 3; ++match) {
                auto shotItem = item;
                if (match < 2) shotItem.enchantment = "fire";
                if (match == 0) shotItem.form.localID = 0x456;
                auto& b = s.weaponBindings[(first ? 3 : 0) + match];
                auto* expected = first ? &b.fpProfiles[3].noise : staff ? &b.NoiseProfilesFor(s.RuntimeEnv())[3] : &b.HandNoiseFor(s.RuntimeEnv())[0][3];
                Require(resolve(key, first, shotItem) == expected, "Magic binding crosses item scope, firing hand, environment or POV");
                const auto locationKey = first ? s.FpBindingLocationKey(b, 3) : s.BindingNoiseLocationKey(b, 3);
                Require(s.IsHitShakeLocationKey(locationKey), "Magic binding location hides controls");
                if (!staff && !first) for (int h = 0; h < 3; ++h) {
                    const auto hkey = "magic.destruction.sneak.fire_and_forget.hand." + std::string(s.GetMagicHandTomlKey(h));
                    auto* p = resolve(hkey, false, shotItem);
                    Require(p == &b.HandNoiseFor(s.RuntimeEnv())[h][3] && p->hitShake.strength == h &&
                            p->hitShake.bounce == (s.indoorMode ? .125f : .625f), "Specific spell hand tuning/zero fails persistence");
                } else {
                    auto tuning = first ? fp : tp;
                    if (!first && s.indoorMode) tuning.texture = .125f;
                    Require(expected->hitShake == tuning, "Staff/specific FP tuning loses a control on reload");
                }
            }
        };
        verifyBindings(); RoundTrip(s); verifyBindings(); s.indoorMode = true; verifyBindings();
        SettingsManager::LocationOverride specificRoom;
        specificRoom.stateNoise[s.BindingNoiseLocationKey(s.weaponBindings[2], 3)].enabled = true;
        specificRoom.fpState[s.FpBindingLocationKey(s.weaponBindings[5], 3)].noise.hitShake.authored = true;
        s.locationOverrides.push_back(specificRoom);
        RoundTrip(s); s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
        Require(resolve(key, false, item)->hitShake.strength == 0 && resolve(key, true, item)->hitShake.strength == 0,
                "Specific magic location does not override its binding and hand");
        for (int slot = 0; slot < static_cast<int>(s.GetBindingSubStateCount(category)); ++slot)
            Require(s.IsHitShakeBindingSlot(s.weaponBindings[2], slot) == (slot == 3 || (staff && slot == 0)), "Magic binding advertises inactive movement slots");
    }
    s.ResetAllToVanilla();
}
