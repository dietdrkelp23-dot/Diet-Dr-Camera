#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DietDrCamera::ItemBindings
{
    // Missing scope in older presets means the original exact-form behavior.
    enum class Scope : std::uint8_t { ExactForm, BaseItem, Enchantment };

    struct FormIdentity {
        std::string plugin;
        std::uint32_t localID{};
        [[nodiscard]] bool Valid() const { return localID != 0 && !plugin.empty(); }
        bool operator==(const FormIdentity&) const = default;
    };

    inline std::string CanonicalPlugin(std::string value)
    {
        for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return value;
    }

    inline std::string FormKey(const FormIdentity& form)
    {
        if (!form.Valid()) return {};
        char number[12];
        std::snprintf(number, sizeof(number), "%08X", form.localID);
        return CanonicalPlugin(form.plugin) + "|" + number;
    }

    struct EquippedItem {
        FormIdentity form;
        FormIdentity base;
        std::string enchantment;
        std::string baseName;
        std::string displayName;
        std::string variantName;
        bool enchantable{};
        bool enchanted{};
    };

    struct EnchantmentEffect {
        FormIdentity effect;
        float magnitude{};
        std::uint32_t area{}, duration{};
    };

    // Dynamic player-created enchantments have no stable plugin/FormID. Describe
    // their authored effects instead. Names, charge, tempering, and runtime IDs
    // are deliberately absent. Full keys avoid hash collisions in saved presets.
    inline std::string EffectSignature(std::span<const EnchantmentEffect> effects,
                                       std::uint32_t casting, std::uint32_t delivery)
    {
        if (effects.empty()) return {};
        std::vector<std::string> parts;
        for (const auto& e : effects) {
            if (!e.effect.Valid() || !std::isfinite(e.magnitude)) return {};
            char data[48];
            const auto magnitude = std::bit_cast<std::uint32_t>(e.magnitude == 0 ? 0.0f : e.magnitude);
            std::snprintf(data, sizeof(data), ":%08X:%08X:%08X", magnitude, e.area, e.duration);
            parts.push_back(FormKey(e.effect) + data);
        }
        std::sort(parts.begin(), parts.end());
        std::string result = "effects-v1:" + std::to_string(casting) + ":" + std::to_string(delivery);
        for (const auto& part : parts) result += ";" + part;
        return result;
    }

    inline bool SameForm(const FormIdentity& a, const FormIdentity& b)
    {
        return a.Valid() && b.Valid() && a.localID == b.localID &&
               CanonicalPlugin(a.plugin) == CanonicalPlugin(b.plugin);
    }

    inline int MatchRank(Scope scope, const FormIdentity& bound,
                         std::string_view enchantment, const EquippedItem& item)
    {
        switch (scope) {
        case Scope::ExactForm: return SameForm(bound, item.form) ? 2 : 0;
        case Scope::BaseItem: return SameForm(bound, item.base) ? 1 : 0;
        case Scope::Enchantment:
            return SameForm(bound, item.base) && !enchantment.empty() && enchantment == item.enchantment ? 3 : 0;
        }
        return 0;
    }

    inline const char* ScopeName(Scope scope)
    {
        switch (scope) {
        case Scope::BaseItem: return "base";
        case Scope::Enchantment: return "enchantment";
        default: return "form";
        }
    }

    inline bool ParseScope(std::string_view text, Scope& scope)
    {
        if (text == "form") scope = Scope::ExactForm;
        else if (text == "base") scope = Scope::BaseItem;
        else if (text == "enchantment") scope = Scope::Enchantment;
        else return false;
        return true;
    }

    inline std::string LocationKey(Scope scope, const FormIdentity& form,
                                   std::string_view enchantment, int slot)
    {
        char tail[32];
        std::snprintf(tail, sizeof(tail), "|%08X.s%d", form.localID, slot);
        // Keep old keys byte-for-byte; existing location overrides still resolve.
        std::string result = "binding." + form.plugin + tail;
        if (scope == Scope::BaseItem) result += "|base";
        if (scope == Scope::Enchantment) result += "|enchantment:" + std::string(enchantment);
        return result;
    }

    // Table is the preset codec's TOML table. Kept generic so the matching
    // helpers themselves have no dependency on the engine or a parser.
    template <class Table, class Binding>
    bool ReadMetadata(const Table& table, Binding& binding)
    {
        if (table.contains("binding_scope") && !table["binding_scope"].is_string()) return false;
        if (!ParseScope(table["binding_scope"].template value_or<std::string>("form"), binding.bindingScope)) return false;
        binding.enchantmentKey = table["enchantment_key"].template value_or<std::string>("");
        if (binding.bindingScope == Scope::Enchantment && binding.enchantmentKey.empty()) return false;
        binding.settingsSeeded = table["settings_seeded"].value_or(false);
        return true;
    }

    template <class Table, class Binding>
    void WriteMetadata(Table& table, const Binding& binding)
    {
        table.insert("binding_scope", ScopeName(binding.bindingScope));
        if (!binding.enchantmentKey.empty()) table.insert("enchantment_key", binding.enchantmentKey);
        if (binding.settingsSeeded) table.insert("settings_seeded", true);
    }

    // Shared by all runtime perspectives/subsystems. Input order breaks ties
    // only between duplicate bindings of exactly the same specificity.
    template <class Bindings>
    auto* FindBest(Bindings& bindings, int category, bool firstPerson, const EquippedItem& item)
    {
        using Binding = typename Bindings::value_type;
        Binding* best = nullptr;
        int rank = 0;
        for (auto& b : bindings) {
            if (static_cast<int>(b.category) != category || b.fpOnly != firstPerson) continue;
            const int candidate = MatchRank(b.bindingScope, {b.pluginName, b.formID}, b.enchantmentKey, item);
            if (candidate > rank) { best = &b; rank = candidate; }
        }
        return best;
    }
}
