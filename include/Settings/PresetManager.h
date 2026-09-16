#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DietDrCamera
{
    class PresetManager
    {
    public:
        [[nodiscard]] static PresetManager& GetSingleton();

        [[nodiscard]] std::vector<std::string> ListPresets() const;
        // Monotonic counter bumped on every preset mutation. The menu caches
        // ListPresets() and re-scans only when this changes (avoids a per-frame
        // filesystem walk on the Presets panel).
        [[nodiscard]] std::uint32_t GetListGeneration() const;

        bool SavePreset(const std::string& name);
        bool UpdatePreset(const std::string& name);
        // By value, not const ref: LoadPreset's ResetAllToVanilla() clears
        // SettingsManager::activePresetName, so a caller passing that member
        // (e.g. the startup auto-load) would have its argument blanked
        // mid-call. The copy is immune to that aliasing.
        bool LoadPreset(std::string name);
        bool DeletePreset(const std::string& name);
        bool RenamePreset(const std::string& oldName, const std::string& newName);
        [[nodiscard]] const std::string& LastError() const { return _lastError; }

        // Free-form description text persisted in the preset file under
        // [meta]. Read directly from disk so changes by other tools surface,
        // and updated in place without rewriting the entire preset (which
        // would risk overwriting newer settings if the user had loaded but
        // not yet re-saved).
        [[nodiscard]] std::string GetDescription(const std::string& name) const;
        bool SetDescription(const std::string& name, const std::string& description);

    private:
        std::string _lastError;
        PresetManager() = default;
    };
}
