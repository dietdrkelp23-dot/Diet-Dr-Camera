#include "PCH.h"
#include "Settings/PresetManager.h"
#include "Core/AtomicFile.h"
#include "Settings/SettingsManager.h"
#include "Settings/CinematicViewSettings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>

// THERE IS NO SERIALIZATION IN THIS FILE, AND THERE MUST NOT BE.
//
// A preset IS the settings table: SavePreset calls
// SettingsManager::BuildSaveTable and LoadPreset calls
// SettingsManager::ApplyTable. One codec, so every field added to
// SettingsManager flows into presets with no change here.
//
// This file carried ~190 lines of a second, parallel set of Read/Write
// helpers (profiles, enemy overrides, dialogue looks/buckets, an
// InsertNested) left behind when save/load were delegated. Nothing had
// called them for months and they had already rotted out of step — the
// dialogue writer omitted `uid`, which would have orphaned every
// per-location dialogue override, and no profile writer knew about the
// transition family. Deleted 2026-08-25. If preset I/O needs changing,
// change the codec in SettingsManager.cpp. See PRESET-COMPAT.md.

namespace DietDrCamera
{
    static constexpr const char* kPresetsDir = "Data/SKSE/Plugins/DietDrCamera/Presets";

    // Bumped on every preset mutation (save/delete/rename). The menu caches
    // ListPresets() and only re-scans the filesystem when this changes, so the
    // Presets panel doesn't walk the preset dir every frame.
    static std::uint32_t g_presetListGeneration = 0;

    // ── Helpers ────────────────────────────────────────────────────────────

    static bool IsValidFilenameChar(char c)
    {
        static constexpr const char* kInvalid = "\\/:*?\"<>|";
        return static_cast<unsigned char>(c) >= 32 && !std::strchr(kInvalid, c);
    }

    static std::string SanitizeName(const std::string& name)
    {
        std::string out;
        out.reserve(name.size());
        for (char c : name) {
            if (IsValidFilenameChar(c)) out += c;
        }
        // Trim trailing spaces/dots (Windows restriction)
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        return out;
    }

    static std::filesystem::path PresetPath(const std::string& name)
    {
        return std::filesystem::path(kPresetsDir) / std::filesystem::u8path(SanitizeName(name) + ".toml");
    }

    static std::string PathUtf8(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    static bool CanApplyPreset(const toml::table& table)
    {
        std::vector<CinematicViews::View> views;
        return CanReadPreset(table) && CinematicViews::ReadViews(table["cinematic_views"],views,
            [](const toml::table&, CameraProfile&) {}); // Validate shape/identity before resetting current settings.
    }

    // Hotkeys are global keybinds, untied to any preset (quick tune, preset
    // cycle, shoulder swap, dialogue cycle next/prev, death-cam skip). Loading
    // a preset does a full reset + apply, which would otherwise clear or
    // overwrite them — so snapshot every binding before, restore it after.
    struct HotkeySnapshot
    {
        std::uint32_t quickTune, presetCycle, shoulderSwap, dlgNext, dlgPrev, deathSkip,
                      deathFade, ragdollFade, entryCopy, entryPaste;
    };
    static HotkeySnapshot SnapshotHotkeys(const SettingsManager& s)
    {
        return { s.quickTuneHotkey, s.presetCycleNextKey, s.categoriesShoulderSwapKey,
                 s.dialogueCycleNextKey, s.dialogueCyclePrevKey, s.deathCameraSkipKey,
                 s.deathCamFadeKey, s.ragdollCamFadeKey, s.entryCopyKey, s.entryPasteKey };
    }
    static void RestoreHotkeys(SettingsManager& s, const HotkeySnapshot& h)
    {
        s.quickTuneHotkey           = h.quickTune;
        s.presetCycleNextKey        = h.presetCycle;
        s.categoriesShoulderSwapKey = h.shoulderSwap;
        s.dialogueCycleNextKey      = h.dlgNext;
        s.dialogueCyclePrevKey      = h.dlgPrev;
        s.deathCameraSkipKey        = h.deathSkip;
        s.deathCamFadeKey           = h.deathFade;
        s.ragdollCamFadeKey         = h.ragdollFade;
        s.entryCopyKey              = h.entryCopy;
        s.entryPasteKey             = h.entryPaste;
    }

    // ── PresetManager ──────────────────────────────────────────────────────

    PresetManager& PresetManager::GetSingleton()
    {
        static PresetManager instance;
        return instance;
    }

    std::uint32_t PresetManager::GetListGeneration() const
    {
        return g_presetListGeneration;
    }

    std::vector<std::string> PresetManager::ListPresets() const try
    {
        std::vector<std::string> names;

        // Native DDC presets (.toml) — the only preset source.
        if (std::filesystem::exists(kPresetsDir)) {
            for (const auto& entry : std::filesystem::directory_iterator(kPresetsDir)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".toml") continue;
                const auto utf8 = entry.path().stem().u8string();
                names.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            }
        }

        std::sort(names.begin(), names.end());
        return names;
    }
    catch (const std::exception& error) {
        spdlog::error("PresetManager: ListPresets failed: {}", error.what());
        return {};
    }

    bool PresetManager::SavePreset(const std::string& name) try
    {
        ++g_presetListGeneration;
        std::string sanitized = SanitizeName(name);
        if (sanitized.empty()) {
            spdlog::warn("PresetManager: cannot save preset with empty name");
            return false;
        }

        auto path = PresetPath(sanitized);
        if (std::filesystem::exists(path)) {
            spdlog::warn("PresetManager: preset '{}' already exists", sanitized);
            return false;
        }

        std::filesystem::create_directories(kPresetsDir);

        auto& s = SettingsManager::GetSingleton();
        // Delegate to SettingsManager::BuildSaveTable so every settings
        // field that the main config persists also ends up in the preset
        // — weapon bindings, melee overrides + their TL / noise variants,
        // ward enable flags, indoor variants, all of it. Future fields
        // added to SettingsManager auto-flow into presets with no
        // PresetManager change required.
        toml::table root = s.BuildSaveTable();

        std::ostringstream contents;
        contents.exceptions(std::ios::badbit | std::ios::failbit);
        contents << root;
        std::string error;
        if (!WriteFileAtomically(path, contents.str(), false, error)) {
            spdlog::error("PresetManager: failed to save '{}': {}", PathUtf8(path), error);
            return false;
        }
        spdlog::info("PresetManager: saved preset '{}'", sanitized);
        s.activePresetName = sanitized;
        s.Save();
        return true;
    }
    catch (const std::exception& error) {
        spdlog::error("PresetManager: SavePreset failed: {}", error.what());
        return false;
    }

    bool PresetManager::LoadPreset(std::string name) try
    {
        _lastError = "Failed to load preset; see DietDrCamera.log.";
        auto path = PresetPath(name);
        if (!std::filesystem::exists(path)) {
            spdlog::warn("PresetManager: preset '{}' not found", name);
            return false;
        }

        toml::table tbl;
        try {
            tbl = toml::parse_file(PathUtf8(path));
        } catch (const toml::parse_error& e) {
            spdlog::warn("PresetManager: failed to parse preset '{}': {}", name, e.what());
            return false;
        }

        auto& s = SettingsManager::GetSingleton();

        // Hotkeys are system-wide bindings, NOT part of a preset — the user
        // sets them once and they must persist across every preset load
        // (otherwise they'd re-bind on every preset). Preserve all of them
        // across the reset + apply below.
        if (!CanApplyPreset(tbl)) {
            _lastError = "Preset is invalid or needs a newer Diet Dr Camera. Current settings were kept.";
            spdlog::warn("PresetManager: '{}': {}", name, _lastError);
            return false;
        }
        const HotkeySnapshot keepHotkeys = SnapshotHotkeys(s);

        // Reset everything to defaults first. Loading a preset is a full
        // replacement, never a merge — any field the preset doesn't
        // explicitly define falls back to vanilla, not to whatever the
        // previous preset had. (Without this, switching from preset A to
        // preset B would leave A's values for any setting B didn't bother
        // to specify.)
        s.ResetAllToVanilla();
        // Delegate every settings field to SettingsManager::ApplyTable
        // so the preset captures the full state SettingsManager knows
        // about — weapon bindings, melee overrides + TL/Noise variants,
        // indoor overlays, ward flags, every future field. Mirrors the
        // BuildSaveTable delegation in SavePreset above. ApplyTable
        // already calls Validate + the engine-side primer.
        const bool applied = s.ApplyTable(tbl);

        RestoreHotkeys(s, keepHotkeys);   // global keybinds, not preset-scoped
        if (!applied) return false;
        _lastError.clear();
        s.activePresetName = name;
        // Persist the selection at the operation, not a native menu-close
        // event. Launchers such as Risa toggle the SMF window directly, and
        // preset hotkeys do not open the Journal either.
        s.Save();
        spdlog::info("PresetManager: loaded preset '{}'", name);
        return true;
    }
    catch (const std::exception& error) {
        spdlog::error("PresetManager: LoadPreset failed: {}", error.what());
        return false;
    }

    bool PresetManager::DeletePreset(const std::string& name) try
    {
        ++g_presetListGeneration;
        const auto path = PresetPath(name);
        if (!std::filesystem::exists(path)) {
            spdlog::warn("PresetManager: preset '{}' not found for deletion", name);
            return false;
        }

        std::filesystem::remove(path);
        spdlog::info("PresetManager: deleted preset '{}'", name);
        // If we just removed the active preset, clear the pointer so the
        // UI stops showing it as "currently editing" and the Update path
        // reverts to "no active preset" until the user loads/saves another.
        auto& s = SettingsManager::GetSingleton();
        if (s.activePresetName == name || s.activePresetName == SanitizeName(name)) {
            s.activePresetName.clear();
            s.Save();
        }
        return true;
    }
    catch (const std::exception& error) {
        spdlog::error("PresetManager: DeletePreset failed: {}", error.what());
        return false;
    }

    bool PresetManager::RenamePreset(const std::string& oldName, const std::string& newName) try
    {
        ++g_presetListGeneration;
        std::string sanitizedNew = SanitizeName(newName);
        if (sanitizedNew.empty()) {
            spdlog::warn("PresetManager: cannot rename to empty name");
            return false;
        }

        std::filesystem::path oldPath = PresetPath(oldName);
        std::filesystem::path newPath = PresetPath(sanitizedNew);
        const std::string newDisplay = sanitizedNew;

        if (!std::filesystem::exists(oldPath)) {
            spdlog::warn("PresetManager: preset '{}' not found for rename", oldName);
            return false;
        }
        if (std::filesystem::exists(newPath)) {
            spdlog::warn("PresetManager: preset '{}' already exists", sanitizedNew);
            return false;
        }

        std::filesystem::rename(oldPath, newPath);
        spdlog::info("PresetManager: renamed '{}' -> '{}'", oldName, newDisplay);
        auto& s = SettingsManager::GetSingleton();
        if (s.activePresetName == oldName || s.activePresetName == SanitizeName(oldName)) {
            s.activePresetName = newDisplay;
            s.Save();
        }
        return true;
    }
    catch (const std::exception& error) {
        spdlog::error("PresetManager: RenamePreset failed: {}", error.what());
        return false;
    }

    std::string PresetManager::GetDescription(const std::string& name) const try
    {
        auto path = PresetPath(name);
        if (!std::filesystem::exists(path)) return {};
        try {
            auto tbl = toml::parse_file(PathUtf8(path));
            return tbl["meta"]["description"].value_or<std::string>("");
        } catch (...) {
            return {};
        }
    }

    catch (const std::exception& error) {
        spdlog::error("PresetManager: GetDescription failed: {}", error.what());
        return {};
    }

    bool PresetManager::SetDescription(const std::string& name, const std::string& description) try
    {
        auto path = PresetPath(name);
        if (!std::filesystem::exists(path)) return false;
        toml::table tbl;
        try {
            tbl = toml::parse_file(PathUtf8(path));
        } catch (const toml::parse_error& e) {
            spdlog::warn("PresetManager: failed to parse preset '{}' for SetDescription: {}", name, e.what());
            return false;
        }
        if (auto* meta = tbl["meta"].as_table()) {
            if (description.empty()) meta->erase("description");
            else                     meta->insert_or_assign("description", description);
        } else if (!description.empty()) {
            toml::table meta;
            meta.insert("description", description);
            tbl.insert("meta", std::move(meta));
        }
        std::ostringstream contents;
        contents.exceptions(std::ios::badbit | std::ios::failbit);
        contents << tbl;
        std::string error;
        if (!WriteFileAtomically(path, contents.str(), true, error)) {
            spdlog::error("PresetManager: failed to update description '{}': {}", PathUtf8(path), error);
            return false;
        }
        return true;
    }
    catch (const std::exception& error) {
        spdlog::error("PresetManager: SetDescription failed: {}", error.what());
        return false;
    }

    bool PresetManager::UpdatePreset(const std::string& name)
    {
        _lastError = "Preset update failed; the previous file was kept.";
        try {
            const auto path = PresetPath(name);
            const auto previous = toml::parse_file(PathUtf8(path));
            if (!CanApplyPreset(previous)) {
                _lastError = "Preset is invalid or needs a newer Diet Dr Camera. The file was kept.";
                spdlog::warn("PresetManager: '{}': {}", name, _lastError);
                return false;
            }
            auto& settings = SettingsManager::GetSingleton();
            auto table = settings.BuildSaveTable();
            if (const auto description = previous["meta"]["description"].value<std::string>()) {
                table["meta"].as_table()->insert_or_assign("description", *description);
            }
            std::ostringstream contents;
            contents.exceptions(std::ios::badbit | std::ios::failbit);
            contents << table;
            std::string error;
            if (!WriteFileAtomically(path, contents.str(), true, error)) {
                spdlog::error("PresetManager: failed to update '{}': {}", name, error);
                return false;
            }
            ++g_presetListGeneration;
            settings.activePresetName = name;
            settings.Save();
            _lastError.clear();
            spdlog::info("PresetManager: updated preset '{}'", name);
            return true;
        } catch (const std::exception& error) {
            spdlog::error("PresetManager: failed to update '{}': {}", name, error.what());
            return false;
        }
    }
}
