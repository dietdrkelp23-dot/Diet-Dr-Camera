void CheckLauncherPresetLifecycle(SettingsManager& s)
{
    const auto originalDirectory = fs::current_path();
    const auto directory = originalDirectory / ("launcher-preset-check-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    fs::current_path(directory);
    struct RestoreDirectory { fs::path path; ~RestoreDirectory() { fs::current_path(path); } } restore{originalDirectory};
    const fs::path config = "Data/SKSE/Plugins/DietDrCamera/DietDrCamera.toml";
    const auto selected = [&]() { return toml::parse_file(config.string())["general"]["active_preset_name"].value_or(std::string{}); };
    auto& pm = PresetManager::GetSingleton();
    s.ResetAllToVanilla();
    s.quickTuneHotkey = 99;
    s.sheathed.zoom = 37;
    Require(pm.SavePreset("Custom"),"Launcher fixture save failed");
    s.Save(); // Existing Save as New UI explicitly flushes preferences.
    s.sheathed.zoom = 0;
    Require(pm.SavePreset("Other"),"Second launcher fixture save failed");
    s.Save();

    // Risa opens/closes the SMF window directly. No Journal close and no
    // save-game message occur between selecting a preset and restarting Skyrim.
    Require(pm.LoadPreset("Custom"),"Launcher preset selection failed");
    Require(selected() == "Custom","Selected preset is forgotten without Journal close / game save");
    s.ResetAllToVanilla(); s.Load();
    Require(s.activePresetName == "Custom" && pm.LoadPreset(s.activePresetName),"Restart did not restore the selected preset");
    Require(s.sheathed.zoom == 37 && s.quickTuneHotkey == 99,"Restart lost tuning or global hotkeys");
    s.sheathed.zoom = 63;
    Require(pm.UpdatePreset("Custom"),"Launcher preset update failed");
    s.ResetAllToVanilla(); s.Load();
    Require(pm.LoadPreset(s.activePresetName) && s.sheathed.zoom == 63,"Updated preset changed on restart");
    Require(pm.RenamePreset("Custom","Renamed Custom") && selected() == "Renamed Custom","Renamed active preset left a stale startup selection");

    const auto before = s.BuildSaveTable();
    const auto prefs = ReadBytes(config);
    const fs::path bad = "Data/SKSE/Plugins/DietDrCamera/Presets/Bad Views.toml";
    std::ofstream(bad) << "[meta]\nformat = 12\n[cinematic_views]\nbroken = true\n";
    Require(!pm.LoadPreset("Bad Views") && s.BuildSaveTable() == before && ReadBytes(config) == prefs,
        "Rejected view data reset the live camera or startup selection");
    Require(pm.DeletePreset("Renamed Custom") && selected().empty(),"Deleted preset remained selected on disk");
}

void CheckPresetReselectionWorkflow(SettingsManager& s)
{
    const auto originalDirectory = fs::current_path();
    const auto directory = originalDirectory / ("preset-reselection-check-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    fs::current_path(directory);
    struct RestoreDirectory { fs::path path; ~RestoreDirectory() { fs::current_path(path); } } restore{originalDirectory};
    const fs::path preset = "Data/SKSE/Plugins/DietDrCamera/Presets/New Camera.toml";
    auto& pm = PresetManager::GetSingleton();
    PresetSelection selection;
    const auto shownPreset = [&]() {
        const auto names = pm.ListPresets();
        const auto index = selection.Resolve(names, s.activePresetName);
        return index < 0 ? std::string{} : names[index];
    };

    // Clicking any preset, including the active one, intentionally reloads
    // its last explicit save. Unsaved edits must never be saved by selection.
    s.ResetAllToVanilla();
    Require(pm.SavePreset("New Camera"), "Reselection fixture save failed");
    Require(shownPreset() == "New Camera", "Save New did not select its detail card");
    const auto originalFile = ReadBytes(preset);
    const auto original = Snapshot(s);
    const auto tune = [&]() {
        s.sheathed.zoom = 37;
        s.sheathed.fov = 106;
        s.IndoorVariantOf(&s.sheathed)->height = 29;
        s.tlSheathed.sideOffset = -48;
        s.globalNoise.amp = 0.42f;
        s.firstPersonGlobal.worldFov = 103;
        s.firstPersonGlobal.handsFov = 86;
        s.deathCameraHoldDuration = 14;
    };
    tune();

    selection.Select("New Camera");
    Require(pm.LoadPreset("New Camera"), "Active preset row reload failed");
    Compare(original, Snapshot(s), "Active preset selection did not discard unsaved edits");
    Require(ReadBytes(preset) == originalFile, "Row selection implicitly overwrote the preset");

    // Update first, then reload: saved tuning must survive both selection
    // and restart. A second Update becomes the new rollback point.
    tune();
    const auto firstUpdate = Snapshot(s);
    Require(pm.UpdatePreset("New Camera"), "First preset update failed");
    s.sheathed.zoom = 91;
    s.firstPersonGlobal.worldFov = 120;
    Require(pm.LoadPreset("New Camera"), "Reload after first Update failed");
    Compare(firstUpdate, Snapshot(s), "Reload lost explicitly updated tuning");
    s.sheathed.zoom = 45;
    s.sheathed.fov = 112;
    const auto latestUpdate = Snapshot(s);
    Require(pm.UpdatePreset("New Camera"), "Second preset update failed");
    const auto updatedFile = ReadBytes(preset);
    s.sheathed.zoom = 94;
    s.firstPersonGlobal.worldFov = 120;
    Require(pm.LoadPreset("New Camera"), "Reload after second Update failed");
    Compare(latestUpdate, Snapshot(s), "Active preset selection did not restore the latest save");
    Require(ReadBytes(preset) == updatedFile, "Reload changed the saved preset file");

    s.sheathed.zoom = 96; // Unsaved tuning is not part of a game save.
    s.Save(); // The game's Save message persists preferences, not camera tuning.
    Require(ReadBytes(preset) == updatedFile, "Saving the game rewrote the preset file");
    s.ResetAllToVanilla();
    s.Load();
    Require(s.activePresetName == "New Camera" && pm.LoadPreset(s.activePresetName),
        "Restart did not restore the updated preset");
    std::cout << "Preset Update/reload/restart: Zoom=" << s.sheathed.zoom
              << " FOV=" << s.sheathed.fov << " FP FOV=" << s.firstPersonGlobal.worldFov << '\n';
    Compare(latestUpdate, Snapshot(s), "Restart lost the latest explicitly saved tuning");
    Require(ReadBytes(preset) == updatedFile, "Startup changed the saved preset file");

    // The same selection action is used by the cycle hotkey. With one preset,
    // cycling back to itself also restores its saved tuning.
    s.sheathed.zoom = 51;
    Require(pm.LoadPreset(pm.ListPresets().front()) && s.sheathed.zoom == 45,
        "Cycling the only preset failed to restore saved tuning");

    // Save as New in either editor makes a different preset active. A cached
    // row index used to leave the old detail card/Update target selected.
    const auto originalUpdatedFile = ReadBytes(preset);
    Require(pm.SavePreset("A Copy"), "Save as New fixture failed");
    Require(shownPreset() == "A Copy", "Save as New left Update pointing at the previous preset");
    s.sheathed.zoom = 64;
    Require(pm.UpdatePreset(shownPreset()), "Update of the newly selected copy failed");
    Require(ReadBytes(preset) == originalUpdatedFile, "Updating the new copy overwrote the original preset");
    Require(pm.RenamePreset("A Copy", "Z Copy"), "Selected copy rename failed");
    Require(shownPreset() == "Z Copy", "Rename/re-sort changed the detail card's preset identity");

    Require(pm.LoadPreset("New Camera") && s.sheathed.zoom == 45,
        "Selecting a different preset did not load its saved tuning");
    Require(shownPreset() == "New Camera", "External preset selection left the detail card on another preset");
    Require(pm.LoadPreset("Z Copy") && s.sheathed.zoom == 64, "Copied preset lost its update");

    // Failed loads retain edits, but their row can still be selected for
    // inspection/deletion. Removing it returns the card to the active preset.
    const fs::path broken = preset.parent_path() / "A Broken.toml";
    std::ofstream(broken) << "invalid = [";
    shownPreset();
    selection.Select("A Broken");
    Require(!pm.LoadPreset("A Broken") && s.sheathed.zoom == 64,
        "Failed row selection changed camera settings");
    Require(shownPreset() == "A Broken", "Failed preset could not be selected for deletion");
    Require(pm.DeletePreset("A Broken") && shownPreset() == "Z Copy",
        "Deleting an inactive row moved the action target to a different preset");
    Require(pm.DeletePreset("Z Copy") && shownPreset().empty(),
        "Deleting the active preset left Update targeting another file");
    Require(pm.LoadPreset("New Camera") && s.sheathed.zoom == 45,
        "Remaining preset changed after deleting the copy");
}
