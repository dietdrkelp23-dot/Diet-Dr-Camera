void CheckDamageReactionPresets(SettingsManager& s)
{
    s.ResetAllToVanilla();
    Require(s.damageReaction.intensity == 0 && s.damageReactionFp.intensity == 0, "Damage reaction default is not silent");
    Require(!s.BuildSaveTable()["cinematic"]["damage_reaction"], "Default damage reaction produced a preset payload");
    const DamageReaction::Tuning third{1.25f};
    const DamageReaction::Tuning first{.65f};
    s.damageReaction = third; s.damageReactionFp = first;
    auto saved = s.BuildSaveTable();
    Require(saved["meta"]["format"].value_or(0) == kCurrentPresetFormat && kCurrentPresetFormat >= 10,
        "New effect was not protected by the preset format");
    s.ResetAllToVanilla();
    Require(s.ApplyTable(saved) && s.damageReaction == third && s.damageReactionFp == first, "Damage reaction POV tuning did not round-trip");
    s.damageReaction.intensity = 0;
    saved = s.BuildSaveTable();
    s.ResetAllToVanilla();
    Require(s.ApplyTable(saved) && s.damageReaction.intensity == 0 && s.damageReactionFp == first,
        "Turning damage reaction off changed the other view");
    Require(s.ApplyTable(toml::parse("[cinematic.damage_reaction.third_person]\nintensity = 100.0\nrecovery = -1.0\ndirection = -2.0\ntexture = 9.0")),
        "Finite damage tuning rejected");
    Require(s.damageReaction == DamageReaction::Tuning{3}, "Damage intensity did not clamp");
    saved = s.BuildSaveTable();
    const auto view = saved["cinematic"]["damage_reaction"]["third_person"];
    Require(view.as_table() && view.as_table()->size() == 1 && view["intensity"].value_or(0.0) == 3,
        "Retired experimental damage controls survived preset saving");
    const auto before = s.BuildSaveTable();
    Require(!s.ApplyTable(toml::parse("[cinematic.damage_reaction.first_person]\nintensity = nan")) && s.BuildSaveTable() == before,
        "Invalid damage tuning changed the live preset");
    s.ResetAllToVanilla();
    Require(s.ApplyTable(toml::parse("[meta]\nformat = 9")) && s.damageReaction.intensity == 0 && s.damageReactionFp.intensity == 0,
        "An old preset enabled incoming damage motion");
}
