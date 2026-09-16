#include "Settings/ItemBindingIdentity.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <limits>
#include <toml++/toml.hpp>

using namespace DietDrCamera::ItemBindings;
namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    struct Binding {
        int category{};
        bool fpOnly{};
        Scope bindingScope{};
        std::string pluginName;
        std::uint32_t formID{};
        std::string enchantmentKey;
        bool settingsSeeded{};
    };
}

int main()
{
    try {
        const FormIdentity dagger{"Skyrim.esm", 0x1397E};
        const FormIdentity enchantedDagger{"Skyrim.esm", 0xA123};
        EquippedItem plain{dagger, dagger};
        EquippedItem frost{enchantedDagger, dagger, "form:skyrim.esm|0000B123"};
        EquippedItem fire{enchantedDagger, dagger, "form:skyrim.esm|0000B456"};
        std::vector<Binding> bindings{
            {0, false, Scope::BaseItem, dagger.plugin, dagger.localID, {}},
            {0, false, Scope::Enchantment, dagger.plugin, dagger.localID, frost.enchantment},
            {0, true, Scope::Enchantment, dagger.plugin, dagger.localID, frost.enchantment},
            {1, false, Scope::Enchantment, dagger.plugin, dagger.localID, frost.enchantment}
        };
        Require(FindBest(bindings, 0, false, plain) == &bindings[0], "plain weapon must use base");
        Require(FindBest(bindings, 0, false, frost) == &bindings[1], "variant must beat base");
        Require(FindBest(bindings, 0, false, fire) == &bindings[0], "other enchantment must use base");
        Require(FindBest(bindings, 0, true, frost) == &bindings[2], "perspectives must remain independent");
        Require(!FindBest(bindings, 0, true, plain), "3p binding must not leak into 1p");
        Require(FindBest(bindings, 1, false, frost) == &bindings[3], "categories must remain independent");
        std::reverse(bindings.begin(), bindings.end());
        Require(FindBest(bindings, 0, false, frost)->bindingScope == Scope::Enchantment, "creation order cannot decide specificity");
        frost.displayName = "Renamed Legendary Dagger";
        frost.baseName = "Localized item name";
        Require(FindBest(bindings, 0, false, frost)->bindingScope == Scope::Enchantment, "names and smithing labels cannot affect matching");
        Require(MatchRank(Scope::ExactForm, dagger, {}, plain) > 0, "legacy bind still matches its original form");
        Require(MatchRank(Scope::ExactForm, dagger, {}, frost) == 0, "legacy preset must not silently widen");
        Require(MatchRank(Scope::BaseItem, {"skyrim.ESM", dagger.localID}, {}, frost) > 0, "plugin case is not identity");
        Require(MatchRank(Scope::BaseItem, {"Other.esp", dagger.localID}, {}, frost) == 0, "same local id from another plugin must not match");
        Require(MatchRank(Scope::Enchantment, dagger, {}, plain) == 0, "missing signature must fail closed");

        std::array<EnchantmentEffect, 2> effects{{{{"Skyrim.esm", 0x111}, 12.0f, 0, 3},
                                                  {{"Magic.esp", 0x222}, 5.0f, 10, 1}}};
        const auto signature = EffectSignature(effects, 1, 2);
        std::reverse(effects.begin(), effects.end());
        Require(EffectSignature(effects, 1, 2) == signature, "effect ordering cannot break a generated enchantment");
        effects[0].magnitude += 1;
        Require(EffectSignature(effects, 1, 2) != signature, "different strength must not match");
        effects[0].magnitude -= 1;
        effects[0].duration += 1;
        Require(EffectSignature(effects, 1, 2) != signature, "different duration must not match");
        effects[0].effect = {};
        Require(EffectSignature(effects, 1, 2).empty(), "unidentifiable effect must fail closed");
        Require(EffectSignature({}, 1, 2).empty(), "empty enchantment must fail closed");
        EquippedItem crafted{dagger, dagger, signature};
        Require(MatchRank(Scope::BaseItem, dagger, {}, crafted) > 0, "enchanting the original item must retain base binding");
        Require(MatchRank(Scope::Enchantment, dagger, signature, crafted) > 0, "generated enchantment must match without runtime form id");

        const auto legacy = LocationKey(Scope::ExactForm, dagger, {}, 3);
        const auto base = LocationKey(Scope::BaseItem, dagger, {}, 3);
        const auto variant = LocationKey(Scope::Enchantment, dagger, signature, 3);
        Require(legacy == "binding.Skyrim.esm|0001397E.s3", "legacy location keys must remain unchanged");
        Require(legacy != base && base != variant && variant != legacy, "binding scopes need independent location settings");
        Require(variant != LocationKey(Scope::Enchantment, dagger, signature, 4), "substates need independent location settings");
        Scope scope{};
        for (auto value : {Scope::ExactForm, Scope::BaseItem, Scope::Enchantment})
            Require(ParseScope(ScopeName(value), scope) && scope == value, "saved scope must round trip");
        Require(!ParseScope("future-unknown", scope), "unknown saved scope must fail closed");
        Binding restored;
        Require(ReadMetadata(toml::table{}, restored) && restored.bindingScope == Scope::ExactForm &&
            !restored.settingsSeeded, "old presets must preserve exact-form matching and opt-in FP behavior");
        Require(!ReadMetadata(toml::parse("binding_scope = 42"), restored), "malformed scope must not widen matching");
        Require(!ReadMetadata(toml::parse("binding_scope = 'unknown'"), restored), "future scope must not widen matching");
        Require(!ReadMetadata(toml::parse("binding_scope = 'enchantment'"), restored), "missing saved enchantment must fail closed");
        Binding saved{0, true, Scope::Enchantment, dagger.plugin, dagger.localID, signature, true};
        toml::table table;
        WriteMetadata(table, saved);
        std::ostringstream stream;
        stream << table;
        Require(ReadMetadata(toml::parse(stream.str()), restored) && restored.bindingScope == saved.bindingScope &&
            restored.enchantmentKey == signature && restored.settingsSeeded, "generated enchantment and FP snapshot metadata must survive TOML round trip");
        effects[0] = {{"Skyrim.esm", 0x111}, std::numeric_limits<float>::quiet_NaN(), 0, 0};
        Require(EffectSignature(effects, 1, 2).empty(), "nonfinite effect must fail closed");
        std::cout << "Item binding checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
