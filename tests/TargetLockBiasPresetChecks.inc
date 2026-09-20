// Exercise the production codec and enemy composition, including disabled
// tuning and explicit zero overrides (which must override a nonzero entry).
void CheckTargetLockBiasPresets(SettingsManager& s)
{
    struct BiasField {
        float CameraProfile::* value;
        bool CameraProfile::* enabled;
        const char* key;
    };
    const BiasField fields[] = {
        {&CameraProfile::transitionHeightBias, &CameraProfile::transitionSetHeightBias, "transition_height_bias"},
        {&CameraProfile::transitionZoomBias, &CameraProfile::transitionSetZoomBias, "transition_zoom_bias"},
        {&CameraProfile::transitionFOVBias, &CameraProfile::transitionSetFOVBias, "transition_fov_bias"}
    };
    for (const auto& field : fields) {
        s.ResetAllToVanilla();
        s.indoorMode = false;
        s.tlSheathed.*field.value = -0.75f;
        // Keep this row disabled to verify stored-but-inactive tuning.
        auto& enemy = s.enemyOverrides[0][0].profile;
        enemy.*field.enabled = true;
        enemy.SyncTransitionOverride();
        auto saved = RoundTrip(s);
        Require(saved["meta"]["format"].value_or(0) == kCurrentPresetFormat && kCurrentPresetFormat >= 9,
            "New bias settings were not stamped with a compatible format");
        Require(s.tlSheathed.*field.value == -0.75f && !(s.tlSheathed.*field.enabled),
                std::string("Disabled bias tuning lost: ") + field.key);
        const auto& restoredEnemy = s.enemyOverrides[0][0].profile;
        Require(restoredEnemy.*field.enabled && restoredEnemy.*field.value == 0 && restoredEnemy.TransitionAnySet(),
                std::string("Neutral enemy bias was pruned: ") + field.key);
        Require(!s.enemyOverrides[0][0].fieldsEnabled, "A bias armed the enemy's camera-field override");

        // Each enemy channel overrides independently, even with camera fields
        // disabled; an unticked bias continues to inherit the entry value.
        CameraProfile entry;
        for (const auto& other : fields) {
            entry.*other.value = 1.0f;
            entry.*other.enabled = true;
        }
        entry.SyncTransitionOverride();
        CameraProfile composed = entry;
        s.ApplyEnemyOverrideResolved(composed, 0, &s.tlSheathed,
            static_cast<SettingsManager::TLSlot>(0), nullptr, -1);
        for (const auto& other : fields)
            Require(composed.*other.enabled && composed.*other.value == (other.value == field.value ? 0.0f : 1.0f),
                    "Enemy bias changed an unrelated channel or lost its explicit zero");

        CameraProfile tuned;
        tuned.*field.value = 0.625f;
        tuned.*field.enabled = true;
        tuned.SyncTransitionOverride();
        SettingsManager::AnimationCameraEntry animation;
        animation.uid = 4567;
        animation.animationPath = "fixture/bias.hkx";
        animation.tlProfile = tuned;
        animation.tlProfileIndoor = tuned;
        Require(animation.TlTuned(0) && animation.TlTuned(1), "Bias-only animation does not select its Target Lock profile");
        s.animationCameras.push_back(animation);
        s.paraglideTLProfile = tuned;
        SettingsManager::WeaponBinding binding;
        binding.pluginName = "Skyrim.esm";
        binding.formID = 0x1397E;
        binding.category = SettingsManager::BindingCategory::Melee;
        // Match normal binding creation: the ten regular slots are always
        // enabled; directional power attacks retain their optional toggles.
        for (std::size_t slot = 0; slot < 10; ++slot) {
            binding.enabled[slot] = binding.tlEnabled[slot] = binding.noiseEnabled[slot] = true;
            binding.noiseProfiles[slot].enabled = binding.noiseProfilesIndoor[slot].enabled = true;
            binding.profiles[slot].parentSeeded = binding.profilesIndoor[slot].parentSeeded = true;
            binding.tlProfiles[slot].parentSeeded = binding.tlProfilesIndoor[slot].parentSeeded = true;
            binding.noiseProfiles[slot].parentSeeded = binding.noiseProfilesIndoor[slot].parentSeeded = true;
        }
        binding.tlProfiles[0] = binding.tlProfilesIndoor[0] = tuned;
        binding.tlProfiles[0].parentSeeded = binding.tlProfilesIndoor[0].parentSeeded = true;
        s.weaponBindings.push_back(binding);
        SettingsManager::LocationOverride location;
        location.name = "Bias fixture";
        location.plugin = "Skyrim.esm";
        location.formID = 0x12345;
        s.SizeLocationOverride(location);
        std::fill(location.profiles.begin(), location.profiles.end(), tuned);
        std::fill(location.profilesIndoor.begin(), location.profilesIndoor.end(), tuned);
        std::fill(location.profileSet.begin(), location.profileSet.end(), true);
        std::fill(location.profileSetIndoor.begin(), location.profileSetIndoor.end(), true);
        const auto bindingKey = SettingsManager::BindingCamLocationKey(binding, 0, true);
        location.bindingCam[bindingKey] = tuned;
        s.locationOverrides.push_back(std::move(location));
        saved = RoundTrip(s);
        const auto check = [&](const CameraProfile& p) {
            Require(p.*field.enabled && p.*field.value == 0.625f && p.TransitionAnySet(),
                    std::string("Nested bias override lost: ") + field.key);
        };
        Require(s.animationCameras.size() == 1 && s.weaponBindings.size() == 1 && s.locationOverrides.size() == 1,
                "Bias-only nested profiles disappeared on reload");
        check(s.animationCameras[0].tlProfile);
        check(s.animationCameras[0].tlProfileIndoor);
        check(s.paraglideTLProfile);
        check(s.weaponBindings[0].tlProfiles[0]);
        check(s.weaponBindings[0].tlProfilesIndoor[0]);
        for (const auto& p : s.locationOverrides[0].profiles) check(p);
        for (const auto& p : s.locationOverrides[0].profilesIndoor) check(p);
        check(s.locationOverrides[0].bindingCam.at(bindingKey));
        if (saved != s.BuildSaveTable()) {
            std::ofstream("bias-before.toml") << saved;
            std::ofstream("bias-after.toml") << s.BuildSaveTable();
            throw std::runtime_error("Bias preset rewrite is unstable (bias-before/after.toml)");
        }

        // Malformed values fail atomically, and finite values honor the slider range.
        auto invalid = toml::parse("[meta]\nformat = 9\n[target_lock.sheathed]\n");
        invalid["target_lock"]["sheathed"].as_table()->insert(field.key, std::numeric_limits<double>::infinity());
        Require(!s.ApplyTable(invalid) && saved == s.BuildSaveTable(), "Invalid bias changed the active settings");
        for (float outside : {-10.0f, 10.0f}) {
            auto clamped = toml::parse("[meta]\nformat = 9\n[target_lock.sheathed]\n");
            clamped["target_lock"]["sheathed"].as_table()->insert(field.key, outside);
            Require(s.ApplyTable(clamped), "Finite bias tuning was rejected");
            Require(s.tlSheathed.*field.value == (outside < 0 ? -1.5f : 1.5f), "Bias range was not enforced");
        }
        s.ResetAllToVanilla();
        Require(s.tlSheathed.*field.value == 0 && !(s.tlSheathed.*field.enabled) &&
                !s.tlSheathed.ProximityBiasAnySet(), "Bias did not reset to neutral");
    }
    // Preserve format-8 Pitch Bias exactly; all newly added channels stay off.
    s.ResetAllToVanilla();
    Require(s.ApplyTable(toml::parse("[meta]\nformat = 8\n[target_lock.sheathed]\n"
        "transition_set_pitch_bias = true\ntransition_pitch_bias = -0.75\n")), "Format-8 preset was rejected");
    Require(s.tlSheathed.transitionSetPitchBias && s.tlSheathed.transitionPitchBias == -0.75f,
            "Format-8 Pitch Bias changed");
    for (const auto& field : fields)
        Require(!(s.tlSheathed.*field.enabled) && s.tlSheathed.*field.value == 0,
                "An old preset enabled a new bias");
    s.ResetAllToVanilla();
}
