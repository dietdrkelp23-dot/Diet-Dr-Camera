void CheckProjectileTracingPresets(SettingsManager& s)
{
    s.ResetAllToVanilla();
    Require(!s.GetProjectileTracing(false).Enabled() && !s.GetProjectileTracing(true).Enabled(), "Tracing defaults must be off in both views");
    Require(!s.BuildSaveTable()["projectile_tracing"], "Default first-person tracing is not sparse");
    Require(s.ApplyTable(toml::parse("[meta]\nformat = 10\n[general]\nspell_tracing_enabled = true\nprojectile_reticle_size_scale = 1.75\nprojectile_reticle_thickness = 3.5\nsneak_meter_offset_x = -300.0\nsneak_meter_offset_y = -50.0")),
        "Legacy shared tracing settings did not load");
    const auto legacy = s.GetProjectileTracing(false);
    Require(s.GetProjectileTracing(true) == legacy && legacy.spellEnabled && !legacy.archeryEnabled && legacy.sneakEyeX == -300,
        "Legacy spell/staff tracing did not retain its reticle and sneak eye in both views");
    s.projectileTracingFp = {true,false,.75f,1.5f,210,-240};
    const auto first = s.projectileTracingFp;
    auto saved = s.BuildSaveTable();
    Require(saved["meta"]["format"].value_or(0) >= 11, "Independent tracing uses an old preset format");
    s.ResetAllToVanilla();
    Require(s.ApplyTable(saved) && s.GetProjectileTracing(false) == legacy && s.projectileTracingFp == first,
        "Tracing values leaked between views during save/load");
    Require(s.BuildSaveTable() == saved, "Independent tracing rewrites are unstable");
    s.projectileTracingFp = {};
    saved = s.BuildSaveTable();
    s.ResetAllToVanilla();
    Require(s.ApplyTable(saved) && s.GetProjectileTracing(false).spellEnabled && !s.GetProjectileTracing(true).Enabled(),
        "First-person off inherited third-person tracing again");
    s.projectileTracingFp = first; s.projectileTracingFp.archeryEnabled = false;
    saved = s.BuildSaveTable();
    const auto disabled = s.projectileTracingFp;
    s.ResetAllToVanilla();
    Require(s.ApplyTable(saved) && s.projectileTracingFp == disabled, "Disabled first-person tracing lost its authored appearance");
    const auto before = s.BuildSaveTable();
    Require(!s.ApplyTable(toml::parse("[projectile_tracing.first_person]\nreticle_size = nan")) && s.BuildSaveTable() == before,
        "Invalid first-person tracing changed the live preset");
    s.ResetAllToVanilla();
    Require(s.projectileTracingFp == ProjectileTracingSettings{}, "Reset retained first-person tracing tuning");
}
