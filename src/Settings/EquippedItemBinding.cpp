#include "PCH.h"
#include "Settings/EquippedItemBinding.h"

#include <array>
#include <cmath>

namespace DietDrCamera::ItemBindings
{
    FormIdentity Identify(RE::TESForm* form)
    {
        if (form) if (auto* file = form->GetFile(0)) return {file->fileName, form->GetLocalFormID()};
        return {};
    }

    namespace
    {
        RE::TESForm* Parent(RE::TESForm* form)
        {
            if (auto* weapon = form->As<RE::TESObjectWEAP>()) return weapon->templateWeapon;
            if (auto* armor = form->As<RE::TESObjectARMO>()) return armor->templateArmor;
            return nullptr;
        }

        RE::TESForm* BaseItem(RE::TESForm* form)
        {
            std::array<RE::TESForm*, 32> seen{};
            auto* current = form;
            for (std::size_t i = 0; i < seen.size(); ++i) {
                seen[i] = current;
                auto* parent = Parent(current);
                if (!parent) return current;
                if (parent->GetFormType() != form->GetFormType()) return form;
                for (std::size_t j = 0; j <= i; ++j) if (seen[j] == parent) return form;
                current = parent;
            }
            return form;  // corrupt/deep templates never broaden a binding
        }

        RE::EnchantmentItem* BuiltInEnchantment(RE::TESForm* form)
        {
            if (auto* weapon = form->As<RE::TESObjectWEAP>()) return weapon->formEnchanting;
            if (auto* armor = form->As<RE::TESObjectARMO>()) return armor->formEnchanting;
            return nullptr;
        }

        std::string EnchantmentKey(RE::EnchantmentItem* enchantment)
        {
            if (!enchantment) return {};
            const auto identity = Identify(enchantment);
            if (identity.Valid()) return "form:" + FormKey(identity);
            std::vector<EnchantmentEffect> effects;
            for (const auto* effect : enchantment->effects) {
                if (!effect || !effect->baseEffect || !std::isfinite(effect->effectItem.magnitude)) return {};
                // Generated vanilla enchantments carry plain effects. Unknown
                // conditional effects cannot safely share this signature.
                if (effect->conditions.head) return {};
                effects.push_back({Identify(effect->baseEffect), effect->effectItem.magnitude,
                    effect->effectItem.area, effect->effectItem.duration});
            }
            return EffectSignature(effects, static_cast<std::uint32_t>(enchantment->GetCastingType()),
                static_cast<std::uint32_t>(enchantment->GetDelivery()));
        }
    }

    EquippedItem DescribeForm(RE::TESForm* form)
    {
        EquippedItem item;
        if (!form) return item;
        item.form = Identify(form);
        auto* base = BaseItem(form);
        item.base = Identify(base);
        item.enchantable = form->As<RE::TESObjectWEAP>() || form->As<RE::TESObjectARMO>();
        auto* enchantment = BuiltInEnchantment(form);
        item.enchanted = enchantment != nullptr;
        item.enchantment = EnchantmentKey(enchantment);
        return item;
    }

    EquippedItem DescribeEquipped(RE::Actor* actor, bool leftHand, bool includeNames)
    {
        auto* form = actor ? actor->GetEquippedObject(leftHand) : nullptr;
        auto item = DescribeForm(form);
        if (!form) return item;
        auto* enchantment = BuiltInEnchantment(form);
        auto* entry = actor->GetEquippedEntryData(leftHand);
        // The process supplies a hand-specific entry. Reject stale entries
        // during an equip transition rather than borrow the other item's extras.
        if (entry && entry->object != form) entry = nullptr;
        if (entry && item.enchantable) {
            if (auto* actual = entry->GetEnchantment()) enchantment = actual;
        }
        item.enchanted = enchantment != nullptr;
        item.enchantment = EnchantmentKey(enchantment);
        if (includeNames) {
            auto* base = BaseItem(form);
            const char* name = base->GetName();
            item.baseName = name && *name ? name : "Unnamed Item";
            name = entry ? entry->GetDisplayName() : form->GetName();
            item.displayName = name && *name ? name : item.baseName;
            // Keep the list label free of the engine's tempering suffix. A
            // player-chosen name has an explicit pre-tempering length; do not
            // guess by stripping localized words such as "Legendary".
            name = form->GetName();
            item.variantName = name && *name ? name : item.baseName;
            if (entry && entry->extraLists) for (auto* extra : *entry->extraLists) {
                auto* text = extra ? extra->GetByType<RE::ExtraTextDisplayData>() : nullptr;
                if (text && text->IsPlayerSet() && text->customNameLength > 0) {
                    const std::string custom = text->displayName.c_str();
                    if (text->customNameLength <= custom.size())
                        item.variantName = custom.substr(0, text->customNameLength);
                }
            }
        }
        return item;
    }
}
