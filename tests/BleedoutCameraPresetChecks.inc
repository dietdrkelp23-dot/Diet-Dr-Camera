static void CheckBleedoutCameraPresets(SettingsManager& s)
{
    Require(kCurrentPresetFormat >= 17, "Camera suppression requires format 17");
    for (bool death : {false, true}) for (bool ragdoll : {false, true}) {
        s.ResetAllToVanilla();
        s.disableDeathCamera = death;
        s.disableRagdollCamera = ragdoll;
        s.deathCameraFov = 107;
        s.ragdollCamFov = 82;
        s.ragdollCamSlowmoStrength = 45;
        s.deathCameraHoldDuration = 9;
        s.sheathed.zoom = 22;
        const auto saved = s.BuildSaveTable();
        Require(bool(saved["general"]["disable_death_camera"]) == death &&
            bool(saved["general"]["disable_ragdoll_camera"]) == ragdoll,
            "Default camera suppression flags were not pruned independently");
        RoundTrip(s);
        Require(s.disableDeathCamera == death && s.disableRagdollCamera == ragdoll &&
            s.deathCameraFov == 107 && s.ragdollCamFov == 82 && s.ragdollCamSlowmoStrength == 45 &&
            s.deathCameraHoldDuration == 9 && s.sheathed.zoom == 22 && saved == s.BuildSaveTable(),
            "Camera suppression roundtrip lost toggles, hidden tuning, or unrelated framing");
    }
    for (int format = 1; format <= 16; ++format) {
        s.disableDeathCamera = s.disableRagdollCamera = true;
        s.ResetAllToVanilla();
        Require(!s.disableDeathCamera && !s.disableRagdollCamera, "Reset retained a camera suppression toggle");
        Require(s.ApplyTable(toml::table{{"meta", toml::table{{"format", format}}}}) &&
            !s.disableDeathCamera && !s.disableRagdollCamera, "An older preset enabled camera suppression");
    }
    s.ResetAllToVanilla();
}
