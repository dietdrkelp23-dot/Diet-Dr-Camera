// Included after the production-code preset harness and RoundTrip helper.
void CheckHitShakePresets(SettingsManager& s)
{
    using Weapon = MeleeWeaponType;
    using Noise = SettingsManager::NoiseProfile;
    using Category = SettingsManager::BindingCategory;
    using Scope = ItemBindings::Scope;
    s.ResetAllToVanilla();
    s.indoorMode = false;
    s.noiseEnabled = s.firstPersonNoiseEnabled = false;
    auto resolve = [&](std::string_view key, bool fp, std::size_t weapon = 0,
                       const ItemBindings::EquippedItem& item = {}, std::string_view keyword = {}) {
        return s.ResolveHitShakeProfile(key, fp, static_cast<Weapon>(weapon), item, keyword);
    };
    auto tuning = [](std::size_t index, bool fp) {
        return HitShake::Tuning{0.25f + static_cast<float>(index) * 0.0625f + (fp ? 0.5f : 0.0f),
                               0.375f, 0.625f, 0.8125f};
    };
    std::vector<std::string_view> keys(HitShake::kMeleeKeys.begin(), HitShake::kMeleeKeys.end());
    keys.insert(keys.end(), HitShake::kOtherKeys.begin(), HitShake::kOtherKeys.end());
    keys.insert(keys.end(), HitShake::kArcheryKeys.begin(), HitShake::kArcheryKeys.end());
    for (const auto key : keys)
        Require(!resolve(key, false) && !resolve(key, true), "Default attack has a hit profile before customization");
    // Reproduce graph-driven power attacks with normal hits enabled and power
    // Strength at zero. The native event alone can look like a normal attack.
    for (bool fp : {false, true}) {
        auto& normalHit = fp ? s.stateFirstPerson["weapons.melee.attack"].noise : s.stateNoise["weapons.melee.attack"];
        auto& powerHit = fp ? s.stateFirstPerson["weapons.melee.power_attack"].noise : s.stateNoise["weapons.melee.power_attack"];
        normalHit.enabled = powerHit.enabled = true;
        normalHit.hitShake = {3, 1, 1, 1, true};
        powerHit.hitShake = {0, 0.5f, 0.5f, 0, true};
    }
    const auto verifyMutedPower = [&] {
        for (bool fp : {false, true}) for (bool native : {false, true}) for (bool attack : {false, true}) {
            const auto powerKey = HitShake::AttackKey(HitShake::Family::Melee,
                HitShake::ContactPower(native, attack, true), false, false, 0);
            const auto* p = resolve(powerKey, fp);
            Require(p && p->hitShake.strength == 0,
                    "Graph-announced power contact borrows normal-hit Strength despite a zero power profile");
            HitShake::Mixer muted;
            muted.Arm(p->hitShake);
            Require(muted.Advance(0.06f).pitch == 0, "Muted power profile still produces a hit pulse");
            const auto normalKey = HitShake::AttackKey(HitShake::Family::Melee,
                HitShake::ContactPower(false, false, false), false, false);
            Require(resolve(normalKey, fp)->hitShake.strength == 3, "Power mute disables normal-hit customization");
        }
    };
    verifyMutedPower(); RoundTrip(s); verifyMutedPower();
    s.ResetAllToVanilla();
    s.noiseEnabled = s.firstPersonNoiseEnabled = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const std::string key(keys[i]);
        s.stateNoise[key].enabled = true;
        s.stateNoise[key].hitShake = tuning(i, false);
        s.stateFirstPerson[key].noise.hitShake = tuning(i, true);
    }
    const auto verifyBase = [&] {
        for (std::size_t i = 0; i < keys.size(); ++i) for (bool fp : {false, true}) {
            const auto* p = resolve(keys[i], fp);
            Require(p && p->hitShake == tuning(i, fp) && p->amp == 0 && p->speed == 0,
                    "Hit tuning lost, crossed a POV/attack, or requires swing noise");
        }
    };
    verifyBase();
    RoundTrip(s);
    verifyBase();

    const std::string normal = "weapons.melee.attack";
    s.stateNoiseIndoor[normal].enabled = true;
    s.stateNoiseIndoor[normal].hitShake = {2, 0.25f, 1};
    SettingsManager::LocationOverride location;
    location.name = "Hit shake test room";
    location.stateNoise[normal].enabled = true;
    location.stateNoise[normal].hitShake = {0, 0.125f, 0.5f, 0.875f};
    location.fpState[normal].noise.hitShake = {0, 0.25f, 0.75f, 0.125f};
    s.locationOverrides.push_back(std::move(location));
    RoundTrip(s);
    s.indoorMode = true;
    Require(resolve(normal, false)->hitShake.strength == 2, "Indoor hit tuning is ignored");
    s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
    Require(resolve(normal, false)->hitShake == HitShake::Tuning{0, 0.125f, 0.5f, 0.875f} &&
            resolve(normal, true)->hitShake == HitShake::Tuning{0, 0.25f, 0.75f, 0.125f},
            "Location loses its muted tuning or crosses hit controls between POVs");
    s.activeLocationChain.clear(); s.locationOverrides.clear(); s.indoorMode = false;

    std::array<SettingsManager::MeleeWeaponNoiseOverrides*, 11> overrides{
        &s.weaponsMeleeAttackNoiseOverrides, &s.weaponsMeleePowerAttackNoiseOverrides,
        &s.weaponsMeleeSprintAttackNoiseOverrides, &s.weaponsMeleeSprintPowerAttackNoiseOverrides,
        &s.weaponsMeleeSneakAttackNoiseOverrides, &s.weaponsMeleeSneakPowerAttackNoiseOverrides,
        &s.weaponsMeleePowerAttackDirNoiseOverrides[0], &s.weaponsMeleePowerAttackDirNoiseOverrides[1],
        &s.weaponsMeleePowerAttackDirNoiseOverrides[2], &s.weaponsMeleePowerAttackDirNoiseOverrides[3],
        &s.weaponsMeleePowerAttackDirNoiseOverrides[4]
    };
    for (std::size_t i = 0; i < overrides.size(); ++i) for (std::size_t weapon = 0; weapon < kMeleeWeaponCount; ++weapon) {
        auto& tp = overrides[i]->perWeapon[weapon];
        tp.parentSeeded = true;
        tp.hitShake = tuning(weapon, false);
        overrides[i]->perWeaponSet[weapon] = true;
        auto& fp = s.stateFpMeleeOverrides[std::string(HitShake::kMeleeKeys[i])];
        fp.perWeaponSetNoise[weapon] = true;
        fp.perWeapon[weapon].noise.hitShake = tuning(weapon, true);
    }
    const auto verifyTypes = [&] {
        for (std::size_t i = 0; i < overrides.size(); ++i) for (std::size_t weapon = 0; weapon < kMeleeWeaponCount; ++weapon)
            for (bool fp : {false, true}) {
                const auto* p = resolve(HitShake::kMeleeKeys[i], fp, weapon);
                Require(p && p->hitShake == tuning(weapon, fp), "Hit shake weapon type override missing in an attack/POV");
            }
    };
    verifyTypes(); RoundTrip(s); verifyTypes();
    for (std::size_t i = 0; i < overrides.size(); ++i) {
        overrides[i]->perWeaponSet[0] = false;
        s.stateFpMeleeOverrides[std::string(HitShake::kMeleeKeys[i])].perWeaponSetNoise[0] = false;
        Require(resolve(HitShake::kMeleeKeys[i], false)->hitShake == tuning(i, false), "Disabled weapon type still overrides hit shake");
    }
    SettingsManager::FpCustomMeleeSlot custom;
    custom.keyword = "WeapTypeRapier"; custom.setNoise = true;
    custom.profile.noise.hitShake = {2.5f, 0.125f, 0.875f};
    s.stateFpMeleeOverrides[normal].custom.push_back(custom);
    RoundTrip(s);
    Require(resolve(normal, true, 0, {}, "WeapTypeRapier")->hitShake == custom.profile.noise.hitShake,
            "Custom weapon keyword hit shake fails after reload");

    s.ResetAllToVanilla();
    const ItemBindings::EquippedItem item{{"Weapons.esp", 0x123}, {"Weapons.esp", 0x100}, "frost"};
    for (bool fp : {false, true}) for (Scope scope : {Scope::BaseItem, Scope::ExactForm, Scope::Enchantment}) {
        SettingsManager::WeaponBinding b;
        b.category = Category::Melee; b.fpOnly = fp; b.bindingScope = scope; b.settingsSeeded = true;
        b.pluginName = "Weapons.esp"; b.formID = scope == Scope::ExactForm ? 0x123 : 0x100;
        b.enchantmentKey = scope == Scope::Enchantment ? "frost" : "";
        for (const int slot : HitShake::kMeleeSlots) {
            b.noiseEnabled[slot] = true;
            b.noiseProfiles[slot].parentSeeded = true;
            b.noiseProfiles[slot].hitShake = tuning(static_cast<std::size_t>(slot), false);
            b.noiseProfilesIndoor[slot] = b.noiseProfiles[slot];
            b.noiseProfilesIndoor[slot].hitShake.texture = 0.25f;
            b.fpProfiles[slot].noise.hitShake = tuning(static_cast<std::size_t>(slot), true);
        }
        s.weaponBindings.push_back(std::move(b));
    }
    const auto verifyBindings = [&] {
        for (bool fp : {false, true}) for (std::size_t i = 0; i < HitShake::kMeleeKeys.size(); ++i) {
            const auto key = HitShake::kMeleeKeys[i];
            const int slot = HitShake::kMeleeSlots[i];
            auto& b = s.weaponBindings[fp ? 5 : 2];
            Require(resolve(key, fp, 0, item) == (fp ? &b.fpProfiles[slot].noise : &b.NoiseProfilesFor(s.RuntimeEnv())[slot]),
                    "Specific weapon hit tuning crossed slot, scope, environment or POV");
            auto expected = tuning(static_cast<std::size_t>(slot), fp);
            if (!fp && s.indoorMode) expected.texture = 0.25f;
            Require(resolve(key, fp, 0, item)->hitShake == expected,
                    "Specific enchantment binding loses a feel control on save/reload");
            auto other = item; other.enchantment = "fire";
            auto& exact = s.weaponBindings[fp ? 4 : 1];
            Require(resolve(key, fp, 0, other) == (fp ? &exact.fpProfiles[slot].noise : &exact.NoiseProfilesFor(s.RuntimeEnv())[slot]),
                    "Exact weapon hit tuning is skipped");
            Require(resolve(key, fp, 0, other)->hitShake == expected,
                    "Exact weapon binding loses a feel control on save/reload");
            other.form.localID = 0x456;
            auto& base = s.weaponBindings[fp ? 3 : 0];
            Require(resolve(key, fp, 0, other) == (fp ? &base.fpProfiles[slot].noise : &base.NoiseProfilesFor(s.RuntimeEnv())[slot]),
                    "Base weapon hit tuning is skipped");
            Require(resolve(key, fp, 0, other)->hitShake == expected,
                    "Base weapon binding loses a feel control on save/reload");
        }
    };
    verifyBindings(); RoundTrip(s); verifyBindings(); s.indoorMode = true; verifyBindings();
    s.weaponBindings[5].fpProfiles[10].noise.hitShake = {};
    Require(resolve("weapons.melee.power_attack.dir.standing", true, 0, item) == &s.weaponBindings[5].fpProfiles[5].noise,
            "Untouched FP direction masks its base power attack's hit shake");
    s.weaponBindings[5].fpProfiles[10].noise.hitShake.authored = true;
    RoundTrip(s);
    Require(resolve("weapons.melee.power_attack.dir.standing", true, 0, item) == &s.weaponBindings[5].fpProfiles[10].noise &&
            resolve("weapons.melee.power_attack.dir.standing", true, 0, item)->hitShake.strength == 0,
            "FP direction's authored zero is lost or inherits its parent's strength");
    SettingsManager::LocationOverride specificLocation;
    specificLocation.stateNoise[SettingsManager::BindingNoiseLocationKey(s.weaponBindings[2], 7)].enabled = true;
    const auto fpLocationKey = SettingsManager::FpBindingLocationKey(s.weaponBindings[5], 7);
    specificLocation.fpState[fpLocationKey] = {};
    s.locationOverrides.push_back(specificLocation);
    RoundTrip(s); s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
    Require(resolve("weapons.melee.sprint_power_attack", false, 0, item)->hitShake.strength == 0 &&
            resolve("weapons.melee.sprint_power_attack", true, 0, item)->hitShake.strength == 0,
            "Specific weapon location zero fails to override hit shake");
    Require(s.IsHitShakeLocationKey(fpLocationKey), "Specific weapon location controls are hidden");
    Require(!s.IsHitShakeLocationKey(SettingsManager::FpBindingLocationKey(s.weaponBindings[5], 0)),
            "Idle specific weapon location shows hit controls");
    Require(!resolve("weapons.bow.draw", false, 0, item), "Ranged attack acquires melee hit tuning");
    s.ResetAllToVanilla();
    Require(!resolve(normal, false) && !resolve(normal, true), "Reset retains hit shake tuning");
    for (int format = 1; format <= 5; ++format) {
        s.ResetAllToVanilla();
        const auto old = toml::parse("[meta]\nformat = " + std::to_string(format) +
            "\n[noise.states.\"weapons.melee.attack\"]\nenabled = true\namp = 2.0\n");
        Require(s.ApplyTable(old), "Older preset stopped loading after hit shake was added");
        Require(s.stateNoise[normal].enabled && s.stateNoise[normal].amp == 2.0f,
                "Adding hit shake changes an older preset's existing noise");
        Require(s.stateNoise[normal].hitShake == HitShake::Tuning{}, "Old preset unexpectedly enables hit shake");
    }
    // The production reader converts the format-6 wave once; modern fields
    // must take precedence even if an author left old keys in the table.
    s.ResetAllToVanilla();
    const auto legacy = toml::parse(R"toml(
[meta]
format = 6
[noise.states."weapons.melee.attack"]
enabled = true
amp = 2.0
hit_shake_strength = 1.5
hit_shake_feel = 0.75
hit_shake_recovery = 0.40
hit_shake_set = true
[noise.states."weapons.melee.power_attack"]
enabled = true
hit_shake_set = true
)toml");
    Require(s.ApplyTable(legacy), "Format 6 no longer loads");
    const auto migrated = HitShake::MigrateLegacy(1.5f, 0.75f, 0.4f, true);
    Require(s.stateNoise[normal].hitShake == migrated && s.stateNoise[normal].amp == 2,
            "Legacy hit tuning was reset/reinterpreted or changed ordinary noise");
    Require(s.stateNoise["weapons.melee.power_attack"].hitShake.authored &&
            s.stateNoise["weapons.melee.power_attack"].hitShake.strength == 0,
            "Migration loses a legacy explicit zero");
    RoundTrip(s);
    Require(s.stateNoise[normal].hitShake == migrated, "Migrated tuning is converted a second time on save/reload");
    auto modern = s.BuildSaveTable();
    Require(kCurrentPresetFormat == 7, "New hit controls did not advance the storage format");
    auto* row = modern["noise"]["states"][normal].as_table();
    Require(row && row->contains("hit_shake_speed") && row->contains("hit_shake_bounce") &&
            row->contains("hit_shake_texture") && !row->contains("hit_shake_feel") && !row->contains("hit_shake_recovery"),
            "Modern tuning is saved ambiguously as legacy tuning");
    row->insert_or_assign("hit_shake_feel", 0.0);
    row->insert_or_assign("hit_shake_recovery", 1.5);
    s.ResetAllToVanilla();
    Require(s.ApplyTable(modern) && s.stateNoise[normal].hitShake == migrated,
            "Old keys override explicit modern controls");
    // Exact defaults with an authored marker still need a modern stamp.
    s.stateNoise[normal].hitShake = {};
    s.stateNoise[normal].hitShake.authored = true;
    RoundTrip(s);
    Require(s.stateNoise[normal].hitShake.speed == 0.5f && s.stateNoise[normal].hitShake.bounce == 0.5f &&
            s.stateNoise[normal].hitShake.texture == 0 && s.stateNoise[normal].hitShake.authored,
            "A reset/default override is mistaken for a legacy wave");

    // Archery uses the existing Drawing/Zoom rows and binding slots, including
    // explicit zero, both POVs, item scopes, indoor and named-location copies.
    for (bool crossbow : {false, true}) {
        s.ResetAllToVanilla();
        s.indoorMode = false;
        const auto category = crossbow ? Category::Crossbow : Category::Bow;
        const auto draw = std::string(HitShake::ArcheryKey(crossbow, false, false));
        const auto zoom = std::string(HitShake::ArcheryKey(crossbow, true, true));
        auto archeryResolve = [&](std::string_view key, bool fp, const ItemBindings::EquippedItem& shotItem,
                                  bool mounted = false) {
            return s.ResolveHitShakeProfile(key, fp, Weapon::Unarmed, shotItem, {}, nullptr, mounted);
        };
        s.stateNoise[draw].enabled = true;
        s.stateNoise[draw].hitShake = {1, 0.25f, 0.75f, 0.5f, true};
        s.stateFirstPerson[draw].noise.hitShake = {2, 0.75f, 0.25f, 0.5f, true};
        Require(archeryResolve(zoom, false, item)->hitShake.strength == 1 && archeryResolve(zoom, true, item)->hitShake.strength == 2,
                "Unconfigured archery Zoom does not fall back to Drawing in the same POV");
        s.stateNoise[zoom].enabled = true;
        s.stateNoise[zoom].hitShake.authored = true;
        s.stateFirstPerson[zoom].noise.hitShake.authored = true;
        RoundTrip(s);
        Require(archeryResolve(zoom, false, item)->hitShake.strength == 0 && archeryResolve(zoom, true, item)->hitShake.strength == 0,
                "Explicit-zero archery Zoom borrows Drawing Strength after reload");
        for (bool fp : {false, true}) {
            const auto key = std::string(HitShake::kArcheryKeys[9]);
            auto& p = fp ? s.stateFirstPerson[key].noise : s.stateNoise[key];
            p.enabled = true;
            p.hitShake = tuning(20, fp);
            Require(archeryResolve(zoom, fp, {}, true)->hitShake == tuning(20, fp),
                    "Mounted archery without a binding borrows the on-foot profile");
        }
        for (bool fp : {false, true}) for (Scope scope : {Scope::BaseItem, Scope::ExactForm, Scope::Enchantment}) {
            SettingsManager::WeaponBinding b;
            b.category = category; b.fpOnly = fp; b.bindingScope = scope; b.settingsSeeded = true;
            b.pluginName = "Weapons.esp"; b.formID = scope == Scope::ExactForm ? 0x123 : 0x100;
            b.enchantmentKey = scope == Scope::Enchantment ? "frost" : "";
            for (int slot : HitShake::kArcherySlots) {
                b.noiseEnabled[slot] = true;
                b.noiseProfiles[slot].parentSeeded = true;
                b.noiseProfiles[slot].hitShake = tuning(static_cast<std::size_t>(slot), false);
                b.noiseProfilesIndoor[slot] = b.noiseProfiles[slot];
                b.noiseProfilesIndoor[slot].hitShake.bounce = 0.125f;
                b.fpProfiles[slot].noise.hitShake = tuning(static_cast<std::size_t>(slot), true);
            }
            s.weaponBindings.push_back(std::move(b));
        }
        const auto verifyArcheryBindings = [&] {
            for (bool fp : {false, true}) for (bool sneak : {false, true}) for (bool zoomed : {false, true}) {
                const auto key = HitShake::ArcheryKey(crossbow, sneak, zoomed);
                const int slot = HitShake::kArcherySlots[(sneak ? 1 : 0) + (zoomed ? 2 : 0)];
                for (int match = 0; match < 3; ++match) {
                    auto shotItem = item;
                    if (match < 2) shotItem.enchantment = "fire";
                    if (match == 0) shotItem.form.localID = 0x456;
                    auto& b = s.weaponBindings[(fp ? 3 : 0) + match];
                    auto* expected = fp ? &b.fpProfiles[slot].noise : &b.NoiseProfilesFor(s.RuntimeEnv())[slot];
                    for (bool mounted : {false, true})
                        Require(archeryResolve(key, fp, shotItem, mounted) == expected,
                                "Archery binding crosses weapon, scope, slot, mount, environment or POV");
                    auto expectedTuning = tuning(static_cast<std::size_t>(slot), fp);
                    if (!fp && s.indoorMode) expectedTuning.bounce = 0.125f;
                    Require(expected->hitShake == expectedTuning, "Archery binding loses a control during persistence");
                    const auto locationKey = fp ? SettingsManager::FpBindingLocationKey(b, slot) : SettingsManager::BindingNoiseLocationKey(b, slot);
                    Require(s.IsHitShakeLocationKey(locationKey), "Archery binding location hides Hit Shake controls");
                }
            }
        };
        verifyArcheryBindings(); RoundTrip(s); verifyArcheryBindings();
        s.indoorMode = true; verifyArcheryBindings();
        auto& specific = s.weaponBindings[2];
        specific.noiseEnabled[5] = specific.noiseEnabled[7] = false;
        Require(archeryResolve(zoom, false, item) == &specific.NoiseProfilesFor(s.RuntimeEnv())[2],
                "Disabled specific sneak/zoom rows still override archery Drawing");
        specific.noiseEnabled[5] = specific.noiseEnabled[7] = true;
        SettingsManager::LocationOverride room;
        for (int slot : HitShake::kArcherySlots) {
            room.stateNoise[SettingsManager::BindingNoiseLocationKey(s.weaponBindings[2], slot)].enabled = true;
            room.fpState[SettingsManager::FpBindingLocationKey(s.weaponBindings[5], slot)].noise.hitShake.authored = true;
        }
        s.locationOverrides.push_back(room);
        RoundTrip(s); s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
        for (bool fp : {false, true}) for (bool sneak : {false, true}) for (bool zoomed : {false, true})
            Require(archeryResolve(HitShake::ArcheryKey(crossbow, sneak, zoomed), fp, item)->hitShake.strength == 0,
                    "Location's zero archery Strength fails to override its specific item");
        for (int slot : {0, 1, 3, 4})
            Require(!s.IsHitShakeLocationKey(SettingsManager::BindingNoiseLocationKey(s.weaponBindings[2], slot)),
                    "Idle/sprint/swim specific archery location shows impact sliders");
        Require(!archeryResolve(HitShake::ArcheryKey(!crossbow, false, false), false, item),
                "Bow binding leaks into crossbow or vice versa");
    }
    s.ResetAllToVanilla();
}
