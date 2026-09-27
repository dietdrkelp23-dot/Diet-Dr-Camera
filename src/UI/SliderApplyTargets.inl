// Included inside MenuUI's anonymous namespace after the shared tab builders.
// Keep the Apply tree on the same tab/entry definitions as the visible menu.
static bool ScalarStateOverride(std::string_view key)
{
    return key.find(".hand.") != key.npos || key.find(".dir.") != key.npos ||
        (key.starts_with("shouts.") && key.find(".base") == key.npos);
}

static SliderApply::NoiseFeatures ScalarNoiseFeatures(const EntryHoverTarget& entry, bool fp)
{
    SliderApply::NoiseFeatures f;
    if (entry.binding) {
        const auto& b = *entry.binding;
        const int slot = entry.bindingSlot;
        using B = SettingsManager::BindingCategory;
        f.shout = b.category == B::Shout;
        f.hit = SettingsManager::IsHitShakeBindingSlot(b, slot);
        f.attack = b.category == B::Melee && (slot == 3 || slot == 5 || slot >= 6);
        f.launch = fp ? (b.category == B::Spell && slot == 3 &&
            b.castType == SettingsManager::SpellCastType::FireAndForget) :
            (f.shout || b.category == B::Staff || (b.category == B::Spell && slot == 3) ||
             ((b.category == B::Bow || b.category == B::Crossbow) && (slot == 2 || slot == 5)));
        return f;
    }
    const std::string_view key = entry.noiseKey;
    f.shout = key.starts_with("shouts.");
    f.hit = HitShake::IsAttackKey(key);
    f.attack = key.ends_with(".attack") || key.ends_with(".power_attack") ||
        key.ends_with(".sneak_attack") || key.ends_with(".sneak_power_attack") ||
        key.ends_with(".sprint_attack") || key.ends_with(".sprint_power_attack");
    f.launch = fp ? (key.ends_with(".fire_and_forget") || key.ends_with(".ritual")) :
        (f.shout || key.starts_with("magic.") || key.starts_with("staves.") ||
         key == "transformations.werewolf.roar" || key == "transformations.vampire_lord.fire_and_forget" ||
         key == "transformations.vampire_lord.concentration" ||
         ((key.starts_with("weapons.bow") || key.starts_with("weapons.crossbow") ||
           key.starts_with("mounts.horseback.archery")) && (key.ends_with(".draw") || key.ends_with(".zoom"))));
    return f;
}

static void CollectSliderApplyTargets(SettingsManager& s, std::string_view section, SliderApply::Collector& out)
{
    using namespace SliderApply;
    const bool tl = section == "Target Lock";
    const bool fp = section == "First Person";
    const bool noise = section == "Camera Noise";
    if (!tl && !fp && !noise && section != "Third Person") return;

    const auto collectTab = [&](std::string_view name, const EntryHoverTarget& tab) {
        for (const auto& entry : tab.tabEntries) {
            const int env = entry.tabEnvironment;
            const auto environment = env == SettingsManager::kEnvIndoor ? Environment::Indoor : Environment::Outdoor;
            const bool overrides = entry.sliderOverride ||
                (entry.tabMapState && ScalarStateOverride(entry.noiseKey));
            const auto features = ScalarNoiseFeatures(entry, fp);
            if (entry.kind == EntryClipKind::Camera || entry.kind == EntryClipKind::Transitions) {
                auto* p = static_cast<CameraProfile*>(entry.ptr);
                if (!p) continue;
                const auto& zoom = s.ZoomRangeFor(entry.outdoorBase ? entry.outdoorBase : p);
                const bool transitionOnly = entry.kind == EntryClipKind::Transitions;
                const auto camera = [&](CameraProfile& value, bool isOverride, Environment targetEnvironment) {
                    Camera(out, value, name, isOverride, tl, entry.sliderShout, zoom, entry.sliderDragon, transitionOnly,
                        targetEnvironment);
                };
                // Walk every registered camera's locations too, including hand
                // and weapon-type locations, without inventing new bindings.
                const auto cameraWithLocations = [&](CameraProfile* value, CameraProfile* outdoor, bool isOverride) {
                    if (!value) return;
                    // A non-variant profile is shared by both environments.
                    const bool shared = outdoor && s.VariantOf(outdoor, SettingsManager::kEnvIndoor) == outdoor;
                    camera(*value, isOverride, shared ? Environment::Shared : environment);
                    if (const auto index = s.outdoorIdxMap.find(outdoor); index != s.outdoorIdxMap.end()) {
                        for (auto& location : s.locationOverrides) {
                            auto& profiles = location.ProfilesFor(env);
                            auto& set = location.ProfileSetFor(env);
                            const auto i = index->second;
                            if (i < profiles.size() && i < set.size() && set[i])
                                Camera(out, profiles[i], name, true, tl, false, zoom, false, transitionOnly, environment);
                        }
                    }
                };
                cameraWithLocations(p, entry.outdoorBase, overrides);
                if (entry.meleeOv) {
                    for (auto& weapon : entry.meleeOv->perWeapon)
                        cameraWithLocations(s.VariantOf(&weapon, env), &weapon, true);
                    for (auto& weapon : entry.meleeOv->custom) camera(weapon.For(env == SettingsManager::kEnvIndoor), true, environment);
                }
                if (entry.handSet)
                    for (auto& hand : entry.handSet->profiles)
                        cameraWithLocations(s.VariantOf(&hand, env), &hand, true);
                if (entry.binding && entry.bindingSlot == 3 && entry.binding->category == SettingsManager::BindingCategory::Spell) {
                    auto& hands = tl ? entry.binding->HandTlProfilesFor(env) : entry.binding->HandProfilesFor(env);
                    for (auto& hand : hands) camera(hand[3], true, environment);
                }
                // Binding/animation location cameras have shared storage.
                if (!entry.noiseKey.empty())
                    for (auto& location : s.locationOverrides)
                        if (auto it = location.bindingCam.find(entry.noiseKey); it != location.bindingCam.end())
                            Camera(out, it->second, name, true, tl, false, Defaults::Zoom, false, transitionOnly);
                if (tl && entry.enemyOv.valid) {
                    const auto enemyCamera = [&](CameraProfile& value) {
                        Camera(out, value, name, true, true, false, Defaults::Zoom, false, false, environment);
                    };
                    for (auto* enemy : entry.enemyOv.fields) if (enemy) enemyCamera(enemy->profile);
                    if (EnemyBindingSlotValid(entry.enemyOv)) {
                        for (auto& grid : entry.enemyOv.binding->customEnemyGrids)
                            enemyCamera(grid.SlotsFor(env)[static_cast<std::size_t>(entry.enemyOv.bindingSlot)].profile);
                    } else {
                        for (auto& enemy : s.customEnemyOverrides)
                            if (auto* cell = EnemyCellForCustom(enemy, entry.enemyOv, env)) enemyCamera(cell->profile);
                    }
                }
            } else if (entry.kind == EntryClipKind::Noise) {
                auto* p = static_cast<SettingsManager::NoiseProfile*>(entry.ptr);
                Profile<SettingsManager::NoiseProfile> profile(*p, environment);
                if (entry.tabMapState) {
                    const auto key = entry.noiseKey;
                    const auto it = s.StateNoiseFor(env).find(key);
                    profile = { it == s.StateNoiseFor(env).end() ? nullptr : &it->second,
                        [&s, env, key, overrides]() -> SettingsManager::NoiseProfile& {
                            auto& map = s.StateNoiseFor(env);
                            auto [created, inserted] = map.try_emplace(key, s.GlobalNoiseFor(env));
                            if (inserted) created->second.enabled = !overrides;
                            // Ordinary noise entries have no enable toggle; the
                            // page makes them active when opened too.
                            if (!overrides) created->second.enabled = true;
                            return created->second;
                        }, environment};
                }
                Noise(out, profile, name, overrides, features);
                // These legacy weapon-type and location noise profiles have
                // one shared value, so an environment-only apply leaves them alone.
                if (entry.meleeNoiseOv)
                    for (auto& weapon : entry.meleeNoiseOv->perWeapon) Noise(out, Profile(weapon), name, true, features);
                if (!entry.noiseKey.empty())
                    for (auto& location : s.locationOverrides)
                        if (auto it = location.stateNoise.find(entry.noiseKey); it != location.stateNoise.end())
                            Noise(out, Profile(it->second), name, true, features);
            } else if (entry.kind == EntryClipKind::FirstPerson) {
                auto* p = static_cast<SettingsManager::FirstPersonProfile*>(entry.ptr);
                Profile<SettingsManager::FirstPersonProfile> profile(*p);
                if (entry.tabMapState) {
                    const auto key = entry.noiseKey;
                    const auto it = s.stateFirstPerson.find(key);
                    profile = { it == s.stateFirstPerson.end() ? nullptr : &it->second,
                        [&s, key]() -> SettingsManager::FirstPersonProfile& { return s.EnsureStateFp(key); }};
                }
                FirstPerson(out, profile, name, overrides, features);
                if (auto it = s.stateFpMeleeOverrides.find(entry.noiseKey); it != s.stateFpMeleeOverrides.end()) {
                    for (auto& weapon : it->second.perWeapon) FirstPerson(out, Profile(weapon), name, true, features);
                    for (auto& weapon : it->second.custom) FirstPerson(out, Profile(weapon.profile), name, true, features);
                }
                if (!entry.noiseKey.empty())
                    for (auto& location : s.locationOverrides)
                        if (auto it = location.fpState.find(entry.noiseKey); it != location.fpState.end())
                            FirstPerson(out, Profile(it->second), name, true, features);
            } else if (entry.kind == EntryClipKind::FxBeat) {
                const int index = entry.fxBeatIdx;
                if (index == 4) Beat(out, s.eventBeats[0], name, false, true);
                else {
                    float* intensity[] = { &s.werewolfTransformIntensity, &s.werewolfRevertIntensity,
                        &s.vampireLordTransformIntensity, &s.vampireLordRevertIntensity, nullptr, &s.vampireLordBatsIntensity };
                    float* speed[] = { &s.werewolfTransformSpeed, &s.werewolfRevertSpeed,
                        &s.vampireLordTransformSpeed, &s.vampireLordRevertSpeed, nullptr, &s.vampireLordBatsSpeed };
                    SettingsManager::CinematicShakeChar* character[] = { &s.werewolfTransformChar, &s.werewolfRevertChar,
                        &s.vampireLordTransformChar, &s.vampireLordRevertChar, nullptr, &s.vampireLordBatsChar };
                    if (index >= 0 && index < 6) {
                        out.Add("noise.intensity", name, false, *intensity[index], 0.0f, 5.0f);
                        out.Add("noise.speed", name, false, *speed[index], 0.1f, 5.0f);
                        Character(out, *character[index], name, false);
                    }
                }
                if (index >= 0 && index < 6)
                    for (auto& location : s.locationOverrides)
                        if (auto it = location.fxBeats.find(SettingsManager::kFxBeatLocKeys[index]); it != location.fxBeats.end())
                            Beat(out, it->second, name, true, index == 4);
            }
        }
    };

    if (tl) {
        for (const auto& tab : BuildTLTabs(s)) collectTab(tab.label, CameraTabTarget(s, tab, true));
        collectTab("Specific Weapons", WeaponsTabTarget(s, SpecWeaponsSection::TargetLock));
        collectTab("Specific Animations", AnimationsTabTarget(s, SpecWeaponsSection::TargetLock));
        // General settings already cover the section. Aim Bias also has a
        // named per-entry counterpart, but General's own value stays global.
        out.Add("camera.transitionAimBias", "General", false, s.targetLockAimBias, 0.0f, 1.5f);
        out.Add("general.looseness", "General", false, s.targetLockTrackSeconds, 0.0f, 1.0f);
    } else {
        for (const auto& tab : BuildCategoryTabs(s)) {
            const std::string_view name = tab.label;
            if (fp && name != "Sheathed" && name != "Melee" && name != "Archery" &&
                name != "Magic" && name != "Staves" && name != "Blocking" && name != "Specific Weapons") continue;
            collectTab(name, (noise || fp) ? NoiseOrFirstPersonTabTarget(s, tab, fp) : CameraTabTarget(s, tab, false));
        }
        if (fp) {
            auto tab = NewTabTarget("Shouts");
            for (const char* suffix : { "", ".sneak" }) {
                const auto add = [&](std::string key) {
                    EntryHoverTarget child;
                    child.kind = EntryClipKind::FirstPerson;
                    child.noiseKey = key;
                    child.tabMapState = true;
                    const auto found = s.stateFirstPerson.find(key);
                    child.ptr = found == s.stateFirstPerson.end() ? &s.firstPersonGlobal : &found->second;
                    AddTabTarget(tab, key, std::move(child), 0);
                };
                add(std::string("shouts.base") + suffix);
                for (const auto& shout : kShouts) add(std::string("shouts.") + shout.tomlKey + suffix);
            }
            collectTab("Shouts", tab);
        } else collectTab("Specific Animations", AnimationsTabTarget(s, noise ? SpecWeaponsSection::Noise : SpecWeaponsSection::Categories));
    }
}
