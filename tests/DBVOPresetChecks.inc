static void CheckDBVOCameraPresets(SettingsManager& s)
{
    Require(kCurrentPresetFormat >= 16, "DBVO camera switching needs its own preset format");
    for (const bool enabled : {false, true}) {
        s.ResetAllToVanilla();
        s.dialogueDBVOCameraSwitching = enabled;
        s.dialogueMulPosition = .65f;
        s.sheathed.fov = 93;
        const auto saved = s.BuildSaveTable();
        if (enabled) Require(!saved["general"]["dialogue_dbvo_camera_switching"],
            "Default DBVO behavior creates unnecessary preset data");
        else Require(saved["general"]["dialogue_dbvo_camera_switching"].value<bool>() == false,
            "Disabled DBVO camera switching was pruned from the preset");
        RoundTrip(s);
        Require(s.dialogueDBVOCameraSwitching == enabled && s.dialogueMulPosition == .65f && s.sheathed.fov == 93,
            "DBVO camera preference or unrelated tuning changed during roundtrip");
        Require(saved == s.BuildSaveTable(), "DBVO camera preference rewrite is unstable");
    }
    for (int format = 1; format <= 15; ++format) {
        s.dialogueDBVOCameraSwitching = false;
        s.ResetAllToVanilla();
        Require(s.dialogueDBVOCameraSwitching, "Reset retained disabled DBVO camera switching");
        const toml::table legacy{{"meta",toml::table{{"format",format}}},
            {"sheathed",toml::table{{"fov",93.0}}}};
        Require(s.ApplyTable(legacy) && s.dialogueDBVOCameraSwitching && s.sheathed.fov == 93,
            "Older preset changed DBVO behavior or unrelated framing");
    }
    s.ResetAllToVanilla();
}
