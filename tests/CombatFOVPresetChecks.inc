static void CheckCombatFOVPresets(SettingsManager& s)
{
    Require(kCurrentPresetFormat >= 15, "Combat FOV needs a new preset format");
    for (const bool enabled : {false,true}) for (float fov : {50.0f,80.0f,112.0f,140.0f})
        for (float speed : {.05f,1.0f,10.0f}) {
            s.ResetAllToVanilla();
            const CombatFOV::Tuning authored{enabled,fov,speed};
            s.combatFov = authored;
            s.sheathed.fov = 93;
            const auto saved = s.BuildSaveTable();
            RoundTrip(s);
            Require(s.combatFov == authored && s.sheathed.fov == 93,
                "Combat FOV or the normal profile changed during preset roundtrip");
            Require(saved == s.BuildSaveTable(), "Combat FOV preset rewriting is unstable");
        }
    s.ResetAllToVanilla();
    Require(s.combatFov == CombatFOV::Tuning{}, "Reset All retained Combat FOV");
    Require(!s.BuildSaveTable()["cinematic"]["combat_fov"],
        "Default Combat FOV creates unnecessary preset data");
    for (int format = 1; format <= 14; ++format) {
        s.ResetAllToVanilla();
        const toml::table legacy{{"meta",toml::table{{"format",format}}},
            {"sheathed",toml::table{{"fov",93.0}}}};
        Require(s.ApplyTable(legacy), "An older preset was rejected");
        Require(s.combatFov == CombatFOV::Tuning{} && s.sheathed.fov == 93,
            "An older preset gained a combat effect or lost its normal FOV");
    }
    s.ResetAllToVanilla();
    auto malformed = s.BuildSaveTable();
    malformed.insert_or_assign("cinematic",toml::table{{"combat_fov",
        toml::table{{"enabled",true},{"fov",1000.0},{"transition_speed",-1.0}}}});
    Require(s.ApplyTable(malformed) &&
        s.combatFov == CombatFOV::Tuning{true,CombatFOV::kMaxFOV,CombatFOV::kMinSpeed},
        "Combat FOV values are not sanitized on load");
    s.ResetAllToVanilla();
}
