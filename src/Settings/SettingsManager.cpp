#include "PCH.h"
#include "Camera/AnimationCameraController.h"  // RebuildMatchIndex after preset load
#include "Camera/CameraController.h"  // for ResetZoomBaseline on Reset To Vanilla
#include "Camera/StateResolver.h"  // for MagicSchool / CastType enum definitions
#include "Shouts/ShoutRegistry.h"  // 1p Shouts tab: per-shout key resolution
#include "Settings/Defaults.h"
#include "Settings/SettingsManager.h"
#include "Settings/EquippedItemBinding.h"
#include "Settings/OverrideInheritance.h"
#include "Settings/PresetTable.h"
#include "Settings/LocationProfileIdentity.h"
#include "Locations/LocationDetector.h"

#include <algorithm>
#include "Core/AtomicFile.h"
#include "Camera/CameraEffectClock.h"
#include <sstream>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <toml++/toml.hpp>

namespace DietDrCamera
{
    namespace
    {
        constexpr auto kConfigPath = "Data/SKSE/Plugins/DietDrCamera/DietDrCamera.toml";

        // --- Two-band noise fields (Jun-3 rework) ---
        // NoiseProfile (de)serialization is hand-rolled at ~16 sites. These
        // helpers keep the extra fields (drift_jitter / roughness) in sync
        // everywhere so a site can't silently drop one. (The old "breathing"
        // field was removed; any legacy `breathing = ...` key in an existing
        // TOML is simply ignored on load and dropped on the next save.)
        inline void ReadNoiseExtra(const toml::table& t, SettingsManager::NoiseProfile& p)
        {
            ReadOverrideSeed(t, p);
            p.driftJitter       = t["drift_jitter"]      .value_or(p.driftJitter);
            p.roughness         = t["roughness"]         .value_or(p.roughness);
            p.shoutFadeDuration = t["shout_fade_duration"].value_or(p.shoutFadeDuration);
            p.repulse           = t["repulse"]           .value_or(p.repulse);
            p.repulseFeel       = t["repulse_feel"]      .value_or(p.repulseFeel);
            p.attackDuration    = t["attack_duration"]   .value_or(p.attackDuration);
            const bool modernHit = t.contains("hit_shake_speed") || t.contains("hit_shake_bounce") || t.contains("hit_shake_texture");
            const bool legacyHit = t.contains("hit_shake_strength") || t.contains("hit_shake_feel") ||
                                   t.contains("hit_shake_recovery") || t.contains("hit_shake_set");
            if (modernHit) {
                p.hitShake.strength = t["hit_shake_strength"].value_or(p.hitShake.strength);
                p.hitShake.speed = t["hit_shake_speed"].value_or(p.hitShake.speed);
                p.hitShake.bounce = t["hit_shake_bounce"].value_or(p.hitShake.bounce);
                p.hitShake.texture = t["hit_shake_texture"].value_or(p.hitShake.texture);
                p.hitShake.authored = t["hit_shake_set"].value_or(p.hitShake.authored);
            } else if (legacyHit) {
                p.hitShake = HitShake::MigrateLegacy(t["hit_shake_strength"].value_or(0.0f),
                    t["hit_shake_feel"].value_or(0.5f), t["hit_shake_recovery"].value_or(0.30f),
                    t["hit_shake_set"].value_or(false));
            }
            p.hitShake = HitShake::Sanitize(p.hitShake);
        }
        inline void WriteNoiseExtra(toml::table& t, const SettingsManager::NoiseProfile& p)
        {
            WriteOverrideSeed(t, p);
            const SettingsManager::NoiseProfile def;
            if (p.driftJitter != def.driftJitter) t.insert("drift_jitter", static_cast<double>(p.driftJitter));
            if (p.roughness   != def.roughness)   t.insert("roughness",    static_cast<double>(p.roughness));
            if (p.shoutFadeDuration != def.shoutFadeDuration) t.insert("shout_fade_duration", static_cast<double>(p.shoutFadeDuration));
            if (p.repulse != def.repulse) t.insert("repulse", static_cast<double>(p.repulse));
            if (p.repulseFeel != def.repulseFeel) t.insert("repulse_feel", static_cast<double>(p.repulseFeel));
            if (p.attackDuration != def.attackDuration) t.insert("attack_duration", static_cast<double>(p.attackDuration));
            if (HitShake::HasTuning(p.hitShake)) {
                t.insert("hit_shake_strength", static_cast<double>(p.hitShake.strength));
                // Always stamp modern controls, including defaults/explicit
                // zero, so a second load can never migrate this tuning again.
                t.insert("hit_shake_speed", static_cast<double>(p.hitShake.speed));
                t.insert("hit_shake_bounce", static_cast<double>(p.hitShake.bounce));
                t.insert("hit_shake_texture", static_cast<double>(p.hitShake.texture));
                if (p.hitShake.authored) t.insert("hit_shake_set", true);
            }
        }
        // True when the extra fields are all at their defaults â€” folded
        // into each write site's existing "is this profile default?" guard.
        inline bool NoiseExtraIsDefault(const SettingsManager::NoiseProfile& p)
        {
            const SettingsManager::NoiseProfile def;
            return !p.parentSeeded &&
                   p.driftJitter       == def.driftJitter &&
                   p.roughness         == def.roughness &&
                   p.shoutFadeDuration == def.shoutFadeDuration &&
                   p.repulse           == def.repulse &&
                   p.repulseFeel       == def.repulseFeel &&
                   p.attackDuration    == def.attackDuration && p.hitShake == def.hitShake;
        }

        // FirstPersonProfile (de)serialization for the binding fp_profiles
        // tables. Same schema as the [first_person.states.*] entries (the
        // readFp/writeFp lambdas inside Load/BuildSaveTable) so the two
        // stay recognizably one format.
        inline void ReadFpProfileTable(const toml::table& src, SettingsManager::FirstPersonProfile& dst)
        {
            dst.transitionSpeed = src["transition_speed"].value_or(dst.transitionSpeed);
            dst.worldFov        = src["world_fov"].value_or(dst.worldFov);
            dst.handsFov        = src["hands_fov"].value_or(dst.handsFov);
            dst.repulse         = src["repulse"]  .value_or(dst.repulse);
            dst.repulseFeel     = src["repulse_feel"].value_or(dst.repulseFeel);
            dst.fofFadeDuration   = src["fof_fade_duration"]  .value_or(dst.fofFadeDuration);
            dst.shoutFadeDuration = src["shout_fade_duration"].value_or(dst.shoutFadeDuration);
            if (auto* n = src["noise"].as_table()) {
                dst.noise.enabled = (*n)["enabled"].value_or(dst.noise.enabled);
                dst.noise.amp     = (*n)["amp"]    .value_or(dst.noise.amp);
                dst.noise.speed   = (*n)["speed"]  .value_or(dst.noise.speed);
                dst.noise.sway    = (*n)["sway"]   .value_or(dst.noise.sway);
                dst.noise.tilt    = (*n)["tilt"]   .value_or(dst.noise.tilt);
                dst.noise.wobble  = (*n)["wobble"] .value_or(dst.noise.wobble);
                ReadNoiseExtra(*n, dst.noise);
            }
        }
        inline toml::table WriteFpProfileTable(const SettingsManager::FirstPersonProfile& src)
        {
            const SettingsManager::FirstPersonProfile def;
            toml::table out;
            if (src.transitionSpeed != def.transitionSpeed)
                out.insert("transition_speed", static_cast<double>(src.transitionSpeed));
            if (src.worldFov != def.worldFov) out.insert("world_fov", static_cast<double>(src.worldFov));
            if (src.handsFov != def.handsFov) out.insert("hands_fov", static_cast<double>(src.handsFov));
            if (src.repulse  != def.repulse)  out.insert("repulse",   static_cast<double>(src.repulse));
            if (src.repulseFeel != def.repulseFeel)
                out.insert("repulse_feel", static_cast<double>(src.repulseFeel));
            if (src.fofFadeDuration != def.fofFadeDuration)
                out.insert("fof_fade_duration", static_cast<double>(src.fofFadeDuration));
            if (src.shoutFadeDuration != def.shoutFadeDuration)
                out.insert("shout_fade_duration", static_cast<double>(src.shoutFadeDuration));
            toml::table n;
            if (src.noise.enabled != def.noise.enabled) n.insert("enabled", src.noise.enabled);
            if (src.noise.amp     != def.noise.amp)     n.insert("amp",     static_cast<double>(src.noise.amp));
            if (src.noise.speed   != def.noise.speed)   n.insert("speed",   static_cast<double>(src.noise.speed));
            if (src.noise.sway    != def.noise.sway)    n.insert("sway",    static_cast<double>(src.noise.sway));
            if (src.noise.tilt    != def.noise.tilt)    n.insert("tilt",    static_cast<double>(src.noise.tilt));
            if (src.noise.wobble  != def.noise.wobble)  n.insert("wobble",  static_cast<double>(src.noise.wobble));
            WriteNoiseExtra(n, src.noise);
            if (!n.empty()) out.insert("noise", std::move(n));
            return out;
        }

        // TOML keys for the 7 shoutable states. Order must match
        // kShoutableStates in StateResolver.h exactly.
        constexpr const char* kShoutableStateKeys[SettingsManager::kShoutableStateCount] = {
            "sheathed", "melee", "bow", "crossbow", "magic", "staves"
        };

        // TOML keys for the 5 power-attack directions. Order must match
        // StateResolver::PowerAttackDirection exactly:
        // 0=InPlace, 1=Forward, 2=Back, 3=Left, 4=Right.
        constexpr const char* kPADirKeys[SettingsManager::kPowerAttackDirectionCount] = {
            "standing", "forward", "back", "left", "right"
        };


        // TOML keys for each TL slot. Generated by the same X-list as the
        // TLSlot enum so order is guaranteed to match.
        constexpr const char* kTLSlotKeys[] = {
#define DDC_X(name, key, field) key,
            DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X
        };

        // Top-level TOML names for each enemy type. Order matches the
        // EnemyType enum / EnemyTypeIndex() mapping.
        constexpr const char* kEnemyKeys[] = {
            "dragons", "giants", "mammoths", "centurions", "lurkers"
        };

        // Defined below; the enemy-override helpers reuse the profile I/O.
        void ReadProfile(const toml::table& tbl, CameraProfile& profile);
        toml::table WriteProfile(const CameraProfile& profile, const CameraProfile& defaults);

        void ReadEnemyOverrideAt(const toml::table& root, std::string_view path, SettingsManager::EnemyFieldOverride& o)
        {
            const auto* tbl = root.at_path(path).as_table();
            if (!tbl) return;
            ReadProfile(*tbl, o.profile);
            o.fieldsEnabled  = (*tbl)["fields_enabled"].value_or(false);
            // Aim bias is a CameraProfile field now — transition_aim_bias,
            // inside `profile` above.
        }

        bool EnemyOverrideHasContent(const SettingsManager::EnemyFieldOverride& o)
        {
            // A transitions-only override (camera fields untouched) is
            // content too â€” without this it would be pruned/skipped on save.
            return o.fieldsEnabled || o.profile.TransitionAnySet();
        }

        toml::table WriteEnemyOverride(const SettingsManager::EnemyFieldOverride& o)
        {
            // Reuse the standard profile writer (diffs against vanilla), then
            // tack on the enemy-specific fields. Keeps the camera-field schema
            // identical to every other profile in the file.
            toml::table tbl = WriteProfile(o.profile, CameraProfile{});
            if (o.fieldsEnabled)   tbl.insert("fields_enabled", true);
            return tbl;
        }


        void ReadProfile(const toml::table& tbl, CameraProfile& profile)
        {
            ReadOverrideSeed(tbl, profile);
            profile.sideOffset  = tbl["side_offset"].value_or(profile.sideOffset);
            profile.height      = tbl["height"].value_or(profile.height);
            profile.zoom        = tbl["zoom_offset"].value_or(profile.zoom);
            profile.fov         = tbl["fov"].value_or(profile.fov);
            profile.rotation    = tbl["rotation"].value_or(profile.rotation);
            profile.pitchOffset = tbl["pitch_offset"].value_or(profile.pitchOffset);
            profile.transitionOverride = tbl["transition_override"].value_or(profile.transitionOverride);
            profile.transitionRotation = tbl["transition_rotation"].value_or(profile.transitionRotation);
            profile.transitionPitch    = tbl["transition_pitch"].value_or(profile.transitionPitch);
            profile.transitionPosition = tbl["transition_position"].value_or(profile.transitionPosition);
            profile.transitionZoom     = tbl["transition_zoom"].value_or(profile.transitionZoom);
            profile.transitionFOV      = tbl["transition_fov"].value_or(profile.transitionFOV);
            profile.transitionLooseness = tbl["transition_looseness"].value_or(profile.transitionLooseness);
            // transition_pers_* (personalities) were read here until
            // 2026-08-17. The feature is gone; an older preset's keys are
            // simply ignored, which is the intended migration.
            profile.transitionWeight       = tbl["transition_weight"].value_or(profile.transitionWeight);
            profile.shoutLag               = tbl["shout_lag"].value_or(profile.shoutLag);

            profile.transitionSetRotation = tbl["transition_set_rotation"].value_or(profile.transitionSetRotation);
            profile.transitionSetPitch = tbl["transition_set_pitch"].value_or(profile.transitionSetPitch);
            profile.transitionSetPosition = tbl["transition_set_position"].value_or(profile.transitionSetPosition);
            profile.transitionSetZoom = tbl["transition_set_zoom"].value_or(profile.transitionSetZoom);
            profile.transitionSetFOV = tbl["transition_set_fov"].value_or(profile.transitionSetFOV);
            profile.transitionSetLooseness = tbl["transition_set_looseness"].value_or(profile.transitionSetLooseness);
            profile.transitionSetWeight = tbl["transition_set_weight"].value_or(profile.transitionSetWeight);
            profile.transitionSetAimBias = tbl["transition_set_aim_bias"].value_or(profile.transitionSetAimBias);
            profile.transitionAimBias = tbl["transition_aim_bias"].value_or(profile.transitionAimBias);
            profile.SyncTransitionOverride();

        }

        void ReadProfileAt(const toml::table& root, std::string_view path, CameraProfile& profile)
        {
            const auto* node = root.at_path(path).as_table();
            if (node) {
                ReadProfile(*node, profile);
            }
        }

        toml::table WriteProfile(const CameraProfile& profile, const CameraProfile& defaults)
        {
            toml::table tbl;
            WriteOverrideSeed(tbl, profile);
            if (profile.sideOffset  != defaults.sideOffset)  tbl.insert("side_offset",  profile.sideOffset);
            if (profile.height      != defaults.height)      tbl.insert("height",       profile.height);
            if (profile.zoom        != defaults.zoom)        tbl.insert("zoom_offset",  profile.zoom);
            if (profile.fov         != defaults.fov)         tbl.insert("fov",          profile.fov);
            if (profile.rotation    != defaults.rotation)    tbl.insert("rotation",     profile.rotation);
            if (profile.pitchOffset != defaults.pitchOffset) tbl.insert("pitch_offset", profile.pitchOffset);
            if (profile.transitionOverride != defaults.transitionOverride) tbl.insert("transition_override", profile.transitionOverride);
            if (profile.transitionRotation != defaults.transitionRotation) tbl.insert("transition_rotation", static_cast<double>(profile.transitionRotation));
            if (profile.transitionPitch    != defaults.transitionPitch)    tbl.insert("transition_pitch",    static_cast<double>(profile.transitionPitch));
            if (profile.transitionPosition != defaults.transitionPosition) tbl.insert("transition_position", static_cast<double>(profile.transitionPosition));
            if (profile.transitionZoom     != defaults.transitionZoom)     tbl.insert("transition_zoom",     static_cast<double>(profile.transitionZoom));
            if (profile.transitionFOV      != defaults.transitionFOV)      tbl.insert("transition_fov",      static_cast<double>(profile.transitionFOV));
            if (profile.transitionLooseness != defaults.transitionLooseness) tbl.insert("transition_looseness", static_cast<double>(profile.transitionLooseness));
            if (profile.transitionWeight       != defaults.transitionWeight)       tbl.insert("transition_weight",        static_cast<double>(profile.transitionWeight));
            if (profile.shoutLag               != defaults.shoutLag)               tbl.insert("shout_lag",                static_cast<double>(profile.shoutLag));
            // Store enabled channels together. Inactive channels retain their
            // tuned values, including Aim Bias, for later re-enabling.
            if (profile.TransitionAnySet()) {
                tbl.insert("transition_set_rotation",  profile.transitionSetRotation);
                tbl.insert("transition_set_pitch",     profile.transitionSetPitch);
                tbl.insert("transition_set_position",  profile.transitionSetPosition);
                tbl.insert("transition_set_zoom",      profile.transitionSetZoom);
                tbl.insert("transition_set_fov",       profile.transitionSetFOV);
                tbl.insert("transition_set_looseness", profile.transitionSetLooseness);
                tbl.insert("transition_set_weight",    profile.transitionSetWeight);
                tbl.insert("transition_set_aim_bias",  profile.transitionSetAimBias);
            }
            if (profile.transitionAimBias != defaults.transitionAimBias)
                tbl.insert("transition_aim_bias", static_cast<double>(profile.transitionAimBias));
            return tbl;
        }

        // MeleeWeaponOverrides serialization. Each set lives under
        // `<basePath>.overrides.per_weapon.<key>` with an "enabled" marker
        // per slot disambiguating "unset, fall back to base" from "set to
        // vanilla."
        void ReadMeleeOverrides(const toml::table& root, std::string_view basePath, MeleeWeaponOverrides& o)
        {
            // Vector storage must not accumulate across successive loads
            // (arrays get overwritten, vectors would append).
            o.custom.clear();
            const std::string overridesPath = std::string(basePath) + ".overrides";
            const auto* tbl = root.at_path(overridesPath).as_table();
            if (!tbl) return;
            for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
                const auto wt   = static_cast<MeleeWeaponType>(i);
                const auto path = std::string("per_weapon.") + MeleeWeaponTypeTomlKey(wt);
                if (const auto* wtbl = tbl->at_path(path).as_table()) {
                    o.perWeaponSet[i] = (*wtbl)["enabled"].value_or(false);
                    ReadProfile(*wtbl, o.perWeapon[i]);
                }
            }
            // Mod-added weapon-type slots (keyword-keyed; own indoor twin).
            if (const auto* custArr = tbl->at_path("custom").as_array()) {
                for (const auto& node : *custArr) {
                    const auto* ct = node.as_table();
                    if (!ct) continue;
                    CustomMeleeSlot cs;
                    cs.keyword = (*ct)["keyword"].value_or(std::string{});
                    if (cs.keyword.empty()) continue;
                    cs.set = (*ct)["enabled"].value_or(false);
                    ReadProfile(*ct, cs.profile);
                    if (const auto* it2 = ct->at_path("indoor").as_table())
                        ReadProfile(*it2, cs.profileIndoor);
                    o.custom.push_back(std::move(cs));
                }
            }
        }

        toml::table WriteMeleeOverrides(const MeleeWeaponOverrides& o, const CameraProfile& vanilla)
        {
            toml::table tbl;
            toml::table perWeapon;
            for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
                if (!o.perWeaponSet[i] && o.perWeapon[i] == vanilla) continue;
                auto sub = WriteProfile(o.perWeapon[i], vanilla);
                sub.insert("enabled", o.perWeaponSet[i]);
                perWeapon.insert(MeleeWeaponTypeTomlKey(static_cast<MeleeWeaponType>(i)), std::move(sub));
            }
            if (!perWeapon.empty()) tbl.insert("per_weapon", std::move(perWeapon));
            // Mod-added weapon-type slots â€” only enabled ones persist. Diffed
            // against the ZERO profile (not vanilla) so the round-trip into a
            // default-constructed slot is exact.
            toml::array custArr;
            for (const auto& cs : o.custom) {
                if (cs.keyword.empty()) continue;
                auto sub = WriteProfile(cs.profile, CameraProfile{});
                sub.insert("keyword", cs.keyword);
                sub.insert("enabled", cs.set);
                if (cs.profileIndoor != CameraProfile{})
                    sub.insert("indoor", WriteProfile(cs.profileIndoor, CameraProfile{}));
                custArr.push_back(std::move(sub));
            }
            if (!custArr.empty()) tbl.insert("custom", std::move(custArr));
            return tbl;
        }

        // DialogueLook / DialogueBucket TOML round-trip. Each look writes
        // out as a [[looks]] sub-array entry; per-bucket scalar fields
        // (active_index, random_enabled) live alongside.
        toml::table WriteDialogueLook(const DialogueLook& look, bool isSpecificNpcBucket)
        {
            toml::table tbl = WriteProfile(look.profile, CameraProfile{});
            tbl.insert("name", look.name);
            // Written unconditionally once assigned â€” it is the key a place's
            // dialogue override points at, and losing it would orphan the
            // override rather than merely reset a slider.
            if (look.uid != 0)
                tbl.insert("uid", static_cast<std::int64_t>(look.uid));
            if (isSpecificNpcBucket) {
                tbl.insert("target_form_id",      static_cast<std::int64_t>(look.targetFormID));
                tbl.insert("target_plugin_name",  look.targetPluginName);
                tbl.insert("target_display_name", look.targetDisplayName);
            }
            return tbl;
        }

        DialogueLook ReadDialogueLook(const toml::table& tbl)
        {
            DialogueLook look;
            look.name = tbl["name"].value_or<std::string>("Preset");
            look.uid  = static_cast<std::uint32_t>(tbl["uid"].value_or<std::int64_t>(0));
            ReadProfile(tbl, look.profile);
            look.targetFormID        = static_cast<std::uint32_t>(tbl["target_form_id"].value_or<std::int64_t>(0));
            look.targetPluginName    = tbl["target_plugin_name"].value_or<std::string>("");
            look.targetDisplayName   = tbl["target_display_name"].value_or<std::string>("");
            return look;
        }

        toml::table WriteDialogueBucket(const DialogueBucket& bucket, bool isSpecificNpcBucket)
        {
            toml::table tbl;
            if (bucket.activeIndex   != -1)    tbl.insert("active_index",   static_cast<std::int64_t>(bucket.activeIndex));
            if (bucket.randomEnabled)          tbl.insert("random_enabled", true);
            if (!bucket.looks.empty()) {
                toml::array arr;
                for (const auto& look : bucket.looks) {
                    arr.push_back(WriteDialogueLook(look, isSpecificNpcBucket));
                }
                tbl.insert("looks", std::move(arr));
            }
            return tbl;
        }

        void ReadDialogueBucket(const toml::table& tbl, DialogueBucket& bucket)
        {
            bucket.activeIndex   = static_cast<int>(tbl["active_index"].value_or<std::int64_t>(-1));
            bucket.randomEnabled = tbl["random_enabled"].value_or(false);
            bucket.looks.clear();
            if (auto* arr = tbl["looks"].as_array()) {
                for (const auto& el : *arr) {
                    if (auto* lookTbl = el.as_table()) {
                        bucket.looks.push_back(ReadDialogueLook(*lookTbl));
                    }
                }
            }
            if (bucket.activeIndex >= static_cast<int>(bucket.looks.size())) {
                bucket.activeIndex = bucket.looks.empty() ? -1 : 0;
            }
        }

    }

    SettingsManager& SettingsManager::GetSingleton()
    {
        static SettingsManager instance;
        return instance;
    }

    // GetBindingSubStateCount is now constexpr in the header so the
    // preset-compatibility static_asserts can pin it.

    const char* SettingsManager::GetCastTypeSuffix(SpellCastType c)
    {
        switch (c) {
        case SpellCastType::FireAndForget: return " (Fire & Forget)";
        case SpellCastType::Concentration: return " (Concentration)";
        case SpellCastType::Unknown:       return "";
        }
        return "";
    }

    const char* SettingsManager::GetBindingSubStateName(BindingCategory c, std::size_t idx)
    {
        switch (c) {
        // Labels mirror the canonical Categories/Target Lock sub-state names.
        // The binding is already category-specific, so the weapon prefix is
        // dropped (e.g. "Sprinting", not "Bow Sprinting").
        case BindingCategory::Melee: {
            static constexpr const char* k[] = {
                "Unsheathed", "Sprinting", "Swimming", "Normal Attack", "Sneaking",
                "Power Attack", "Sprinting Normal Attack", "Sprinting Power Attack",
                "Sneaking Normal Attack", "Sneaking Power Attack",
                "Standing Power Attack", "Forward Power Attack", "Backwards Power Attack",
                "Left Power Attack", "Right Power Attack"
            };
            return (idx < 15) ? k[idx] : "";
        }
        case BindingCategory::Bow: {
            // 6-7 APPENDED 2026-08-23. Binding sub-state indices are
            // persisted as "s<N>", so they may only ever be added at the end
            // of a category's range â€” never inserted next to Drawing where
            // they read better.
            static constexpr const char* k[] = {
                "Unsheathed", "Sprinting", "Drawing", "Swimming",
                "Sneaking", "Drawing (Sneaking)",
                "Zoomed", "Zoomed (Sneaking)"
            };
            return (idx < 8) ? k[idx] : "";
        }
        case BindingCategory::Crossbow: {
            // 6-7 APPENDED 2026-08-23. Binding sub-state indices are
            // persisted as "s<N>", so they may only ever be added at the end
            // of a category's range â€” never inserted next to Drawing where
            // they read better.
            static constexpr const char* k[] = {
                "Unsheathed", "Sprinting", "Drawing", "Swimming",
                "Sneaking", "Drawing (Sneaking)",
                "Zoomed", "Zoomed (Sneaking)"
            };
            return (idx < 8) ? k[idx] : "";
        }
        case BindingCategory::Spell: {
            static constexpr const char* k[] = {
                "Unsheathed", "Sprinting", "Swimming", "Casting", "Sneaking"
            };
            return (idx < 5) ? k[idx] : "";
        }
        case BindingCategory::Staff: {
            static constexpr const char* k[] = {
                "Unsheathed", "Sprinting", "Swimming", "Sneaking"
            };
            return (idx < 4) ? k[idx] : "";
        }
        case BindingCategory::Shield: {
            static constexpr const char* k[] = {
                "Shield", "Shield (Sneaking)", "Shield (Sprinting)"
            };
            return (idx < 3) ? k[idx] : "";
        }
        case BindingCategory::Shout: {
            static constexpr const char* k[] = {
                "Shouting", "Shouting (Sneaking)"
            };
            return (idx < 2) ? k[idx] : "";
        }
        }
        return "";
    }

    const char* SettingsManager::GetBindingCategoryName(BindingCategory c)
    {
        switch (c) {
        case BindingCategory::Melee:    return "Melee";
        case BindingCategory::Bow:      return "Bow";
        case BindingCategory::Crossbow: return "Crossbow";
        case BindingCategory::Spell:    return "Spell";
        case BindingCategory::Staff:    return "Staff";
        case BindingCategory::Shield:   return "Shield";
        case BindingCategory::Shout:    return "Shout";
        }
        return "";
    }

    const char* SettingsManager::GetMagicHandName(std::size_t h)
    {
        static constexpr const char* k[kMagicHandCount] = {
            "Left Hand", "Both Hands", "Right Hand"
        };
        return (h < kMagicHandCount) ? k[h] : "";
    }

    const char* SettingsManager::GetMagicHandTomlKey(std::size_t h)
    {
        static constexpr const char* k[kMagicHandCount] = { "left", "both", "right" };
        return (h < kMagicHandCount) ? k[h] : "";
    }

    const char* SettingsManager::GetMagicSchoolTomlKey(std::size_t schoolIdx)
    {
        static constexpr const char* k[5] = {
            "alteration", "conjuration", "destruction", "illusion", "restoration"
        };
        return (schoolIdx < 5) ? k[schoolIdx] : "";
    }

    const char* SettingsManager::GetMagicCastTomlKey(std::size_t castIdx)
    {
        static constexpr const char* k[3] = {
            "concentration", "fire_and_forget", "ritual"
        };
        return (castIdx < 3) ? k[castIdx] : "";
    }

    SettingsManager::MagicHandOverrideSet* SettingsManager::GetMagicHandSet(
        MagicSchool school, CastType castType, bool isSneaking, bool a_targetLock)
    {
        // Enum values start at None=0; the grids index from Alteration/
        // Concentration = 0, so shift down by one and bounds-check.
        const int si = static_cast<int>(school) - 1;
        const int ci = static_cast<int>(castType) - 1;
        if (si < 0 || si >= 5 || ci < 0 || ci >= 3) return nullptr;
        auto& grid = a_targetLock ? tlMagicHandOverrides : magicHandOverrides;
        return &grid[si][ci][isSneaking ? 1 : 0];
    }

    int SettingsManager::ResolveSpellBindingHand(const WeaponBinding& b, int a_slot)
    {
        // Per-hand overrides are CASTING-ONLY by design (matching the
        // school grids, which live behind PickMagicProfile's active-cast
        // gate). Only the Casting sub-state (slot 3) resolves a hand, and
        // only from the live cast-hand signal â€” merely HOLDING the bound
        // spell (Unsheathed / Sprinting / Swimming / Sneaking slots) never
        // routes per-hand. Rides the same linger freeze as the school
        // routing so the two agree through a cast's tail.
        if (b.category != BindingCategory::Spell || a_slot != 3) return -1;
        const auto ch = StateResolver::GetSingleton().GetCastingHand();
        return (ch != CastingHand::None) ? static_cast<int>(ch) : -1;
    }

    void SettingsManager::AssignDialogueLookUids()
    {
        // Give every look a stable id, without ever reusing one. Scanning for
        // the max each call (rather than keeping a counter) is what makes this
        // safe to run on a freshly loaded preset: the ids already in the file
        // are authoritative, and a counter reset by a preset switch would hand
        // out an id some other look is already using â€” which, since ids are the
        // key a place's dialogue override points at, would silently transplant
        // one look's per-place framing onto another.
        std::uint32_t maxUid = 0;
        for (const auto& b : dialogueBuckets)
            for (const auto& l : b.looks)
                maxUid = (std::max)(maxUid, l.uid);
        for (auto& b : dialogueBuckets)
            for (auto& l : b.looks)
                if (l.uid == 0) l.uid = ++maxUid;
    }

    void SettingsManager::EnsureDialogueDefaultLooks()
    {
        // The environmental catch-alls (Outdoors, Indoors, Horseback) always
        // carry a default look; the specializations (Dragons, Creatures,
        // Specific NPCs) stay empty so they inherit down the resolver chain.
        // Runs-every-call / never-touches-existing-looks contract: a bucket
        // with looks is the user's and is left alone.
        //
        // Outdoors first — Indoors seeds by COPYING it, so order matters.
        // Without a look here the resolver bottoms out at the vanilla
        // dialogue camera, which is fine at runtime but leaves the menu box
        // with nothing to select or tune.
        auto ensureDefault = [&](DialogueCategory cat, DialoguePOV pov) {
            auto& b = dialogueBuckets[DialogueBucketIndex(cat, pov, DialogueEnv::Outdoor)];
            if (!b.looks.empty()) return;
            b.looks.push_back({ "Default",
                pov == DialoguePOV::FirstPerson ? dialogueFirstPersonProfile
                                                : dialogueProfile });
            b.activeIndex = 0;
        };
        ensureDefault(DialogueCategory::Outdoors, DialoguePOV::ThirdPerson);
        ensureDefault(DialogueCategory::Outdoors, DialoguePOV::FirstPerson);

        // Indoors catch-all. Without this the Indoors bucket starts empty, so
        // the resolver falls back to the Outdoors bucket and talking indoors
        // silently used the outdoor look ("indoor dialogue didn't switch") —
        // and tuning it actually edited the outdoor fallback. Seed from the
        // matching Outdoors bucket (the user's existing setup) when empty.
        // The Indoors category lives at the Outdoor env slot (intrinsic-env).
        auto ensureIndoor = [&](DialoguePOV pov) {
            auto& indoor = dialogueBuckets[DialogueBucketIndex(
                DialogueCategory::Indoors, pov, DialogueEnv::Outdoor)];
            if (!indoor.looks.empty()) return;
            const auto& outdoor = dialogueBuckets[DialogueBucketIndex(
                DialogueCategory::Outdoors, pov, DialogueEnv::Outdoor)];
            indoor.looks       = outdoor.looks;
            indoor.activeIndex = outdoor.activeIndex >= 0 ? outdoor.activeIndex : 0;
        };
        ensureIndoor(DialoguePOV::ThirdPerson);
        ensureIndoor(DialoguePOV::FirstPerson);

        // Horseback (3p) gets a default look too (user request 2026-08-31):
        // the bucket started empty, so mounted dialogue had no preset row
        // until the user created one.
        ensureDefault(DialogueCategory::Horseback, DialoguePOV::ThirdPerson);

        // The Indoors seed copies looks wholesale, so copies share their
        // sources' ids — and ids are the key a place's dialogue override
        // points at, so two looks would claim one place framing. Clear any
        // duplicated id and let the assigner below hand out fresh ones.
        {
            auto& in3 = dialogueBuckets[DialogueBucketIndex(
                DialogueCategory::Indoors, DialoguePOV::ThirdPerson, DialogueEnv::Outdoor)];
            auto& in1 = dialogueBuckets[DialogueBucketIndex(
                DialogueCategory::Indoors, DialoguePOV::FirstPerson, DialogueEnv::Outdoor)];
            auto dedupe = [&](DialogueBucket& b) {
                for (auto& l : b.looks) {
                    if (l.uid == 0) continue;
                    // Does some OTHER look already claim this id?
                    int seen = 0;
                    for (const auto& ob : dialogueBuckets)
                        for (const auto& ol : ob.looks)
                            if (ol.uid == l.uid) ++seen;
                    if (seen > 1) l.uid = 0;
                }
            };
            dedupe(in3);
            dedupe(in1);
        }
        AssignDialogueLookUids();
    }

    CameraProfile* SettingsManager::ActiveLocationDialogueLook(std::uint32_t a_uid)
    {
        return const_cast<CameraProfile*>(static_cast<const SettingsManager&>(*this).ActiveLocationDialogueLook(a_uid));
    }

    const CameraProfile* SettingsManager::ActiveLocationDialogueLook(std::uint32_t a_uid) const
    {
        if (!locationOverridesEnabled || a_uid == 0) return nullptr;
        const std::string key = std::to_string(a_uid);
        // Narrowest place first, same walk every other per-entry lookup uses â€”
        // a place on the inn beats a place on the hold.
        for (int idx : activeLocationChain) {
            if (idx < 0 || idx >= static_cast<int>(locationOverrides.size())) continue;
            auto& lo = locationOverrides[static_cast<std::size_t>(idx)];
            if (!lo.enabled) continue;
            if (auto it = lo.dlgLooks.find(key); it != lo.dlgLooks.end())
                return &it->second;
        }
        return nullptr;
    }

    CameraProfile* SettingsManager::ResolveDialogueProfile(bool a_firstPerson)
    {
        return const_cast<CameraProfile*>(static_cast<const SettingsManager&>(*this).ResolveDialogueProfile(a_firstPerson));
    }

    const CameraProfile* SettingsManager::ResolveDialogueProfile(bool a_firstPerson) const
    {
        if (auto* look = GetActiveDialogueLook()) {
            if (auto* place = ActiveLocationDialogueLook(look->uid)) return place;
            return &look->profile;
        }
        return a_firstPerson ? &dialogueFirstPersonProfile : &dialogueProfile;
    }

    DialogueLook* SettingsManager::GetActiveDialogueLook()
    {
        if (activeDialogueBucketIdx < 0 ||
            activeDialogueBucketIdx >= static_cast<int>(dialogueBuckets.size())) {
            return nullptr;
        }
        auto& bucket = dialogueBuckets[activeDialogueBucketIdx];
        if (activeDialogueLookIdx < 0 ||
            activeDialogueLookIdx >= static_cast<int>(bucket.looks.size())) {
            return nullptr;
        }
        return &bucket.looks[activeDialogueLookIdx];
    }

    const DialogueLook* SettingsManager::GetActiveDialogueLook() const
    {
        if (activeDialogueBucketIdx < 0 ||
            activeDialogueBucketIdx >= static_cast<int>(dialogueBuckets.size())) {
            return nullptr;
        }
        const auto& bucket = dialogueBuckets[activeDialogueBucketIdx];
        if (activeDialogueLookIdx < 0 ||
            activeDialogueLookIdx >= static_cast<int>(bucket.looks.size())) {
            return nullptr;
        }
        return &bucket.looks[activeDialogueLookIdx];
    }

    void SettingsManager::Load()
    {
        if (!std::filesystem::exists(kConfigPath)) {
            // Nothing to apply — the fields are already at their shipped
            // defaults (CameraProfile::Default3p and the member initializers).
            //
            // This used to call SaveDefault(), which wrote a 355-line
            // hand-authored TOML and returned WITHOUT applying it. That blob
            // had drifted years out of step with the code (side 0 / height 0 /
            // zoom 150 / fov 65 against the shipped 30 / -10 / 10 / 80), so a
            // fresh install ran on the real defaults for exactly one launch and
            // then parsed the blob back in on the next one. Removed 2026-08-25:
            // every preset is stored SPARSELY, which means an unstated key
            // resolves to "the current default" — so the defaults have to have
            // exactly one definition, and it is the code. See PRESET-COMPAT.md.
            //
            // The file is created on the next Save() with the [general]
            // globals; camera settings live only in presets.
            //
            // The dialogue catch-alls still need their Default looks — the
            // seeding lives on the ApplyTable path, which a fresh install
            // never reaches, so the very first launch used to start every
            // dialogue preset box empty.
            EnsureDialogueDefaultLooks();
            spdlog::info("No config file yet — using shipped defaults");
            return;
        }

        toml::table tbl;
        try {
            tbl = toml::parse_file(kConfigPath);
        } catch (const toml::parse_error& e) {
            spdlog::warn("Failed to parse config: {}", e.what());
            return;
        }
        ApplyTable(tbl);
    }

    bool SettingsManager::ApplyTable(const toml::table& tbl)
    {
        // Reject before touching any settings, including on direct codec loads.
        if (!CanReadPreset(tbl)) {
            spdlog::warn("Preset is invalid or requires a newer Diet Dr Camera; settings were kept.");
            return false;
        }
        presetFormatLoaded = static_cast<int>(tbl["meta"]["format"].value_or<std::int64_t>(0));
        CameraController::InvalidateProfileReferences();

        // [general]
        cameraCollision.Read(tbl);
        disableCollisionTrees  = tbl["general"]["camera_collision_trees"].value_or(disableCollisionTrees);
        disableCollisionProps  = tbl["general"]["camera_collision_props"].value_or(disableCollisionProps);
        disableCollisionActors = tbl["general"]["camera_collision_actors"].value_or(disableCollisionActors);
        transitionMulRotation = tbl["general"]["transition_mul_rotation"].value_or(transitionMulRotation);
        transitionMulPitch    = tbl["general"]["transition_mul_pitch"].value_or(transitionMulPitch);
        transitionMulPosition = tbl["general"]["transition_mul_position"].value_or(transitionMulPosition);
        transitionMulZoom     = tbl["general"]["transition_mul_zoom"].value_or(transitionMulZoom);
        transitionMulFOV      = tbl["general"]["transition_mul_fov"].value_or(transitionMulFOV);
        transitionWeight      = tbl["general"]["transition_weight"].value_or(transitionWeight);
        // ["transition_coordinate"] and ["transition_arc"] are RETIRED
        // (2026-08-16): Coordinate folded into group-scoped Weight, Arc cut.
        // Old presets carrying either key are simply ignored.
        cameraLooseness       = tbl["general"]["camera_looseness"].value_or(cameraLooseness);
        // ["height_pivot"] is RETIRED (built 2026-08-20, made unconditional the
        // same day, REVERTED 2026-08-21). Ignored on load.
        // ["look_up_limit_deg"] / ["look_down_limit_deg"] are RETIRED
        // (2026-08-20, built and reverted the same day). Ignored on load.
        dragonShakeBreathEnabled    = tbl["cinematic"]["dragons"]["breath_enabled"]   .value_or(dragonShakeBreathEnabled);
        dragonShakeBreathAmp        = tbl["cinematic"]["dragons"]["breath_amp"]       .value_or(dragonShakeBreathAmp);
        dragonShakeBreathSpeed      = tbl["cinematic"]["dragons"]["breath_speed"]     .value_or(dragonShakeBreathSpeed);
        dragonShakeBreathRange      = tbl["cinematic"]["dragons"]["breath_range"]     .value_or(dragonShakeBreathRange);
        dragonShakeProjectileEnabled = tbl["cinematic"]["dragons"]["projectile_enabled"].value_or(dragonShakeProjectileEnabled);
        dragonShakeProjectileAmp    = tbl["cinematic"]["dragons"]["projectile_amp"]   .value_or(dragonShakeProjectileAmp);
        dragonShakeProjectileSpeed  = tbl["cinematic"]["dragons"]["projectile_speed"] .value_or(dragonShakeProjectileSpeed);
        dragonShakeProjectileRange  = tbl["cinematic"]["dragons"]["projectile_range"] .value_or(dragonShakeProjectileRange);
        dragonShakeBiteEnabled      = tbl["cinematic"]["dragons"]["bite_enabled"]     .value_or(dragonShakeBiteEnabled);
        dragonShakeBiteAmp          = tbl["cinematic"]["dragons"]["bite_amp"]         .value_or(dragonShakeBiteAmp);
        dragonShakeBiteSpeed        = tbl["cinematic"]["dragons"]["bite_speed"]       .value_or(dragonShakeBiteSpeed);
        dragonShakeBiteRange        = tbl["cinematic"]["dragons"]["bite_range"]       .value_or(dragonShakeBiteRange);
        dragonShakeTailEnabled      = tbl["cinematic"]["dragons"]["tail_enabled"]     .value_or(dragonShakeTailEnabled);
        dragonShakeTailAmp          = tbl["cinematic"]["dragons"]["tail_amp"]         .value_or(dragonShakeTailAmp);
        dragonShakeTailSpeed        = tbl["cinematic"]["dragons"]["tail_speed"]       .value_or(dragonShakeTailSpeed);
        dragonShakeTailRange        = tbl["cinematic"]["dragons"]["tail_range"]       .value_or(dragonShakeTailRange);
        dragonShakeWingEnabled      = tbl["cinematic"]["dragons"]["wing_enabled"]     .value_or(dragonShakeWingEnabled);
        dragonShakeWingAmp          = tbl["cinematic"]["dragons"]["wing_amp"]         .value_or(dragonShakeWingAmp);
        dragonShakeWingSpeed        = tbl["cinematic"]["dragons"]["wing_speed"]       .value_or(dragonShakeWingSpeed);
        dragonShakeWingRange        = tbl["cinematic"]["dragons"]["wing_range"]       .value_or(dragonShakeWingRange);
        dragonShakeLandingEnabled   = tbl["cinematic"]["dragons"]["landing_enabled"]  .value_or(dragonShakeLandingEnabled);
        dragonShakeLandingAmp      = tbl["cinematic"]["dragons"]["landing_amp"]     .value_or(dragonShakeLandingAmp);
        dragonShakeLandingSpeed    = tbl["cinematic"]["dragons"]["landing_speed"]   .value_or(dragonShakeLandingSpeed);
        dragonShakeLandingRange    = tbl["cinematic"]["dragons"]["landing_range"]   .value_or(dragonShakeLandingRange);
        dragonShakeTakeoffEnabled  = tbl["cinematic"]["dragons"]["takeoff_enabled"] .value_or(dragonShakeTakeoffEnabled);
        dragonShakeTakeoffAmp      = tbl["cinematic"]["dragons"]["takeoff_amp"]     .value_or(dragonShakeTakeoffAmp);
        dragonShakeTakeoffSpeed    = tbl["cinematic"]["dragons"]["takeoff_speed"]   .value_or(dragonShakeTakeoffSpeed);
        dragonShakeTakeoffRange    = tbl["cinematic"]["dragons"]["takeoff_range"]   .value_or(dragonShakeTakeoffRange);
        centurionShakeWalkEnabled  = tbl["cinematic"]["centurion"]["walk_enabled"] .value_or(centurionShakeWalkEnabled);
        centurionShakeWalkAmp      = tbl["cinematic"]["centurion"]["walk_amp"]     .value_or(centurionShakeWalkAmp);
        centurionShakeWalkSpeed    = tbl["cinematic"]["centurion"]["walk_speed"]   .value_or(centurionShakeWalkSpeed);
        centurionShakeWalkRange    = tbl["cinematic"]["centurion"]["walk_range"]   .value_or(centurionShakeWalkRange);
        centurionShakeMeleeEnabled = tbl["cinematic"]["centurion"]["melee_enabled"].value_or(centurionShakeMeleeEnabled);
        centurionShakeMeleeAmp     = tbl["cinematic"]["centurion"]["melee_amp"]    .value_or(centurionShakeMeleeAmp);
        centurionShakeMeleeSpeed   = tbl["cinematic"]["centurion"]["melee_speed"]  .value_or(centurionShakeMeleeSpeed);
        centurionShakeMeleeRange   = tbl["cinematic"]["centurion"]["melee_range"]  .value_or(centurionShakeMeleeRange);
        centurionShakeSteamEnabled = tbl["cinematic"]["centurion"]["steam_enabled"].value_or(centurionShakeSteamEnabled);
        centurionShakeSteamAmp     = tbl["cinematic"]["centurion"]["steam_amp"]    .value_or(centurionShakeSteamAmp);
        centurionShakeSteamSpeed   = tbl["cinematic"]["centurion"]["steam_speed"]  .value_or(centurionShakeSteamSpeed);
        centurionShakeSteamRange   = tbl["cinematic"]["centurion"]["steam_range"]  .value_or(centurionShakeSteamRange);
        werewolfTransformIntensity    = tbl["cinematic"]["werewolf_transform"]["intensity"]    .value_or(werewolfTransformIntensity);
        werewolfTransformSpeed        = tbl["cinematic"]["werewolf_transform"]["speed"]        .value_or(werewolfTransformSpeed);
        vampireLordTransformIntensity = tbl["cinematic"]["vampire_lord_transform"]["intensity"].value_or(vampireLordTransformIntensity);
        vampireLordTransformSpeed     = tbl["cinematic"]["vampire_lord_transform"]["speed"]    .value_or(vampireLordTransformSpeed);
        vampireLordBatsIntensity      = tbl["cinematic"]["vampire_lord_bats"]["intensity"]    .value_or(vampireLordBatsIntensity);
        vampireLordBatsSpeed          = tbl["cinematic"]["vampire_lord_bats"]["speed"]        .value_or(vampireLordBatsSpeed);
        reanimateShakeIntensity       = tbl["cinematic"]["reanimate"]["intensity"]            .value_or(reanimateShakeIntensity);
        reanimateShakeSpeed           = tbl["cinematic"]["reanimate"]["speed"]                .value_or(reanimateShakeSpeed);
        reanimateShakeRange           = tbl["cinematic"]["reanimate"]["range"]                .value_or(reanimateShakeRange);
        summonShakeIntensity          = tbl["cinematic"]["summon"]["intensity"]               .value_or(summonShakeIntensity);
        summonShakeSpeed              = tbl["cinematic"]["summon"]["speed"]                   .value_or(summonShakeSpeed);
        summonShakeRange              = tbl["cinematic"]["summon"]["range"]                   .value_or(summonShakeRange);
        slowTimeNoiseStrength         = tbl["cinematic"]["slow_time"]["strength"]             .value_or(slowTimeNoiseStrength);
        weaponDrawNoiseIntensity      = tbl["cinematic"]["weapon_draw"]["intensity"]          .value_or(weaponDrawNoiseIntensity);
        weaponDrawNoiseDuration       = tbl["cinematic"]["weapon_draw"]["duration"]           .value_or(weaponDrawNoiseDuration);
        weaponDrawNoiseSpeed          = tbl["cinematic"]["weapon_draw"]["speed"]              .value_or(weaponDrawNoiseSpeed);
        // First-person halves. New keys in the source's OWN table, the way
        // head bob carries "intensity_fp"; a preset predating the split says
        // nothing about them and loads the initializers (rule 2).
        weaponDrawNoiseIntensityFp    = tbl["cinematic"]["weapon_draw"]["intensity_fp"]       .value_or(weaponDrawNoiseIntensityFp);
        weaponDrawNoiseDurationFp     = tbl["cinematic"]["weapon_draw"]["duration_fp"]        .value_or(weaponDrawNoiseDurationFp);
        weaponDrawNoiseSpeedFp        = tbl["cinematic"]["weapon_draw"]["speed_fp"]           .value_or(weaponDrawNoiseSpeedFp);
        // ["attack_lag"]["melee"] is retired â€” Attack Lag is projectile-only
        // now. An older preset carrying the key is simply ignored.
        attackLagMagic                = tbl["cinematic"]["attack_lag"]["magic"]                .value_or(attackLagMagic);
        attackLagArchery              = tbl["cinematic"]["attack_lag"]["archery"]              .value_or(attackLagArchery);
        projectileRepulseArchery      = tbl["cinematic"]["projectile_repulse"]["archery"]      .value_or(projectileRepulseArchery);
        projectileRepulseMagic        = tbl["cinematic"]["projectile_repulse"]["magic"]        .value_or(projectileRepulseMagic);
        // ["repulse_character"] is retired â€” the shape lives per entry as
        // `repulse_feel` on the noise profiles now. Old keys are ignored.
        fleeFramingStrength           = tbl["cinematic"]["flee_framing"]["strength"]           .value_or(fleeFramingStrength);
        jumpNoiseAmp                  = tbl["cinematic"]["jumping"]["amp"]                     .value_or(jumpNoiseAmp);
        // Falling split out of Jumping 2026-08-15. Old presets carry only
        // ["amp"], which used to scale the WHOLE arc â€” inherit it so the
        // airborne half doesn't silently go quiet on upgrade.
        // (Read `.value_or(jumpNoiseAmp)` until 2026-08-17 so pre-split
        // presets inherited the combined Jumping value; now its own default.)
        fallNoiseAmp                  = tbl["cinematic"]["jumping"]["fall_amp"]                .value_or(fallNoiseAmp);
        jumpRepulse                   = tbl["cinematic"]["jumping"]["repulse"]                 .value_or(jumpRepulse);
        jumpRepulseFeel               = tbl["cinematic"]["jumping"]["repulse_feel"]            .value_or(jumpRepulseFeel);
        jumpNoiseAmpFp                = tbl["cinematic"]["jumping"]["amp_fp"]                  .value_or(jumpNoiseAmpFp);
        fallNoiseAmpFp                = tbl["cinematic"]["jumping"]["fall_amp_fp"]             .value_or(fallNoiseAmpFp);
        jumpRepulseFp                 = tbl["cinematic"]["jumping"]["repulse_fp"]              .value_or(jumpRepulseFp);
        jumpRepulseFeelFp             = tbl["cinematic"]["jumping"]["repulse_feel_fp"]         .value_or(jumpRepulseFeelFp);
        // Paragliding (mod support): a full camera profile + noise cell under
        // one cinematic key. The profile reuses the standard per-state profile
        // codec; the noise reads the same fields the global cell does.
        paraglideEnabled              = tbl["cinematic"]["paraglide"]["enabled"]               .value_or(paraglideEnabled);
        paraglideLandFade             = tbl["cinematic"]["paraglide"]["land_fade"]             .value_or(paraglideLandFade);
        ReadProfileAt(tbl, "cinematic.paraglide.camera", paraglideProfile);
        ReadProfileAt(tbl, "cinematic.paraglide.target_lock", paraglideTLProfile);
        if (auto* pg = tbl["cinematic"]["paraglide"]["noise"].as_table()) {
            paraglideNoise.amp    = (*pg)["amp"]   .value_or(paraglideNoise.amp);
            paraglideNoise.speed  = (*pg)["speed"] .value_or(paraglideNoise.speed);
            paraglideNoise.sway   = (*pg)["sway"]  .value_or(paraglideNoise.sway);
            paraglideNoise.tilt   = (*pg)["tilt"]  .value_or(paraglideNoise.tilt);
            paraglideNoise.wobble = (*pg)["wobble"].value_or(paraglideNoise.wobble);
            ReadNoiseExtra(*pg, paraglideNoise);
        }
        // Indoor twin: absent table = a copy of the outdoor cell (the same
        // rule the camera indoor variants follow); present table = read from
        // defaults exactly like the outdoor one, so the two never bleed.
        paraglideNoiseIndoor = paraglideNoise;
        if (auto* pi = tbl["cinematic"]["paraglide"]["noise_indoor"].as_table()) {
            paraglideNoiseIndoor = NoiseProfile{};
            paraglideNoiseIndoor.amp    = (*pi)["amp"]   .value_or(paraglideNoiseIndoor.amp);
            paraglideNoiseIndoor.speed  = (*pi)["speed"] .value_or(paraglideNoiseIndoor.speed);
            paraglideNoiseIndoor.sway   = (*pi)["sway"]  .value_or(paraglideNoiseIndoor.sway);
            paraglideNoiseIndoor.tilt   = (*pi)["tilt"]  .value_or(paraglideNoiseIndoor.tilt);
            paraglideNoiseIndoor.wobble = (*pi)["wobble"].value_or(paraglideNoiseIndoor.wobble);
            ReadNoiseExtra(*pi, paraglideNoiseIndoor);
        }
        // First-person halves of the creature shakes (2026-09-06). New keys
        // in each source's OWN table, the head-bob convention; absent = the
        // initializer (PRESET-COMPAT rule 2).
        dragonShakeBreathAmpFp = tbl["cinematic"]["dragons"]["breath_amp_fp"].value_or(dragonShakeBreathAmpFp);
        dragonShakeBreathSpeedFp = tbl["cinematic"]["dragons"]["breath_speed_fp"].value_or(dragonShakeBreathSpeedFp);
        dragonShakeBreathRangeFp = tbl["cinematic"]["dragons"]["breath_range_fp"].value_or(dragonShakeBreathRangeFp);
        dragonShakeProjectileAmpFp = tbl["cinematic"]["dragons"]["projectile_amp_fp"].value_or(dragonShakeProjectileAmpFp);
        dragonShakeProjectileSpeedFp = tbl["cinematic"]["dragons"]["projectile_speed_fp"].value_or(dragonShakeProjectileSpeedFp);
        dragonShakeProjectileRangeFp = tbl["cinematic"]["dragons"]["projectile_range_fp"].value_or(dragonShakeProjectileRangeFp);
        dragonShakeBiteAmpFp = tbl["cinematic"]["dragons"]["bite_amp_fp"].value_or(dragonShakeBiteAmpFp);
        dragonShakeBiteSpeedFp = tbl["cinematic"]["dragons"]["bite_speed_fp"].value_or(dragonShakeBiteSpeedFp);
        dragonShakeBiteRangeFp = tbl["cinematic"]["dragons"]["bite_range_fp"].value_or(dragonShakeBiteRangeFp);
        dragonShakeTailAmpFp = tbl["cinematic"]["dragons"]["tail_amp_fp"].value_or(dragonShakeTailAmpFp);
        dragonShakeTailSpeedFp = tbl["cinematic"]["dragons"]["tail_speed_fp"].value_or(dragonShakeTailSpeedFp);
        dragonShakeTailRangeFp = tbl["cinematic"]["dragons"]["tail_range_fp"].value_or(dragonShakeTailRangeFp);
        dragonShakeWingAmpFp = tbl["cinematic"]["dragons"]["wing_amp_fp"].value_or(dragonShakeWingAmpFp);
        dragonShakeWingSpeedFp = tbl["cinematic"]["dragons"]["wing_speed_fp"].value_or(dragonShakeWingSpeedFp);
        dragonShakeWingRangeFp = tbl["cinematic"]["dragons"]["wing_range_fp"].value_or(dragonShakeWingRangeFp);
        dragonShakeLandingAmpFp = tbl["cinematic"]["dragons"]["landing_amp_fp"].value_or(dragonShakeLandingAmpFp);
        dragonShakeLandingSpeedFp = tbl["cinematic"]["dragons"]["landing_speed_fp"].value_or(dragonShakeLandingSpeedFp);
        dragonShakeLandingRangeFp = tbl["cinematic"]["dragons"]["landing_range_fp"].value_or(dragonShakeLandingRangeFp);
        dragonShakeTakeoffAmpFp = tbl["cinematic"]["dragons"]["takeoff_amp_fp"].value_or(dragonShakeTakeoffAmpFp);
        dragonShakeTakeoffSpeedFp = tbl["cinematic"]["dragons"]["takeoff_speed_fp"].value_or(dragonShakeTakeoffSpeedFp);
        dragonShakeTakeoffRangeFp = tbl["cinematic"]["dragons"]["takeoff_range_fp"].value_or(dragonShakeTakeoffRangeFp);
        centurionShakeWalkAmpFp = tbl["cinematic"]["centurion"]["walk_amp_fp"].value_or(centurionShakeWalkAmpFp);
        centurionShakeWalkSpeedFp = tbl["cinematic"]["centurion"]["walk_speed_fp"].value_or(centurionShakeWalkSpeedFp);
        centurionShakeWalkRangeFp = tbl["cinematic"]["centurion"]["walk_range_fp"].value_or(centurionShakeWalkRangeFp);
        centurionShakeMeleeAmpFp = tbl["cinematic"]["centurion"]["melee_amp_fp"].value_or(centurionShakeMeleeAmpFp);
        centurionShakeMeleeSpeedFp = tbl["cinematic"]["centurion"]["melee_speed_fp"].value_or(centurionShakeMeleeSpeedFp);
        centurionShakeMeleeRangeFp = tbl["cinematic"]["centurion"]["melee_range_fp"].value_or(centurionShakeMeleeRangeFp);
        centurionShakeSteamAmpFp = tbl["cinematic"]["centurion"]["steam_amp_fp"].value_or(centurionShakeSteamAmpFp);
        centurionShakeSteamSpeedFp = tbl["cinematic"]["centurion"]["steam_speed_fp"].value_or(centurionShakeSteamSpeedFp);
        centurionShakeSteamRangeFp = tbl["cinematic"]["centurion"]["steam_range_fp"].value_or(centurionShakeSteamRangeFp);
        reanimateShakeIntensityFp = tbl["cinematic"]["reanimate"]["intensity_fp"].value_or(reanimateShakeIntensityFp);
        reanimateShakeSpeedFp     = tbl["cinematic"]["reanimate"]["speed_fp"]    .value_or(reanimateShakeSpeedFp);
        reanimateShakeRangeFp     = tbl["cinematic"]["reanimate"]["range_fp"]    .value_or(reanimateShakeRangeFp);
        summonShakeIntensityFp    = tbl["cinematic"]["summon"]["intensity_fp"]   .value_or(summonShakeIntensityFp);
        summonShakeSpeedFp        = tbl["cinematic"]["summon"]["speed_fp"]       .value_or(summonShakeSpeedFp);
        summonShakeRangeFp        = tbl["cinematic"]["summon"]["range_fp"]       .value_or(summonShakeRangeFp);
        headBobIntensity              = tbl["cinematic"]["head_bob"]["intensity"]              .value_or(headBobIntensity);
        headBobIntensityFp            = tbl["cinematic"]["head_bob"]["intensity_fp"]           .value_or(headBobIntensityFp);
        stairSmoothStrength           = tbl["cinematic"]["stairs"]["strength"]                 .value_or(stairSmoothStrength);
        stairSmoothLimit              = tbl["cinematic"]["stairs"]["limit"]                    .value_or(stairSmoothLimit);
        combatPulseIntensity          = tbl["cinematic"]["combat_pulse"]["intensity"]          .value_or(combatPulseIntensity);
        combatPulseDuration           = tbl["cinematic"]["combat_pulse"]["duration"]           .value_or(combatPulseDuration);
        npcNoiseIntensity             = tbl["cinematic"]["npc_noise"]["intensity"]              .value_or(npcNoiseIntensity);
        npcNoiseIntensityFp           = tbl["cinematic"]["npc_noise"]["intensity_fp"]           .value_or(npcNoiseIntensityFp);
        const bool oldShoutAmounts = tbl["meta"]["format"].value_or(0) < 5 &&
                                     tbl["cinematic"]["npc_noise"].is_table();
        npcShoutNoiseIntensity = tbl["cinematic"]["npc_noise"]["shout_intensity"].value_or(
            oldShoutAmounts ? npcNoiseIntensity : npcShoutNoiseIntensity);
        npcShoutNoiseIntensityFp = tbl["cinematic"]["npc_noise"]["shout_intensity_fp"].value_or(
            oldShoutAmounts ? npcNoiseIntensityFp : npcShoutNoiseIntensityFp);
        npcMeleeNoiseIntensity = tbl["cinematic"]["npc_noise"]["melee_intensity"].value_or(npcMeleeNoiseIntensity);
        npcMeleeNoiseIntensityFp = tbl["cinematic"]["npc_noise"]["melee_intensity_fp"].value_or(npcMeleeNoiseIntensityFp);
        npcArcheryNoiseIntensity = tbl["cinematic"]["npc_noise"]["archery_intensity"].value_or(npcArcheryNoiseIntensity);
        npcArcheryNoiseIntensityFp = tbl["cinematic"]["npc_noise"]["archery_intensity_fp"].value_or(npcArcheryNoiseIntensityFp);
        // Before format 4, NPC transformation amounts belonged to Magic.
        // Preserve those authored amounts when splitting the control.
        const bool oldNpcAmounts = tbl["meta"]["format"].value_or(0) < 4 &&
                                   tbl["cinematic"]["npc_noise"].is_table();
        npcTransformNoiseIntensity = tbl["cinematic"]["npc_noise"]["transform_intensity"].value_or(
            oldNpcAmounts ? npcNoiseIntensity : npcTransformNoiseIntensity);
        npcTransformNoiseIntensityFp = tbl["cinematic"]["npc_noise"]["transform_intensity_fp"].value_or(
            oldNpcAmounts ? npcNoiseIntensityFp : npcTransformNoiseIntensityFp);
        // Per-source cinematic-shake character (Rotation/Position Shake, Drift,
        // Roughness). Keys live alongside the source's amp/speed/range.
        {
            auto readChar = [&](const char* group, std::string pfx, CinematicShakeChar& c) {
                c.rotShake    = tbl["cinematic"][group][pfx + "_rot_shake"].value_or(c.rotShake);
                c.posShake    = tbl["cinematic"][group][pfx + "_pos_shake"].value_or(c.posShake);
                c.driftJitter  = tbl["cinematic"][group][pfx + "_drift"].value_or(c.driftJitter);
                c.roughness    = tbl["cinematic"][group][pfx + "_rough"].value_or(c.roughness);
                c.fadeDuration = tbl["cinematic"][group][pfx + "_fade"] .value_or(c.fadeDuration);
            };
            readChar("dragons",   "breath",     dragonShakeBreathChar);
            readChar("dragons",   "projectile", dragonShakeProjectileChar);
            readChar("dragons",   "bite",       dragonShakeBiteChar);
            readChar("dragons",   "tail",       dragonShakeTailChar);
            readChar("dragons",   "wing",       dragonShakeWingChar);
            readChar("dragons",   "landing",    dragonShakeLandingChar);
            readChar("dragons",   "takeoff",    dragonShakeTakeoffChar);
            readChar("centurion", "walk",       centurionShakeWalkChar);
            readChar("centurion", "melee",      centurionShakeMeleeChar);
            readChar("centurion", "steam",      centurionShakeSteamChar);
            readChar("werewolf_transform",      "char", werewolfTransformChar);
            readChar("vampire_lord_transform",  "char", vampireLordTransformChar);
            // Revert (the OUT edge), split from the transform. A preset from
            // before the split carries no revert table â€” seed it from the
            // transform values just read, so the old shared-tuning behaviour
            // carries over byte-for-byte until the user edits the new entry.
            if (tbl["cinematic"]["werewolf_revert"].is_table()) {
                werewolfRevertIntensity = tbl["cinematic"]["werewolf_revert"]["intensity"].value_or(werewolfRevertIntensity);
                werewolfRevertSpeed     = tbl["cinematic"]["werewolf_revert"]["speed"]    .value_or(werewolfRevertSpeed);
                readChar("werewolf_revert", "char", werewolfRevertChar);
            } else {
                werewolfRevertIntensity = werewolfTransformIntensity;
                werewolfRevertSpeed     = werewolfTransformSpeed;
                werewolfRevertChar      = werewolfTransformChar;
            }
            if (tbl["cinematic"]["vampire_lord_revert"].is_table()) {
                vampireLordRevertIntensity = tbl["cinematic"]["vampire_lord_revert"]["intensity"].value_or(vampireLordRevertIntensity);
                vampireLordRevertSpeed     = tbl["cinematic"]["vampire_lord_revert"]["speed"]    .value_or(vampireLordRevertSpeed);
                readChar("vampire_lord_revert", "char", vampireLordRevertChar);
            } else {
                vampireLordRevertIntensity = vampireLordTransformIntensity;
                vampireLordRevertSpeed     = vampireLordTransformSpeed;
                vampireLordRevertChar      = vampireLordTransformChar;
            }
            readChar("vampire_lord_bats",       "char", vampireLordBatsChar);
            readChar("reanimate",               "char", reanimateShakeChar);
            readChar("summon",                  "char", summonShakeChar);
            readChar("dragons",   "breath_fp",     dragonShakeBreathCharFp);
            readChar("dragons",   "projectile_fp",     dragonShakeProjectileCharFp);
            readChar("dragons",   "bite_fp",     dragonShakeBiteCharFp);
            readChar("dragons",   "tail_fp",     dragonShakeTailCharFp);
            readChar("dragons",   "wing_fp",     dragonShakeWingCharFp);
            readChar("dragons",   "landing_fp",     dragonShakeLandingCharFp);
            readChar("dragons",   "takeoff_fp",     dragonShakeTakeoffCharFp);
            readChar("centurion", "walk_fp",     centurionShakeWalkCharFp);
            readChar("centurion", "melee_fp",     centurionShakeMeleeCharFp);
            readChar("centurion", "steam_fp",     centurionShakeSteamCharFp);
            readChar("reanimate",               "char_fp", reanimateShakeCharFp);
            readChar("summon",                  "char_fp", summonShakeCharFp);
            readChar("weapon_draw",             "char", weaponDrawNoiseChar);
            readChar("weapon_draw",             "char_fp", weaponDrawNoiseCharFp);
            // Table-driven event beats â€” one loop covers the whole family.
            for (std::size_t i = 0; i < kEventBeatCount; ++i) {
                const char* key = kEventBeatDefs[i].tomlKey;
                auto&       bt  = eventBeats[i];
                bt.intensity = tbl["cinematic"][key]["intensity"].value_or(bt.intensity);
                bt.speed     = tbl["cinematic"][key]["speed"]    .value_or(bt.speed);
                bt.range     = tbl["cinematic"][key]["range"]    .value_or(bt.range);
                bt.direction = tbl["cinematic"][key]["direction"].value_or(bt.direction);
                readChar(key, "char", bt.chr);
            }
        }
        disableVanityCamera = tbl["general"]["disable_vanity_camera"].value_or(disableVanityCamera);

        // Per-menu Show Player In Menus settings. Offsets are
        // CAMERA-relative, matching every other page in the mod.

        auto loadEntry = [&](const char* menuKey, ShowPlayerInMenuEntry& entry) {
            auto sub = tbl["show_player_in_menus"][menuKey];
            entry.enabled       = sub["enabled"]       .value_or(entry.enabled);
            entry.dragonOnly    = sub["dragon_only"]   .value_or(entry.dragonOnly);
            entry.offsetX       = sub["offset_x"]      .value_or(entry.offsetX);
            entry.offsetY       = sub["offset_y"]      .value_or(entry.offsetY);
            entry.offsetZ       = sub["offset_z"]      .value_or(entry.offsetZ);
            entry.yaw           = sub["yaw"]           .value_or(entry.yaw);
            entry.fov           = sub["fov"]           .value_or(entry.fov);
            entry.unpauseGame            = sub["unpause_game"]            .value_or(entry.unpauseGame);
            entry.allowMovement          = sub["allow_movement"]          .value_or(entry.allowMovement);
            entry.allowCameraControl     = sub["allow_camera_control"]    .value_or(entry.allowCameraControl);
            entry.force3pFromFirstPerson = sub["force_3p_from_1p"]        .value_or(entry.force3pFromFirstPerson);
        };
        // Barter's default for allowMovement is FALSE (the dialogue
        // camera wants the player rooted). Setting it before loadEntry
        // means: if the user's TOML has a value, it wins; otherwise
        // Barter blocks movement out of the box.
        showPlayerInBarter.allowMovement = false;
        // (The legacy single-entry migration â€” a top-level "<menu>_enabled"
        // bool plus shared offset/yaw/fov â€” lived here; removed 2026-08-17.)
        loadEntry("inventory", showPlayerInInventory);
        loadEntry("container", showPlayerInContainer);
        loadEntry("barter",    showPlayerInBarter);
        loadEntry("magic",     showPlayerInMagic);
        loadEntry("tween",     showPlayerInTween);
        loadEntry("wait",      showPlayerInWait);
        loadEntry("favorites", showPlayerInFavorites);
        // Non-Barter rows merged into a single "Allow Movement" toggle
        // â€” keep the underlying allowCameraControl mirrored to
        // allowMovement so the combined toggle's display matches what
        // the runtime gates read. Barter intentionally has no camera
        // control (dialogue camera owns the framing).
        showPlayerInInventory.allowCameraControl = showPlayerInInventory.allowMovement;
        showPlayerInContainer.allowCameraControl = showPlayerInContainer.allowMovement;
        showPlayerInMagic    .allowCameraControl = showPlayerInMagic    .allowMovement;
        showPlayerInTween    .allowCameraControl = showPlayerInTween    .allowMovement;
        showPlayerInWait     .allowCameraControl = showPlayerInWait     .allowMovement;
        showPlayerInFavorites.allowCameraControl = showPlayerInFavorites.allowMovement;
        showPlayerInBarter   .allowCameraControl = false;
        archeryTracingEnabled   = tbl["general"]["archery_tracing_enabled"].value_or(archeryTracingEnabled);
        spellTracingEnabled     = tbl["general"]["spell_tracing_enabled"].value_or(spellTracingEnabled);
        projectileReticleColorR      = tbl["general"]["projectile_reticle_color_r"].value_or(projectileReticleColorR);
        projectileReticleColorG      = tbl["general"]["projectile_reticle_color_g"].value_or(projectileReticleColorG);
        projectileReticleColorB      = tbl["general"]["projectile_reticle_color_b"].value_or(projectileReticleColorB);
        projectileReticleSizeScale   = tbl["general"]["projectile_reticle_size_scale"].value_or(projectileReticleSizeScale);
        projectileReticleThickness   = tbl["general"]["projectile_reticle_thickness"].value_or(projectileReticleThickness);
        archeryTracingSmoothTau = tbl["general"]["archery_tracing_smooth_tau"].value_or(archeryTracingSmoothTau);
        sneakMeterOffsetX       = tbl["general"]["sneak_meter_offset_x"].value_or(sneakMeterOffsetX);
        sneakMeterOffsetY       = tbl["general"]["sneak_meter_offset_y"].value_or(sneakMeterOffsetY);
        // First-person settings. Schema:
        //   [first_person] transition_speed
        //   [first_person.global] world_fov, hands_fov + .noise
        //   [first_person.states.<dotted-categories-key>] same shape
        {
            // First Person is always enabled now â€” the per-subsystem
            // Enable toggles were removed. Pin both true regardless of any
            // legacy persisted value so old presets (which only wrote these
            // keys when true) come up fully enabled.
            firstPersonFovEnabled   = true;
            firstPersonNoiseEnabled = true;
            // first_person.spell_level_scaling is intentionally not read â€”
            // the feature was removed 2026-08-01. Old presets keep the key
            // harmlessly until their next save.
            firstPersonTransitionSpeed = tbl["first_person"]["transition_speed"]
                .value_or(firstPersonTransitionSpeed);

            const auto readFp = [](const toml::table& src, FirstPersonProfile& dst) {
                dst.transitionSpeed = src["transition_speed"].value_or(dst.transitionSpeed);
                dst.worldFov        = src["world_fov"].value_or(dst.worldFov);
                dst.handsFov        = src["hands_fov"].value_or(dst.handsFov);
                dst.repulse         = src["repulse"]  .value_or(dst.repulse);
                dst.repulseFeel     = src["repulse_feel"].value_or(dst.repulseFeel);
                dst.fofFadeDuration   = src["fof_fade_duration"]  .value_or(dst.fofFadeDuration);
                dst.shoutFadeDuration = src["shout_fade_duration"].value_or(dst.shoutFadeDuration);
                if (auto* n = src["noise"].as_table()) {
                    dst.noise.enabled = (*n)["enabled"].value_or(dst.noise.enabled);
                    dst.noise.amp     = (*n)["amp"]    .value_or(dst.noise.amp);
                    dst.noise.speed   = (*n)["speed"]  .value_or(dst.noise.speed);
                    dst.noise.sway    = (*n)["sway"]   .value_or(dst.noise.sway);
                    dst.noise.tilt    = (*n)["tilt"]   .value_or(dst.noise.tilt);
                    dst.noise.wobble  = (*n)["wobble"] .value_or(dst.noise.wobble);
                    ReadNoiseExtra(*n, dst.noise);
                }
            };

            if (auto* g = tbl["first_person"]["global"].as_table()) {
                readFp(*g, firstPersonGlobal);
            }
            if (auto* states = tbl["first_person"]["states"].as_table()) {
                for (auto&& [key, node] : *states) {
                    if (auto* sub = node.as_table()) {
                        FirstPersonProfile p;
                        readFp(*sub, p);
                        stateFirstPerson.emplace(std::string{key.str()}, p);
                    }
                }
            }
            // Per-weapon-type FP overrides, keyed by the same dotted state
            // key. Each weapon slot stores independent FOV/Noise enables
            // plus a full FirstPersonProfile (reuses readFp).
            if (auto* mo = tbl["first_person"]["melee_overrides"].as_table()) {
                for (auto&& [key, node] : *mo) {
                    auto* stateTbl = node.as_table();
                    if (!stateTbl) continue;
                    auto* pw = (*stateTbl)["per_weapon"].as_table();
                    if (!pw) continue;
                    FpMeleeWeaponOverrides ov;
                    for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
                        const auto wt = static_cast<MeleeWeaponType>(i);
                        if (auto* sub = (*pw)[MeleeWeaponTypeTomlKey(wt)].as_table()) {
                            ov.perWeaponSetFov[i]   = (*sub)["enabled_fov"]  .value_or(false);
                            ov.perWeaponSetNoise[i] = (*sub)["enabled_noise"].value_or(false);
                            readFp(*sub, ov.perWeapon[i]);
                        }
                    }
                    if (auto* cu = (*stateTbl)["custom"].as_table()) {
                        for (auto&& [ck, cnode] : *cu) {
                            if (auto* sub = cnode.as_table()) {
                                FpCustomMeleeSlot cs;
                                cs.keyword  = std::string{ ck.str() };
                                cs.setFov   = (*sub)["enabled_fov"]  .value_or(false);
                                cs.setNoise = (*sub)["enabled_noise"].value_or(false);
                                readFp(*sub, cs.profile);
                                ov.custom.push_back(std::move(cs));
                            }
                        }
                    }
                    stateFpMeleeOverrides.emplace(std::string{key.str()}, std::move(ov));
                }
            }
        }
        deathCameraFov              = tbl["general"]["death_camera_fov"].value_or(deathCameraFov);
        deathCameraHoldDuration     = tbl["general"]["death_camera_hold_duration"].value_or(deathCameraHoldDuration);
        deathCameraInfiniteDuration = tbl["general"]["death_camera_infinite_duration"].value_or(deathCameraInfiniteDuration);
        deathCameraFreeLook         = tbl["general"]["death_camera_free_look"].value_or(deathCameraFreeLook);
        deathCameraSkipKey      = static_cast<std::uint32_t>(
            tbl["general"]["death_camera_skip_key"].value_or<std::int64_t>(deathCameraSkipKey));
        deathCameraSlowmoStrength = tbl["general"]["death_camera_slowmo_strength"].value_or(deathCameraSlowmoStrength);
        deathCameraSlowmoDuration = tbl["general"]["death_camera_slowmo_duration"].value_or(deathCameraSlowmoDuration);
        deathCamFadeKey           = static_cast<std::uint32_t>(
            tbl["general"]["death_cam_fade_key"].value_or<std::int64_t>(deathCamFadeKey));
        ragdollCamFreeLook        = tbl["general"]["ragdoll_cam_free_look"].value_or(ragdollCamFreeLook);
        ragdollCamFov             = tbl["general"]["ragdoll_cam_fov"].value_or(ragdollCamFov);
        ragdollCamSlowmoStrength  = tbl["general"]["ragdoll_cam_slowmo_strength"].value_or(ragdollCamSlowmoStrength);
        ragdollCamSlowmoDuration  = tbl["general"]["ragdoll_cam_slowmo_duration"].value_or(ragdollCamSlowmoDuration);
        ragdollCamFadeKey         = static_cast<std::uint32_t>(
            tbl["general"]["ragdoll_cam_fade_key"].value_or<std::int64_t>(ragdollCamFadeKey));
        ragdollCamHoldParentState = tbl["general"]["ragdoll_cam_hold_parent_state"].value_or(ragdollCamHoldParentState);
        verboseLogging          = tbl["general"]["verbose_logging"].value_or(verboseLogging);
        targetLockAimBias       = tbl["general"]["target_lock_aim_bias"].value_or(targetLockAimBias);
        // Historical key name â€” it stores LOOSENESS (see the field comment).
        targetLockTrackSeconds = tbl["general"]["target_lock_acquire_seconds"].value_or(targetLockTrackSeconds);
        targetLockAcquireSwingSeconds = tbl["general"]["target_lock_acquire_swing_seconds"].value_or(targetLockAcquireSwingSeconds);
        targetLockSwitchSpeed = tbl["general"]["target_lock_switch_speed"].value_or(targetLockSwitchSpeed);
        // dialogueEnabled / dialogueFirstPersonEnabled are always on now (no
        // toggle); the legacy TOML keys are intentionally not loaded.
        dialogueMovementEnabled    = tbl["general"]["dialogue_movement_enabled"].value_or(dialogueMovementEnabled);
        dialogueMulRotation        = tbl["general"]["dialogue_mul_rotation"].value_or(dialogueMulRotation);
        dialogueMulPitch           = tbl["general"]["dialogue_mul_pitch"]   .value_or(dialogueMulPitch);
        dialogueMulPosition        = tbl["general"]["dialogue_mul_position"].value_or(dialogueMulPosition);
        dialogueMulZoom            = tbl["general"]["dialogue_mul_zoom"]    .value_or(dialogueMulZoom);
        dialogueMulFOV             = tbl["general"]["dialogue_mul_fov"]     .value_or(dialogueMulFOV);
        dialogueRandomEnabled      = tbl["general"]["dialogue_random_enabled"].value_or(dialogueRandomEnabled);
        for (std::size_t i = 0; i < dialogueRandomOnEntry.size(); ++i) {
            const std::string keyE = std::string("dialogue_random_on_entry_") + DialogueCategoryKey(static_cast<DialogueCategory>(i));
            const std::string keyO = std::string("dialogue_random_on_option_") + DialogueCategoryKey(static_cast<DialogueCategory>(i));
            dialogueRandomOnEntry[i]  = tbl["general"][keyE].value_or(false);
            dialogueRandomOnOption[i] = tbl["general"][keyO].value_or(false);
        }
        dialogueSwitchOnNpcLine    = tbl["general"]["dialogue_switch_on_npc_line"] .value_or(dialogueSwitchOnNpcLine);
        dialogueSkipShortNpcLines  = tbl["general"]["dialogue_skip_short_npc_lines"].value_or(dialogueSkipShortNpcLines);
        dialogueAutoSwitchMin      = tbl["general"]["dialogue_auto_switch_min"]    .value_or(dialogueAutoSwitchMin);
        dialogueAutoSwitchMax      = tbl["general"]["dialogue_auto_switch_max"]    .value_or(dialogueAutoSwitchMax);
        dialogueRandomEveryTime    = tbl["general"]["dialogue_random_every_time"]  .value_or(dialogueRandomEveryTime);
        dialogueMinShotSec         = tbl["general"]["dialogue_min_shot_sec"]       .value_or(dialogueMinShotSec);
        dialogueSchemaVersion      = static_cast<int>(tbl["general"]["dialogue_schema_version"].value_or<std::int64_t>(1));
        dialogueCycleNextKey       = static_cast<std::uint32_t>(tbl["general"]["dialogue_cycle_next_key"].value_or<std::int64_t>(0));
        dialogueCyclePrevKey       = static_cast<std::uint32_t>(tbl["general"]["dialogue_cycle_prev_key"].value_or<std::int64_t>(0));
        categoriesShoulderSwapKey  = static_cast<std::uint32_t>(tbl["general"]["categories_shoulder_swap_key"].value_or<std::int64_t>(0));
        presetCycleNextKey         = static_cast<std::uint32_t>(tbl["general"]["preset_cycle_next_key"].value_or<std::int64_t>(0));
        entryCopyKey               = static_cast<std::uint32_t>(tbl["general"]["entry_copy_key"] .value_or<std::int64_t>(0));
        entryPasteKey              = static_cast<std::uint32_t>(tbl["general"]["entry_paste_key"].value_or<std::int64_t>(0));
        quickTuneHotkey            = static_cast<std::uint32_t>(tbl["general"]["quick_tune_hotkey"].value_or<std::int64_t>(0));
        activePresetName           = tbl["general"]["active_preset_name"].value_or<std::string>("");
        ReadProfileAt(tbl, "dialogue",              dialogueProfile);
        ReadProfileAt(tbl, "dialogue.first_person", dialogueFirstPersonProfile);

        // Dialogue buckets. env=Outdoor lives at the base path (no env
        // suffix); env=Indoor at a `.indoor` sub-path, and only for
        // Dragons / Creatures / SpecificNPC (Outdoors/Indoors categories
        // ignore env at runtime).
        for (std::size_t cat = 0; cat < static_cast<std::size_t>(DialogueCategory::Count); ++cat) {
            const auto catE = static_cast<DialogueCategory>(cat);
            // Horseback is deliberately NOT env-aware: it is a third value of
            // the Outdoor/Indoor toggle, not a category that splits by one.
            const bool envAware =
                catE == DialogueCategory::Dragons   ||
                catE == DialogueCategory::Creatures ||
                catE == DialogueCategory::SpecificNPC;
            for (std::size_t pov = 0; pov < static_cast<std::size_t>(DialoguePOV::Count); ++pov) {
                const auto povE = static_cast<DialoguePOV>(pov);
                const std::string basePath = std::string("dialogue.buckets.")
                    + DialogueCategoryKey(catE) + "."
                    + DialoguePOVKey(povE);
                // env=Outdoor at the base path (no suffix)
                {
                    const auto idx = DialogueBucketIndex(catE, povE, DialogueEnv::Outdoor);
                    if (auto* bucketTbl = tbl.at_path(basePath).as_table()) {
                        ReadDialogueBucket(*bucketTbl, dialogueBuckets[idx]);
                    }
                }
                // env=Indoor â€” only for env-aware categories, at `.indoor`
                if (envAware) {
                    const auto idx = DialogueBucketIndex(catE, povE, DialogueEnv::Indoor);
                    const std::string path = basePath + "." + DialogueEnvKey(DialogueEnv::Indoor);
                    if (auto* bucketTbl = tbl.at_path(path).as_table()) {
                        ReadDialogueBucket(*bucketTbl, dialogueBuckets[idx]);
                    }
                }
            }
        }

        // (The v1->v2 and v2->v3 dialogue migrations lived here; removed
        // 2026-08-17 in the release cleanup â€” no preset outside this machine
        // was ever written in either old shape. dialogueSchemaVersion itself
        // is KEPT and still written: it is the hook a POST-release migration
        // would need, and adding it back later can't retroactively label
        // presets already in the wild.)
        dialogueSchemaVersion = 3;

        // Seed the environmental catch-alls (Outdoors / Indoors / Horseback)
        // and stamp uids. Shared with the fresh-install and Reset All paths
        // so every route into a runnable state agrees on which boxes start
        // with a Default preset (they didn't: fresh installs seeded nothing,
        // Reset All seeded everything but Horseback, loads seeded everything
        // but Outdoors).
        EnsureDialogueDefaultLooks();
        // Master noise toggle is gone; force on regardless of TOML so an
        // old config that had it off doesn't suppress shake when the
        // user hasn't touched the values since.
        (void)tbl["noise"]["enabled"].value_or(false);
        noiseEnabled = true;

        // Global noise profile â€” applies to every state whose own
        // customization is OFF (or absent from the per-state map).
        // Indoor variant lives under "global_indoor" â€” picked by the
        // runtime when s.indoorMode is true.
        auto readGlobal = [&](const char* key, NoiseProfile& gp) {
            if (auto* g = tbl["noise"][key].as_table()) {
                gp.amp    = (*g)["amp"]   .value_or(gp.amp);
                gp.speed  = (*g)["speed"] .value_or(gp.speed);
                gp.sway   = (*g)["sway"]  .value_or(gp.sway);
                gp.tilt   = (*g)["tilt"]  .value_or(gp.tilt);
                gp.wobble = (*g)["wobble"].value_or(gp.wobble);
                ReadNoiseExtra(*g, gp);
            }
        };
        readGlobal("global",        globalNoise);
        readGlobal("global_indoor", globalNoiseIndoor);

        // Per-state customizations: walk every saved entry under
        // [noise.states.*] and [noise.states_indoor.*]. Keys mirror the
        // dotted tomlKeys produced by GetIndoorEligibleProfiles
        // (e.g. "weapons.melee.sprint"). Existence of an entry means
        // the user opened it once; the entry's `enabled` flag controls
        // whether it overrides global.
        auto readStates = [&](const char* key,
                              std::unordered_map<std::string, NoiseProfile>& dst) {
            if (auto* states = tbl["noise"][key].as_table()) {
                for (auto&& [k, node] : *states) {
                    if (auto* sub = node.as_table()) {
                        NoiseProfile p;
                        p.enabled = (*sub)["enabled"].value_or(false);
                        p.amp     = (*sub)["amp"]    .value_or(0.0f);
                        p.speed   = (*sub)["speed"]  .value_or(0.0f);
                        p.sway    = (*sub)["sway"]   .value_or(0.0f);
                        p.tilt    = (*sub)["tilt"]   .value_or(0.0f);
                        p.wobble  = (*sub)["wobble"] .value_or(0.0f);
                        ReadNoiseExtra(*sub, p);
                        dst.emplace(std::string{k.str()}, p);
                    }
                }
            }
        };
        readStates("states",        stateNoise);
        readStates("states_indoor", stateNoiseIndoor);

        // Per-weapon-type noise overrides â€” parallel to stateNoise but
        // keyed by Melee bucket then by weapon-type. Path:
        // [noise.melee_overrides.<base_key>.per_weapon.<weapon_key>].
        {
            auto readMNO = [&](std::string_view basePath, MeleeWeaponNoiseOverrides& o) {
                const std::string path = std::string("noise.melee_overrides.") + std::string(basePath) + ".per_weapon";
                // The writer stores dotted state names as literal table keys.
                const auto* pw = tbl["noise"]["melee_overrides"][basePath]["per_weapon"].as_table();
                if (!pw) pw = tbl.at_path(path).as_table(); // older nested-key files
                if (!pw) return;
                for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
                    const auto wt = static_cast<MeleeWeaponType>(i);
                    if (const auto* sub = pw->at_path(MeleeWeaponTypeTomlKey(wt)).as_table()) {
                        o.perWeaponSet[i] = (*sub)["enabled"].value_or(false);
                        NoiseProfile& p   = o.perWeapon[i];
                        p.enabled = (*sub)["np_enabled"].value_or(false);
                        p.amp     = (*sub)["amp"]    .value_or(0.0f);
                        p.speed   = (*sub)["speed"]  .value_or(0.0f);
                        p.sway    = (*sub)["sway"]   .value_or(0.0f);
                        p.tilt    = (*sub)["tilt"]   .value_or(0.0f);
                        p.wobble  = (*sub)["wobble"] .value_or(0.0f);
                        ReadNoiseExtra(*sub, p);
                    }
                }
            };
            readMNO("weapons.melee",         weaponsMeleeNoiseOverrides);
            readMNO("weapons.melee.sprint",  weaponsMeleeSprintNoiseOverrides);
            readMNO("weapons.melee.swim",    weaponsMeleeSwimNoiseOverrides);
            readMNO("weapons.melee.attack",      weaponsMeleeAttackNoiseOverrides);
            readMNO("weapons.melee.sneak",       weaponsMeleeSneakNoiseOverrides);
            readMNO("weapons.melee.shout",       weaponsMeleeShoutNoiseOverrides);
            readMNO("weapons.melee.shout.sneak", weaponsMeleeShoutSneakNoiseOverrides);
            readMNO("weapons.melee.power_attack",        weaponsMeleePowerAttackNoiseOverrides);
            readMNO("weapons.melee.sneak_attack",        weaponsMeleeSneakAttackNoiseOverrides);
            readMNO("weapons.melee.sneak_power_attack",  weaponsMeleeSneakPowerAttackNoiseOverrides);
            readMNO("weapons.melee.sprint_attack",       weaponsMeleeSprintAttackNoiseOverrides);
            readMNO("weapons.melee.sprint_power_attack", weaponsMeleeSprintPowerAttackNoiseOverrides);
            for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                readMNO(std::string("weapons.melee.power_attack.dir.") + kPADirKeys[d],
                        weaponsMeleePowerAttackDirNoiseOverrides[d]);
            }
        }



        // Extras
        ReadProfileAt(tbl, "extras.vanity", vanityCamera);
        vanityIdleSeconds = tbl["extras"]["vanity"]["idle_seconds"].value_or(120.0f);
        if (!std::isfinite(vanityIdleSeconds)) vanityIdleSeconds = 120.0f;
        vanityIdleSeconds = std::clamp(vanityIdleSeconds, 5.0f, 600.0f);

        // Sheathed
        ReadProfileAt(tbl, "sheathed",        sheathed);
        ReadProfileAt(tbl, "sheathed.sprint", sheathedSprint);
        ReadProfileAt(tbl, "sheathed.swim",   sheathedSwim);
        ReadProfileAt(tbl, "sheathed.sneak",  sheathedSneak);

        // Weapons â€” Melee
        ReadProfileAt(tbl, "weapons.melee",         weaponsMelee);
        ReadProfileAt(tbl, "weapons.melee.sprint",  weaponsMeleeSprint);
        ReadProfileAt(tbl, "weapons.melee.swim",    weaponsMeleeSwim);
        ReadProfileAt(tbl, "weapons.melee.attack",  weaponsMeleeAttack);
        ReadProfileAt(tbl, "weapons.melee.sneak",   weaponsMeleeSneak);
        ReadProfileAt(tbl, "weapons.melee.power_attack",        weaponsMeleePowerAttack);
        ReadProfileAt(tbl, "weapons.melee.sneak_attack",        weaponsMeleeSneakAttack);
        ReadProfileAt(tbl, "weapons.melee.sneak_power_attack",  weaponsMeleeSneakPowerAttack);
        ReadProfileAt(tbl, "weapons.melee.sprint_attack",       weaponsMeleeSprintAttack);
        ReadProfileAt(tbl, "weapons.melee.sprint_power_attack", weaponsMeleeSprintPowerAttack);
        ReadMeleeOverrides(tbl, "weapons.melee",         weaponsMeleeOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.sprint",  weaponsMeleeSprintOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.swim",    weaponsMeleeSwimOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.attack",      weaponsMeleeAttackOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.sneak",       weaponsMeleeSneakOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.shout",       weaponsMeleeShoutOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.shout.sneak", weaponsMeleeShoutSneakOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.power_attack",        weaponsMeleePowerAttackOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.sneak_attack",        weaponsMeleeSneakAttackOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.sneak_power_attack",  weaponsMeleeSneakPowerAttackOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.sprint_attack",       weaponsMeleeSprintAttackOverrides);
        ReadMeleeOverrides(tbl, "weapons.melee.sprint_power_attack", weaponsMeleeSprintPowerAttackOverrides);

        // Directional power-attack overrides (Categories). Mirrors the shouts
        // override layout: per-direction tables with optional enabled=true.
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            const std::string key = std::string("weapons.melee.power_attack.dir.") + kPADirKeys[d];
            ReadProfileAt(tbl, key, weaponsMeleePowerAttackDir[d]);
            if (auto t = tbl.at_path(key).as_table()) {
                if (auto en = (*t)["enabled"].as_boolean()) {
                    weaponsMeleePowerAttackDirEnabled[d] = en->get();
                }
            }
            ReadMeleeOverrides(tbl, key, weaponsMeleePowerAttackDirOverrides[d]);
        }

        // Per-specific-form bindings: weapons.bound_forms[] (includes
        // the category; every entry lands in weaponBindings).

        weaponBindings.clear();
        auto readBindingEntry = [&](const toml::table& sub, BindingCategory defaultCat) {
            WeaponBinding b;
            b.formID      = static_cast<std::uint32_t>(sub["form_id"].value_or<std::int64_t>(0));
            b.pluginName  = sub["plugin"]  .value_or<std::string>(std::string{});
            b.displayName = sub["display"] .value_or<std::string>(std::string{});
            if (!ItemBindings::ReadMetadata(sub, b)) return;
            const auto rawCat = sub["category"].value_or<std::int64_t>(
                static_cast<std::int64_t>(defaultCat));
            if (rawCat >= 0 && rawCat < static_cast<std::int64_t>(kBindingCategoryCount)) {
                b.category = static_cast<BindingCategory>(rawCat);
            } else {
                b.category = defaultCat;
            }
            const auto rawCast = sub["cast_type"].value_or<std::int64_t>(0);
            if (rawCast >= 0 && rawCast <= 2) {
                b.castType = static_cast<SpellCastType>(rawCast);
            }
            if (const auto* profs = sub.at_path("profiles").as_table()) {
                for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                    char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                    if (const auto* pt = profs->at_path(key).as_table()) {
                        b.enabled[i] = (*pt)["enabled"].value_or(false);
                        ReadProfile(*pt, b.profiles[i]);
                    }
                }
            }
            if (const auto* tlProfs = sub.at_path("tl_profiles").as_table()) {
                for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                    char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                    if (const auto* pt = tlProfs->at_path(key).as_table()) {
                        b.tlEnabled[i] = (*pt)["enabled"].value_or(false);
                        ReadProfile(*pt, b.tlProfiles[i]);
                    }
                }
            }
            if (const auto* noiseProfs = sub.at_path("noise_profiles").as_table()) {
                for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                    char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                    if (const auto* pt = noiseProfs->at_path(key).as_table()) {
                        b.noiseEnabled[i] = (*pt)["enabled"].value_or(false);
                        auto& np = b.noiseProfiles[i];
                        np.enabled = (*pt)["np_enabled"].value_or(np.enabled);
                        np.amp     = (*pt)["amp"]       .value_or(np.amp);
                        np.speed   = (*pt)["speed"]     .value_or(np.speed);
                        np.sway    = (*pt)["sway"]      .value_or(np.sway);
                        np.tilt    = (*pt)["tilt"]      .value_or(np.tilt);
                        np.wobble  = (*pt)["wobble"]    .value_or(np.wobble);
                        ReadNoiseExtra(*pt, np);
                    }
                }
            }
            // Indoor variants (enabled flag shared with outdoor). Written as
            // one reader per kind, called once per environment, so a new
            // environment can never be half-wired.
            {
                const auto readEnvCam =
                    [&](const char* tblKey,
                        std::array<CameraProfile, kWeaponBindingSubStates>& dst,
                        std::array<bool, kWeaponBindingSubStates>& en) {
                    const auto* t = sub.at_path(tblKey).as_table();
                    if (!t) return;
                    for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                        char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                        if (const auto* pt = t->at_path(key).as_table()) {
                            en[i] = (*pt)["enabled"].value_or(en[i]);
                            ReadProfile(*pt, dst[i]);
                        }
                    }
                };
                readEnvCam("profiles_indoor",    b.profilesIndoor,   b.enabled);
                readEnvCam("tl_profiles_indoor", b.tlProfilesIndoor, b.tlEnabled);

                const auto readEnvNoise =
                    [&](const char* tblKey,
                        std::array<NoiseProfile, kWeaponBindingSubStates>& dst) {
                    const auto* t = sub.at_path(tblKey).as_table();
                    if (!t) return;
                    for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                        char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                        if (const auto* pt = t->at_path(key).as_table()) {
                            b.noiseEnabled[i] = (*pt)["enabled"].value_or(b.noiseEnabled[i]);
                            auto& np = dst[i];
                            np.enabled = (*pt)["np_enabled"].value_or(np.enabled);
                            np.amp     = (*pt)["amp"]       .value_or(np.amp);
                            np.speed   = (*pt)["speed"]     .value_or(np.speed);
                            np.sway    = (*pt)["sway"]      .value_or(np.sway);
                            np.tilt    = (*pt)["tilt"]      .value_or(np.tilt);
                            np.wobble  = (*pt)["wobble"]    .value_or(np.wobble);
                            ReadNoiseExtra(*pt, np);
                        }
                    }
                };
                readEnvNoise("noise_profiles_indoor", b.noiseProfilesIndoor);
            }
            // Per-hand overrides (Spell bindings) â€” six hand tables, each
            // shaped [<left|both|right>.s<N>]. Camera tables carry the
            // shared "enabled" marker (outdoor table wins; indoor repeats
            // it harmlessly); noise tables carry "enabled" (the hand
            // layer's own flag) plus the standard noise fields.
            {
                const auto readHandCam =
                    [&](const char* tblKey,
                        std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount>& dst,
                        std::array<std::array<bool, kWeaponBindingSubStates>, kMagicHandCount>& en) {
                    const auto* ht = sub.at_path(tblKey).as_table();
                    if (!ht) return;
                    for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                        const auto* hs = ht->at_path(GetMagicHandTomlKey(h)).as_table();
                        if (!hs) continue;
                        for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                            char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                            if (const auto* pt = hs->at_path(key).as_table()) {
                                en[h][i] = (*pt)["enabled"].value_or(en[h][i]);
                                ReadProfile(*pt, dst[h][i]);
                            }
                        }
                    }
                };
                readHandCam("hand_profiles",           b.handProfiles,         b.handEnabled);
                readHandCam("hand_profiles_indoor",    b.handProfilesIndoor,   b.handEnabled);
                readHandCam("hand_tl_profiles",        b.handTlProfiles,       b.handTlEnabled);
                readHandCam("hand_tl_profiles_indoor", b.handTlProfilesIndoor, b.handTlEnabled);
                const auto readHandNoise =
                    [&](const char* tblKey,
                        std::array<std::array<NoiseProfile, kWeaponBindingSubStates>, kMagicHandCount>& dst) {
                    const auto* ht = sub.at_path(tblKey).as_table();
                    if (!ht) return;
                    for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                        const auto* hs = ht->at_path(GetMagicHandTomlKey(h)).as_table();
                        if (!hs) continue;
                        for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                            char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                            if (const auto* pt = hs->at_path(key).as_table()) {
                                b.handNoiseEnabled[h][i] = (*pt)["enabled"].value_or(b.handNoiseEnabled[h][i]);
                                auto& np = dst[h][i];
                                np.enabled = (*pt)["np_enabled"].value_or(np.enabled);
                                np.amp     = (*pt)["amp"]   .value_or(np.amp);
                                np.speed   = (*pt)["speed"] .value_or(np.speed);
                                np.sway    = (*pt)["sway"]  .value_or(np.sway);
                                np.tilt    = (*pt)["tilt"]  .value_or(np.tilt);
                                np.wobble  = (*pt)["wobble"].value_or(np.wobble);
                                ReadNoiseExtra(*pt, np);
                            }
                        }
                    }
                };
                readHandNoise("hand_noise",        b.handNoise);
                readHandNoise("hand_noise_indoor", b.handNoiseIndoor);
            }
            // First Person profiles (per sub-state, opt-in via each entry's
            // customize flags).
            if (const auto* fpProfs = sub.at_path("fp_profiles").as_table()) {
                for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                    char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                    if (const auto* pt = fpProfs->at_path(key).as_table()) {
                        ReadFpProfileTable(*pt, b.fpProfiles[i]);
                    }
                }
            }
            // Per-(enemy, sub-state) enemy overrides for this binding.
            for (std::size_t e = 0; e < kEnemyOverrideEnemies; ++e) {
                for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                    char path[64];
                    std::snprintf(path, sizeof(path), "tl_enemy_overrides.%s.s%zu", kEnemyKeys[e], i);
                    ReadEnemyOverrideAt(sub, path, b.tlEnemyOverrides[e][i]);
                    std::snprintf(path, sizeof(path), "tl_enemy_overrides_indoor.%s.s%zu",
                                  kEnemyKeys[e], i);
                    ReadEnemyOverrideAt(sub, path, b.tlEnemyOverridesIndoor[e][i]);
                }
            }
            // Per-CUSTOM-enemy overrides owned by this binding.
            if (const auto* custArr = sub.at_path("custom_enemy_overrides").as_array()) {
                for (const auto& node : *custArr) {
                    const auto* gt = node.as_table();
                    if (!gt) continue;
                    WeaponBinding::CustomEnemyGrid g;
                    g.identityKey = (*gt)["key"].value_or(std::string{});
                    if (g.identityKey.empty()) continue;
                    auto readArr = [&](const char* tblKey,
                                       std::array<EnemyFieldOverride, kWeaponBindingSubStates>& dst) {
                        for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                            char path[32];
                            std::snprintf(path, sizeof(path), "%s.s%zu", tblKey, i);
                            ReadEnemyOverrideAt(*gt, path, dst[i]);
                        }
                    };
                    readArr("slots",        g.slots);
                    readArr("slots_indoor", g.slotsIndoor);
                    b.customEnemyGrids.push_back(std::move(g));
                }
            }
            // Indoor variants default to VANILLA (not a copy of the configured
            // outdoor), exactly like the base Melee tab â€” a fresh weapon shows
            // vanilla indoors until the user edits it or hits Copy Outdoor To
            // Indoor. Only the camera profile needs a seed: its empty {} is all
            // zeros (FOV 0 etc.), so an enabled slot with an untouched indoor
            // gets the real vanilla baseline. TL ({}) and noise (zero) already
            // ARE their vanilla defaults.
            for (std::size_t i = 0; i < kWeaponBindingSubStates; ++i) {
                // Bound weapons are always authoritative (no enable toggle) and
                // own their full set, seeded to the shipped third-person
                // default. This used to seed VanillaCombat, because the struct
                // default was the sheathed framing and a binding showing that
                // in a combat state was wrong. The authored defaults
                // (2026-08-22) collapsed both into one baseline, so the seed and
                // the struct default are now the same value and these lines are
                // no-ops â€” kept because they still express the intent, and they
                // start doing work again the moment the two diverge.
                if (b.profiles[i]         == CameraProfile{}) b.profiles[i]         = CameraProfile::Default3p();
                if (b.profilesIndoor[i]   == CameraProfile{}) b.profilesIndoor[i]   = CameraProfile::Default3p();
                if (b.tlProfiles[i]       == CameraProfile{}) b.tlProfiles[i]       = CameraProfile::Default3p();
                if (b.tlProfilesIndoor[i] == CameraProfile{}) b.tlProfilesIndoor[i] = CameraProfile::Default3p();
            }
            // Every regular sub-state is ALWAYS ACTIVE at its (seeded)
            // defaults â€” the per-slot Enable Override toggles were removed,
            // so normalize the flags true on load (also migrates configs
            // saved when the toggles existed: a previously-disabled
            // sub-state now owns its vanilla-seeded set instead of
            // inheriting the generic state). Only the melee DIRECTIONAL
            // power-attack slots (10-14) stay optional â€” the Power Attack
            // base blankets the directions unless one is overridden, same
            // as the main Melee tab.
            {
                // An fpOnly binding (created from the First Person tab) is
                // exempt from the normalize: forcing its 3p enables true here
                // is exactly what kept resurrecting the "FP bind changes my
                // 3p framing" leak on every preset load.
                b.fpOnly = sub.at_path("fp_only").value_or(b.fpOnly);
                if (!b.fpOnly) {
                    const std::size_t n = GetBindingSubStateCount(b.category);
                    for (std::size_t i = 0; i < n && i < kWeaponBindingSubStates; ++i) {
                        if (b.category == BindingCategory::Melee && i >= 10) continue;
                        b.enabled[i]      = true;
                        b.tlEnabled[i]    = true;
                        b.noiseEnabled[i] = true;
                    }
                } else {
                    const std::size_t n = GetBindingSubStateCount(b.category);
                    for (std::size_t i = 0; i < n && i < kWeaponBindingSubStates; ++i) {
                        b.enabled[i]      = false;
                        b.tlEnabled[i]    = false;
                        b.noiseEnabled[i] = false;
                    }
                }
            }
            if (b.formID != 0 && !b.pluginName.empty()) {
                weaponBindings.push_back(std::move(b));
            }
        };
        if (const auto* arr = tbl.at_path("weapons.bound_forms").as_array()) {
            weaponBindings.reserve(arr->size());
            for (auto&& node : *arr) {
                if (const auto* sub = node.as_table()) {
                    readBindingEntry(*sub, BindingCategory::Melee);
                }
            }
        }


        // Weapons — Blocking (9 sub-states).
        {
            CameraProfile* const blockSlots[9] = {
                &weaponsBlockingOneHanded,
                &weaponsBlockingTwoHanded,
                &weaponsBlockingShield,
                &weaponsBlockingWard,
                &weaponsBlockingOneHandedSneak,
                &weaponsBlockingTwoHandedSneak,
                &weaponsBlockingShieldSneak,
                &weaponsBlockingWardSneak,
                &weaponsBlockingShieldSprint,
            };
            constexpr const char* kBlockKeys[9] = {
                "weapons.blocking.one_handed",
                "weapons.blocking.two_handed",
                "weapons.blocking.shield",
                "weapons.blocking.ward",
                "weapons.blocking.one_handed.sneak",
                "weapons.blocking.two_handed.sneak",
                "weapons.blocking.shield.sneak",
                "weapons.blocking.ward.sneak",
                "weapons.blocking.shield.sprint",
            };
            for (std::size_t i = 0; i < 9; ++i) {
                ReadProfileAt(tbl, kBlockKeys[i], *blockSlots[i]);
            }
        }

        // Weapons â€” Bow
        ReadProfileAt(tbl, "weapons.bow",        weaponsBow);
        ReadProfileAt(tbl, "weapons.bow.sprint", weaponsBowSprint);
        ReadProfileAt(tbl, "weapons.bow.swim",   weaponsBowSwim);
        ReadProfileAt(tbl, "weapons.bow.draw",   weaponsBowDraw);
        ReadProfileAt(tbl, "weapons.bow.sneak",  weaponsBowSneak);
        ReadProfileAt(tbl, "weapons.bow.sneak.draw", weaponsBowSneakDraw);
        ReadProfileAt(tbl, "weapons.bow.zoom",       weaponsBowZoom);
        ReadProfileAt(tbl, "weapons.bow.sneak.zoom", weaponsBowSneakZoom);

        // Weapons â€” Crossbow
        ReadProfileAt(tbl, "weapons.crossbow",        weaponsCrossbow);
        ReadProfileAt(tbl, "weapons.crossbow.sprint", weaponsCrossbowSprint);
        ReadProfileAt(tbl, "weapons.crossbow.swim",   weaponsCrossbowSwim);
        ReadProfileAt(tbl, "weapons.crossbow.draw",   weaponsCrossbowDraw);
        ReadProfileAt(tbl, "weapons.crossbow.sneak",  weaponsCrossbowSneak);
        ReadProfileAt(tbl, "weapons.crossbow.sneak.draw", weaponsCrossbowSneakDraw);
        ReadProfileAt(tbl, "weapons.crossbow.zoom",       weaponsCrossbowZoom);
        ReadProfileAt(tbl, "weapons.crossbow.sneak.zoom", weaponsCrossbowSneakZoom);

        // Weapons â€” Magic base
        ReadProfileAt(tbl, "weapons.magic",        weaponsMagic);
        ReadProfileAt(tbl, "weapons.magic.sprint", weaponsMagicSprint);
        ReadProfileAt(tbl, "weapons.magic.swim",   weaponsMagicSwim);
        ReadProfileAt(tbl, "weapons.magic.sneak",  weaponsMagicSneak);

        // Magic schools â€” Alteration
        ReadProfileAt(tbl, "weapons.magic.alteration.concentration",  magicAlterationConcentration);
        ReadProfileAt(tbl, "weapons.magic.alteration.fire_and_forget", magicAlterationFireAndForget);
        ReadProfileAt(tbl, "weapons.magic.alteration.ritual",         magicAlterationRitual);

        // Magic schools â€” Conjuration
        ReadProfileAt(tbl, "weapons.magic.conjuration.concentration",  magicConjurationConcentration);
        ReadProfileAt(tbl, "weapons.magic.conjuration.fire_and_forget", magicConjurationFireAndForget);
        ReadProfileAt(tbl, "weapons.magic.conjuration.ritual",         magicConjurationRitual);

        // Magic schools â€” Destruction
        ReadProfileAt(tbl, "weapons.magic.destruction.concentration",  magicDestructionConcentration);
        ReadProfileAt(tbl, "weapons.magic.destruction.fire_and_forget", magicDestructionFireAndForget);
        ReadProfileAt(tbl, "weapons.magic.destruction.ritual",         magicDestructionRitual);

        // Magic schools â€” Illusion
        ReadProfileAt(tbl, "weapons.magic.illusion.concentration",  magicIllusionConcentration);
        ReadProfileAt(tbl, "weapons.magic.illusion.fire_and_forget", magicIllusionFireAndForget);
        ReadProfileAt(tbl, "weapons.magic.illusion.ritual",         magicIllusionRitual);

        // Magic schools â€” Restoration
        ReadProfileAt(tbl, "weapons.magic.restoration.concentration",  magicRestorationConcentration);
        ReadProfileAt(tbl, "weapons.magic.restoration.fire_and_forget", magicRestorationFireAndForget);
        ReadProfileAt(tbl, "weapons.magic.restoration.ritual",         magicRestorationRitual);

        // Magic schools â€” sneak variants
        ReadProfileAt(tbl, "weapons.magic.alteration.sneak.concentration",   magicAlterationConcentrationSneak);
        ReadProfileAt(tbl, "weapons.magic.alteration.sneak.fire_and_forget", magicAlterationFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.magic.alteration.sneak.ritual",          magicAlterationRitualSneak);
        ReadProfileAt(tbl, "weapons.magic.conjuration.sneak.concentration",   magicConjurationConcentrationSneak);
        ReadProfileAt(tbl, "weapons.magic.conjuration.sneak.fire_and_forget", magicConjurationFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.magic.conjuration.sneak.ritual",          magicConjurationRitualSneak);
        ReadProfileAt(tbl, "weapons.magic.destruction.sneak.concentration",   magicDestructionConcentrationSneak);
        ReadProfileAt(tbl, "weapons.magic.destruction.sneak.fire_and_forget", magicDestructionFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.magic.destruction.sneak.ritual",          magicDestructionRitualSneak);
        ReadProfileAt(tbl, "weapons.magic.illusion.sneak.concentration",   magicIllusionConcentrationSneak);
        ReadProfileAt(tbl, "weapons.magic.illusion.sneak.fire_and_forget", magicIllusionFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.magic.illusion.sneak.ritual",          magicIllusionRitualSneak);
        ReadProfileAt(tbl, "weapons.magic.restoration.sneak.concentration",   magicRestorationConcentrationSneak);
        ReadProfileAt(tbl, "weapons.magic.restoration.sneak.fire_and_forget", magicRestorationFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.magic.restoration.sneak.ritual",          magicRestorationRitualSneak);

        // Magic per-hand overrides â€” nested under each school entry:
        // [<prefix><school>[.sneak].<cast>.hand.<left|both|right>], profile
        // schema plus an "enabled" marker. Shared reader for the Categories
        // grid here and the Target Lock grid (called again below).
        const auto readMagicHandGrid = [&](const char* prefix, MagicHandGrid& grid) {
            for (std::size_t si = 0; si < 5; ++si)
                for (std::size_t ci = 0; ci < 3; ++ci)
                    for (std::size_t sn = 0; sn < 2; ++sn)
                        for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                            const std::string path = std::string(prefix) +
                                GetMagicSchoolTomlKey(si) + (sn ? ".sneak." : ".") +
                                GetMagicCastTomlKey(ci) + ".hand." + GetMagicHandTomlKey(h);
                            if (const auto* sub = tbl.at_path(path).as_table()) {
                                auto& set = grid[si][ci][sn];
                                set.enabled[h] = (*sub)["enabled"].value_or(false);
                                ReadProfile(*sub, set.profiles[h]);
                            }
                        }
        };
        readMagicHandGrid("weapons.magic.", magicHandOverrides);

        // Weapons â€” Staves
        ReadProfileAt(tbl, "weapons.staves",        weaponsStaves);
        ReadProfileAt(tbl, "weapons.staves.sprint", weaponsStavesSprint);
        ReadProfileAt(tbl, "weapons.staves.swim",   weaponsStavesSwim);
        ReadProfileAt(tbl, "weapons.staves.sneak",  weaponsStavesSneak);

        // Staves schools
        ReadProfileAt(tbl, "weapons.staves.alteration.concentration",   stavesAlterationConcentration);
        ReadProfileAt(tbl, "weapons.staves.alteration.fire_and_forget", stavesAlterationFireAndForget);
        ReadProfileAt(tbl, "weapons.staves.alteration.ritual", stavesAlterationRitual);
        ReadProfileAt(tbl, "weapons.staves.conjuration.concentration",   stavesConjurationConcentration);
        ReadProfileAt(tbl, "weapons.staves.conjuration.fire_and_forget", stavesConjurationFireAndForget);
        ReadProfileAt(tbl, "weapons.staves.conjuration.ritual", stavesConjurationRitual);
        ReadProfileAt(tbl, "weapons.staves.destruction.concentration",   stavesDestructionConcentration);
        ReadProfileAt(tbl, "weapons.staves.destruction.fire_and_forget", stavesDestructionFireAndForget);
        ReadProfileAt(tbl, "weapons.staves.destruction.ritual", stavesDestructionRitual);
        ReadProfileAt(tbl, "weapons.staves.illusion.concentration",   stavesIllusionConcentration);
        ReadProfileAt(tbl, "weapons.staves.illusion.fire_and_forget", stavesIllusionFireAndForget);
        ReadProfileAt(tbl, "weapons.staves.illusion.ritual", stavesIllusionRitual);
        ReadProfileAt(tbl, "weapons.staves.restoration.concentration",   stavesRestorationConcentration);
        ReadProfileAt(tbl, "weapons.staves.restoration.fire_and_forget", stavesRestorationFireAndForget);
        ReadProfileAt(tbl, "weapons.staves.restoration.ritual", stavesRestorationRitual);

        // Staves schools â€” sneak variants
        ReadProfileAt(tbl, "weapons.staves.alteration.sneak.concentration",   stavesAlterationConcentrationSneak);
        ReadProfileAt(tbl, "weapons.staves.alteration.sneak.fire_and_forget", stavesAlterationFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.staves.alteration.sneak.ritual", stavesAlterationRitualSneak);
        ReadProfileAt(tbl, "weapons.staves.conjuration.sneak.concentration",   stavesConjurationConcentrationSneak);
        ReadProfileAt(tbl, "weapons.staves.conjuration.sneak.fire_and_forget", stavesConjurationFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.staves.conjuration.sneak.ritual", stavesConjurationRitualSneak);
        ReadProfileAt(tbl, "weapons.staves.destruction.sneak.concentration",   stavesDestructionConcentrationSneak);
        ReadProfileAt(tbl, "weapons.staves.destruction.sneak.fire_and_forget", stavesDestructionFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.staves.destruction.sneak.ritual", stavesDestructionRitualSneak);
        ReadProfileAt(tbl, "weapons.staves.illusion.sneak.concentration",   stavesIllusionConcentrationSneak);
        ReadProfileAt(tbl, "weapons.staves.illusion.sneak.fire_and_forget", stavesIllusionFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.staves.illusion.sneak.ritual", stavesIllusionRitualSneak);
        ReadProfileAt(tbl, "weapons.staves.restoration.sneak.concentration",   stavesRestorationConcentrationSneak);
        ReadProfileAt(tbl, "weapons.staves.restoration.sneak.fire_and_forget", stavesRestorationFireAndForgetSneak);
        ReadProfileAt(tbl, "weapons.staves.restoration.sneak.ritual", stavesRestorationRitualSneak);

        // --- Target Lock parallel tree ---
        // Mirrors every non-lock profile under target_lock.*. See MEMORY.md
        // feedback_tdm_lockable_states: TDM supports lock in any on-foot,
        // horseback, or transformed state; dragon requires the Lock-On
        // Extension Patch.
        ReadProfileAt(tbl, "target_lock.sheathed",           tlSheathed);
        ReadProfileAt(tbl, "target_lock.sheathed.sprint",    tlSheathedSprint);
        ReadProfileAt(tbl, "target_lock.sheathed.swim",      tlSheathedSwim);
        ReadProfileAt(tbl, "target_lock.sheathed.sneak",     tlSheathedSneak);

        ReadProfileAt(tbl, "target_lock.weapons.melee",        tlWeaponsMelee);
        ReadProfileAt(tbl, "target_lock.weapons.melee.sprint", tlWeaponsMeleeSprint);
        ReadProfileAt(tbl, "target_lock.weapons.melee.swim",   tlWeaponsMeleeSwim);
        ReadProfileAt(tbl, "target_lock.weapons.melee.attack", tlWeaponsMeleeAttack);
        ReadProfileAt(tbl, "target_lock.weapons.melee.sneak",  tlWeaponsMeleeSneak);
        ReadProfileAt(tbl, "target_lock.weapons.melee.power_attack",        tlWeaponsMeleePowerAttack);
        ReadProfileAt(tbl, "target_lock.weapons.melee.sneak_attack",        tlWeaponsMeleeSneakAttack);
        ReadProfileAt(tbl, "target_lock.weapons.melee.sneak_power_attack",  tlWeaponsMeleeSneakPowerAttack);
        ReadProfileAt(tbl, "target_lock.weapons.melee.sprint_attack",       tlWeaponsMeleeSprintAttack);
        ReadProfileAt(tbl, "target_lock.weapons.melee.sprint_power_attack", tlWeaponsMeleeSprintPowerAttack);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee",         tlWeaponsMeleeOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.sprint",  tlWeaponsMeleeSprintOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.swim",    tlWeaponsMeleeSwimOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.attack",      tlWeaponsMeleeAttackOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.sneak",       tlWeaponsMeleeSneakOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.shout",       tlWeaponsMeleeShoutOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.shout.sneak", tlWeaponsMeleeShoutSneakOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.power_attack",        tlWeaponsMeleePowerAttackOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.sneak_attack",        tlWeaponsMeleeSneakAttackOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.sneak_power_attack",  tlWeaponsMeleeSneakPowerAttackOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.sprint_attack",       tlWeaponsMeleeSprintAttackOverrides);
        ReadMeleeOverrides(tbl, "target_lock.weapons.melee.sprint_power_attack", tlWeaponsMeleeSprintPowerAttackOverrides);

        // Directional power-attack overrides (Target Lock).
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            const std::string key = std::string("target_lock.weapons.melee.power_attack.dir.") + kPADirKeys[d];
            ReadProfileAt(tbl, key, tlWeaponsMeleePowerAttackDir[d]);
            if (auto t = tbl.at_path(key).as_table()) {
                if (auto en = (*t)["enabled"].as_boolean()) {
                    tlWeaponsMeleePowerAttackDirEnabled[d] = en->get();
                }
            }
            ReadMeleeOverrides(tbl, key, tlWeaponsMeleePowerAttackDirOverrides[d]);
        }

        // Target-lock blocking (9 sub-states).
        {
            CameraProfile* const tlBlockSlots[9] = {
                &tlWeaponsBlockingOneHanded,
                &tlWeaponsBlockingTwoHanded,
                &tlWeaponsBlockingShield,
                &tlWeaponsBlockingWard,
                &tlWeaponsBlockingOneHandedSneak,
                &tlWeaponsBlockingTwoHandedSneak,
                &tlWeaponsBlockingShieldSneak,
                &tlWeaponsBlockingWardSneak,
                &tlWeaponsBlockingShieldSprint,
            };
            constexpr const char* kTLBlockKeys[9] = {
                "target_lock.weapons.blocking.one_handed",
                "target_lock.weapons.blocking.two_handed",
                "target_lock.weapons.blocking.shield",
                "target_lock.weapons.blocking.ward",
                "target_lock.weapons.blocking.one_handed.sneak",
                "target_lock.weapons.blocking.two_handed.sneak",
                "target_lock.weapons.blocking.shield.sneak",
                "target_lock.weapons.blocking.ward.sneak",
                "target_lock.weapons.blocking.shield.sprint",
            };
            for (std::size_t i = 0; i < 9; ++i) {
                ReadProfileAt(tbl, kTLBlockKeys[i], *tlBlockSlots[i]);
            }
        }

        ReadProfileAt(tbl, "target_lock.weapons.bow",             tlWeaponsBow);
        ReadProfileAt(tbl, "target_lock.weapons.bow.sprint",      tlWeaponsBowSprint);
        ReadProfileAt(tbl, "target_lock.weapons.bow.swim",        tlWeaponsBowSwim);
        ReadProfileAt(tbl, "target_lock.weapons.bow.draw",        tlWeaponsBowDraw);
        ReadProfileAt(tbl, "target_lock.weapons.bow.sneak",       tlWeaponsBowSneak);
        ReadProfileAt(tbl, "target_lock.weapons.bow.sneak.draw",  tlWeaponsBowSneakDraw);
        ReadProfileAt(tbl, "target_lock.weapons.bow.zoom",        tlWeaponsBowZoom);
        ReadProfileAt(tbl, "target_lock.weapons.bow.sneak.zoom",  tlWeaponsBowSneakZoom);

        ReadProfileAt(tbl, "target_lock.weapons.crossbow",             tlWeaponsCrossbow);
        ReadProfileAt(tbl, "target_lock.weapons.crossbow.sprint",      tlWeaponsCrossbowSprint);
        ReadProfileAt(tbl, "target_lock.weapons.crossbow.swim",        tlWeaponsCrossbowSwim);
        ReadProfileAt(tbl, "target_lock.weapons.crossbow.draw",        tlWeaponsCrossbowDraw);
        ReadProfileAt(tbl, "target_lock.weapons.crossbow.sneak",       tlWeaponsCrossbowSneak);
        ReadProfileAt(tbl, "target_lock.weapons.crossbow.sneak.draw",  tlWeaponsCrossbowSneakDraw);
        ReadProfileAt(tbl, "target_lock.weapons.crossbow.zoom",        tlWeaponsCrossbowZoom);
        ReadProfileAt(tbl, "target_lock.weapons.crossbow.sneak.zoom",  tlWeaponsCrossbowSneakZoom);

        ReadProfileAt(tbl, "target_lock.weapons.magic",        tlWeaponsMagic);
        ReadProfileAt(tbl, "target_lock.weapons.magic.sprint", tlWeaponsMagicSprint);
        ReadProfileAt(tbl, "target_lock.weapons.magic.swim",   tlWeaponsMagicSwim);
        ReadProfileAt(tbl, "target_lock.weapons.magic.sneak",  tlWeaponsMagicSneak);

        ReadProfileAt(tbl, "target_lock.weapons.magic.alteration.concentration",    tlMagicAlterationConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.magic.alteration.fire_and_forget",  tlMagicAlterationFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.magic.alteration.ritual",           tlMagicAlterationRitual);
        ReadProfileAt(tbl, "target_lock.weapons.magic.conjuration.concentration",   tlMagicConjurationConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.magic.conjuration.fire_and_forget", tlMagicConjurationFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.magic.conjuration.ritual",          tlMagicConjurationRitual);
        ReadProfileAt(tbl, "target_lock.weapons.magic.destruction.concentration",   tlMagicDestructionConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.magic.destruction.fire_and_forget", tlMagicDestructionFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.magic.destruction.ritual",          tlMagicDestructionRitual);
        ReadProfileAt(tbl, "target_lock.weapons.magic.illusion.concentration",      tlMagicIllusionConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.magic.illusion.fire_and_forget",    tlMagicIllusionFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.magic.illusion.ritual",             tlMagicIllusionRitual);
        ReadProfileAt(tbl, "target_lock.weapons.magic.restoration.concentration",   tlMagicRestorationConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.magic.restoration.fire_and_forget", tlMagicRestorationFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.magic.restoration.ritual",          tlMagicRestorationRitual);

        ReadProfileAt(tbl, "target_lock.weapons.magic.alteration.sneak.concentration",    tlMagicAlterationConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.alteration.sneak.fire_and_forget",  tlMagicAlterationFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.alteration.sneak.ritual",           tlMagicAlterationRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.conjuration.sneak.concentration",   tlMagicConjurationConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.conjuration.sneak.fire_and_forget", tlMagicConjurationFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.conjuration.sneak.ritual",          tlMagicConjurationRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.destruction.sneak.concentration",   tlMagicDestructionConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.destruction.sneak.fire_and_forget", tlMagicDestructionFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.destruction.sneak.ritual",          tlMagicDestructionRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.illusion.sneak.concentration",      tlMagicIllusionConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.illusion.sneak.fire_and_forget",    tlMagicIllusionFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.illusion.sneak.ritual",             tlMagicIllusionRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.restoration.sneak.concentration",   tlMagicRestorationConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.restoration.sneak.fire_and_forget", tlMagicRestorationFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.magic.restoration.sneak.ritual",          tlMagicRestorationRitualSneak);

        // TL magic per-hand overrides (same schema as the Categories grid).
        readMagicHandGrid("target_lock.weapons.magic.", tlMagicHandOverrides);

        ReadProfileAt(tbl, "target_lock.weapons.staves",        tlWeaponsStaves);
        ReadProfileAt(tbl, "target_lock.weapons.staves.sprint", tlWeaponsStavesSprint);
        ReadProfileAt(tbl, "target_lock.weapons.staves.swim",   tlWeaponsStavesSwim);
        ReadProfileAt(tbl, "target_lock.weapons.staves.sneak",  tlWeaponsStavesSneak);

        ReadProfileAt(tbl, "target_lock.weapons.staves.alteration.concentration",    tlStavesAlterationConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.staves.alteration.fire_and_forget",  tlStavesAlterationFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.staves.alteration.ritual",  tlStavesAlterationRitual);
        ReadProfileAt(tbl, "target_lock.weapons.staves.conjuration.concentration",   tlStavesConjurationConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.staves.conjuration.fire_and_forget", tlStavesConjurationFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.staves.conjuration.ritual", tlStavesConjurationRitual);
        ReadProfileAt(tbl, "target_lock.weapons.staves.destruction.concentration",   tlStavesDestructionConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.staves.destruction.fire_and_forget", tlStavesDestructionFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.staves.destruction.ritual", tlStavesDestructionRitual);
        ReadProfileAt(tbl, "target_lock.weapons.staves.illusion.concentration",      tlStavesIllusionConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.staves.illusion.fire_and_forget",    tlStavesIllusionFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.staves.illusion.ritual",    tlStavesIllusionRitual);
        ReadProfileAt(tbl, "target_lock.weapons.staves.restoration.concentration",   tlStavesRestorationConcentration);
        ReadProfileAt(tbl, "target_lock.weapons.staves.restoration.fire_and_forget", tlStavesRestorationFireAndForget);
        ReadProfileAt(tbl, "target_lock.weapons.staves.restoration.ritual", tlStavesRestorationRitual);

        ReadProfileAt(tbl, "target_lock.weapons.staves.alteration.sneak.concentration",    tlStavesAlterationConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.alteration.sneak.fire_and_forget",  tlStavesAlterationFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.alteration.sneak.ritual",  tlStavesAlterationRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.conjuration.sneak.concentration",   tlStavesConjurationConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.conjuration.sneak.fire_and_forget", tlStavesConjurationFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.conjuration.sneak.ritual", tlStavesConjurationRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.destruction.sneak.concentration",   tlStavesDestructionConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.destruction.sneak.fire_and_forget", tlStavesDestructionFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.destruction.sneak.ritual", tlStavesDestructionRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.illusion.sneak.concentration",      tlStavesIllusionConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.illusion.sneak.fire_and_forget",    tlStavesIllusionFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.illusion.sneak.ritual",    tlStavesIllusionRitualSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.restoration.sneak.concentration",   tlStavesRestorationConcentrationSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.restoration.sneak.fire_and_forget", tlStavesRestorationFireAndForgetSneak);
        ReadProfileAt(tbl, "target_lock.weapons.staves.restoration.sneak.ritual", tlStavesRestorationRitualSneak);

        ReadProfileAt(tbl, "target_lock.transformations.werewolf",        tlTransformationsWerewolf);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.sheathed", tlTransformationsWerewolfSheathed);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.sprint", tlTransformationsWerewolfSprint);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.swim",   tlTransformationsWerewolfSwim);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.attack",             tlTransformationsWerewolfAttack);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.power_attack",        tlTransformationsWerewolfPowerAttack);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.sprint_power_attack", tlTransformationsWerewolfSprintPowerAttack);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.roar",               tlTransformationsWerewolfRoar);
        ReadProfileAt(tbl, "target_lock.transformations.werewolf.feeding",            tlTransformationsWerewolfFeeding);

        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.sheathed",        tlVampireLordSheathed);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.sheathed.levitating", tlVampireLordSheathedLevitating);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.melee",           tlVampireLordMelee);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.melee.attack",       tlVampireLordMeleeAttack);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.melee.power_attack", tlVampireLordMeleePowerAttack);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.magic",           tlVampireLordMagic);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.concentration",   tlVampireLordConcentration);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.fire_and_forget", tlVampireLordFireAndForget);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.sprint",          tlVampireLordSprint);
        ReadProfileAt(tbl, "target_lock.transformations.vampire_lord.sprint.levitating", tlVampireLordSprintLevitating);

        ReadProfileAt(tbl, "target_lock.mounts.horseback",         tlMountsHorseback);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.sprint",  tlMountsHorsebackSprint);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.swim",    tlMountsHorsebackSwim);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.melee",   tlMountsHorsebackMelee);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.archery", tlMountsHorsebackArchery);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.archery.draw", tlMountsHorsebackArcheryDraw);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.archery.zoom", tlMountsHorsebackArcheryZoom);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.melee.attack_left",  tlMountsHorsebackMeleeLeft);
        ReadProfileAt(tbl, "target_lock.mounts.horseback.melee.attack_right", tlMountsHorsebackMeleeRight);

        // Target-lock shouts (parallel to the regular shouts block below).
        for (std::size_t si = 0; si < kShoutableStateCount; ++si) {
            char key[192];
            std::snprintf(key, sizeof(key), "target_lock.shouts.%s.base", kShoutableStateKeys[si]);
            ReadProfileAt(tbl, key, tlShoutsBaseByState[si]);
            std::snprintf(key, sizeof(key), "target_lock.shouts.%s.base.sneak", kShoutableStateKeys[si]);
            ReadProfileAt(tbl, key, tlShoutsBaseByStateSneak[si]);

            for (std::size_t i = 0; i < kShoutCount; ++i) {
                const auto& entry = kShouts[i];
                std::snprintf(key, sizeof(key), "target_lock.shouts.%s.%s", kShoutableStateKeys[si], entry.tomlKey);
                ReadProfileAt(tbl, key, tlShoutOverrideByState[si][i]);
                tlShoutOverrideByStateEnabled[si][i] =
                    tbl.at_path(std::string(key) + ".enabled").value_or(false);

                std::snprintf(key, sizeof(key), "target_lock.shouts.%s.%s.sneak", kShoutableStateKeys[si], entry.tomlKey);
                ReadProfileAt(tbl, key, tlShoutOverrideByStateSneak[si][i]);
                tlShoutOverrideByStateEnabledSneak[si][i] =
                    tbl.at_path(std::string(key) + ".enabled").value_or(false);
            }
        }

        // --- Enemy Overrides ---
        // target_lock.enemy.<enemy>.<slot_key>.{side_offset, ..., aim_bias, ...}
        // (aim bias is now per-entry, stored in each EnemyFieldOverride.)
        for (std::size_t e = 0; e < kEnemyOverrideEnemies; ++e) {
            const std::string root = std::string("target_lock.enemy.") + kEnemyKeys[e];
            for (std::size_t s = 0; s < kTLSlotCount; ++s) {
                const std::string path = root + "." + kTLSlotKeys[s];
                ReadEnemyOverrideAt(tbl, path, enemyOverrides[e][s]);
                // Indoor twin under ".indoor.", the same shape the custom
                // enemy grids already use.
                ReadEnemyOverrideAt(tbl, root + ".indoor." + kTLSlotKeys[s],
                                    enemyOverridesIndoor[e][s]);
            }
            for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                const std::string path = root + ".power_attack_dir." + kPADirKeys[d];
                ReadEnemyOverrideAt(tbl, path, tlEnemyOverridesPowerAttackDir[e][d]);
                ReadEnemyOverrideAt(tbl, root + ".indoor.power_attack_dir." + kPADirKeys[d],
                                    tlEnemyOverridesPowerAttackDirIndoor[e][d]);
            }
        }

        // --- Custom (player-bound) Enemy Overrides --- iterate contiguous
        // indices until the first missing .meta sub-table.
        customEnemyOverrides.clear();
        for (std::size_t ci = 0; ; ++ci) {
            const std::string root = "target_lock.custom_enemy." + std::to_string(ci);
            const auto* metaNode = tbl.at_path(root + ".meta").as_table();
            if (!metaNode) break;
            CustomEnemyOverride c;
            const std::string mt = (*metaNode)["match_type"].value_or(std::string("race"));
            c.matchType   = (mt == "keyword")     ? CustomEnemyOverride::MatchType::Keyword
                          : (mt == "race_family") ? CustomEnemyOverride::MatchType::RaceFamily
                          : (mt == "npc")         ? CustomEnemyOverride::MatchType::NPC
                          : (mt == "faction")     ? CustomEnemyOverride::MatchType::Faction
                          : (mt == "actor_name")  ? CustomEnemyOverride::MatchType::Name
                                                  : CustomEnemyOverride::MatchType::Race;
            c.pluginName  = (*metaNode)["plugin"].value_or(std::string{});
            c.formID      = static_cast<std::uint32_t>((*metaNode)["form_id"].value_or(std::int64_t{0}));
            c.matchKey    = (*metaNode)["match_key"].value_or(std::string{});
            c.displayName = (*metaNode)["name"].value_or(std::string{});
            c.trackingSmoothing = std::clamp(
                static_cast<float>((*metaNode)["tracking_smoothing"].value_or(0.0)), 0.0f, 3.0f);
            for (std::size_t s = 0; s < kTLSlotCount; ++s) {
                ReadEnemyOverrideAt(tbl, root + "." + kTLSlotKeys[s], c.slots[s]);
                ReadEnemyOverrideAt(tbl, root + ".indoor." + kTLSlotKeys[s], c.slotsIndoor[s]);
            }
            for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                ReadEnemyOverrideAt(tbl, root + ".power_attack_dir." + kPADirKeys[d], c.paDir[d]);
                ReadEnemyOverrideAt(tbl, root + ".indoor.power_attack_dir." + kPADirKeys[d], c.paDirIndoor[d]);
            }
            customEnemyOverrides.push_back(std::move(c));
        }

        // --- Custom (mod-added) weapon type catalog ---
        customWeaponTypes.clear();
        if (const auto* cwArr = tbl.at_path("weapons.custom_weapon_types").as_array()) {
            for (const auto& node : *cwArr) {
                const auto* ct = node.as_table();
                if (!ct) continue;
                CustomWeaponType t;
                t.keyword     = (*ct)["keyword"].value_or(std::string{});
                t.displayName = (*ct)["name"].value_or(std::string{});
                if (t.keyword.empty()) continue;
                if (t.displayName.empty()) t.displayName = t.keyword;
                customWeaponTypes.push_back(std::move(t));
            }
        }

        // Shouts â€” base + per-shout profiles (normal + sneak variants) with
        // a per-entry `enabled` toggle.
        for (std::size_t si = 0; si < kShoutableStateCount; ++si) {
            char key[192];
            std::snprintf(key, sizeof(key), "shouts.%s.base", kShoutableStateKeys[si]);
            ReadProfileAt(tbl, key, shoutsBaseByState[si]);
            std::snprintf(key, sizeof(key), "shouts.%s.base.sneak", kShoutableStateKeys[si]);
            ReadProfileAt(tbl, key, shoutsBaseByStateSneak[si]);

            for (std::size_t i = 0; i < kShoutCount; ++i) {
                const auto& entry = kShouts[i];
                std::snprintf(key, sizeof(key), "shouts.%s.%s", kShoutableStateKeys[si], entry.tomlKey);
                ReadProfileAt(tbl, key, shoutOverrideByState[si][i]);
                shoutOverrideByStateEnabled[si][i] =
                    tbl.at_path(std::string(key) + ".enabled").value_or(false);

                std::snprintf(key, sizeof(key), "shouts.%s.%s.sneak", kShoutableStateKeys[si], entry.tomlKey);
                ReadProfileAt(tbl, key, shoutOverrideByStateSneak[si][i]);
                shoutOverrideByStateEnabledSneak[si][i] =
                    tbl.at_path(std::string(key) + ".enabled").value_or(false);
            }
        }

        // Transformations â€” Werewolf
        ReadProfileAt(tbl, "transformations.werewolf",        transformationsWerewolf);
        ReadProfileAt(tbl, "transformations.werewolf.sheathed", transformationsWerewolfSheathed);
        ReadProfileAt(tbl, "transformations.werewolf.sprint", transformationsWerewolfSprint);
        ReadProfileAt(tbl, "transformations.werewolf.swim",   transformationsWerewolfSwim);
        ReadProfileAt(tbl, "transformations.werewolf.attack", transformationsWerewolfAttack);
        ReadProfileAt(tbl, "transformations.werewolf.power_attack",        transformationsWerewolfPowerAttack);
        ReadProfileAt(tbl, "transformations.werewolf.sprint_power_attack", transformationsWerewolfSprintPowerAttack);
        ReadProfileAt(tbl, "transformations.werewolf.roar",   transformationsWerewolfRoar);
        ReadProfileAt(tbl, "transformations.werewolf.feeding", transformationsWerewolfFeeding);

        ReadProfileAt(tbl, "transformations.vampire_lord.sheathed",                 vampireLordSheathed);
        ReadProfileAt(tbl, "transformations.vampire_lord.sheathed.levitating",      vampireLordSheathedLevitating);
        ReadProfileAt(tbl, "transformations.vampire_lord.melee",                    vampireLordMelee);
        ReadProfileAt(tbl, "transformations.vampire_lord.melee.attack",             vampireLordMeleeAttack);
        ReadProfileAt(tbl, "transformations.vampire_lord.melee.power_attack",       vampireLordMeleePowerAttack);
        ReadProfileAt(tbl, "transformations.vampire_lord.magic",                    vampireLordMagic);
        ReadProfileAt(tbl, "transformations.vampire_lord.concentration",            vampireLordConcentration);
        ReadProfileAt(tbl, "transformations.vampire_lord.fire_and_forget",          vampireLordFireAndForget);
        ReadProfileAt(tbl, "transformations.vampire_lord.sprint",                   vampireLordSprint);
        ReadProfileAt(tbl, "transformations.vampire_lord.sprint.levitating",        vampireLordSprintLevitating);

        // VL default migration. Earlier versions used three different
        // bad defaults that all produced "head fills the screen":
        //   1. VanillaCombat â€” combat framing is too tight for VL.
        //   2. Vanilla() â€” fov=0, which renders worldFOV=0 (extreme zoom).
        //   3. Pulled-back attempt with zoom=-1 â€” zoom is multiplied by
        //      0.01 so -1 is essentially nothing.
        // Catch any of those exact matches AND any profile with a
        // suspiciously low FOV (< 30, which is below practical use)
        // since fov=0 from old configs is the most common "head only"
        // failure mode. Replace with the new kVampireLordVanilla.
        // REMOVED 2026-08-17. It was a pre-release migration AND a live
        // landmine: alongside the three exact old defaults it also rewrote
        // ANY Vampire Lord profile with `fov < 30`, so a user who
        // deliberately tuned a VL slot below 30 would have had it silently
        // replaced on every single load, with no way to make it stick. The
        // new default is the constructor's business, not the loader's.

        // Mounts â€” Horseback
        ReadProfileAt(tbl, "mounts.horseback",         mountsHorseback);
        ReadProfileAt(tbl, "mounts.horseback.sprint",  mountsHorsebackSprint);
        ReadProfileAt(tbl, "mounts.horseback.swim",    mountsHorsebackSwim);
        ReadProfileAt(tbl, "mounts.horseback.melee",   mountsHorsebackMelee);
        ReadProfileAt(tbl, "mounts.horseback.archery", mountsHorsebackArchery);
        ReadProfileAt(tbl, "mounts.horseback.archery.draw", mountsHorsebackArcheryDraw);
        ReadProfileAt(tbl, "mounts.horseback.archery.zoom", mountsHorsebackArcheryZoom);
        ReadProfileAt(tbl, "mounts.horseback.melee.attack_left",  mountsHorsebackMeleeLeft);
        ReadProfileAt(tbl, "mounts.horseback.melee.attack_right", mountsHorsebackMeleeRight);

        // Mounts â€” Dragon Riding
        ReadProfileAt(tbl, "mounts.dragon_riding", mountsDragonRiding);
        ReadProfileAt(tbl, "mounts.dragon_riding.perched",  mountsDragonRidingPerched);
        ReadProfileAt(tbl, "mounts.dragon_riding.hovering", mountsDragonRidingHovering);
        ReadProfileAt(tbl, "mounts.dragon_riding.takeoff",  mountsDragonRidingTakeoff);
        ReadProfileAt(tbl, "mounts.dragon_riding.landing",  mountsDragonRidingLanding);
        ReadProfileAt(tbl, "mounts.dragon_riding.attack.grounded", mountsDragonRidingAttackGrounded);
        ReadProfileAt(tbl, "mounts.dragon_riding.attack.hovering", mountsDragonRidingAttackHovering);
        ReadProfileAt(tbl, "mounts.dragon_riding.attack.flying",   mountsDragonRidingAttackFlying);
        ReadProfileAt(tbl, "mounts.dragon_riding.breath.grounded", mountsDragonRidingBreathGrounded);
        ReadProfileAt(tbl, "mounts.dragon_riding.breath.hovering", mountsDragonRidingBreathHovering);
        ReadProfileAt(tbl, "mounts.dragon_riding.breath.flying",   mountsDragonRidingBreathFlying);

        // Indoor variants. Init storage AFTER outdoor profiles are loaded â€”
        // the InitIndoorOverrides default seeds each indoor entry from the
        // current outdoor value, which is now the user's saved values. Then
        // overlay any user-tuned indoor values from the "indoor.*" table.
        InitIndoorOverrides();
        categoriesEditTab = static_cast<int>(tbl["general"]["categories_edit_tab"].value_or<std::int64_t>(0));
        if (categoriesEditTab < 0 || categoriesEditTab >= kEnvCount) categoriesEditTab = 0;

        weaponsBlockingWardEnabled      = tbl["general"]["blocking_ward_enabled"]      .value_or(false);
        weaponsBlockingWardSneakEnabled = tbl["general"]["blocking_ward_sneak_enabled"].value_or(false);
        for (auto& e : GetIndoorEligibleProfiles()) {
            if (const auto* saved = tbl.at_path(std::string("indoor.") + e.tomlKey).as_table()) {
                auto& profile = *IndoorVariantOf(e.outdoor);
                profile = CameraProfile::Default3p();
                ReadProfile(*saved, profile);
            }
        }

        // ---- Location overrides -------------------------------------------
        // Read AFTER the indoor pass so the eligible list, outdoorIdxMap and
        // every outdoor value are final; a place's storage is sized off that
        // same list.
        locationOverridesEnabled =
            tbl["location_overrides"]["enabled"].value_or(locationOverridesEnabled);
        locationOverrides.clear();
        LocationDetector::GetSingleton().Invalidate();
        {
            const auto eligible = GetIndoorEligibleProfiles();
            for (int i = 0; ; ++i) {
                const std::string root = "location_override." + std::to_string(i);
                const auto* t = tbl.at_path(root).as_table();
                if (!t) break;
                LocationOverride lo;
                lo.name           = (*t)["name"]            .value_or(std::string{"Location"});
                lo.enabled        = (*t)["enabled"]         .value_or(true);
                lo.builtIn        = (*t)["built_in"]        .value_or(false);
                lo.keyword        = (*t)["keyword"]         .value_or(std::string{});
                lo.parentName     = (*t)["parent"]          .value_or(std::string{});
                lo.plugin         = (*t)["plugin"]          .value_or(std::string{});
                lo.formID = static_cast<std::uint32_t>((*t)["form_id"].value_or<std::int64_t>(0));
                const auto kindStr = (*t)["kind"].value_or(std::string{});
                lo.kind = !lo.keyword.empty()      ? LocationOverride::Kind::Keyword
                        : kindStr == "worldspace"  ? LocationOverride::Kind::Worldspace
                        : kindStr == "cell"        ? LocationOverride::Kind::Cell
                        : kindStr == "region"      ? LocationOverride::Kind::Region
                        : kindStr == "room"        ? LocationOverride::Kind::Room
                                                   : LocationOverride::Kind::Location;
                // weather_mask is deliberately not read â€” weather-conditioned
                // places were removed 2026-08-13.
                SizeLocationOverride(lo);
                // Per-entry camera overrides. Presence of the table IS the
                // per-entry enable, which keeps an untuned place free.
                for (std::size_t k = 0; k < eligible.size(); ++k) {
                    const std::string path = root + ".profiles." + eligible[k].tomlKey;
                    if (const auto* pt = tbl.at_path(path).as_table()) {
                        CameraProfile prof = CameraProfile::Default3p();
                        ReadProfile(*pt, prof);
                        lo.profiles[k]   = prof;
                        lo.profileSet[k] = true;
                    }
                    // The Indoor variant's own slot — an individual entry
                    // with its own bind, never inherited from the outdoor
                    // one. Seeded from the indoor variant the same way the
                    // outdoor slot seeds from the outdoor value (the indoor
                    // pass above ran first, so the variant is final).
                    const std::string ipath = root + ".profiles_indoor." + eligible[k].tomlKey;
                    if (const auto* pt = tbl.at_path(ipath).as_table()) {
                        CameraProfile prof = CameraProfile::Default3p();
                        ReadProfile(*pt, prof);
                        lo.profilesIndoor[k]   = prof;
                        lo.profileSetIndoor[k] = true;
                    }
                }
                // Camera Noise.
                if (const auto* g = tbl.at_path(root + ".noise.global").as_table()) {
                    NoiseProfile gp;
                    gp.amp    = (*g)["amp"]   .value_or(gp.amp);
                    gp.speed  = (*g)["speed"] .value_or(gp.speed);
                    gp.sway   = (*g)["sway"]  .value_or(gp.sway);
                    gp.tilt   = (*g)["tilt"]  .value_or(gp.tilt);
                    gp.wobble = (*g)["wobble"].value_or(gp.wobble);
                    ReadNoiseExtra(*g, gp);
                    lo.globalNoise    = gp;
                    lo.globalNoiseSet = true;
                }
                if (const auto* states = tbl.at_path(root + ".noise.states").as_table()) {
                    for (auto&& [k, node] : *states) {
                        if (const auto* sub = node.as_table()) {
                            NoiseProfile np;
                            np.enabled = (*sub)["enabled"].value_or(false);
                            np.amp     = (*sub)["amp"]    .value_or(0.0f);
                            np.speed   = (*sub)["speed"]  .value_or(0.0f);
                            np.sway    = (*sub)["sway"]   .value_or(0.0f);
                            np.tilt    = (*sub)["tilt"]   .value_or(0.0f);
                            np.wobble  = (*sub)["wobble"] .value_or(0.0f);
                            ReadNoiseExtra(*sub, np);
                            lo.stateNoise.emplace(std::string{k.str()}, np);
                        }
                    }
                }
                // First person. Presence is the bind, same convention as
                // the noise states above; codec shared with the binding
                // fp_profiles tables.
                if (const auto* fpStates = tbl.at_path(root + ".fp.states").as_table()) {
                    for (auto&& [k, node] : *fpStates) {
                        if (const auto* sub = node.as_table()) {
                            FirstPersonProfile p;
                            ReadFpProfileTable(*sub, p);
                            lo.fpState.emplace(std::string{k.str()}, p);
                        }
                    }
                }
                // Specific-weapon binding camera slots. Presence is the bind.
                if (const auto* bcStates = tbl.at_path(root + ".binding_cam").as_table()) {
                    for (auto&& [k, node] : *bcStates) {
                        if (const auto* sub = node.as_table()) {
                            CameraProfile p{};
                            ReadProfile(*sub, p);
                            lo.bindingCam.emplace(std::string{k.str()}, p);
                        }
                    }
                }
                // Transformation-beat tunings. Presence is the bind.
                if (const auto* fxb = tbl.at_path(root + ".fx_beats").as_table()) {
                    for (auto&& [k, node] : *fxb) {
                        if (const auto* sub = node.as_table()) {
                            BeatTuning bt{};
                            bt.intensity = (*sub)["intensity"].value_or(bt.intensity);
                            bt.speed     = (*sub)["speed"]    .value_or(bt.speed);
                            bt.range     = (*sub)["range"]    .value_or(bt.range);
                            bt.direction = (*sub)["direction"].value_or(bt.direction);
                            bt.chr.rotShake     = (*sub)["rot_shake"]    .value_or(bt.chr.rotShake);
                            bt.chr.posShake     = (*sub)["pos_shake"]    .value_or(bt.chr.posShake);
                            bt.chr.driftJitter  = (*sub)["drift_jitter"] .value_or(bt.chr.driftJitter);
                            bt.chr.roughness    = (*sub)["roughness"]    .value_or(bt.chr.roughness);
                            bt.chr.fadeDuration = (*sub)["fade_duration"].value_or(bt.chr.fadeDuration);
                            lo.fxBeats.emplace(std::string{k.str()}, bt);
                        }
                    }
                }
                // Dialogue look overrides. Presence is the bind, same as
                // binding_cam above; the key is the look's uid in decimal.
                if (const auto* dl = tbl.at_path(root + ".dialogue_looks").as_table()) {
                    for (auto&& [k, node] : *dl) {
                        if (const auto* sub = node.as_table()) {
                            CameraProfile p{};
                            ReadProfile(*sub, p);
                            lo.dlgLooks.emplace(std::string{k.str()}, p);
                        }
                    }
                }
                // Legacy multi-rule places (the same-day first cut, which let one
                // place carry keyword tiers plus a list of locations) and the old
                // City environment are DROPPED, not migrated - user ruling
                // 2026-08-02: "my city settings seem to still exist which they
                // shouldnt". A place is one rule read off the world now, and a
                // silently-resurrected city tuning is exactly the thing that made
                // the old feature feel out of control.
                if ((*t)["match_cities"].value_or(false) ||
                    (*t)["match_towns"].value_or(false) ||
                    (*t)["match_settlements"].value_or(false) ||
                    (*t)["locations"].as_array()) {
                    spdlog::info("Dropped legacy multi-rule location override \"{}\"", lo.name);
                    continue;
                }
                locationOverrides.push_back(std::move(lo));
            }

            // The old City environment (city.* / noise.*_city / [city_override])
            // is NOT migrated. It was folded into a location override for one
            // build and the user asked for it gone: "my city settings seem to
            // still exist which they shouldnt". Those keys are simply ignored,
            // and a pre-change backup of the preset is the recovery path.
        }
        // Places are created from the Location popup against what the player
        // is actually standing in, so there is nothing to seed - an empty list
        // is the correct fresh-install state.
        for (auto& lo : locationOverrides) SizeLocationOverride(lo);
        if (locationEditIdx < 0 || locationEditIdx >= static_cast<int>(locationOverrides.size()))
            locationEditIdx = 0;

        // ---- [CLIPCAM] animation cameras ----------------------------------
        // Self-describing indexed list (PRESET-COMPAT rule 5: numbering is
        // internal to the file that saved it). Absent sections leave the
        // vector empty and the master at its initializer — a 1.0 preset is
        // completely inert here.
        // (animation_cameras.enabled retired 2026-08-31 — the feature is
        // permanently on; a bound entry is simply active.)
        animationCameras.clear();
        for (int i = 0; ; ++i) {
            const std::string root = "animation_camera." + std::to_string(i);
            const auto* t = tbl.at_path(root).as_table();
            if (!t) break;
            AnimationCameraEntry e;
            e.uid           = static_cast<std::uint32_t>((*t)["uid"].value_or(std::int64_t{ 0 }));
            e.name          = (*t)["name"]   .value_or(std::string{ "Animation" });
            e.animationPath = (*t)["path"]   .value_or(std::string{});
            e.subModName    = (*t)["sub_mod"].value_or(std::string{});
            e.modName       = (*t)["mod"]    .value_or(std::string{});
            // Profiles are written sparsely against the FIELD INITIALIZER
            // (CameraProfile{} == Default3p, the rule-7 baseline), so the
            // default-constructed members are already the right seed.
            if (const auto* pt = tbl.at_path(root + ".profile").as_table())
                ReadProfile(*pt, e.profile);
            if (const auto* pt = tbl.at_path(root + ".profile_indoor").as_table())
                ReadProfile(*pt, e.profileIndoor);
            else
                e.profileIndoor = e.profile;  // no divergence saved — twin mirrors outdoor
            if (const auto* pt = tbl.at_path(root + ".tl_profile").as_table())
                ReadProfile(*pt, e.tlProfile);
            if (const auto* pt = tbl.at_path(root + ".tl_profile_indoor").as_table())
                ReadProfile(*pt, e.tlProfileIndoor);
            if (const auto* sub = tbl.at_path(root + ".noise").as_table()) {
                e.noise.enabled = (*sub)["enabled"].value_or(false);
                e.noise.amp     = (*sub)["amp"]    .value_or(0.0f);
                e.noise.speed   = (*sub)["speed"]  .value_or(0.0f);
                e.noise.sway    = (*sub)["sway"]   .value_or(0.0f);
                e.noise.tilt    = (*sub)["tilt"]   .value_or(0.0f);
                e.noise.wobble  = (*sub)["wobble"] .value_or(0.0f);
                ReadNoiseExtra(*sub, e.noise);
            }
            if (const auto* sub = tbl.at_path(root + ".noise_indoor").as_table()) {
                e.noiseIndoor.enabled = (*sub)["enabled"].value_or(false);
                e.noiseIndoor.amp     = (*sub)["amp"]    .value_or(0.0f);
                e.noiseIndoor.speed   = (*sub)["speed"]  .value_or(0.0f);
                e.noiseIndoor.sway    = (*sub)["sway"]   .value_or(0.0f);
                e.noiseIndoor.tilt    = (*sub)["tilt"]   .value_or(0.0f);
                e.noiseIndoor.wobble  = (*sub)["wobble"] .value_or(0.0f);
                ReadNoiseExtra(*sub, e.noiseIndoor);
            } else {
                e.noiseIndoor = e.noise;   // mirror until diverged
            }
            animationCameras.push_back(std::move(e));
        }
        AssignAnimationCameraUids();
        // The controller matches against its own snapshot, never this vector
        // (the hook runs on graph worker threads) — resync it now.
        AnimationCameraController::GetSingleton().RebuildMatchIndex();


        Validate();

        ApplyEngineSettings();
        spdlog::info("Config loaded successfully");
        return true;
    }
    void SettingsManager::Save() try
    {
        // Preset-as-source-of-truth model: camera settings live ONLY inside
        // named presets (PresetManager::SavePreset -> BuildSaveTable). The
        // global config persists just the things intentionally untied to any
        // preset â€” the keybinds and which preset was last active (so it can be
        // re-loaded at launch). This is NOT a settings autosave; a settings
        // change persists only when the user saves/updates a preset.
        toml::table gen;
        if (quickTuneHotkey           != 0) gen.insert("quick_tune_hotkey",            static_cast<std::int64_t>(quickTuneHotkey));
        if (presetCycleNextKey        != 0) gen.insert("preset_cycle_next_key",        static_cast<std::int64_t>(presetCycleNextKey));
        if (entryCopyKey              != 0) gen.insert("entry_copy_key",               static_cast<std::int64_t>(entryCopyKey));
        if (entryPasteKey             != 0) gen.insert("entry_paste_key",              static_cast<std::int64_t>(entryPasteKey));
        if (categoriesShoulderSwapKey != 0) gen.insert("categories_shoulder_swap_key", static_cast<std::int64_t>(categoriesShoulderSwapKey));
        if (dialogueCycleNextKey      != 0) gen.insert("dialogue_cycle_next_key",      static_cast<std::int64_t>(dialogueCycleNextKey));
        if (dialogueCyclePrevKey      != 0) gen.insert("dialogue_cycle_prev_key",      static_cast<std::int64_t>(dialogueCyclePrevKey));
        if (deathCameraSkipKey        != 0) gen.insert("death_camera_skip_key",        static_cast<std::int64_t>(deathCameraSkipKey));
        if (deathCamFadeKey           != 0) gen.insert("death_cam_fade_key",           static_cast<std::int64_t>(deathCamFadeKey));
        if (ragdollCamFadeKey         != 0) gen.insert("ragdoll_cam_fade_key",         static_cast<std::int64_t>(ragdollCamFadeKey));
        if (!activePresetName.empty())      gen.insert("active_preset_name", activePresetName);

        toml::table root;
        root.insert("general", std::move(gen));
        std::ostringstream contents;
        contents.exceptions(std::ios::badbit | std::ios::failbit);
        contents << root;
        std::string error;
        if (!WriteFileAtomically(kConfigPath, contents.str(), true, error)) {
            spdlog::error("Failed to save global preferences: {}", error);
            return;
        }
        spdlog::info("Global prefs saved (hotkeys + active preset)");
    }
    catch (const std::exception& error) {
        spdlog::error("Failed to save global preferences: {}", error.what());
    }

    toml::table SettingsManager::BuildSaveTable()
    {
        toml::table root;

        // Preset storage-format stamp. NOT the mod version and not the
        // preset's name — it is invisible to the user and bumps only when a
        // future update changes what stored data MEANS, so a migration can ask
        // "was this written under the old rules?" deterministically instead of
        // sniffing for the presence of some key. A preset with no [meta].format
        // is pre-1.0. It cannot be added to presets already in the wild, which
        // is the same reasoning that kept dialogue_schema_version alive.
        // Bumping this is a deliberate act — read PRESET-COMPAT.md first.
        {
            toml::table meta;
            meta.insert("format", static_cast<std::int64_t>(kPresetFormatVersion));
            root.insert("meta", std::move(meta));
        }

        // INVARIANT: the baseline handed to WriteProfile must equal the FIELD'S
        // INITIALIZER, because WriteProfile omits any value equal to it and
        // ReadProfile then leaves the field at whatever it was initialized to.
        // Baseline and initializer disagreeing means a value equal to the
        // baseline is silently dropped and comes back as the initializer.
        //
        // This tracked CameraProfile::Vanilla() while that WAS the initializer.
        // The authored defaults (2026-08-22) moved the initializer to
        // Default3p(), so a user setting zoom back to vanilla 0 would have had
        // the key dropped and reloaded as 10 â€” hence this follows Default3p now.
        //
        // Specialized families use their own initializer below. Indoor and
        // location snapshots have an explicit Default3p seed on read.
        const auto vanilla = CameraProfile::Default3p();

        auto insertTable = [&](std::string_view path, toml::table&& table) {
            InsertPresetTable(root, path, table);
        };

        // [general]
        {
            toml::table gen;
            cameraCollision.Write(root);
            if (disableCollisionTrees  != false) gen.insert("camera_collision_trees",  disableCollisionTrees);
            if (disableCollisionProps  != false) gen.insert("camera_collision_props",  disableCollisionProps);
            if (disableCollisionActors != false) gen.insert("camera_collision_actors", disableCollisionActors);
            if (transitionMulRotation != 0.5f) gen.insert("transition_mul_rotation", static_cast<double>(transitionMulRotation));
            if (transitionMulPitch    != 0.5f) gen.insert("transition_mul_pitch",    static_cast<double>(transitionMulPitch));
            if (transitionMulPosition != 0.5f) gen.insert("transition_mul_position", static_cast<double>(transitionMulPosition));
            if (transitionMulZoom     != 0.5f) gen.insert("transition_mul_zoom",     static_cast<double>(transitionMulZoom));
            if (transitionMulFOV      != 0.5f) gen.insert("transition_mul_fov",      static_cast<double>(transitionMulFOV));
            if (transitionWeight      != 0.0f) gen.insert("transition_weight",       static_cast<double>(transitionWeight));
            if (cameraLooseness       != 0.0f) gen.insert("camera_looseness",        static_cast<double>(cameraLooseness));
            if (disableVanityCamera != false) gen.insert("disable_vanity_camera", disableVanityCamera);
            if (archeryTracingEnabled   != false) gen.insert("archery_tracing_enabled",   archeryTracingEnabled);
            if (spellTracingEnabled     != false) gen.insert("spell_tracing_enabled",     spellTracingEnabled);
            if (projectileReticleColorR    != 1.0f) gen.insert("projectile_reticle_color_r",    static_cast<double>(projectileReticleColorR));
            if (projectileReticleColorG    != 1.0f) gen.insert("projectile_reticle_color_g",    static_cast<double>(projectileReticleColorG));
            if (projectileReticleColorB    != 1.0f) gen.insert("projectile_reticle_color_b",    static_cast<double>(projectileReticleColorB));
            if (projectileReticleSizeScale != 1.0f) gen.insert("projectile_reticle_size_scale", static_cast<double>(projectileReticleSizeScale));
            if (projectileReticleThickness != 2.0f) gen.insert("projectile_reticle_thickness",  static_cast<double>(projectileReticleThickness));
            if (archeryTracingSmoothTau != 0.015f) gen.insert("archery_tracing_smooth_tau", static_cast<double>(archeryTracingSmoothTau));
            if (sneakMeterOffsetX != -500.0f) gen.insert("sneak_meter_offset_x", static_cast<double>(sneakMeterOffsetX));
            if (sneakMeterOffsetY != -100.0f) gen.insert("sneak_meter_offset_y", static_cast<double>(sneakMeterOffsetY));

            // Legacy [general].first_person_world_fov/hands_fov keys
            // dropped â€” per-state values write under [first_person.<state>]
            // below. Old saves' legacy keys still load (migration in
            // Load()) but new writes use the structured form only.
            if (deathCameraFov != 90.0f) gen.insert("death_camera_fov", static_cast<double>(deathCameraFov));
            if (deathCameraHoldDuration != 5.0f) gen.insert("death_camera_hold_duration", static_cast<double>(deathCameraHoldDuration));
            if (deathCameraInfiniteDuration != false) gen.insert("death_camera_infinite_duration", deathCameraInfiniteDuration);
            if (deathCameraFreeLook != false) gen.insert("death_camera_free_look", deathCameraFreeLook);
            if (deathCameraSkipKey != 0u) gen.insert("death_camera_skip_key", static_cast<std::int64_t>(deathCameraSkipKey));
            if (deathCameraSlowmoStrength != 0.0f) gen.insert("death_camera_slowmo_strength", static_cast<double>(deathCameraSlowmoStrength));
            // Always emitted: the default moved 4.0 -> 0.0 on 2026-08-19, so a
            // missing key can no longer be trusted to mean "the old default".
            gen.insert("death_camera_slowmo_duration", static_cast<double>(deathCameraSlowmoDuration));
            if (deathCamFadeKey != 0u) gen.insert("death_cam_fade_key", static_cast<std::int64_t>(deathCamFadeKey));
            if (ragdollCamFreeLook != false) gen.insert("ragdoll_cam_free_look", ragdollCamFreeLook);
            if (ragdollCamFov != 90.0f) gen.insert("ragdoll_cam_fov", static_cast<double>(ragdollCamFov));
            if (ragdollCamSlowmoStrength != 0.0f) gen.insert("ragdoll_cam_slowmo_strength", static_cast<double>(ragdollCamSlowmoStrength));
            gen.insert("ragdoll_cam_slowmo_duration", static_cast<double>(ragdollCamSlowmoDuration));
            if (ragdollCamFadeKey != 0u) gen.insert("ragdoll_cam_fade_key", static_cast<std::int64_t>(ragdollCamFadeKey));
            if (ragdollCamHoldParentState != false) gen.insert("ragdoll_cam_hold_parent_state", ragdollCamHoldParentState);
            if (verboseLogging) gen.insert("verbose_logging", true);
            if (targetLockAimBias != 1.0f) gen.insert("target_lock_aim_bias", static_cast<double>(targetLockAimBias));
            if (targetLockTrackSeconds != 0.0f) gen.insert("target_lock_acquire_seconds", static_cast<double>(targetLockTrackSeconds));
            if (targetLockAcquireSwingSeconds != 0.20f) gen.insert("target_lock_acquire_swing_seconds", static_cast<double>(targetLockAcquireSwingSeconds));
            if (targetLockSwitchSpeed != 300.0f) gen.insert("target_lock_switch_speed", static_cast<double>(targetLockSwitchSpeed));
            // dialogueEnabled / dialogueFirstPersonEnabled are always on now â€”
            // not persisted.
            if (dialogueMovementEnabled != false) gen.insert("dialogue_movement_enabled", dialogueMovementEnabled);
            if (dialogueMulRotation != 0.2f) gen.insert("dialogue_mul_rotation", static_cast<double>(dialogueMulRotation));
            if (dialogueMulPitch    != 0.2f) gen.insert("dialogue_mul_pitch",    static_cast<double>(dialogueMulPitch));
            if (dialogueMulPosition != 0.2f) gen.insert("dialogue_mul_position", static_cast<double>(dialogueMulPosition));
            if (dialogueMulZoom     != 0.2f) gen.insert("dialogue_mul_zoom",     static_cast<double>(dialogueMulZoom));
            if (dialogueMulFOV      != 0.2f) gen.insert("dialogue_mul_fov",      static_cast<double>(dialogueMulFOV));
            // Legacy flag â€” only write if true so old loaders see it.
            // Future loads (schema v3+) ignore this in favor of the per-
            // category arrays.
            if (dialogueRandomEnabled != false) gen.insert("dialogue_random_enabled", dialogueRandomEnabled);
            for (std::size_t i = 0; i < dialogueRandomOnEntry.size(); ++i) {
                if (dialogueRandomOnEntry[i]) {
                    gen.insert(std::string("dialogue_random_on_entry_") + DialogueCategoryKey(static_cast<DialogueCategory>(i)),
                               dialogueRandomOnEntry[i]);
                }
                if (dialogueRandomOnOption[i]) {
                    gen.insert(std::string("dialogue_random_on_option_") + DialogueCategoryKey(static_cast<DialogueCategory>(i)),
                               dialogueRandomOnOption[i]);
                }
            }
            if (dialogueSwitchOnNpcLine)  gen.insert("dialogue_switch_on_npc_line",  dialogueSwitchOnNpcLine);
            if (dialogueSkipShortNpcLines) gen.insert("dialogue_skip_short_npc_lines", dialogueSkipShortNpcLines);
            if (dialogueAutoSwitchMin != 0.0f) gen.insert("dialogue_auto_switch_min", static_cast<double>(dialogueAutoSwitchMin));
            if (dialogueAutoSwitchMax != 0.0f) gen.insert("dialogue_auto_switch_max", static_cast<double>(dialogueAutoSwitchMax));
            if (dialogueRandomEveryTime) gen.insert("dialogue_random_every_time", dialogueRandomEveryTime);
            if (dialogueMinShotSec != 0.0f) gen.insert("dialogue_min_shot_sec", static_cast<double>(dialogueMinShotSec));
            if (dialogueSchemaVersion != 1) gen.insert("dialogue_schema_version", static_cast<std::int64_t>(dialogueSchemaVersion));
            if (dialogueCycleNextKey != 0) gen.insert("dialogue_cycle_next_key", static_cast<std::int64_t>(dialogueCycleNextKey));
            if (dialogueCyclePrevKey != 0) gen.insert("dialogue_cycle_prev_key", static_cast<std::int64_t>(dialogueCyclePrevKey));
            if (categoriesShoulderSwapKey != 0) gen.insert("categories_shoulder_swap_key", static_cast<std::int64_t>(categoriesShoulderSwapKey));
            if (presetCycleNextKey != 0) gen.insert("preset_cycle_next_key", static_cast<std::int64_t>(presetCycleNextKey));
            if (entryCopyKey       != 0) gen.insert("entry_copy_key",        static_cast<std::int64_t>(entryCopyKey));
            if (entryPasteKey      != 0) gen.insert("entry_paste_key",       static_cast<std::int64_t>(entryPasteKey));
            if (quickTuneHotkey    != 0) gen.insert("quick_tune_hotkey",    static_cast<std::int64_t>(quickTuneHotkey));
            if (!activePresetName.empty()) gen.insert("active_preset_name", activePresetName);
            if (categoriesEditTab != 0) gen.insert("categories_edit_tab", static_cast<std::int64_t>(categoriesEditTab));
            if (weaponsBlockingWardEnabled)      gen.insert("blocking_ward_enabled",       weaponsBlockingWardEnabled);
            if (weaponsBlockingWardSneakEnabled) gen.insert("blocking_ward_sneak_enabled", weaponsBlockingWardSneakEnabled);
            insertTable("general", std::move(gen));
        }

        // [cinematic]
        {
            toml::table ds;
            if (dragonShakeBreathEnabled    != false)   ds.insert("breath_enabled",     dragonShakeBreathEnabled);
            if (dragonShakeBreathAmp        != 0.0f)    ds.insert("breath_amp",         static_cast<double>(dragonShakeBreathAmp));
            if (dragonShakeBreathSpeed      != 1.0f)    ds.insert("breath_speed",       static_cast<double>(dragonShakeBreathSpeed));
            if (dragonShakeBreathRange      != 3000.0f) ds.insert("breath_range",       static_cast<double>(dragonShakeBreathRange));
            if (dragonShakeProjectileEnabled != false)  ds.insert("projectile_enabled", dragonShakeProjectileEnabled);
            if (dragonShakeProjectileAmp    != 0.0f)    ds.insert("projectile_amp",     static_cast<double>(dragonShakeProjectileAmp));
            if (dragonShakeProjectileSpeed  != 1.0f)    ds.insert("projectile_speed",   static_cast<double>(dragonShakeProjectileSpeed));
            if (dragonShakeProjectileRange  != 3000.0f) ds.insert("projectile_range",   static_cast<double>(dragonShakeProjectileRange));
            if (dragonShakeBiteEnabled      != false)   ds.insert("bite_enabled",       dragonShakeBiteEnabled);
            if (dragonShakeBiteAmp          != 0.0f)    ds.insert("bite_amp",           static_cast<double>(dragonShakeBiteAmp));
            if (dragonShakeBiteSpeed        != 1.0f)    ds.insert("bite_speed",         static_cast<double>(dragonShakeBiteSpeed));
            if (dragonShakeBiteRange        != 3000.0f) ds.insert("bite_range",         static_cast<double>(dragonShakeBiteRange));
            if (dragonShakeTailEnabled      != false)   ds.insert("tail_enabled",       dragonShakeTailEnabled);
            if (dragonShakeTailAmp          != 0.0f)    ds.insert("tail_amp",           static_cast<double>(dragonShakeTailAmp));
            if (dragonShakeTailSpeed        != 1.0f)    ds.insert("tail_speed",         static_cast<double>(dragonShakeTailSpeed));
            if (dragonShakeTailRange        != 3000.0f) ds.insert("tail_range",         static_cast<double>(dragonShakeTailRange));
            if (dragonShakeWingEnabled      != false)   ds.insert("wing_enabled",       dragonShakeWingEnabled);
            if (dragonShakeWingAmp          != 0.0f)    ds.insert("wing_amp",           static_cast<double>(dragonShakeWingAmp));
            if (dragonShakeWingSpeed        != 1.0f)    ds.insert("wing_speed",         static_cast<double>(dragonShakeWingSpeed));
            if (dragonShakeWingRange        != 3000.0f) ds.insert("wing_range",         static_cast<double>(dragonShakeWingRange));
            if (dragonShakeLandingEnabled   != false)   ds.insert("landing_enabled",    dragonShakeLandingEnabled);
            if (dragonShakeLandingAmp      != 0.0f)    ds.insert("landing_amp",      static_cast<double>(dragonShakeLandingAmp));
            if (dragonShakeLandingSpeed    != 1.0f)    ds.insert("landing_speed",    static_cast<double>(dragonShakeLandingSpeed));
            if (dragonShakeLandingRange    != 3000.0f) ds.insert("landing_range",    static_cast<double>(dragonShakeLandingRange));
            if (dragonShakeTakeoffEnabled  != false)   ds.insert("takeoff_enabled",  dragonShakeTakeoffEnabled);
            if (dragonShakeTakeoffAmp      != 0.0f)    ds.insert("takeoff_amp",      static_cast<double>(dragonShakeTakeoffAmp));
            if (dragonShakeTakeoffSpeed    != 1.0f)    ds.insert("takeoff_speed",    static_cast<double>(dragonShakeTakeoffSpeed));
            if (dragonShakeTakeoffRange    != 3000.0f) ds.insert("takeoff_range",    static_cast<double>(dragonShakeTakeoffRange));

            toml::table cen;
            if (centurionShakeWalkEnabled)              cen.insert("walk_enabled",  centurionShakeWalkEnabled);
            if (centurionShakeWalkAmp     != 0.0f)      cen.insert("walk_amp",      static_cast<double>(centurionShakeWalkAmp));
            if (centurionShakeWalkSpeed   != 1.0f)      cen.insert("walk_speed",    static_cast<double>(centurionShakeWalkSpeed));
            if (centurionShakeWalkRange   != 3000.0f)   cen.insert("walk_range",    static_cast<double>(centurionShakeWalkRange));
            if (centurionShakeMeleeEnabled)             cen.insert("melee_enabled", centurionShakeMeleeEnabled);
            if (centurionShakeMeleeAmp    != 0.0f)      cen.insert("melee_amp",     static_cast<double>(centurionShakeMeleeAmp));
            if (centurionShakeMeleeSpeed  != 1.0f)      cen.insert("melee_speed",   static_cast<double>(centurionShakeMeleeSpeed));
            if (centurionShakeMeleeRange  != 3000.0f)   cen.insert("melee_range",   static_cast<double>(centurionShakeMeleeRange));
            if (centurionShakeSteamEnabled)             cen.insert("steam_enabled", centurionShakeSteamEnabled);
            if (centurionShakeSteamAmp    != 0.0f)      cen.insert("steam_amp",     static_cast<double>(centurionShakeSteamAmp));
            if (centurionShakeSteamSpeed  != 1.0f)      cen.insert("steam_speed",   static_cast<double>(centurionShakeSteamSpeed));
            if (centurionShakeSteamRange  != 3000.0f)   cen.insert("steam_range",   static_cast<double>(centurionShakeSteamRange));

            toml::table ww;
            if (werewolfTransformIntensity != 0.0f)
                ww.insert("intensity", static_cast<double>(werewolfTransformIntensity));
            if (werewolfTransformSpeed != 1.6f)
                ww.insert("speed", static_cast<double>(werewolfTransformSpeed));

            toml::table vl;
            if (vampireLordTransformIntensity != 0.0f)
                vl.insert("intensity", static_cast<double>(vampireLordTransformIntensity));
            if (vampireLordTransformSpeed != 0.7f)
                vl.insert("speed", static_cast<double>(vampireLordTransformSpeed));

            toml::table vb;
            if (vampireLordBatsIntensity != 0.0f)
                vb.insert("intensity", static_cast<double>(vampireLordBatsIntensity));
            if (vampireLordBatsSpeed != 1.4f)
                vb.insert("speed", static_cast<double>(vampireLordBatsSpeed));

            toml::table ra;
            if (reanimateShakeIntensity != 0.0f)
                ra.insert("intensity", static_cast<double>(reanimateShakeIntensity));
            if (reanimateShakeSpeed != 0.9f)
                ra.insert("speed", static_cast<double>(reanimateShakeSpeed));
            if (reanimateShakeRange != 1200.0f)
                ra.insert("range", static_cast<double>(reanimateShakeRange));

            toml::table su;
            if (summonShakeIntensity != 0.0f)
                su.insert("intensity", static_cast<double>(summonShakeIntensity));
            if (summonShakeSpeed != 1.1f)
                su.insert("speed", static_cast<double>(summonShakeSpeed));
            if (summonShakeRange != 1500.0f)
                su.insert("range", static_cast<double>(summonShakeRange));

            if (reanimateShakeIntensityFp != 0.0f)    ra.insert("intensity_fp", static_cast<double>(reanimateShakeIntensityFp));
            if (reanimateShakeSpeedFp     != 0.9f)    ra.insert("speed_fp",     static_cast<double>(reanimateShakeSpeedFp));
            if (reanimateShakeRangeFp     != 1200.0f) ra.insert("range_fp",     static_cast<double>(reanimateShakeRangeFp));
            if (summonShakeIntensityFp    != 0.0f)    su.insert("intensity_fp", static_cast<double>(summonShakeIntensityFp));
            if (summonShakeSpeedFp        != 1.1f)    su.insert("speed_fp",     static_cast<double>(summonShakeSpeedFp));
            if (summonShakeRangeFp        != 1500.0f) su.insert("range_fp",     static_cast<double>(summonShakeRangeFp));

            toml::table stn;
            if (slowTimeNoiseStrength != 0.0f)
                stn.insert("strength", static_cast<double>(slowTimeNoiseStrength));

            toml::table wd;
            if (weaponDrawNoiseIntensity != 0.0f)
                wd.insert("intensity", static_cast<double>(weaponDrawNoiseIntensity));
            if (weaponDrawNoiseDuration != 0.30f)
                wd.insert("duration", static_cast<double>(weaponDrawNoiseDuration));
            if (weaponDrawNoiseSpeed != 1.0f)
                wd.insert("speed", static_cast<double>(weaponDrawNoiseSpeed));
            if (weaponDrawNoiseIntensityFp != 0.0f)
                wd.insert("intensity_fp", static_cast<double>(weaponDrawNoiseIntensityFp));
            if (weaponDrawNoiseDurationFp != 0.30f)
                wd.insert("duration_fp", static_cast<double>(weaponDrawNoiseDurationFp));
            if (weaponDrawNoiseSpeedFp != 1.0f)
                wd.insert("speed_fp", static_cast<double>(weaponDrawNoiseSpeedFp));

            toml::table cp;
            if (combatPulseIntensity != 0.0f) cp.insert("intensity", static_cast<double>(combatPulseIntensity));
            if (combatPulseDuration  != 0.8f) cp.insert("duration",  static_cast<double>(combatPulseDuration));

            // NPC Noise â€” diffed against 1.0 (the shipped-behaviour default),
            // NOT 0: an absent key must mean "unchanged", never "off".
            toml::table npcn;
            // ALWAYS emitted. The default moved 1.0 -> 0.0 on 2026-08-19, and
            // diff-suppression against a default that has changed is exactly
            // how an old preset silently reinterprets a missing key. Writing
            // it every time means every preset saved from here on is explicit.
            npcn.insert("intensity", static_cast<double>(npcNoiseIntensity));
            if (npcShoutNoiseIntensity != 0.0f)
                npcn.insert("shout_intensity", static_cast<double>(npcShoutNoiseIntensity));
            if (npcShoutNoiseIntensityFp != 0.0f)
                npcn.insert("shout_intensity_fp", static_cast<double>(npcShoutNoiseIntensityFp));
            if (npcNoiseIntensityFp != 0.0f)
                npcn.insert("intensity_fp", static_cast<double>(npcNoiseIntensityFp));
            if (npcMeleeNoiseIntensity != 0.0f)
                npcn.insert("melee_intensity", static_cast<double>(npcMeleeNoiseIntensity));
            if (npcMeleeNoiseIntensityFp != 0.0f)
                npcn.insert("melee_intensity_fp", static_cast<double>(npcMeleeNoiseIntensityFp));
            if (npcArcheryNoiseIntensity != 0.0f)
                npcn.insert("archery_intensity", static_cast<double>(npcArcheryNoiseIntensity));
            if (npcArcheryNoiseIntensityFp != 0.0f)
                npcn.insert("archery_intensity_fp", static_cast<double>(npcArcheryNoiseIntensityFp));
            if (npcTransformNoiseIntensity != 0.0f)
                npcn.insert("transform_intensity", static_cast<double>(npcTransformNoiseIntensity));
            if (npcTransformNoiseIntensityFp != 0.0f)
                npcn.insert("transform_intensity_fp", static_cast<double>(npcTransformNoiseIntensityFp));

            toml::table al;
            if (attackLagMagic   != 0.0f) al.insert("magic",   static_cast<double>(attackLagMagic));
            if (attackLagArchery != 0.0f) al.insert("archery", static_cast<double>(attackLagArchery));

            // Projectile Repulse's global sliders are RETIRED â€” the values
            // live per-entry on the noise profiles now (`repulse`), and the
            // old keys are read once for migration and never written back.
            toml::table pr;  // stays empty; kept so the OR-list below reads uniformly

            toml::table ff;
            if (fleeFramingStrength != 0.0f) ff.insert("strength", static_cast<double>(fleeFramingStrength));

            toml::table jp;
            if (jumpNoiseAmp != 0.0f) jp.insert("amp", static_cast<double>(jumpNoiseAmp));
            // fall_amp inherits ["amp"] on load (pre-split presets), so it must
            // be written whenever the inherited value would be wrong â€” i.e.
            // whenever either dial is nonzero.
            if (fallNoiseAmp != 0.0f || jumpNoiseAmp != 0.0f)
                jp.insert("fall_amp", static_cast<double>(fallNoiseAmp));
            if (jumpRepulse != 0.0f) jp.insert("repulse", static_cast<double>(jumpRepulse));
            if (jumpRepulseFeel != 0.5f) jp.insert("repulse_feel", static_cast<double>(jumpRepulseFeel));
            // First-person halves. No inheritance quirk to carry here: these
            // keys are new, so an absent one plainly means the initializer.
            if (jumpNoiseAmpFp != 0.0f) jp.insert("amp_fp", static_cast<double>(jumpNoiseAmpFp));
            if (fallNoiseAmpFp != 0.0f) jp.insert("fall_amp_fp", static_cast<double>(fallNoiseAmpFp));
            if (jumpRepulseFp != 0.0f) jp.insert("repulse_fp", static_cast<double>(jumpRepulseFp));
            if (jumpRepulseFeelFp != 0.5f) jp.insert("repulse_feel_fp", static_cast<double>(jumpRepulseFeelFp));

            toml::table hb;
            if (headBobIntensity   != 0.0f)  hb.insert("intensity",    static_cast<double>(headBobIntensity));
            if (headBobIntensityFp != 0.0f)  hb.insert("intensity_fp", static_cast<double>(headBobIntensityFp));

            toml::table st;
            if (stairSmoothStrength != 0.0f)  st.insert("strength", static_cast<double>(stairSmoothStrength));
            if (stairSmoothLimit    != 24.0f) st.insert("limit",    static_cast<double>(stairSmoothLimit));

            // Per-source character (diffed against each source's per-kind default).
            auto writeChar = [](toml::table& t, const char* pfx, const CinematicShakeChar& c, const CinematicShakeChar& def) {
                if (c.rotShake    != def.rotShake)    t.insert(std::string(pfx) + "_rot_shake", static_cast<double>(c.rotShake));
                if (c.posShake    != def.posShake)    t.insert(std::string(pfx) + "_pos_shake", static_cast<double>(c.posShake));
                if (c.driftJitter  != def.driftJitter)  t.insert(std::string(pfx) + "_drift", static_cast<double>(c.driftJitter));
                if (c.roughness    != def.roughness)    t.insert(std::string(pfx) + "_rough", static_cast<double>(c.roughness));
                if (c.fadeDuration != def.fadeDuration) t.insert(std::string(pfx) + "_fade",  static_cast<double>(c.fadeDuration));
            };
            // First-person halves (2026-09-06), sparse against their own initializers.
            if (dragonShakeBreathAmpFp   != 0.0f)    ds.insert("breath_amp_fp",   static_cast<double>(dragonShakeBreathAmpFp));
            if (dragonShakeBreathSpeedFp != 1.0f)    ds.insert("breath_speed_fp", static_cast<double>(dragonShakeBreathSpeedFp));
            if (dragonShakeBreathRangeFp != 3000.0f) ds.insert("breath_range_fp", static_cast<double>(dragonShakeBreathRangeFp));
            if (dragonShakeProjectileAmpFp   != 0.0f)    ds.insert("projectile_amp_fp",   static_cast<double>(dragonShakeProjectileAmpFp));
            if (dragonShakeProjectileSpeedFp != 1.0f)    ds.insert("projectile_speed_fp", static_cast<double>(dragonShakeProjectileSpeedFp));
            if (dragonShakeProjectileRangeFp != 3000.0f) ds.insert("projectile_range_fp", static_cast<double>(dragonShakeProjectileRangeFp));
            if (dragonShakeBiteAmpFp   != 0.0f)    ds.insert("bite_amp_fp",   static_cast<double>(dragonShakeBiteAmpFp));
            if (dragonShakeBiteSpeedFp != 1.0f)    ds.insert("bite_speed_fp", static_cast<double>(dragonShakeBiteSpeedFp));
            if (dragonShakeBiteRangeFp != 3000.0f) ds.insert("bite_range_fp", static_cast<double>(dragonShakeBiteRangeFp));
            if (dragonShakeTailAmpFp   != 0.0f)    ds.insert("tail_amp_fp",   static_cast<double>(dragonShakeTailAmpFp));
            if (dragonShakeTailSpeedFp != 1.0f)    ds.insert("tail_speed_fp", static_cast<double>(dragonShakeTailSpeedFp));
            if (dragonShakeTailRangeFp != 3000.0f) ds.insert("tail_range_fp", static_cast<double>(dragonShakeTailRangeFp));
            if (dragonShakeWingAmpFp   != 0.0f)    ds.insert("wing_amp_fp",   static_cast<double>(dragonShakeWingAmpFp));
            if (dragonShakeWingSpeedFp != 1.0f)    ds.insert("wing_speed_fp", static_cast<double>(dragonShakeWingSpeedFp));
            if (dragonShakeWingRangeFp != 3000.0f) ds.insert("wing_range_fp", static_cast<double>(dragonShakeWingRangeFp));
            if (dragonShakeLandingAmpFp   != 0.0f)    ds.insert("landing_amp_fp",   static_cast<double>(dragonShakeLandingAmpFp));
            if (dragonShakeLandingSpeedFp != 1.0f)    ds.insert("landing_speed_fp", static_cast<double>(dragonShakeLandingSpeedFp));
            if (dragonShakeLandingRangeFp != 3000.0f) ds.insert("landing_range_fp", static_cast<double>(dragonShakeLandingRangeFp));
            if (dragonShakeTakeoffAmpFp   != 0.0f)    ds.insert("takeoff_amp_fp",   static_cast<double>(dragonShakeTakeoffAmpFp));
            if (dragonShakeTakeoffSpeedFp != 1.0f)    ds.insert("takeoff_speed_fp", static_cast<double>(dragonShakeTakeoffSpeedFp));
            if (dragonShakeTakeoffRangeFp != 3000.0f) ds.insert("takeoff_range_fp", static_cast<double>(dragonShakeTakeoffRangeFp));
            if (centurionShakeWalkAmpFp   != 0.0f)    cen.insert("walk_amp_fp",   static_cast<double>(centurionShakeWalkAmpFp));
            if (centurionShakeWalkSpeedFp != 1.0f)    cen.insert("walk_speed_fp", static_cast<double>(centurionShakeWalkSpeedFp));
            if (centurionShakeWalkRangeFp != 3000.0f) cen.insert("walk_range_fp", static_cast<double>(centurionShakeWalkRangeFp));
            if (centurionShakeMeleeAmpFp   != 0.0f)    cen.insert("melee_amp_fp",   static_cast<double>(centurionShakeMeleeAmpFp));
            if (centurionShakeMeleeSpeedFp != 1.0f)    cen.insert("melee_speed_fp", static_cast<double>(centurionShakeMeleeSpeedFp));
            if (centurionShakeMeleeRangeFp != 3000.0f) cen.insert("melee_range_fp", static_cast<double>(centurionShakeMeleeRangeFp));
            if (centurionShakeSteamAmpFp   != 0.0f)    cen.insert("steam_amp_fp",   static_cast<double>(centurionShakeSteamAmpFp));
            if (centurionShakeSteamSpeedFp != 1.0f)    cen.insert("steam_speed_fp", static_cast<double>(centurionShakeSteamSpeedFp));
            if (centurionShakeSteamRangeFp != 3000.0f) cen.insert("steam_range_fp", static_cast<double>(centurionShakeSteamRangeFp));
            writeChar(ds,  "breath",     dragonShakeBreathChar,     kCharSustained);
            writeChar(ds,  "projectile", dragonShakeProjectileChar, kCharSharp);
            writeChar(ds,  "bite",       dragonShakeBiteChar,       kCharSharp);
            writeChar(ds,  "tail",       dragonShakeTailChar,       kCharSharp);
            writeChar(ds,  "wing",       dragonShakeWingChar,       kCharSharp);
            writeChar(ds,  "landing",    dragonShakeLandingChar,    kCharHeavy);
            writeChar(ds,  "takeoff",    dragonShakeTakeoffChar,    kCharHeavy);
            writeChar(cen, "walk",       centurionShakeWalkChar,    kCharHeavy);
            writeChar(cen, "melee",      centurionShakeMeleeChar,   kCharHeavy);
            writeChar(cen, "steam",      centurionShakeSteamChar,   kCharSustained);
            writeChar(ww,  "char",       werewolfTransformChar,     kCharWerewolf);
            writeChar(vl,  "char",       vampireLordTransformChar,  kCharVampLord);

            toml::table wwr;
            if (werewolfRevertIntensity != 0.0f)
                wwr.insert("intensity", static_cast<double>(werewolfRevertIntensity));
            if (werewolfRevertSpeed != 1.6f)
                wwr.insert("speed", static_cast<double>(werewolfRevertSpeed));
            writeChar(wwr, "char", werewolfRevertChar, kCharWerewolf);
            toml::table vlr;
            if (vampireLordRevertIntensity != 0.0f)
                vlr.insert("intensity", static_cast<double>(vampireLordRevertIntensity));
            if (vampireLordRevertSpeed != 0.7f)
                vlr.insert("speed", static_cast<double>(vampireLordRevertSpeed));
            writeChar(vlr, "char", vampireLordRevertChar, kCharVampLord);
            writeChar(vb,  "char",       vampireLordBatsChar,       kCharBats);
            writeChar(ra,  "char",       reanimateShakeChar,        kCharReanimate);
            writeChar(ra,  "char_fp",    reanimateShakeCharFp,      kCharReanimate);
            writeChar(su,  "char",       summonShakeChar,           kCharSummon);
            writeChar(su,  "char_fp",    summonShakeCharFp,         kCharSummon);
            writeChar(ds,  "breath_fp", dragonShakeBreathCharFp, kCharSustained);
            writeChar(ds,  "projectile_fp", dragonShakeProjectileCharFp, kCharSharp);
            writeChar(ds,  "bite_fp", dragonShakeBiteCharFp, kCharSharp);
            writeChar(ds,  "tail_fp", dragonShakeTailCharFp, kCharSharp);
            writeChar(ds,  "wing_fp", dragonShakeWingCharFp, kCharSharp);
            writeChar(ds,  "landing_fp", dragonShakeLandingCharFp, kCharHeavy);
            writeChar(ds,  "takeoff_fp", dragonShakeTakeoffCharFp, kCharHeavy);
            writeChar(cen, "walk_fp", centurionShakeWalkCharFp, kCharHeavy);
            writeChar(cen, "melee_fp", centurionShakeMeleeCharFp, kCharHeavy);
            writeChar(cen, "steam_fp", centurionShakeSteamCharFp, kCharSustained);
            writeChar(wd,  "char",       weaponDrawNoiseChar,       kCharDraw);
            writeChar(wd,  "char_fp",    weaponDrawNoiseCharFp,     kCharDraw);


            // Paragliding (mod support): enabled flag + a full camera profile
            // (standard codec, diffed against defaults like the enemy
            // overrides) + a full noise cell (global-cell field set).
            toml::table pgl;
            if (paraglideEnabled) pgl.insert("enabled", true);
            if (paraglideLandFade != 0.5f)
                pgl.insert("land_fade", static_cast<double>(paraglideLandFade));
            if (auto cam = WriteProfile(paraglideProfile, CameraProfile{}); !cam.empty())
                pgl.insert("camera", std::move(cam));
            if (auto ptl = WriteProfile(paraglideTLProfile, CameraProfile{}); !ptl.empty())
                pgl.insert("target_lock", std::move(ptl));
            {
                const NoiseProfile def;
                toml::table pn;
                if (paraglideNoise.amp    != def.amp)    pn.insert("amp",    static_cast<double>(paraglideNoise.amp));
                if (paraglideNoise.speed  != def.speed)  pn.insert("speed",  static_cast<double>(paraglideNoise.speed));
                if (paraglideNoise.sway   != def.sway)   pn.insert("sway",   static_cast<double>(paraglideNoise.sway));
                if (paraglideNoise.tilt   != def.tilt)   pn.insert("tilt",   static_cast<double>(paraglideNoise.tilt));
                if (paraglideNoise.wobble != def.wobble) pn.insert("wobble", static_cast<double>(paraglideNoise.wobble));
                WriteNoiseExtra(pn, paraglideNoise);
                if (!pn.empty()) pgl.insert("noise", std::move(pn));
            }
            // The indoor twin is written only once it differs from the
            // outdoor cell (the reader seeds it as a copy otherwise). When it
            // is written, it is written whole against defaults, exactly like
            // the outdoor table, and always as a table - even an all-default
            // indoor cell must leave a marker, or the reader would copy the
            // outdoor cell over it again.
            if (paraglideNoiseIndoor != paraglideNoise) {
                const NoiseProfile def;
                const auto& pi = paraglideNoiseIndoor;
                toml::table pnI;
                if (pi.amp    != def.amp)    pnI.insert("amp",    static_cast<double>(pi.amp));
                if (pi.speed  != def.speed)  pnI.insert("speed",  static_cast<double>(pi.speed));
                if (pi.sway   != def.sway)   pnI.insert("sway",   static_cast<double>(pi.sway));
                if (pi.tilt   != def.tilt)   pnI.insert("tilt",   static_cast<double>(pi.tilt));
                if (pi.wobble != def.wobble) pnI.insert("wobble", static_cast<double>(pi.wobble));
                WriteNoiseExtra(pnI, pi);
                if (pnI.empty()) pnI.insert("set", true);
                pgl.insert("noise_indoor", std::move(pnI));
            }

            // Table-driven event beats â€” one sparse table per entry, diffed
            // against its static defaults, collected here and inserted into
            // `cin` below alongside the hand-rolled sources.
            std::vector<std::pair<const char*, toml::table>> beatTables;
            for (std::size_t i = 0; i < kEventBeatCount; ++i) {
                const auto  def = DefaultBeatTuning(i);
                const auto& bt  = eventBeats[i];
                toml::table t;
                if (bt.intensity != def.intensity) t.insert("intensity", static_cast<double>(bt.intensity));
                if (bt.speed     != def.speed)     t.insert("speed",     static_cast<double>(bt.speed));
                if (bt.range     != def.range)     t.insert("range",     static_cast<double>(bt.range));
                if (bt.direction != def.direction) t.insert("direction", static_cast<double>(bt.direction));
                writeChar(t, "char", bt.chr, def.chr);
                if (!t.empty()) beatTables.emplace_back(kEventBeatDefs[i].tomlKey, std::move(t));
            }

            const bool anyCinematic = !ds.empty() || !cen.empty() || !ww.empty() || !vl.empty() || !wwr.empty() || !vlr.empty() || !cp.empty() || !al.empty() || !pr.empty() || !ff.empty() || !jp.empty() || !hb.empty() || !st.empty() || !wd.empty() || !vb.empty() || !ra.empty() || !su.empty() || !stn.empty() || !beatTables.empty() || !npcn.empty() || !pgl.empty();
            if (anyCinematic) {
                toml::table cin;
                if (!ds.empty())  cin.insert("dragons",                std::move(ds));
                if (!cen.empty()) cin.insert("centurion",              std::move(cen));
                if (!ww.empty())  cin.insert("werewolf_transform",     std::move(ww));
                if (!vl.empty())  cin.insert("vampire_lord_transform", std::move(vl));
                if (!wwr.empty()) cin.insert("werewolf_revert",        std::move(wwr));
                if (!vlr.empty()) cin.insert("vampire_lord_revert",    std::move(vlr));
                if (!vb.empty())  cin.insert("vampire_lord_bats",      std::move(vb));
                if (!ra.empty())  cin.insert("reanimate",              std::move(ra));
                if (!su.empty())  cin.insert("summon",                 std::move(su));
                if (!stn.empty()) cin.insert("slow_time",              std::move(stn));
                if (!cp.empty())  cin.insert("combat_pulse",           std::move(cp));
                if (!npcn.empty()) cin.insert("npc_noise",             std::move(npcn));
                if (!al.empty())  cin.insert("attack_lag",             std::move(al));
                if (!pr.empty())  cin.insert("projectile_repulse",     std::move(pr));
                if (!ff.empty())  cin.insert("flee_framing",           std::move(ff));
                if (!jp.empty())  cin.insert("jumping",                std::move(jp));
                if (!hb.empty())  cin.insert("head_bob",               std::move(hb));
                if (!st.empty())  cin.insert("stairs",                 std::move(st));
                if (!wd.empty())  cin.insert("weapon_draw",            std::move(wd));
                if (!pgl.empty()) cin.insert("paraglide",              std::move(pgl));
                for (auto& [key, t] : beatTables) cin.insert(key, std::move(t));
                insertTable("cinematic", std::move(cin));
            }
        }

        // Show Player In Menus â€” per-menu sub-tables. Only emit fields
        // that differ from the entry default so configs stay compact.
        {
            toml::table spim;
            auto saveEntry = [](toml::table& parent, const char* key, const ShowPlayerInMenuEntry& e) {
                ShowPlayerInMenuEntry def;
                toml::table sub;
                if (e.enabled != def.enabled) sub.insert("enabled", e.enabled);
                if (e.dragonOnly != def.dragonOnly) sub.insert("dragon_only", e.dragonOnly);
                if (e.offsetX != def.offsetX) sub.insert("offset_x", static_cast<double>(e.offsetX));
                if (e.offsetY != def.offsetY) sub.insert("offset_y", static_cast<double>(e.offsetY));
                if (e.offsetZ != def.offsetZ) sub.insert("offset_z", static_cast<double>(e.offsetZ));
                if (std::abs(e.yaw - def.yaw) > 0.0001f) sub.insert("yaw", static_cast<double>(e.yaw));
                if (e.fov != def.fov) sub.insert("fov", static_cast<double>(e.fov));
                if (e.unpauseGame != def.unpauseGame) sub.insert("unpause_game", e.unpauseGame);
                // allowMovement is always emitted because Barter's
                // baseline default (false) differs from the struct
                // default (true), so we can't rely on "default = struct
                // default" diff suppression here.
                sub.insert("allow_movement", e.allowMovement);
                if (e.allowCameraControl != def.allowCameraControl)
                    sub.insert("allow_camera_control", e.allowCameraControl);
                if (e.force3pFromFirstPerson != def.force3pFromFirstPerson)
                    sub.insert("force_3p_from_1p", e.force3pFromFirstPerson);
                if (!sub.empty()) parent.insert(key, std::move(sub));
            };
            saveEntry(spim, "inventory", showPlayerInInventory);
            saveEntry(spim, "container", showPlayerInContainer);
            saveEntry(spim, "barter",    showPlayerInBarter);
            saveEntry(spim, "magic",     showPlayerInMagic);
            saveEntry(spim, "tween",     showPlayerInTween);
            saveEntry(spim, "wait",      showPlayerInWait);
            saveEntry(spim, "favorites", showPlayerInFavorites);
            if (!spim.empty()) insertTable("show_player_in_menus", std::move(spim));
        }

        // Dialogue buckets: 5 categories Ã— 2 POVs Ã— 2 envs. Outdoor env
        // writes to the legacy `dialogue.buckets.{cat}.{pov}` path (so
        // v2 configs keep loading); Indoor env writes to a `.indoor`
        // sub-path and only exists for Dragons / Creatures / SpecificNPC.
        // Skip empty buckets to keep TOML compact.
        //
        // These dialogue look "presets" live inside named presets only.
        // BuildSaveTable is called solely by the preset writer; the global
        // config (Save) persists just hotkeys + the active preset name, so
        // dialogue presets never leak into DietDrCamera.toml. A dialogue
        // preset persists only when the user saves/updates a master preset,
        // and loading that preset restores its dialogue buckets.
        for (std::size_t cat = 0; cat < static_cast<std::size_t>(DialogueCategory::Count); ++cat) {
            const auto catE = static_cast<DialogueCategory>(cat);
            // Horseback is deliberately NOT env-aware: it is a third value of
            // the Outdoor/Indoor toggle, not a category that splits by one.
            const bool envAware =
                catE == DialogueCategory::Dragons   ||
                catE == DialogueCategory::Creatures ||
                catE == DialogueCategory::SpecificNPC;
            const bool isSpecificNpc = (catE == DialogueCategory::SpecificNPC);
            for (std::size_t pov = 0; pov < static_cast<std::size_t>(DialoguePOV::Count); ++pov) {
                const auto povE = static_cast<DialoguePOV>(pov);
                const std::string basePath = std::string("dialogue.buckets.")
                    + DialogueCategoryKey(catE) + "."
                    + DialoguePOVKey(povE);

                // env=Outdoor â†’ legacy path
                {
                    const auto idx = DialogueBucketIndex(catE, povE, DialogueEnv::Outdoor);
                    const auto& bucket = dialogueBuckets[idx];
                    if (!(bucket.looks.empty() && bucket.activeIndex == -1 && !bucket.randomEnabled)) {
                        insertTable(basePath, WriteDialogueBucket(bucket, isSpecificNpc));
                    }
                }
                // env=Indoor â†’ only for env-aware categories
                if (envAware) {
                    const auto idx = DialogueBucketIndex(catE, povE, DialogueEnv::Indoor);
                    const auto& bucket = dialogueBuckets[idx];
                    if (!(bucket.looks.empty() && bucket.activeIndex == -1 && !bucket.randomEnabled)) {
                        const std::string path = basePath + "." + DialogueEnvKey(DialogueEnv::Indoor);
                        insertTable(path, WriteDialogueBucket(bucket, isSpecificNpc));
                    }
                }
            }
        }

        // Sheathed
        insertTable("sheathed",        WriteProfile(sheathed, vanilla));
        insertTable("sheathed.sprint", WriteProfile(sheathedSprint, vanilla));
        insertTable("sheathed.swim",   WriteProfile(sheathedSwim, vanilla));
        insertTable("sheathed.sneak",  WriteProfile(sheathedSneak, vanilla));

        // Weapons â€” Melee
        insertTable("weapons.melee",         WriteProfile(weaponsMelee, vanilla));
        insertTable("weapons.melee.sprint",  WriteProfile(weaponsMeleeSprint, vanilla));
        insertTable("weapons.melee.swim",    WriteProfile(weaponsMeleeSwim, vanilla));
        insertTable("weapons.melee.attack",  WriteProfile(weaponsMeleeAttack, vanilla));
        insertTable("weapons.melee.sneak",   WriteProfile(weaponsMeleeSneak, vanilla));
        insertTable("weapons.melee.power_attack",        WriteProfile(weaponsMeleePowerAttack, vanilla));
        insertTable("weapons.melee.sneak_attack",        WriteProfile(weaponsMeleeSneakAttack, vanilla));
        insertTable("weapons.melee.sneak_power_attack",  WriteProfile(weaponsMeleeSneakPowerAttack, vanilla));
        insertTable("weapons.melee.sprint_attack",       WriteProfile(weaponsMeleeSprintAttack, vanilla));
        insertTable("weapons.melee.sprint_power_attack", WriteProfile(weaponsMeleeSprintPowerAttack, vanilla));
        insertTable("weapons.melee.overrides",        WriteMeleeOverrides(weaponsMeleeOverrides,       vanilla));
        insertTable("weapons.melee.sprint.overrides", WriteMeleeOverrides(weaponsMeleeSprintOverrides, vanilla));
        insertTable("weapons.melee.swim.overrides",   WriteMeleeOverrides(weaponsMeleeSwimOverrides,   vanilla));
        insertTable("weapons.melee.attack.overrides",      WriteMeleeOverrides(weaponsMeleeAttackOverrides,     vanilla));
        insertTable("weapons.melee.sneak.overrides",       WriteMeleeOverrides(weaponsMeleeSneakOverrides,      vanilla));
        insertTable("weapons.melee.shout.overrides",       WriteMeleeOverrides(weaponsMeleeShoutOverrides,      vanilla));
        insertTable("weapons.melee.shout.sneak.overrides", WriteMeleeOverrides(weaponsMeleeShoutSneakOverrides, vanilla));
        insertTable("weapons.melee.power_attack.overrides",        WriteMeleeOverrides(weaponsMeleePowerAttackOverrides,       vanilla));
        insertTable("weapons.melee.sneak_attack.overrides",        WriteMeleeOverrides(weaponsMeleeSneakAttackOverrides,       vanilla));
        insertTable("weapons.melee.sneak_power_attack.overrides",  WriteMeleeOverrides(weaponsMeleeSneakPowerAttackOverrides,  vanilla));
        insertTable("weapons.melee.sprint_attack.overrides",       WriteMeleeOverrides(weaponsMeleeSprintAttackOverrides,      vanilla));
        insertTable("weapons.melee.sprint_power_attack.overrides", WriteMeleeOverrides(weaponsMeleeSprintPowerAttackOverrides, vanilla));

        // Directional power-attack overrides (Categories) â€” skip on default-
        // and-disabled to keep the TOML tidy; emit enabled=true when set.
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            auto ovTbl = WriteMeleeOverrides(weaponsMeleePowerAttackDirOverrides[d], CameraProfile{});
            const bool has = weaponsMeleePowerAttackDirEnabled[d] ||
                             weaponsMeleePowerAttackDir[d] != CameraProfile{} ||
                             !ovTbl.empty();
            if (!has) continue;
            auto t = WriteProfile(weaponsMeleePowerAttackDir[d], CameraProfile{});
            if (weaponsMeleePowerAttackDirEnabled[d]) t.insert("enabled", true);
            if (!ovTbl.empty()) t.insert("overrides", std::move(ovTbl));
            insertTable(std::string("weapons.melee.power_attack.dir.") + kPADirKeys[d], std::move(t));
        }

        // Per-specific-form bindings (all categories) â€” array of tables
        // under weapons.bound_forms. Each entry carries identity + category
        // + up to GetBindingSubStateCount(category) sub-state profiles.
        // Profiles diff against vanilla so unchanged slots emit nothing.
        if (!weaponBindings.empty()) {
            toml::array arr;
            for (const auto& b : weaponBindings) {
                if (b.formID == 0 || b.pluginName.empty()) continue;
                toml::table entry;
                entry.insert("form_id", static_cast<std::int64_t>(b.formID));
                entry.insert("plugin",  b.pluginName);
                ItemBindings::WriteMetadata(entry, b);
                if (!b.displayName.empty()) entry.insert("display", b.displayName);
                entry.insert("category", static_cast<std::int64_t>(b.category));
                if (b.castType != SpellCastType::Unknown) {
                    entry.insert("cast_type", static_cast<std::int64_t>(b.castType));
                }
                if (b.fpOnly) entry.insert("fp_only", true);
                const std::size_t slots = GetBindingSubStateCount(b.category);

                // Categories profiles.
                toml::table profs;
                for (std::size_t i = 0; i < slots; ++i) {
                    if (!b.enabled[i] && b.profiles[i] == CameraProfile{}) continue;
                    toml::table sub = WriteProfile(b.profiles[i], vanilla);
                    if (b.enabled[i]) sub.insert("enabled", true);
                    if (!sub.empty()) {
                        char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                        profs.insert(key, std::move(sub));
                    }
                }
                if (!profs.empty()) entry.insert("profiles", std::move(profs));

                // Target Lock profiles â€” vanilla = empty CameraProfile (TL
                // diffs against the empty-baseline elsewhere in this file).
                toml::table tlProfs;
                for (std::size_t i = 0; i < slots; ++i) {
                    if (!b.tlEnabled[i] && b.tlProfiles[i] == CameraProfile{}) continue;
                    toml::table sub = WriteProfile(b.tlProfiles[i], CameraProfile{});
                    if (b.tlEnabled[i]) sub.insert("enabled", true);
                    if (!sub.empty()) {
                        char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                        tlProfs.insert(key, std::move(sub));
                    }
                }
                if (!tlProfs.empty()) entry.insert("tl_profiles", std::move(tlProfs));

                // Noise profiles â€” diff against default NoiseProfile.
                toml::table noiseProfs;
                {
                    const NoiseProfile def;
                    for (std::size_t i = 0; i < slots; ++i) {
                        const auto& n = b.noiseProfiles[i];
                        const bool dflt = (n.enabled == def.enabled &&
                                            n.amp == def.amp && n.speed == def.speed &&
                                            n.sway == def.sway && n.tilt == def.tilt &&
                                            n.wobble == def.wobble &&
                                            NoiseExtraIsDefault(n));
                        if (!b.noiseEnabled[i] && dflt) continue;
                        toml::table sub;
                        if (b.noiseEnabled[i]) sub.insert("enabled", true);
                        if (n.amp    != def.amp)    sub.insert("amp",    static_cast<double>(n.amp));
                        if (n.speed  != def.speed)  sub.insert("speed",  static_cast<double>(n.speed));
                        if (n.sway   != def.sway)   sub.insert("sway",   static_cast<double>(n.sway));
                        if (n.tilt   != def.tilt)   sub.insert("tilt",   static_cast<double>(n.tilt));
                        if (n.wobble != def.wobble) sub.insert("wobble", static_cast<double>(n.wobble));
                        WriteNoiseExtra(sub, n);
                        if (n.enabled != def.enabled) sub.insert("np_enabled", n.enabled);
                        if (!sub.empty()) {
                            char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                            noiseProfs.insert(key, std::move(sub));
                        }
                    }
                }
                if (!noiseProfs.empty()) entry.insert("noise_profiles", std::move(noiseProfs));

                // ----- Indoor variants (same per-slot diff as above) -----
                // One writer per kind, invoked once per environment: the
                // sparse-diff rules stay in a single place, so adding an
                // environment cannot drift them apart.
                {
                    const auto writeEnvCam =
                        [&](const char* tblKey,
                            const std::array<CameraProfile, kWeaponBindingSubStates>& src,
                            const std::array<bool, kWeaponBindingSubStates>& en,
                            const CameraProfile& def) {
                        toml::table t;
                        for (std::size_t i = 0; i < slots; ++i) {
                            if (!en[i] && src[i] == CameraProfile{}) continue;
                            toml::table sub = WriteProfile(src[i], def);
                            if (en[i]) sub.insert("enabled", true);
                            if (!sub.empty()) {
                                char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                                t.insert(key, std::move(sub));
                            }
                        }
                        if (!t.empty()) entry.insert(tblKey, std::move(t));
                    };
                    writeEnvCam("profiles_indoor",    b.profilesIndoor,   b.enabled,   vanilla);
                    writeEnvCam("tl_profiles_indoor", b.tlProfilesIndoor, b.tlEnabled, CameraProfile{});

                    const auto writeEnvNoise =
                        [&](const char* tblKey,
                            const std::array<NoiseProfile, kWeaponBindingSubStates>& src) {
                        toml::table t;
                        const NoiseProfile def;
                        for (std::size_t i = 0; i < slots; ++i) {
                            const auto& n = src[i];
                            const bool dflt = (n.enabled == def.enabled &&
                                               n.amp == def.amp && n.speed == def.speed &&
                                               n.sway == def.sway && n.tilt == def.tilt &&
                                               n.wobble == def.wobble &&
                                               NoiseExtraIsDefault(n));
                            if (!b.noiseEnabled[i] && dflt) continue;
                            toml::table sub;
                            if (b.noiseEnabled[i]) sub.insert("enabled", true);
                            if (n.amp    != def.amp)    sub.insert("amp",    static_cast<double>(n.amp));
                            if (n.speed  != def.speed)  sub.insert("speed",  static_cast<double>(n.speed));
                            if (n.sway   != def.sway)   sub.insert("sway",   static_cast<double>(n.sway));
                            if (n.tilt   != def.tilt)   sub.insert("tilt",   static_cast<double>(n.tilt));
                            if (n.wobble != def.wobble) sub.insert("wobble", static_cast<double>(n.wobble));
                            WriteNoiseExtra(sub, n);
                            if (n.enabled != def.enabled) sub.insert("np_enabled", n.enabled);
                            if (!sub.empty()) {
                                char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                                t.insert(key, std::move(sub));
                            }
                        }
                        if (!t.empty()) entry.insert(tblKey, std::move(t));
                    };
                    writeEnvNoise("noise_profiles_indoor", b.noiseProfilesIndoor);
                }

                // ----- Per-hand overrides (Spell bindings) -----
                // Written sparsely: a hand slot appears only when its enable
                // flag is on or its values differ from the untouched default.
                {
                    const auto writeHandCam =
                        [&](const char* tblKey,
                            const std::array<std::array<CameraProfile, kWeaponBindingSubStates>, kMagicHandCount>& src,
                            const std::array<std::array<bool, kWeaponBindingSubStates>, kMagicHandCount>& en) {
                        toml::table handTbl;
                        for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                            toml::table slotTbl;
                            for (std::size_t i = 0; i < slots; ++i) {
                                if (!en[h][i] && src[h][i] == CameraProfile{}) continue;
                                toml::table s2 = WriteProfile(src[h][i], CameraProfile{});
                                if (en[h][i]) s2.insert("enabled", true);
                                if (!s2.empty()) {
                                    char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                                    slotTbl.insert(key, std::move(s2));
                                }
                            }
                            if (!slotTbl.empty()) handTbl.insert(GetMagicHandTomlKey(h), std::move(slotTbl));
                        }
                        if (!handTbl.empty()) entry.insert(tblKey, std::move(handTbl));
                    };
                    writeHandCam("hand_profiles",           b.handProfiles,         b.handEnabled);
                    writeHandCam("hand_profiles_indoor",    b.handProfilesIndoor,   b.handEnabled);
                    writeHandCam("hand_tl_profiles",        b.handTlProfiles,       b.handTlEnabled);
                    writeHandCam("hand_tl_profiles_indoor", b.handTlProfilesIndoor, b.handTlEnabled);

                    const NoiseProfile def;
                    const auto writeHandNoise =
                        [&](const char* tblKey,
                            const std::array<std::array<NoiseProfile, kWeaponBindingSubStates>, kMagicHandCount>& src) {
                        toml::table handTbl;
                        for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                            toml::table slotTbl;
                            for (std::size_t i = 0; i < slots; ++i) {
                                const auto& n = src[h][i];
                                const bool dflt = (n.enabled == def.enabled &&
                                                   n.amp == def.amp && n.speed == def.speed &&
                                                   n.sway == def.sway && n.tilt == def.tilt &&
                                                   n.wobble == def.wobble &&
                                                   NoiseExtraIsDefault(n));
                                if (!b.handNoiseEnabled[h][i] && dflt) continue;
                                toml::table s2;
                                if (b.handNoiseEnabled[h][i]) s2.insert("enabled", true);
                                if (n.amp    != def.amp)    s2.insert("amp",    static_cast<double>(n.amp));
                                if (n.speed  != def.speed)  s2.insert("speed",  static_cast<double>(n.speed));
                                if (n.sway   != def.sway)   s2.insert("sway",   static_cast<double>(n.sway));
                                if (n.tilt   != def.tilt)   s2.insert("tilt",   static_cast<double>(n.tilt));
                                if (n.wobble != def.wobble) s2.insert("wobble", static_cast<double>(n.wobble));
                                WriteNoiseExtra(s2, n);
                                if (n.enabled != def.enabled) s2.insert("np_enabled", n.enabled);
                                if (!s2.empty()) {
                                    char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                                    slotTbl.insert(key, std::move(s2));
                                }
                            }
                            if (!slotTbl.empty()) handTbl.insert(GetMagicHandTomlKey(h), std::move(slotTbl));
                        }
                        if (!handTbl.empty()) entry.insert(tblKey, std::move(handTbl));
                    };
                    writeHandNoise("hand_noise",        b.handNoise);
                    writeHandNoise("hand_noise_indoor", b.handNoiseIndoor);
                }

                // First Person profiles â€” sparse: a slot appears only when
                // something diverges from the default profile.
                {
                    toml::table fpProfs;
                    for (std::size_t i = 0; i < slots; ++i) {
                        toml::table sub2 = WriteFpProfileTable(b.fpProfiles[i]);
                        if (!sub2.empty()) {
                            char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                            fpProfs.insert(key, std::move(sub2));
                        }
                    }
                    if (!fpProfs.empty()) entry.insert("fp_profiles", std::move(fpProfs));
                }

                // Per-(enemy, sub-state) enemy overrides for this binding.
                const auto writeBindingEnemy =
                    [&](const std::array<std::array<EnemyFieldOverride, kWeaponBindingSubStates>,
                                         kEnemyOverrideEnemies>& grid,
                        const char* tableKey) {
                    toml::table tlEnemyOv;
                    for (std::size_t e = 0; e < kEnemyOverrideEnemies; ++e) {
                        toml::table enemyTbl;
                        for (std::size_t i = 0; i < slots; ++i) {
                            const auto& o = grid[e][i];
                            if (!EnemyOverrideHasContent(o)) {
                                const bool anyValue =
                                    o.profile != CameraProfile{};
                                if (!anyValue) continue;
                            }
                            char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                            enemyTbl.insert(key, WriteEnemyOverride(o));
                        }
                        if (!enemyTbl.empty()) tlEnemyOv.insert(kEnemyKeys[e], std::move(enemyTbl));
                    }
                    if (!tlEnemyOv.empty()) entry.insert(tableKey, std::move(tlEnemyOv));
                };
                writeBindingEnemy(b.tlEnemyOverrides,       "tl_enemy_overrides");
                writeBindingEnemy(b.tlEnemyOverridesIndoor, "tl_enemy_overrides_indoor");

                // Per-CUSTOM-enemy overrides owned by this binding (grids are
                // keyed by the custom enemy's identity string). Empty grids
                // (no configured cell in either environment) are dropped.
                toml::array custGrids;
                for (const auto& g : b.customEnemyGrids) {
                    toml::table gt;
                    toml::table outT, inT;
                    auto writeArr = [&](const std::array<EnemyFieldOverride, kWeaponBindingSubStates>& src,
                                        toml::table& dst) {
                        for (std::size_t i = 0; i < slots; ++i) {
                            const auto& o = src[i];
                            if (!EnemyOverrideHasContent(o)) {
                                const bool anyValue = o.profile != CameraProfile{};
                                if (!anyValue) continue;
                            }
                            char key[8]; std::snprintf(key, sizeof(key), "s%zu", i);
                            dst.insert(key, WriteEnemyOverride(o));
                        }
                    };
                    writeArr(g.slots,       outT);
                    writeArr(g.slotsIndoor, inT);
                    if (outT.empty() && inT.empty()) continue;
                    gt.insert("key", g.identityKey);
                    if (!outT.empty()) gt.insert("slots",        std::move(outT));
                    if (!inT.empty())  gt.insert("slots_indoor", std::move(inT));
                    custGrids.push_back(std::move(gt));
                }
                if (!custGrids.empty()) entry.insert("custom_enemy_overrides", std::move(custGrids));

                arr.push_back(std::move(entry));
            }
            if (!arr.empty()) {
                if (!root.contains("weapons")) root.insert("weapons", toml::table{});
                auto* w = root.get("weapons")->as_table();
                w->insert("bound_forms", std::move(arr));
            }
        }

        // Weapons â€” Blocking (9 sub-states).
        insertTable("weapons.blocking.one_handed",        WriteProfile(weaponsBlockingOneHanded, vanilla));
        insertTable("weapons.blocking.two_handed",        WriteProfile(weaponsBlockingTwoHanded, vanilla));
        insertTable("weapons.blocking.shield",            WriteProfile(weaponsBlockingShield, vanilla));
        insertTable("weapons.blocking.ward",              WriteProfile(weaponsBlockingWard, vanilla));
        insertTable("weapons.blocking.one_handed.sneak",  WriteProfile(weaponsBlockingOneHandedSneak, vanilla));
        insertTable("weapons.blocking.two_handed.sneak",  WriteProfile(weaponsBlockingTwoHandedSneak, vanilla));
        insertTable("weapons.blocking.shield.sneak",      WriteProfile(weaponsBlockingShieldSneak, vanilla));
        insertTable("weapons.blocking.ward.sneak",        WriteProfile(weaponsBlockingWardSneak, vanilla));
        insertTable("weapons.blocking.shield.sprint",     WriteProfile(weaponsBlockingShieldSprint, vanilla));

        // Weapons â€” Bow
        insertTable("weapons.bow",        WriteProfile(weaponsBow, vanilla));
        insertTable("weapons.bow.sprint", WriteProfile(weaponsBowSprint, vanilla));
        insertTable("weapons.bow.swim",   WriteProfile(weaponsBowSwim, vanilla));
        insertTable("weapons.bow.draw",   WriteProfile(weaponsBowDraw, vanilla));
        insertTable("weapons.bow.sneak",  WriteProfile(weaponsBowSneak, vanilla));
        insertTable("weapons.bow.sneak.draw", WriteProfile(weaponsBowSneakDraw, vanilla));
        insertTable("weapons.bow.zoom",       WriteProfile(weaponsBowZoom, vanilla));
        insertTable("weapons.bow.sneak.zoom", WriteProfile(weaponsBowSneakZoom, vanilla));

        // Weapons â€” Crossbow
        insertTable("weapons.crossbow",        WriteProfile(weaponsCrossbow, vanilla));
        insertTable("weapons.crossbow.sprint", WriteProfile(weaponsCrossbowSprint, vanilla));
        insertTable("weapons.crossbow.swim",   WriteProfile(weaponsCrossbowSwim, vanilla));
        insertTable("weapons.crossbow.draw",   WriteProfile(weaponsCrossbowDraw, vanilla));
        insertTable("weapons.crossbow.sneak",  WriteProfile(weaponsCrossbowSneak, vanilla));
        insertTable("weapons.crossbow.sneak.draw", WriteProfile(weaponsCrossbowSneakDraw, vanilla));
        insertTable("weapons.crossbow.zoom",       WriteProfile(weaponsCrossbowZoom, vanilla));
        insertTable("weapons.crossbow.sneak.zoom", WriteProfile(weaponsCrossbowSneakZoom, vanilla));

        // Weapons â€” Magic base
        insertTable("weapons.magic",        WriteProfile(weaponsMagic, vanilla));
        insertTable("weapons.magic.sprint", WriteProfile(weaponsMagicSprint, vanilla));
        insertTable("weapons.magic.swim",   WriteProfile(weaponsMagicSwim, vanilla));
        insertTable("weapons.magic.sneak",  WriteProfile(weaponsMagicSneak, vanilla));

        // Magic schools
        insertTable("weapons.magic.alteration.concentration",   WriteProfile(magicAlterationConcentration, vanilla));
        insertTable("weapons.magic.alteration.fire_and_forget",  WriteProfile(magicAlterationFireAndForget, vanilla));
        insertTable("weapons.magic.alteration.ritual",           WriteProfile(magicAlterationRitual, vanilla));

        insertTable("weapons.magic.conjuration.concentration",   WriteProfile(magicConjurationConcentration, vanilla));
        insertTable("weapons.magic.conjuration.fire_and_forget",  WriteProfile(magicConjurationFireAndForget, vanilla));
        insertTable("weapons.magic.conjuration.ritual",           WriteProfile(magicConjurationRitual, vanilla));

        insertTable("weapons.magic.destruction.concentration",   WriteProfile(magicDestructionConcentration, vanilla));
        insertTable("weapons.magic.destruction.fire_and_forget",  WriteProfile(magicDestructionFireAndForget, vanilla));
        insertTable("weapons.magic.destruction.ritual",           WriteProfile(magicDestructionRitual, vanilla));

        insertTable("weapons.magic.illusion.concentration",   WriteProfile(magicIllusionConcentration, vanilla));
        insertTable("weapons.magic.illusion.fire_and_forget",  WriteProfile(magicIllusionFireAndForget, vanilla));
        insertTable("weapons.magic.illusion.ritual",           WriteProfile(magicIllusionRitual, vanilla));

        insertTable("weapons.magic.restoration.concentration",   WriteProfile(magicRestorationConcentration, vanilla));
        insertTable("weapons.magic.restoration.fire_and_forget",  WriteProfile(magicRestorationFireAndForget, vanilla));
        insertTable("weapons.magic.restoration.ritual",           WriteProfile(magicRestorationRitual, vanilla));

        // Magic schools â€” sneak variants
        insertTable("weapons.magic.alteration.sneak.concentration",   WriteProfile(magicAlterationConcentrationSneak, vanilla));
        insertTable("weapons.magic.alteration.sneak.fire_and_forget", WriteProfile(magicAlterationFireAndForgetSneak, vanilla));
        insertTable("weapons.magic.alteration.sneak.ritual",          WriteProfile(magicAlterationRitualSneak, vanilla));
        insertTable("weapons.magic.conjuration.sneak.concentration",   WriteProfile(magicConjurationConcentrationSneak, vanilla));
        insertTable("weapons.magic.conjuration.sneak.fire_and_forget", WriteProfile(magicConjurationFireAndForgetSneak, vanilla));
        insertTable("weapons.magic.conjuration.sneak.ritual",          WriteProfile(magicConjurationRitualSneak, vanilla));
        insertTable("weapons.magic.destruction.sneak.concentration",   WriteProfile(magicDestructionConcentrationSneak, vanilla));
        insertTable("weapons.magic.destruction.sneak.fire_and_forget", WriteProfile(magicDestructionFireAndForgetSneak, vanilla));
        insertTable("weapons.magic.destruction.sneak.ritual",          WriteProfile(magicDestructionRitualSneak, vanilla));
        insertTable("weapons.magic.illusion.sneak.concentration",   WriteProfile(magicIllusionConcentrationSneak, vanilla));
        insertTable("weapons.magic.illusion.sneak.fire_and_forget", WriteProfile(magicIllusionFireAndForgetSneak, vanilla));
        insertTable("weapons.magic.illusion.sneak.ritual",          WriteProfile(magicIllusionRitualSneak, vanilla));
        insertTable("weapons.magic.restoration.sneak.concentration",   WriteProfile(magicRestorationConcentrationSneak, vanilla));
        insertTable("weapons.magic.restoration.sneak.fire_and_forget", WriteProfile(magicRestorationFireAndForgetSneak, vanilla));
        insertTable("weapons.magic.restoration.sneak.ritual",          WriteProfile(magicRestorationRitualSneak, vanilla));

        // Magic per-hand overrides â€” [<prefix><school>[.sneak].<cast>.hand.
        // <left|both|right>], profile schema + "enabled" marker. Untouched
        // disabled hands are skipped so the file stays compact. Shared
        // writer for the Categories grid here and the TL grid (below).
        const auto writeMagicHandGrid = [&](const char* prefix, MagicHandGrid& grid) {
            for (std::size_t si = 0; si < 5; ++si)
                for (std::size_t ci = 0; ci < 3; ++ci)
                    for (std::size_t sn = 0; sn < 2; ++sn)
                        for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                            auto& set = grid[si][ci][sn];
                            if (!set.enabled[h] && set.profiles[h] == CameraProfile{}) continue;
                            toml::table sub = WriteProfile(set.profiles[h], CameraProfile{});
                            if (set.enabled[h]) sub.insert("enabled", true);
                            insertTable(std::string(prefix) + GetMagicSchoolTomlKey(si) +
                                            (sn ? ".sneak." : ".") + GetMagicCastTomlKey(ci) +
                                            ".hand." + GetMagicHandTomlKey(h),
                                        std::move(sub));
                        }
        };
        writeMagicHandGrid("weapons.magic.", magicHandOverrides);

        // Weapons â€” Staves
        insertTable("weapons.staves",        WriteProfile(weaponsStaves, vanilla));
        insertTable("weapons.staves.sprint", WriteProfile(weaponsStavesSprint, vanilla));
        insertTable("weapons.staves.swim",   WriteProfile(weaponsStavesSwim, vanilla));
        insertTable("weapons.staves.sneak",  WriteProfile(weaponsStavesSneak, vanilla));

        // Staves schools
        insertTable("weapons.staves.alteration.concentration",   WriteProfile(stavesAlterationConcentration, vanilla));
        insertTable("weapons.staves.alteration.fire_and_forget",  WriteProfile(stavesAlterationFireAndForget, vanilla));
        insertTable("weapons.staves.alteration.ritual",  WriteProfile(stavesAlterationRitual, vanilla));
        insertTable("weapons.staves.conjuration.concentration",   WriteProfile(stavesConjurationConcentration, vanilla));
        insertTable("weapons.staves.conjuration.fire_and_forget",  WriteProfile(stavesConjurationFireAndForget, vanilla));
        insertTable("weapons.staves.conjuration.ritual",  WriteProfile(stavesConjurationRitual, vanilla));
        insertTable("weapons.staves.destruction.concentration",   WriteProfile(stavesDestructionConcentration, vanilla));
        insertTable("weapons.staves.destruction.fire_and_forget",  WriteProfile(stavesDestructionFireAndForget, vanilla));
        insertTable("weapons.staves.destruction.ritual",  WriteProfile(stavesDestructionRitual, vanilla));
        insertTable("weapons.staves.illusion.concentration",   WriteProfile(stavesIllusionConcentration, vanilla));
        insertTable("weapons.staves.illusion.fire_and_forget",  WriteProfile(stavesIllusionFireAndForget, vanilla));
        insertTable("weapons.staves.illusion.ritual",  WriteProfile(stavesIllusionRitual, vanilla));
        insertTable("weapons.staves.restoration.concentration",   WriteProfile(stavesRestorationConcentration, vanilla));
        insertTable("weapons.staves.restoration.fire_and_forget",  WriteProfile(stavesRestorationFireAndForget, vanilla));
        insertTable("weapons.staves.restoration.ritual",  WriteProfile(stavesRestorationRitual, vanilla));

        // Staves schools â€” sneak variants
        insertTable("weapons.staves.alteration.sneak.concentration",   WriteProfile(stavesAlterationConcentrationSneak, vanilla));
        insertTable("weapons.staves.alteration.sneak.fire_and_forget", WriteProfile(stavesAlterationFireAndForgetSneak, vanilla));
        insertTable("weapons.staves.alteration.sneak.ritual", WriteProfile(stavesAlterationRitualSneak, vanilla));
        insertTable("weapons.staves.conjuration.sneak.concentration",   WriteProfile(stavesConjurationConcentrationSneak, vanilla));
        insertTable("weapons.staves.conjuration.sneak.fire_and_forget", WriteProfile(stavesConjurationFireAndForgetSneak, vanilla));
        insertTable("weapons.staves.conjuration.sneak.ritual", WriteProfile(stavesConjurationRitualSneak, vanilla));
        insertTable("weapons.staves.destruction.sneak.concentration",   WriteProfile(stavesDestructionConcentrationSneak, vanilla));
        insertTable("weapons.staves.destruction.sneak.fire_and_forget", WriteProfile(stavesDestructionFireAndForgetSneak, vanilla));
        insertTable("weapons.staves.destruction.sneak.ritual", WriteProfile(stavesDestructionRitualSneak, vanilla));
        insertTable("weapons.staves.illusion.sneak.concentration",   WriteProfile(stavesIllusionConcentrationSneak, vanilla));
        insertTable("weapons.staves.illusion.sneak.fire_and_forget", WriteProfile(stavesIllusionFireAndForgetSneak, vanilla));
        insertTable("weapons.staves.illusion.sneak.ritual", WriteProfile(stavesIllusionRitualSneak, vanilla));
        insertTable("weapons.staves.restoration.sneak.concentration",   WriteProfile(stavesRestorationConcentrationSneak, vanilla));
        insertTable("weapons.staves.restoration.sneak.fire_and_forget", WriteProfile(stavesRestorationFireAndForgetSneak, vanilla));
        insertTable("weapons.staves.restoration.sneak.ritual", WriteProfile(stavesRestorationRitualSneak, vanilla));

        // Transformations
        insertTable("transformations.werewolf",                          WriteProfile(transformationsWerewolf, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.sheathed",                 WriteProfile(transformationsWerewolfSheathed, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.sprint",                   WriteProfile(transformationsWerewolfSprint, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.swim",                     WriteProfile(transformationsWerewolfSwim, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.attack",                   WriteProfile(transformationsWerewolfAttack, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.power_attack",             WriteProfile(transformationsWerewolfPowerAttack, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.sprint_power_attack",      WriteProfile(transformationsWerewolfSprintPowerAttack, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.roar",                     WriteProfile(transformationsWerewolfRoar, CameraProfile::WerewolfDefault()));
        insertTable("transformations.werewolf.feeding",                  WriteProfile(transformationsWerewolfFeeding, CameraProfile::WerewolfDefault()));
        insertTable("transformations.vampire_lord.sheathed",             WriteProfile(vampireLordSheathed, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.sheathed.levitating",  WriteProfile(vampireLordSheathedLevitating, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.melee",                WriteProfile(vampireLordMelee, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.melee.attack",         WriteProfile(vampireLordMeleeAttack, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.melee.power_attack",   WriteProfile(vampireLordMeleePowerAttack, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.magic",                WriteProfile(vampireLordMagic, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.concentration",        WriteProfile(vampireLordConcentration, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.fire_and_forget",      WriteProfile(vampireLordFireAndForget, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.sprint",               WriteProfile(vampireLordSprint, kVampireLordVanilla));
        insertTable("transformations.vampire_lord.sprint.levitating",    WriteProfile(vampireLordSprintLevitating, kVampireLordVanilla));

        // Mounts
        insertTable("mounts.horseback",         WriteProfile(mountsHorseback, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.sprint",  WriteProfile(mountsHorsebackSprint, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.swim",    WriteProfile(mountsHorsebackSwim, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.melee",   WriteProfile(mountsHorsebackMelee, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.archery", WriteProfile(mountsHorsebackArchery, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.archery.draw", WriteProfile(mountsHorsebackArcheryDraw, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.archery.zoom", WriteProfile(mountsHorsebackArcheryZoom, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.melee.attack_left",  WriteProfile(mountsHorsebackMeleeLeft, CameraProfile::VanillaHorseback()));
        insertTable("mounts.horseback.melee.attack_right", WriteProfile(mountsHorsebackMeleeRight, CameraProfile::VanillaHorseback()));
        insertTable("mounts.dragon_riding",     WriteProfile(mountsDragonRiding, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.perched",  WriteProfile(mountsDragonRidingPerched, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.hovering", WriteProfile(mountsDragonRidingHovering, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.takeoff",  WriteProfile(mountsDragonRidingTakeoff, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.landing",  WriteProfile(mountsDragonRidingLanding, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.attack.grounded", WriteProfile(mountsDragonRidingAttackGrounded, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.attack.hovering", WriteProfile(mountsDragonRidingAttackHovering, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.attack.flying",   WriteProfile(mountsDragonRidingAttackFlying, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.breath.grounded", WriteProfile(mountsDragonRidingBreathGrounded, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.breath.hovering", WriteProfile(mountsDragonRidingBreathHovering, CameraProfile::VanillaDragonRiding()));
        insertTable("mounts.dragon_riding.breath.flying",   WriteProfile(mountsDragonRidingBreathFlying, CameraProfile::VanillaDragonRiding()));

        // --- Target Lock parallel tree ---
        // Write-if-modified mirrors the convention used by the non-locked
        // profiles above. CameraProfile{} vanilla default = "slot not
        // touched"; Validate() clamps loaded values back to CameraProfile{}
        // for any out-of-range garbage in the TOML.
        auto writeTL = [&](const char* key, const CameraProfile& p) {
            if (p != CameraProfile{}) insertTable(key, WriteProfile(p, CameraProfile{}));
        };
        writeTL("target_lock.sheathed",        tlSheathed);
        writeTL("target_lock.sheathed.sprint", tlSheathedSprint);
        writeTL("target_lock.sheathed.swim",   tlSheathedSwim);
        writeTL("target_lock.sheathed.sneak",  tlSheathedSneak);

        writeTL("target_lock.weapons.melee",        tlWeaponsMelee);
        writeTL("target_lock.weapons.melee.sprint", tlWeaponsMeleeSprint);
        writeTL("target_lock.weapons.melee.swim",   tlWeaponsMeleeSwim);
        writeTL("target_lock.weapons.melee.attack", tlWeaponsMeleeAttack);
        writeTL("target_lock.weapons.melee.sneak",  tlWeaponsMeleeSneak);
        writeTL("target_lock.weapons.melee.power_attack",        tlWeaponsMeleePowerAttack);
        writeTL("target_lock.weapons.melee.sneak_attack",        tlWeaponsMeleeSneakAttack);
        writeTL("target_lock.weapons.melee.sneak_power_attack",  tlWeaponsMeleeSneakPowerAttack);
        writeTL("target_lock.weapons.melee.sprint_attack",       tlWeaponsMeleeSprintAttack);
        writeTL("target_lock.weapons.melee.sprint_power_attack", tlWeaponsMeleeSprintPowerAttack);
        insertTable("target_lock.weapons.melee.overrides",        WriteMeleeOverrides(tlWeaponsMeleeOverrides,       CameraProfile{}));
        insertTable("target_lock.weapons.melee.sprint.overrides", WriteMeleeOverrides(tlWeaponsMeleeSprintOverrides, CameraProfile{}));
        insertTable("target_lock.weapons.melee.swim.overrides",   WriteMeleeOverrides(tlWeaponsMeleeSwimOverrides,   CameraProfile{}));
        insertTable("target_lock.weapons.melee.attack.overrides",      WriteMeleeOverrides(tlWeaponsMeleeAttackOverrides,     CameraProfile{}));
        insertTable("target_lock.weapons.melee.sneak.overrides",       WriteMeleeOverrides(tlWeaponsMeleeSneakOverrides,      CameraProfile{}));
        insertTable("target_lock.weapons.melee.shout.overrides",       WriteMeleeOverrides(tlWeaponsMeleeShoutOverrides,      CameraProfile{}));
        insertTable("target_lock.weapons.melee.shout.sneak.overrides", WriteMeleeOverrides(tlWeaponsMeleeShoutSneakOverrides, CameraProfile{}));
        insertTable("target_lock.weapons.melee.power_attack.overrides",        WriteMeleeOverrides(tlWeaponsMeleePowerAttackOverrides,       CameraProfile{}));
        insertTable("target_lock.weapons.melee.sneak_attack.overrides",        WriteMeleeOverrides(tlWeaponsMeleeSneakAttackOverrides,       CameraProfile{}));
        insertTable("target_lock.weapons.melee.sneak_power_attack.overrides",  WriteMeleeOverrides(tlWeaponsMeleeSneakPowerAttackOverrides,  CameraProfile{}));
        insertTable("target_lock.weapons.melee.sprint_attack.overrides",       WriteMeleeOverrides(tlWeaponsMeleeSprintAttackOverrides,      CameraProfile{}));
        insertTable("target_lock.weapons.melee.sprint_power_attack.overrides", WriteMeleeOverrides(tlWeaponsMeleeSprintPowerAttackOverrides, CameraProfile{}));

        // Directional power-attack overrides (Target Lock) â€” skip on default-
        // and-disabled to keep the TOML tidy.
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            auto ovTbl = WriteMeleeOverrides(tlWeaponsMeleePowerAttackDirOverrides[d], CameraProfile{});
            const bool has = tlWeaponsMeleePowerAttackDirEnabled[d] ||
                             tlWeaponsMeleePowerAttackDir[d] != CameraProfile{} ||
                             !ovTbl.empty();
            if (!has) continue;
            auto t = WriteProfile(tlWeaponsMeleePowerAttackDir[d], CameraProfile{});
            if (tlWeaponsMeleePowerAttackDirEnabled[d]) t.insert("enabled", true);
            if (!ovTbl.empty()) t.insert("overrides", std::move(ovTbl));
            insertTable(std::string("target_lock.weapons.melee.power_attack.dir.") + kPADirKeys[d], std::move(t));
        }

        writeTL("target_lock.weapons.blocking.one_handed",        tlWeaponsBlockingOneHanded);
        writeTL("target_lock.weapons.blocking.two_handed",        tlWeaponsBlockingTwoHanded);
        writeTL("target_lock.weapons.blocking.shield",            tlWeaponsBlockingShield);
        writeTL("target_lock.weapons.blocking.ward",              tlWeaponsBlockingWard);
        writeTL("target_lock.weapons.blocking.one_handed.sneak",  tlWeaponsBlockingOneHandedSneak);
        writeTL("target_lock.weapons.blocking.two_handed.sneak",  tlWeaponsBlockingTwoHandedSneak);
        writeTL("target_lock.weapons.blocking.shield.sneak",      tlWeaponsBlockingShieldSneak);
        writeTL("target_lock.weapons.blocking.ward.sneak",        tlWeaponsBlockingWardSneak);
        writeTL("target_lock.weapons.blocking.shield.sprint",     tlWeaponsBlockingShieldSprint);

        writeTL("target_lock.weapons.bow",             tlWeaponsBow);
        writeTL("target_lock.weapons.bow.sprint",      tlWeaponsBowSprint);
        writeTL("target_lock.weapons.bow.swim",        tlWeaponsBowSwim);
        writeTL("target_lock.weapons.bow.draw",        tlWeaponsBowDraw);
        writeTL("target_lock.weapons.bow.sneak",       tlWeaponsBowSneak);
        writeTL("target_lock.weapons.bow.sneak.draw",  tlWeaponsBowSneakDraw);
        writeTL("target_lock.weapons.bow.zoom",        tlWeaponsBowZoom);
        writeTL("target_lock.weapons.bow.sneak.zoom",  tlWeaponsBowSneakZoom);

        writeTL("target_lock.weapons.crossbow",             tlWeaponsCrossbow);
        writeTL("target_lock.weapons.crossbow.sprint",      tlWeaponsCrossbowSprint);
        writeTL("target_lock.weapons.crossbow.swim",        tlWeaponsCrossbowSwim);
        writeTL("target_lock.weapons.crossbow.draw",        tlWeaponsCrossbowDraw);
        writeTL("target_lock.weapons.crossbow.sneak",       tlWeaponsCrossbowSneak);
        writeTL("target_lock.weapons.crossbow.sneak.draw",  tlWeaponsCrossbowSneakDraw);
        writeTL("target_lock.weapons.crossbow.zoom",        tlWeaponsCrossbowZoom);
        writeTL("target_lock.weapons.crossbow.sneak.zoom",  tlWeaponsCrossbowSneakZoom);

        writeTL("target_lock.weapons.magic",        tlWeaponsMagic);
        writeTL("target_lock.weapons.magic.sprint", tlWeaponsMagicSprint);
        writeTL("target_lock.weapons.magic.swim",   tlWeaponsMagicSwim);
        writeTL("target_lock.weapons.magic.sneak",  tlWeaponsMagicSneak);

        writeTL("target_lock.weapons.magic.alteration.concentration",    tlMagicAlterationConcentration);
        writeTL("target_lock.weapons.magic.alteration.fire_and_forget",  tlMagicAlterationFireAndForget);
        writeTL("target_lock.weapons.magic.alteration.ritual",           tlMagicAlterationRitual);
        writeTL("target_lock.weapons.magic.conjuration.concentration",   tlMagicConjurationConcentration);
        writeTL("target_lock.weapons.magic.conjuration.fire_and_forget", tlMagicConjurationFireAndForget);
        writeTL("target_lock.weapons.magic.conjuration.ritual",          tlMagicConjurationRitual);
        writeTL("target_lock.weapons.magic.destruction.concentration",   tlMagicDestructionConcentration);
        writeTL("target_lock.weapons.magic.destruction.fire_and_forget", tlMagicDestructionFireAndForget);
        writeTL("target_lock.weapons.magic.destruction.ritual",          tlMagicDestructionRitual);
        writeTL("target_lock.weapons.magic.illusion.concentration",      tlMagicIllusionConcentration);
        writeTL("target_lock.weapons.magic.illusion.fire_and_forget",    tlMagicIllusionFireAndForget);
        writeTL("target_lock.weapons.magic.illusion.ritual",             tlMagicIllusionRitual);
        writeTL("target_lock.weapons.magic.restoration.concentration",   tlMagicRestorationConcentration);
        writeTL("target_lock.weapons.magic.restoration.fire_and_forget", tlMagicRestorationFireAndForget);
        writeTL("target_lock.weapons.magic.restoration.ritual",          tlMagicRestorationRitual);

        writeTL("target_lock.weapons.magic.alteration.sneak.concentration",    tlMagicAlterationConcentrationSneak);
        writeTL("target_lock.weapons.magic.alteration.sneak.fire_and_forget",  tlMagicAlterationFireAndForgetSneak);
        writeTL("target_lock.weapons.magic.alteration.sneak.ritual",           tlMagicAlterationRitualSneak);
        writeTL("target_lock.weapons.magic.conjuration.sneak.concentration",   tlMagicConjurationConcentrationSneak);
        writeTL("target_lock.weapons.magic.conjuration.sneak.fire_and_forget", tlMagicConjurationFireAndForgetSneak);
        writeTL("target_lock.weapons.magic.conjuration.sneak.ritual",          tlMagicConjurationRitualSneak);
        writeTL("target_lock.weapons.magic.destruction.sneak.concentration",   tlMagicDestructionConcentrationSneak);
        writeTL("target_lock.weapons.magic.destruction.sneak.fire_and_forget", tlMagicDestructionFireAndForgetSneak);
        writeTL("target_lock.weapons.magic.destruction.sneak.ritual",          tlMagicDestructionRitualSneak);
        writeTL("target_lock.weapons.magic.illusion.sneak.concentration",      tlMagicIllusionConcentrationSneak);
        writeTL("target_lock.weapons.magic.illusion.sneak.fire_and_forget",    tlMagicIllusionFireAndForgetSneak);
        writeTL("target_lock.weapons.magic.illusion.sneak.ritual",             tlMagicIllusionRitualSneak);
        writeTL("target_lock.weapons.magic.restoration.sneak.concentration",   tlMagicRestorationConcentrationSneak);
        writeTL("target_lock.weapons.magic.restoration.sneak.fire_and_forget", tlMagicRestorationFireAndForgetSneak);
        writeTL("target_lock.weapons.magic.restoration.sneak.ritual",          tlMagicRestorationRitualSneak);

        // TL magic per-hand overrides (same schema as the Categories grid).
        writeMagicHandGrid("target_lock.weapons.magic.", tlMagicHandOverrides);

        writeTL("target_lock.weapons.staves",        tlWeaponsStaves);
        writeTL("target_lock.weapons.staves.sprint", tlWeaponsStavesSprint);
        writeTL("target_lock.weapons.staves.swim",   tlWeaponsStavesSwim);
        writeTL("target_lock.weapons.staves.sneak",  tlWeaponsStavesSneak);

        writeTL("target_lock.weapons.staves.alteration.concentration",    tlStavesAlterationConcentration);
        writeTL("target_lock.weapons.staves.alteration.fire_and_forget",  tlStavesAlterationFireAndForget);
        writeTL("target_lock.weapons.staves.alteration.ritual",  tlStavesAlterationRitual);
        writeTL("target_lock.weapons.staves.conjuration.concentration",   tlStavesConjurationConcentration);
        writeTL("target_lock.weapons.staves.conjuration.fire_and_forget", tlStavesConjurationFireAndForget);
        writeTL("target_lock.weapons.staves.conjuration.ritual", tlStavesConjurationRitual);
        writeTL("target_lock.weapons.staves.destruction.concentration",   tlStavesDestructionConcentration);
        writeTL("target_lock.weapons.staves.destruction.fire_and_forget", tlStavesDestructionFireAndForget);
        writeTL("target_lock.weapons.staves.destruction.ritual", tlStavesDestructionRitual);
        writeTL("target_lock.weapons.staves.illusion.concentration",      tlStavesIllusionConcentration);
        writeTL("target_lock.weapons.staves.illusion.fire_and_forget",    tlStavesIllusionFireAndForget);
        writeTL("target_lock.weapons.staves.illusion.ritual",    tlStavesIllusionRitual);
        writeTL("target_lock.weapons.staves.restoration.concentration",   tlStavesRestorationConcentration);
        writeTL("target_lock.weapons.staves.restoration.fire_and_forget", tlStavesRestorationFireAndForget);
        writeTL("target_lock.weapons.staves.restoration.ritual", tlStavesRestorationRitual);

        writeTL("target_lock.weapons.staves.alteration.sneak.concentration",    tlStavesAlterationConcentrationSneak);
        writeTL("target_lock.weapons.staves.alteration.sneak.fire_and_forget",  tlStavesAlterationFireAndForgetSneak);
        writeTL("target_lock.weapons.staves.alteration.sneak.ritual",  tlStavesAlterationRitualSneak);
        writeTL("target_lock.weapons.staves.conjuration.sneak.concentration",   tlStavesConjurationConcentrationSneak);
        writeTL("target_lock.weapons.staves.conjuration.sneak.fire_and_forget", tlStavesConjurationFireAndForgetSneak);
        writeTL("target_lock.weapons.staves.conjuration.sneak.ritual", tlStavesConjurationRitualSneak);
        writeTL("target_lock.weapons.staves.destruction.sneak.concentration",   tlStavesDestructionConcentrationSneak);
        writeTL("target_lock.weapons.staves.destruction.sneak.fire_and_forget", tlStavesDestructionFireAndForgetSneak);
        writeTL("target_lock.weapons.staves.destruction.sneak.ritual", tlStavesDestructionRitualSneak);
        writeTL("target_lock.weapons.staves.illusion.sneak.concentration",      tlStavesIllusionConcentrationSneak);
        writeTL("target_lock.weapons.staves.illusion.sneak.fire_and_forget",    tlStavesIllusionFireAndForgetSneak);
        writeTL("target_lock.weapons.staves.illusion.sneak.ritual",    tlStavesIllusionRitualSneak);
        writeTL("target_lock.weapons.staves.restoration.sneak.concentration",   tlStavesRestorationConcentrationSneak);
        writeTL("target_lock.weapons.staves.restoration.sneak.fire_and_forget", tlStavesRestorationFireAndForgetSneak);
        writeTL("target_lock.weapons.staves.restoration.sneak.ritual", tlStavesRestorationRitualSneak);

        writeTL("target_lock.transformations.werewolf",        tlTransformationsWerewolf);
        writeTL("target_lock.transformations.werewolf.sheathed", tlTransformationsWerewolfSheathed);
        writeTL("target_lock.transformations.werewolf.sprint", tlTransformationsWerewolfSprint);
        writeTL("target_lock.transformations.werewolf.swim",   tlTransformationsWerewolfSwim);
        writeTL("target_lock.transformations.werewolf.attack",             tlTransformationsWerewolfAttack);
        writeTL("target_lock.transformations.werewolf.power_attack",        tlTransformationsWerewolfPowerAttack);
        writeTL("target_lock.transformations.werewolf.sprint_power_attack", tlTransformationsWerewolfSprintPowerAttack);
        writeTL("target_lock.transformations.werewolf.roar",               tlTransformationsWerewolfRoar);
        writeTL("target_lock.transformations.werewolf.feeding",            tlTransformationsWerewolfFeeding);

        writeTL("target_lock.transformations.vampire_lord.sheathed",        tlVampireLordSheathed);
        writeTL("target_lock.transformations.vampire_lord.sheathed.levitating", tlVampireLordSheathedLevitating);
        writeTL("target_lock.transformations.vampire_lord.melee",           tlVampireLordMelee);
        writeTL("target_lock.transformations.vampire_lord.melee.attack",       tlVampireLordMeleeAttack);
        writeTL("target_lock.transformations.vampire_lord.melee.power_attack", tlVampireLordMeleePowerAttack);
        writeTL("target_lock.transformations.vampire_lord.magic",           tlVampireLordMagic);
        writeTL("target_lock.transformations.vampire_lord.concentration",   tlVampireLordConcentration);
        writeTL("target_lock.transformations.vampire_lord.fire_and_forget", tlVampireLordFireAndForget);
        writeTL("target_lock.transformations.vampire_lord.sprint",          tlVampireLordSprint);
        writeTL("target_lock.transformations.vampire_lord.sprint.levitating", tlVampireLordSprintLevitating);

        writeTL("target_lock.mounts.horseback",         tlMountsHorseback);
        writeTL("target_lock.mounts.horseback.sprint",  tlMountsHorsebackSprint);
        writeTL("target_lock.mounts.horseback.swim",    tlMountsHorsebackSwim);
        writeTL("target_lock.mounts.horseback.melee",   tlMountsHorsebackMelee);
        writeTL("target_lock.mounts.horseback.archery", tlMountsHorsebackArchery);
        insertTable("target_lock.mounts.horseback.archery.draw", WriteProfile(tlMountsHorsebackArcheryDraw, CameraProfile{}));
        writeTL("target_lock.mounts.horseback.archery.zoom", tlMountsHorsebackArcheryZoom);
        writeTL("target_lock.mounts.horseback.melee.attack_left",  tlMountsHorsebackMeleeLeft);
        writeTL("target_lock.mounts.horseback.melee.attack_right", tlMountsHorsebackMeleeRight);

        // Target-lock shouts â€” write parallel to regular shouts.
        for (std::size_t si = 0; si < kShoutableStateCount; ++si) {
            if (tlShoutsBaseByState[si] != CameraProfile{}) {
                insertTable(std::string("target_lock.shouts.") + kShoutableStateKeys[si] + ".base",
                            WriteProfile(tlShoutsBaseByState[si], CameraProfile{}));
            }
            if (tlShoutsBaseByStateSneak[si] != CameraProfile{}) {
                insertTable(std::string("target_lock.shouts.") + kShoutableStateKeys[si] + ".base.sneak",
                            WriteProfile(tlShoutsBaseByStateSneak[si], CameraProfile{}));
            }
            for (std::size_t i = 0; i < kShoutCount; ++i) {
                const auto& entry = kShouts[i];
                if (tlShoutOverrideByStateEnabled[si][i] || tlShoutOverrideByState[si][i] != CameraProfile{}) {
                    auto t = WriteProfile(tlShoutOverrideByState[si][i], CameraProfile{});
                    if (tlShoutOverrideByStateEnabled[si][i]) t.insert("enabled", true);
                    insertTable(std::string("target_lock.shouts.") + kShoutableStateKeys[si] + "." + entry.tomlKey, std::move(t));
                }
                if (tlShoutOverrideByStateEnabledSneak[si][i] || tlShoutOverrideByStateSneak[si][i] != CameraProfile{}) {
                    auto t = WriteProfile(tlShoutOverrideByStateSneak[si][i], CameraProfile{});
                    if (tlShoutOverrideByStateEnabledSneak[si][i]) t.insert("enabled", true);
                    insertTable(std::string("target_lock.shouts.") + kShoutableStateKeys[si] + "." + entry.tomlKey + ".sneak", std::move(t));
                }
            }
        }

        // --- Enemy Overrides --- (aim bias is per-entry, inside each override)
        for (std::size_t e = 0; e < kEnemyOverrideEnemies; ++e) {
            const std::string root = std::string("target_lock.enemy.") + kEnemyKeys[e];
            // One emitter, both variants: an outdoor/indoor pair that
            // diverged only because someone updated one loop and not the other
            // is exactly the class of bug this storage split can produce.
            const auto emitEnemy = [&](const EnemyFieldOverride& o, const std::string& path) {
                if (!EnemyOverrideHasContent(o)) {
                    // Still emit values without enables IF the user has
                    // typed numbers, so re-enabling later restores them.
                    const bool anyValue = o.profile != CameraProfile{};
                    if (!anyValue) return;
                }
                insertTable(path, WriteEnemyOverride(o));
            };
            for (std::size_t s = 0; s < kTLSlotCount; ++s) {
                emitEnemy(enemyOverrides[e][s],       root + "." + kTLSlotKeys[s]);
                emitEnemy(enemyOverridesIndoor[e][s], root + ".indoor." + kTLSlotKeys[s]);
            }
            for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                emitEnemy(tlEnemyOverridesPowerAttackDir[e][d],
                          root + ".power_attack_dir." + kPADirKeys[d]);
                emitEnemy(tlEnemyOverridesPowerAttackDirIndoor[e][d],
                          root + ".indoor.power_attack_dir." + kPADirKeys[d]);
            }
        }

        // --- Custom (player-bound) Enemy Overrides --- written under
        // target_lock.custom_enemy.<i> with a .meta sub-table (identity) plus
        // sparse per-slot / per-PA-dir override tables. Indices are contiguous
        // (re-serialized from the vector each save), so the reader stops at the
        // first missing .meta. The .meta is always emitted so a freshly-bound
        // enemy with no edits still persists.
        for (std::size_t ci = 0; ci < customEnemyOverrides.size(); ++ci) {
            const auto& c = customEnemyOverrides[ci];
            const std::string root = "target_lock.custom_enemy." + std::to_string(ci);
            const char* mtStr =
                c.matchType == CustomEnemyOverride::MatchType::Keyword    ? "keyword"     :
                c.matchType == CustomEnemyOverride::MatchType::RaceFamily ? "race_family" :
                c.matchType == CustomEnemyOverride::MatchType::NPC        ? "npc"         :
                c.matchType == CustomEnemyOverride::MatchType::Faction    ? "faction"     :
                c.matchType == CustomEnemyOverride::MatchType::Name       ? "actor_name"  : "race";
            toml::table meta;
            meta.insert("match_type", mtStr);
            meta.insert("plugin",     c.pluginName);
            meta.insert("form_id",    static_cast<std::int64_t>(c.formID));
            meta.insert("match_key",  c.matchKey);
            meta.insert("name",       c.displayName);
            if (c.trackingSmoothing != 0.0f)
                meta.insert("tracking_smoothing", static_cast<double>(c.trackingSmoothing));
            insertTable(root + ".meta", std::move(meta));
            // Sparse per-slot / per-PA-dir tables, emitted for both the outdoor
            // grid and its indoor twin (custom enemy overrides are env-split).
            auto writeGrid = [&](const std::string& pfx,
                                 const std::array<EnemyFieldOverride, kTLSlotCount>& sl,
                                 const std::array<EnemyFieldOverride, kPowerAttackDirectionCount>& pa) {
                for (std::size_t s = 0; s < kTLSlotCount; ++s) {
                    const auto& o = sl[s];
                    if (!EnemyOverrideHasContent(o)) {
                        bool anyValue = o.profile != CameraProfile{};
                        if (!anyValue) continue;
                    }
                    insertTable(pfx + kTLSlotKeys[s], WriteEnemyOverride(o));
                }
                for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                    const auto& o = pa[d];
                    if (!EnemyOverrideHasContent(o)) {
                        bool anyValue = o.profile != CameraProfile{};
                        if (!anyValue) continue;
                    }
                    insertTable(pfx + "power_attack_dir." + kPADirKeys[d], WriteEnemyOverride(o));
                }
            };
            writeGrid(root + ".",       c.slots,       c.paDir);
            writeGrid(root + ".indoor.", c.slotsIndoor, c.paDirIndoor);
        }

        // Custom (mod-added) weapon type catalog.
        if (!customWeaponTypes.empty()) {
            toml::array cwArr;
            for (const auto& t : customWeaponTypes) {
                if (t.keyword.empty()) continue;
                toml::table ct;
                ct.insert("keyword", t.keyword);
                ct.insert("name",    t.displayName);
                cwArr.push_back(std::move(ct));
            }
            if (!cwArr.empty()) {
                if (!root.contains("weapons")) root.insert("weapons", toml::table{});
                root.get("weapons")->as_table()->insert("custom_weapon_types", std::move(cwArr));
            }
        }

        // Per-state shouts. Each shoutable state has its own base profile
        // plus per-shout overrides with enable flags. Write when either
        // the toggle is on OR the profile carries non-default values so
        // tuned values survive a temporary disable.
        for (std::size_t si = 0; si < kShoutableStateCount; ++si) {
            if (shoutsBaseByState[si] != CameraProfile{}) {
                insertTable(std::string("shouts.") + kShoutableStateKeys[si] + ".base",
                            WriteProfile(shoutsBaseByState[si], CameraProfile{}));
            }
            if (shoutsBaseByStateSneak[si] != CameraProfile{}) {
                insertTable(std::string("shouts.") + kShoutableStateKeys[si] + ".base.sneak",
                            WriteProfile(shoutsBaseByStateSneak[si], CameraProfile{}));
            }

            for (std::size_t i = 0; i < kShoutCount; ++i) {
                const auto& entry = kShouts[i];
                const bool normHas = shoutOverrideByStateEnabled[si][i] ||
                                     shoutOverrideByState[si][i] != CameraProfile{};
                if (normHas) {
                    auto key = std::string("shouts.") + kShoutableStateKeys[si] + "." + entry.tomlKey;
                    auto t   = WriteProfile(shoutOverrideByState[si][i], CameraProfile{});
                    if (shoutOverrideByStateEnabled[si][i]) t.insert("enabled", true);
                    insertTable(key, std::move(t));
                }
                const bool sneakHas = shoutOverrideByStateEnabledSneak[si][i] ||
                                      shoutOverrideByStateSneak[si][i] != CameraProfile{};
                if (sneakHas) {
                    auto key = std::string("shouts.") + kShoutableStateKeys[si] + "." + entry.tomlKey + ".sneak";
                    auto t   = WriteProfile(shoutOverrideByStateSneak[si][i], CameraProfile{});
                    if (shoutOverrideByStateEnabledSneak[si][i]) t.insert("enabled", true);
                    insertTable(key, std::move(t));
                }
            }
        }

        // Extras
        {
            auto vanity = WriteProfile(vanityCamera, CameraProfile::Default3p());
            if (vanityIdleSeconds != 120.0f) vanity.insert("idle_seconds", vanityIdleSeconds);
            insertTable("extras.vanity", std::move(vanity));
        }

        // Dialogue camera profile (enable flag lives in [general] above).
        insertTable("dialogue",              WriteProfile(dialogueProfile,            CameraProfile::VanillaCombat()));
        insertTable("dialogue.first_person", WriteProfile(dialogueFirstPersonProfile, CameraProfile::VanillaDialogue1p()));

        // Workstation / Furniture FOVs â€” one sub-key per type, only written
        // when changed from the 80.0 default.
        {
            toml::table noise;
            if (noiseEnabled != false) noise.insert("enabled", noiseEnabled);

            // Global noise â€” only emit fields that differ from defaults.
            // Outdoor + indoor variant write to "global" and "global_indoor".
            auto writeGlobal = [&](const char* key, const NoiseProfile& gp) {
                NoiseProfile def;
                toml::table g;
                if (gp.amp    != def.amp)    g.insert("amp",    static_cast<double>(gp.amp));
                if (gp.speed  != def.speed)  g.insert("speed",  static_cast<double>(gp.speed));
                if (gp.sway   != def.sway)   g.insert("sway",   static_cast<double>(gp.sway));
                if (gp.tilt   != def.tilt)   g.insert("tilt",   static_cast<double>(gp.tilt));
                if (gp.wobble != def.wobble) g.insert("wobble", static_cast<double>(gp.wobble));
                WriteNoiseExtra(g, gp);
                if (!g.empty()) noise.insert(key, std::move(g));
            };
            writeGlobal("global",        globalNoise);
            writeGlobal("global_indoor", globalNoiseIndoor);

            // Per-state customizations under [noise.states.<tomlKey>] and
            // [noise.states_indoor.<tomlKey>]. Skip entries that are exactly
            // default to keep the file compact.
            auto writeStates = [&](const char* key,
                                    const std::unordered_map<std::string, NoiseProfile>& src) {
                NoiseProfile def;
                toml::table states;
                for (const auto& [k, p] : src) {
                    if (p.enabled == def.enabled &&
                        p.amp     == def.amp &&
                        p.speed   == def.speed &&
                        p.sway    == def.sway &&
                        p.tilt    == def.tilt &&
                        p.wobble  == def.wobble &&
                        NoiseExtraIsDefault(p))
                    {
                        continue;
                    }
                    toml::table sub;
                    if (p.enabled != def.enabled) sub.insert("enabled", p.enabled);
                    if (p.amp     != def.amp)     sub.insert("amp",     static_cast<double>(p.amp));
                    if (p.speed   != def.speed)   sub.insert("speed",   static_cast<double>(p.speed));
                    if (p.sway    != def.sway)    sub.insert("sway",    static_cast<double>(p.sway));
                    if (p.tilt    != def.tilt)    sub.insert("tilt",    static_cast<double>(p.tilt));
                    if (p.wobble  != def.wobble)  sub.insert("wobble",  static_cast<double>(p.wobble));
                    WriteNoiseExtra(sub, p);
                    if (!sub.empty()) states.insert(k, std::move(sub));
                }
                if (!states.empty()) noise.insert(key, std::move(states));
            };
            writeStates("states",        stateNoise);
            writeStates("states_indoor", stateNoiseIndoor);

            // Per-weapon-type noise overrides for each Melee bucket.
            // Skip entries that are unset or exactly default to keep the
            // file compact.
            {
                NoiseProfile def;
                auto writeMNO = [&](const char* baseKey, const MeleeWeaponNoiseOverrides& o, toml::table& parent) {
                    toml::table perWeapon;
                    for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
                        if (!o.perWeaponSet[i] &&
                            o.perWeapon[i].enabled == def.enabled &&
                            o.perWeapon[i].amp     == def.amp &&
                            o.perWeapon[i].speed   == def.speed &&
                            o.perWeapon[i].sway    == def.sway &&
                            o.perWeapon[i].tilt    == def.tilt &&
                            o.perWeapon[i].wobble  == def.wobble &&
                            NoiseExtraIsDefault(o.perWeapon[i]))
                        {
                            continue;
                        }
                        toml::table sub;
                        if (o.perWeaponSet[i]) sub.insert("enabled", true);
                        const auto& p = o.perWeapon[i];
                        if (p.enabled != def.enabled) sub.insert("np_enabled", p.enabled);
                        if (p.amp     != def.amp)     sub.insert("amp",        static_cast<double>(p.amp));
                        if (p.speed   != def.speed)   sub.insert("speed",      static_cast<double>(p.speed));
                        if (p.sway    != def.sway)    sub.insert("sway",       static_cast<double>(p.sway));
                        if (p.tilt    != def.tilt)    sub.insert("tilt",       static_cast<double>(p.tilt));
                        if (p.wobble  != def.wobble)  sub.insert("wobble",     static_cast<double>(p.wobble));
                        WriteNoiseExtra(sub, p);
                        perWeapon.insert(MeleeWeaponTypeTomlKey(static_cast<MeleeWeaponType>(i)), std::move(sub));
                    }
                    if (perWeapon.empty()) return;
                    toml::table outer; outer.insert("per_weapon", std::move(perWeapon));
                    parent.insert(baseKey, std::move(outer));
                };
                toml::table mo;
                writeMNO("weapons.melee",         weaponsMeleeNoiseOverrides,       mo);
                writeMNO("weapons.melee.sprint",  weaponsMeleeSprintNoiseOverrides, mo);
                writeMNO("weapons.melee.swim",    weaponsMeleeSwimNoiseOverrides,   mo);
                writeMNO("weapons.melee.attack",      weaponsMeleeAttackNoiseOverrides,     mo);
                writeMNO("weapons.melee.sneak",       weaponsMeleeSneakNoiseOverrides,      mo);
                writeMNO("weapons.melee.shout",       weaponsMeleeShoutNoiseOverrides,      mo);
                writeMNO("weapons.melee.shout.sneak", weaponsMeleeShoutSneakNoiseOverrides, mo);
                writeMNO("weapons.melee.power_attack",        weaponsMeleePowerAttackNoiseOverrides,       mo);
                writeMNO("weapons.melee.sneak_attack",        weaponsMeleeSneakAttackNoiseOverrides,       mo);
                writeMNO("weapons.melee.sneak_power_attack",  weaponsMeleeSneakPowerAttackNoiseOverrides,  mo);
                writeMNO("weapons.melee.sprint_attack",       weaponsMeleeSprintAttackNoiseOverrides,      mo);
                writeMNO("weapons.melee.sprint_power_attack", weaponsMeleeSprintPowerAttackNoiseOverrides, mo);
                for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                    const std::string k = std::string("weapons.melee.power_attack.dir.") + kPADirKeys[d];
                    writeMNO(k.c_str(), weaponsMeleePowerAttackDirNoiseOverrides[d], mo);
                }
                if (!mo.empty()) noise.insert("melee_overrides", std::move(mo));
            }

            // (1p noise moved to per-state save under [first_person.*.noise].)

            if (!noise.empty()) insertTable("noise", std::move(noise));
        }

        // First-person: master gate + transition speed + global +
        // per-state map. Trim policy: only write fields that diverge
        // from FirstPersonProfile defaults so the TOML stays compact.
        {
            toml::table fp;
            // fov_enabled / noise_enabled are no longer written â€” First Person
            // is always enabled (the load path pins both true).
            if (firstPersonTransitionSpeed != 1.0f) {
                fp.insert("transition_speed", static_cast<double>(firstPersonTransitionSpeed));
            }

            auto writeFp = [](const FirstPersonProfile& src) {
                FirstPersonProfile def;
                toml::table out;
                if (src.transitionSpeed != def.transitionSpeed)
                    out.insert("transition_speed", static_cast<double>(src.transitionSpeed));
                if (src.worldFov  != def.worldFov)  out.insert("world_fov", static_cast<double>(src.worldFov));
                if (src.handsFov  != def.handsFov)  out.insert("hands_fov", static_cast<double>(src.handsFov));
                if (src.repulse   != def.repulse)   out.insert("repulse",   static_cast<double>(src.repulse));
                if (src.repulseFeel != def.repulseFeel) {
                    out.insert("repulse_feel", static_cast<double>(src.repulseFeel));
                }
                if (src.fofFadeDuration != def.fofFadeDuration) {
                    out.insert("fof_fade_duration", static_cast<double>(src.fofFadeDuration));
                }
                if (src.shoutFadeDuration != def.shoutFadeDuration) {
                    out.insert("shout_fade_duration", static_cast<double>(src.shoutFadeDuration));
                }
                toml::table n;
                if (src.noise.enabled != def.noise.enabled) n.insert("enabled", src.noise.enabled);
                if (src.noise.amp     != def.noise.amp)     n.insert("amp",     static_cast<double>(src.noise.amp));
                if (src.noise.speed   != def.noise.speed)   n.insert("speed",   static_cast<double>(src.noise.speed));
                if (src.noise.sway    != def.noise.sway)    n.insert("sway",    static_cast<double>(src.noise.sway));
                if (src.noise.tilt    != def.noise.tilt)    n.insert("tilt",    static_cast<double>(src.noise.tilt));
                if (src.noise.wobble  != def.noise.wobble)  n.insert("wobble",  static_cast<double>(src.noise.wobble));
                // Two-band fields (drift_jitter / roughness) â€” the read side
                // has always consumed them; without this they never survived
                // a save for 1p profiles.
                WriteNoiseExtra(n, src.noise);
                if (!n.empty()) out.insert("noise", std::move(n));
                return out;
            };

            if (auto g = writeFp(firstPersonGlobal); !g.empty()) {
                fp.insert("global", std::move(g));
            }
            toml::table states;
            for (auto& [key, p] : stateFirstPerson) {
                auto sub = writeFp(p);
                if (!sub.empty()) states.insert(key, std::move(sub));
            }
            if (!states.empty()) fp.insert("states", std::move(states));

            // Per-weapon-type FP overrides. Each enabled slot persists its
            // FOV/Noise enable plus any non-default profile fields (reuses
            // writeFp). Default-valued disabled slots are pruned.
            toml::table meleeOv;
            for (auto& [key, ov] : stateFpMeleeOverrides) {
                toml::table perWeapon;
                for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
                    auto sub = writeFp(ov.perWeapon[i]);
                    if (ov.perWeaponSetFov[i])   sub.insert("enabled_fov",   true);
                    if (ov.perWeaponSetNoise[i]) sub.insert("enabled_noise", true);
                    if (!sub.empty()) {
                        perWeapon.insert(MeleeWeaponTypeTomlKey(static_cast<MeleeWeaponType>(i)),
                                         std::move(sub));
                    }
                }
                toml::table customTbl;
                for (auto& cs : ov.custom) {
                    auto sub = writeFp(cs.profile);
                    if (cs.setFov)   sub.insert("enabled_fov",   true);
                    if (cs.setNoise) sub.insert("enabled_noise", true);
                    if (!sub.empty()) customTbl.insert(cs.keyword, std::move(sub));
                }
                if (!perWeapon.empty() || !customTbl.empty()) {
                    toml::table stateTbl;
                    if (!perWeapon.empty()) stateTbl.insert("per_weapon", std::move(perWeapon));
                    if (!customTbl.empty()) stateTbl.insert("custom", std::move(customTbl));
                    meleeOv.insert(key, std::move(stateTbl));
                }
            }
            if (!meleeOv.empty()) fp.insert("melee_overrides", std::move(meleeOv));

            if (!fp.empty()) insertTable("first_person", std::move(fp));
        }

        // A missing indoor table mirrors Outdoors. A present one is an
        // independent snapshot against Default3p, including all-default copies.
        const auto eligibleForSave = GetIndoorEligibleProfiles();
        for (auto& e : eligibleForSave) {
            if (auto* iptr = IndoorVariantOf(e.outdoor); iptr && iptr != e.outdoor && *iptr != *e.outdoor) {
                auto profile = WriteProfile(*iptr, vanilla);
                if (profile.empty()) profile.insert("set", true);
                insertTable(std::string("indoor.") + e.tomlKey, std::move(profile));
            }
        }

        // Location overrides. Keyed by INDEX (location_override.<n>) the same
        // way custom enemies already are, so the definition and its profile /
        // noise tables stay together and a place can be renamed freely.
        //
        // Only SET entries are written: profileSet is the per-entry enable, and
        // an unset slot holds a stale copy of whatever outdoor was when the
        // popup last opened. Writing those would resurrect them on load (the
        // reader treats presence as set) and quietly freeze the place against
        // later Outdoor edits.
        if (!locationOverridesEnabled) {
            toml::table lot;
            lot.insert("enabled", false);
            insertTable("location_overrides", std::move(lot));
        }
        // Empty stubs (a place with no bindings in ANY section) are DROPPED
        // from the file â€” they carry nothing, and persisting them is how
        // binding-less places from one preset kept riding into every preset
        // saved afterwards ("other locations leak into Your Locations",
        // user report 2026-08-15). The reader stops at the first missing
        // index, so the survivors are renumbered contiguously.
        std::size_t locOut = 0;
        for (std::size_t i = 0; i < locationOverrides.size(); ++i) {
            const auto& lo   = locationOverrides[i];
            {
                bool anyBinding = lo.builtIn || !lo.stateNoise.empty() ||
                                  lo.globalNoiseSet || !lo.fpState.empty() ||
                                  !lo.bindingCam.empty() || !lo.fxBeats.empty() ||
                                  !lo.dlgLooks.empty();
                if (!anyBinding)
                    for (std::size_t k = 0; k < lo.profileSet.size(); ++k)
                        if (lo.profileSet[k]) { anyBinding = true; break; }
                if (!anyBinding)
                    for (std::size_t k = 0; k < lo.profileSetIndoor.size(); ++k)
                        if (lo.profileSetIndoor[k]) { anyBinding = true; break; }
                if (!anyBinding) continue;
            }
            const std::string root2 = "location_override." + std::to_string(locOut++);
            toml::table def;
            def.insert("name", lo.name);
            if (!lo.enabled)        def.insert("enabled", false);
            if (lo.builtIn)         def.insert("built_in", true);
            if (lo.kind == LocationOverride::Kind::Keyword) {
                def.insert("keyword", lo.keyword);
            } else {
                if (lo.kind == LocationOverride::Kind::Worldspace) def.insert("kind", "worldspace");
                else if (lo.kind == LocationOverride::Kind::Cell)   def.insert("kind", "cell");
                else if (lo.kind == LocationOverride::Kind::Region) def.insert("kind", "region");
                else if (lo.kind == LocationOverride::Kind::Room) {
                    def.insert("kind", "room");
                    if (!lo.parentName.empty()) def.insert("parent", lo.parentName);
                }
                def.insert("plugin",  lo.plugin);
                def.insert("form_id", static_cast<std::int64_t>(lo.formID));
            }
            insertTable(root2, std::move(def));

            for (std::size_t k = 0; k < eligibleForSave.size() && k < lo.profileSet.size(); ++k) {
                if (!lo.profileSet[k]) continue;
                toml::table pt = WriteProfile(lo.profiles[k], vanilla);
                // insertTable drops an empty table, and the READER treats
                // presence as the per-entry enable â€” so an override whose
                // values happen to equal vanilla would silently un-set itself
                // on the next load. Emit a marker so presence survives.
                if (pt.empty()) pt.insert("set", true);
                insertTable(root2 + ".profiles." + eligibleForSave[k].tomlKey, std::move(pt));
            }
            for (std::size_t k = 0; k < eligibleForSave.size() && k < lo.profileSetIndoor.size(); ++k) {
                if (!lo.profileSetIndoor[k]) continue;
                toml::table pt = WriteProfile(lo.profilesIndoor[k], vanilla);
                if (pt.empty()) pt.insert("set", true);
                insertTable(root2 + ".profiles_indoor." + eligibleForSave[k].tomlKey, std::move(pt));
            }

            {
                NoiseProfile ndef;
                if (lo.globalNoiseSet) {
                    toml::table g;
                    const auto& gp = lo.globalNoise;
                    if (gp.amp    != ndef.amp)    g.insert("amp",    static_cast<double>(gp.amp));
                    if (gp.speed  != ndef.speed)  g.insert("speed",  static_cast<double>(gp.speed));
                    if (gp.sway   != ndef.sway)   g.insert("sway",   static_cast<double>(gp.sway));
                    if (gp.tilt   != ndef.tilt)   g.insert("tilt",   static_cast<double>(gp.tilt));
                    if (gp.wobble != ndef.wobble) g.insert("wobble", static_cast<double>(gp.wobble));
                    WriteNoiseExtra(g, gp);
                    // Same marker for the same reason: presence is the flag,
                    // and insertTable refuses an empty table.
                    if (g.empty()) g.insert("set", true);
                    insertTable(root2 + ".noise.global", std::move(g));
                }
                toml::table states;
                for (const auto& [k, np] : lo.stateNoise) {
                    toml::table sub;
                    if (np.enabled != ndef.enabled) sub.insert("enabled", np.enabled);
                    if (np.amp     != ndef.amp)     sub.insert("amp",     static_cast<double>(np.amp));
                    if (np.speed   != ndef.speed)   sub.insert("speed",   static_cast<double>(np.speed));
                    if (np.sway    != ndef.sway)    sub.insert("sway",    static_cast<double>(np.sway));
                    if (np.tilt    != ndef.tilt)    sub.insert("tilt",    static_cast<double>(np.tilt));
                    if (np.wobble  != ndef.wobble)  sub.insert("wobble",  static_cast<double>(np.wobble));
                    WriteNoiseExtra(sub, np);
                    states.insert(k, std::move(sub));
                }
                if (!states.empty()) insertTable(root2 + ".noise.states", std::move(states));
            }

            // First person. Same presence-is-the-bind rule as noise states;
            // the empty-table marker keeps a bound-but-default entry alive
            // (insertTable drops empty tables and the reader treats presence
            // as the bind).
            {
                toml::table fpStates;
                for (const auto& [k, p] : lo.fpState) {
                    toml::table sub = WriteFpProfileTable(p);
                    if (sub.empty()) sub.insert("set", true);
                    fpStates.insert(k, std::move(sub));
                }
                if (!fpStates.empty()) insertTable(root2 + ".fp.states", std::move(fpStates));
            }

            // Specific-weapon binding camera slots â€” same presence-is-the-
            // bind rule; diffed against the zero profile for an exact
            // round-trip.
            {
                toml::table bc;
                for (const auto& [k, p] : lo.bindingCam) {
                    toml::table sub = WriteProfile(p, CameraProfile{});
                    if (sub.empty()) sub.insert("set", true);
                    bc.insert(k, std::move(sub));
                }
                if (!bc.empty()) insertTable(root2 + ".binding_cam", std::move(bc));
            }

            // Transformation-beat tunings â€” presence is the bind; values
            // written in full (a beat's tuning is small and the per-place
            // copy REPLACES the global one wholesale, so there is no
            // meaningful default to diff against).
            {
                toml::table fxb;
                for (const auto& [k, bt] : lo.fxBeats) {
                    toml::table sub;
                    sub.insert("intensity",     static_cast<double>(bt.intensity));
                    sub.insert("speed",         static_cast<double>(bt.speed));
                    sub.insert("range",         static_cast<double>(bt.range));
                    sub.insert("direction",     static_cast<double>(bt.direction));
                    sub.insert("rot_shake",     static_cast<double>(bt.chr.rotShake));
                    sub.insert("pos_shake",     static_cast<double>(bt.chr.posShake));
                    sub.insert("drift_jitter",  static_cast<double>(bt.chr.driftJitter));
                    sub.insert("roughness",     static_cast<double>(bt.chr.roughness));
                    sub.insert("fade_duration", static_cast<double>(bt.chr.fadeDuration));
                    fxb.insert(k, std::move(sub));
                }
                if (!fxb.empty()) insertTable(root2 + ".fx_beats", std::move(fxb));
            }

            // Dialogue look overrides â€” same "presence is the bind" shape as
            // binding_cam, including the `set = true` marker so a place-copy
            // that happens to equal the default still round-trips.
            {
                toml::table dl;
                for (const auto& [k, p] : lo.dlgLooks) {
                    toml::table sub = WriteProfile(p, CameraProfile{});
                    if (sub.empty()) sub.insert("set", true);
                    dl.insert(k, std::move(sub));
                }
                if (!dl.empty()) insertTable(root2 + ".dialogue_looks", std::move(dl));
            }
        }

        // ---- [CLIPCAM] animation cameras ----------------------------------
        // Contiguous renumber on save (the location-override idiom); the
        // numbering is internal to this file, never a cross-preset contract.
        {
            int acOut = 0;
            for (const auto& e : animationCameras) {
                if (e.animationPath.empty()) continue;  // unbindable stub, drop
                const std::string root2 = "animation_camera." + std::to_string(acOut++);
                toml::table def;
                def.insert("uid",  static_cast<std::int64_t>(e.uid));
                def.insert("name", e.name);
                def.insert("path", e.animationPath);
                if (!e.subModName.empty()) def.insert("sub_mod", e.subModName);
                if (!e.modName.empty())    def.insert("mod", e.modName);
                insertTable(root2, std::move(def));

                // Outdoor: sparse vs the field initializer (rule 7); marker
                // when the diff is empty so presence survives the round-trip.
                toml::table pt = WriteProfile(e.profile, CameraProfile{});
                if (pt.empty()) pt.insert("set", true);
                insertTable(root2 + ".profile", std::move(pt));

                // Indoor: written ONLY when diverged from outdoor — absence
                // means "mirror outdoor", which is what the reader does, so
                // an entry the user never split stays split-free and keeps
                // following outdoor edits forever.
                if (e.profileIndoor != e.profile) {
                    toml::table it = WriteProfile(e.profileIndoor, CameraProfile{});
                    if (it.empty()) it.insert("set", true);
                    insertTable(root2 + ".profile_indoor", std::move(it));
                }
                // TL variants: sparse, absent = untuned (TlTuned gates use).
                {
                    toml::table tt = WriteProfile(e.tlProfile, CameraProfile{});
                    if (!tt.empty()) insertTable(root2 + ".tl_profile", std::move(tt));
                    toml::table ti = WriteProfile(e.tlProfileIndoor, CameraProfile{});
                    if (!ti.empty()) insertTable(root2 + ".tl_profile_indoor", std::move(ti));
                }
                // Noise cell — the location stateNoise codec shape.
                {
                    const NoiseProfile nd;
                    const auto& p = e.noise;
                    if (!(p.enabled == nd.enabled && p.amp == nd.amp &&
                          p.speed == nd.speed && p.sway == nd.sway &&
                          p.tilt == nd.tilt && p.wobble == nd.wobble &&
                          NoiseExtraIsDefault(p))) {
                        toml::table sub;
                        if (p.enabled != nd.enabled) sub.insert("enabled", p.enabled);
                        if (p.amp     != nd.amp)     sub.insert("amp",     static_cast<double>(p.amp));
                        if (p.speed   != nd.speed)   sub.insert("speed",   static_cast<double>(p.speed));
                        if (p.sway    != nd.sway)    sub.insert("sway",    static_cast<double>(p.sway));
                        if (p.tilt    != nd.tilt)    sub.insert("tilt",    static_cast<double>(p.tilt));
                        if (p.wobble  != nd.wobble)  sub.insert("wobble",  static_cast<double>(p.wobble));
                        WriteNoiseExtra(sub, p);
                        if (!sub.empty()) insertTable(root2 + ".noise", std::move(sub));
                    }
                    if (!(e.noiseIndoor == e.noise)) {
                        const auto& pi = e.noiseIndoor;
                        toml::table sub;
                        if (pi.enabled != nd.enabled) sub.insert("enabled", pi.enabled);
                        if (pi.amp     != nd.amp)     sub.insert("amp",     static_cast<double>(pi.amp));
                        if (pi.speed   != nd.speed)   sub.insert("speed",   static_cast<double>(pi.speed));
                        if (pi.sway    != nd.sway)    sub.insert("sway",    static_cast<double>(pi.sway));
                        if (pi.tilt    != nd.tilt)    sub.insert("tilt",    static_cast<double>(pi.tilt));
                        if (pi.wobble  != nd.wobble)  sub.insert("wobble",  static_cast<double>(pi.wobble));
                        WriteNoiseExtra(sub, pi);
                        if (sub.empty()) sub.insert("set", true);
                        insertTable(root2 + ".noise_indoor", std::move(sub));
                    }
                }
            }
        }

        if (!CanReadPreset(root)) throw std::runtime_error("Cannot save invalid preset values");
        return root;
    }

    void SettingsManager::AssignAnimationCameraUids()
    {
        // Max-scan, never a counter — the ids already in the loaded preset
        // are authoritative, and a counter reset by a preset switch would
        // hand a live id to a second entry.
        std::uint32_t maxUid = 0;
        for (const auto& e : animationCameras) maxUid = (std::max)(maxUid, e.uid);
        for (auto& e : animationCameras)
            if (e.uid == 0) e.uid = ++maxUid;
    }

    SettingsManager::AnimationCameraEntry* SettingsManager::FindAnimationCamera(std::uint32_t a_uid)
    {
        if (a_uid == 0) return nullptr;
        for (auto& e : animationCameras)
            if (e.uid == a_uid) return &e;
        return nullptr;
    }

    SettingsManager::NoiseProfile* SettingsManager::ResolveAnimationNoise(std::uint32_t a_uid)
    {
        auto* entry = FindAnimationCamera(a_uid);
        if (!entry) return nullptr;
        const std::string key = "animcam." + std::to_string(a_uid);
        if (auto* location = ActiveLocationStateNoise(key)) return location;
        auto& cell = entry->NoiseFor(RuntimeEnv());
        return cell == NoiseProfile{} ? nullptr : &cell;
    }

    void SettingsManager::ApplyLogLevel(bool a_verbose)
    {
        if (auto log = spdlog::default_logger())
            log->set_level(a_verbose ? spdlog::level::debug : spdlog::level::info);
    }

    void SettingsManager::Validate()
    {
        // Every load path ends here, so this is the one place the logger's
        // level can be kept in step with the setting without hunting callers.
        ApplyLogLevel(verboseLogging);

        // Every dragon-riding slot â€” the base and its six sub-states, in both
        // trees and every environment variant â€” takes the WIDE bounds. Built
        // as a set rather than a pointer list because each of those profiles
        // also has indoor variants and TL twins living at other addresses,
        // and clamping the outdoor Categories copy alone would leave the rest
        // to the human-scale clamp above (which would quietly crush a -300
        // dragon height to -200 the first time the user went indoors).
        std::unordered_set<const CameraProfile*> dragonSlots;
        {
            CameraProfile* const kDragonRoots[] = {
                &mountsDragonRiding,
                &mountsDragonRidingPerched,  &mountsDragonRidingHovering,
                &mountsDragonRidingTakeoff,  &mountsDragonRidingLanding,
                &mountsDragonRidingAttackGrounded, &mountsDragonRidingAttackHovering,
                &mountsDragonRidingAttackFlying,
                &mountsDragonRidingBreathGrounded, &mountsDragonRidingBreathHovering,
                &mountsDragonRidingBreathFlying,
            };
            for (auto* root : kDragonRoots) {
                dragonSlots.insert(root);
                for (int env = kEnvIndoor; env < kEnvCount; ++env) {
                    if (auto* v = VariantOf(root, env)) dragonSlots.insert(v);
                }
            }
        }

        // Mount slots take the wide bounds on ZOOM ONLY (see Defaults::
        // MountZoom). Same set-not-list construction and for the same reason:
        // without it, a negative horseback zoom would survive the menu, be
        // written to TOML, and then be crushed back to 0 by the human-scale
        // clamp on the very next load â€” the silent-revert failure mode.
        std::unordered_set<const CameraProfile*> mountSlots;
        {
            CameraProfile* const kMountRoots[] = {
                &mountsHorseback,          &mountsHorsebackSprint,
                &mountsHorsebackSwim,      &mountsHorsebackMelee,
                &mountsHorsebackArchery,   &mountsHorsebackArcheryZoom, &mountsHorsebackArcheryDraw,
                &mountsHorsebackMeleeLeft, &mountsHorsebackMeleeRight,
                &tlMountsHorseback,        &tlMountsHorsebackSprint,
                &tlMountsHorsebackSwim,    &tlMountsHorsebackMelee,
                &tlMountsHorsebackArchery, &tlMountsHorsebackArcheryZoom, &tlMountsHorsebackArcheryDraw,
                &tlMountsHorsebackMeleeLeft, &tlMountsHorsebackMeleeRight,
            };
            for (auto* root : kMountRoots) {
                mountSlots.insert(root);
                for (int env = kEnvIndoor; env < kEnvCount; ++env) {
                    if (auto* v = VariantOf(root, env)) mountSlots.insert(v);
                }
            }
        }
        // Membership is asked through OutdoorOf so LOCATION-OVERRIDE copies
        // are covered too. Those live in per-place vectors, not the env
        // storage VariantOf walks, so a plain pointer test would clamp a
        // location-bound horseback zoom back to 0 while leaving the outdoor
        // one alone â€” the same entry behaving differently in one town.
        auto isMountSlot = [&](CameraProfile* p) {
            return mountSlots.count(p) || mountSlots.count(OutdoorOf(p));
        };

        auto clampMount = [](CameraProfile* p) {
            p->sideOffset  = std::clamp(p->sideOffset,  Defaults::SideOffset.min,  Defaults::SideOffset.max);
            p->height      = std::clamp(p->height,      Defaults::Height.min,      Defaults::Height.max);
            p->zoom        = std::clamp(p->zoom,        Defaults::MountZoom.min,   Defaults::MountZoom.max);
            p->fov         = std::clamp(p->fov,         Defaults::FOV.min,         Defaults::FOV.max);
            p->rotation    = std::clamp(p->rotation,    Defaults::Rotation.min,    Defaults::Rotation.max);
            p->pitchOffset = std::clamp(p->pitchOffset, Defaults::PitchOffset.min, Defaults::PitchOffset.max);
        };

        auto clampDragon = [](CameraProfile* p) {
            p->sideOffset  = std::clamp(p->sideOffset,  Defaults::DragonSideOffset.min,  Defaults::DragonSideOffset.max);
            p->height      = std::clamp(p->height,      Defaults::DragonHeight.min,      Defaults::DragonHeight.max);
            p->zoom        = std::clamp(p->zoom,        Defaults::DragonZoom.min,        Defaults::DragonZoom.max);
            p->fov         = std::clamp(p->fov,         Defaults::DragonFOV.min,         Defaults::DragonFOV.max);
            p->rotation    = std::clamp(p->rotation,    Defaults::Rotation.min,          Defaults::Rotation.max);
            p->pitchOffset = std::clamp(p->pitchOffset, Defaults::DragonPitchOffset.min, Defaults::DragonPitchOffset.max);
        };

        for (auto* p : GetAllProfiles()) {
            // Dragon riding is the one family allowed wider tolerances on
            // every axis except rotation.
            if (dragonSlots.count(p)) { clampDragon(p); continue; }
            if (isMountSlot(p))       { clampMount(p);  continue; }
            p->sideOffset  = std::clamp(p->sideOffset,  Defaults::SideOffset.min,  Defaults::SideOffset.max);
            p->height      = std::clamp(p->height,      Defaults::Height.min,      Defaults::Height.max);
            p->zoom        = std::clamp(p->zoom,        Defaults::Zoom.min,        Defaults::Zoom.max);
            p->fov         = std::clamp(p->fov,         Defaults::FOV.min,         Defaults::FOV.max);
            p->rotation    = std::clamp(p->rotation,    Defaults::Rotation.min,    Defaults::Rotation.max);
            p->pitchOffset = std::clamp(p->pitchOffset, Defaults::PitchOffset.min, Defaults::PitchOffset.max);
        }
        // The roots again, unconditionally: GetAllProfiles may not enumerate
        // every variant address, and a missed dragon slot is invisible until
        // someone reports their framing snapping back.
        for (auto* p : dragonSlots) clampDragon(const_cast<CameraProfile*>(p));
        for (auto* p : mountSlots)  clampMount(const_cast<CameraProfile*>(p));

        deathCameraFov          = std::clamp(deathCameraFov,          Defaults::FirstPersonFOV.min, Defaults::FirstPersonFOV.max);
        deathCameraHoldDuration = std::clamp(deathCameraHoldDuration, 1.0f, 30.0f);
        deathCameraSlowmoStrength = std::clamp(deathCameraSlowmoStrength, 0.0f, 90.0f);
        deathCameraSlowmoDuration = std::clamp(deathCameraSlowmoDuration, 0.0f, 15.0f);
        ragdollCamFov             = std::clamp(ragdollCamFov, 40.0f, 140.0f);
        ragdollCamSlowmoStrength  = std::clamp(ragdollCamSlowmoStrength, 0.0f, 90.0f);
        ragdollCamSlowmoDuration  = std::clamp(ragdollCamSlowmoDuration, 0.0f, 15.0f);
        headBobIntensity        = std::clamp(headBobIntensity,   0.0f, 5.0f);
        headBobIntensityFp      = std::clamp(headBobIntensityFp, 0.0f, 5.0f);
        targetLockAimBias       = std::clamp(targetLockAimBias, -1.5f, 1.5f);
        targetLockTrackSeconds = std::clamp(targetLockTrackSeconds, 0.0f, 1.0f);
        targetLockAcquireSwingSeconds = std::clamp(targetLockAcquireSwingSeconds, 0.06f, 1.20f);
        targetLockSwitchSpeed = std::clamp(targetLockSwitchSpeed, 90.0f, 720.0f);
        // Enemy override values clamp to the same per-channel ranges as
        // CameraProfile fields. Only the enabled channels matter at runtime,
        // but clamping unconditionally protects against corrupt TOML data.
        auto clampEnemyField = [](EnemyFieldOverride& o) {
            o.profile.sideOffset  = std::clamp(o.profile.sideOffset,  Defaults::SideOffset.min,  Defaults::SideOffset.max);
            o.profile.height      = std::clamp(o.profile.height,      Defaults::Height.min,      Defaults::Height.max);
            o.profile.zoom        = std::clamp(o.profile.zoom,        Defaults::Zoom.min,        Defaults::Zoom.max);
            o.profile.fov         = std::clamp(o.profile.fov,         Defaults::FOV.min,         Defaults::FOV.max);
            o.profile.rotation    = std::clamp(o.profile.rotation,    Defaults::Rotation.min,    Defaults::Rotation.max);
            o.profile.pitchOffset = std::clamp(o.profile.pitchOffset, Defaults::PitchOffset.min, Defaults::PitchOffset.max);
            o.profile.transitionAimBias =
                std::clamp(o.profile.transitionAimBias, -1.5f, 1.5f);
        };
        for (auto& row : enemyOverrides)                for (auto& o : row) clampEnemyField(o);
        for (auto& row : enemyOverridesIndoor)          for (auto& o : row) clampEnemyField(o);
        for (auto& row : tlEnemyOverridesPowerAttackDir) for (auto& o : row) clampEnemyField(o);
        for (auto& row : tlEnemyOverridesPowerAttackDirIndoor) for (auto& o : row) clampEnemyField(o);
        for (auto& c : customEnemyOverrides) {
            for (auto& o : c.slots) clampEnemyField(o);
            for (auto& o : c.paDir) clampEnemyField(o);
        }
        // --- Projectile Repulse migration (one-time per old preset) --------
        // The global [cinematic.projectile_repulse] sliders became per-entry
        // `repulse` values on the noise entries. The magic value seeds every
        // magic/staves school cell (plain map â€” WITHOUT touching `enabled`,
        // which would activate custom noise as a side effect; the repulse
        // lookup reads the plain entry regardless); the archery value seeds
        // the drawing entries. The old keys are no longer written, so a
        // migrated preset stops carrying them after its next save.
        {
            const float oldMagic   = projectileRepulseMagic;
            const float oldArchery = projectileRepulseArchery;
            if (oldMagic > 0.0001f || oldArchery > 0.0001f) {
                auto seed = [&](const std::string& key, float v) {
                    if (v <= 0.0001f) return;
                    stateNoise[key].repulse = v;
                    if (auto it = stateNoiseIndoor.find(key); it != stateNoiseIndoor.end())
                        it->second.repulse = v;
                    for (auto& lo : locationOverrides)
                        if (auto it = lo.stateNoise.find(key); it != lo.stateNoise.end())
                            it->second.repulse = v;
                };
                static constexpr const char* kMigSchools[] = {
                    "alteration", "conjuration", "destruction", "illusion", "restoration"
                };
                for (const char* sch : kMigSchools) {
                    for (const char* ct : { "concentration", "fire_and_forget", "ritual" }) {
                        seed(std::string("magic.") + sch + "." + ct, oldMagic);
                        seed(std::string("magic.") + sch + ".sneak." + ct, oldMagic);
                    }
                    for (const char* ct : { "concentration", "fire_and_forget", "ritual" }) {
                        seed(std::string("staves.") + sch + "." + ct, oldMagic);
                        seed(std::string("staves.") + sch + ".sneak." + ct, oldMagic);
                    }
                }
                seed("weapons.bow.draw",            oldArchery);
                seed("weapons.bow.sneak.draw",      oldArchery);
                seed("weapons.crossbow.draw",       oldArchery);
                seed("weapons.crossbow.sneak.draw", oldArchery);
                spdlog::debug("[Repulse] migrated global sliders (magic={:.2f} archery={:.2f}) "
                             "into per-entry values",
                             oldMagic, oldArchery);
                projectileRepulseMagic = projectileRepulseArchery = 0.0f;  // consumed
            }
        }

        auto clampNoise = [](NoiseProfile& p) {
            p.amp    = std::clamp(p.amp,    0.0f, 5.0f);
            p.speed  = std::clamp(p.speed,  0.0f, 4.0f);
            p.sway   = std::clamp(p.sway,   0.0f, 5.0f);
            p.tilt   = std::clamp(p.tilt,   0.0f, 5.0f);
            p.wobble = std::clamp(p.wobble, 0.0f, 3.0f);
            p.repulse = std::clamp(p.repulse, 0.0f, 3.0f);
            p.repulseFeel = std::clamp(p.repulseFeel, 0.0f, 1.0f);
        };
        clampNoise(globalNoise);
        for (auto& [_, p] : stateNoise) clampNoise(p);

        // 1p clamps: global + every per-state entry.
        auto clampFp = [&](FirstPersonProfile& p) {
            p.worldFov = std::clamp(p.worldFov, Defaults::FirstPersonFOV.min, Defaults::FirstPersonFOV.max);
            p.handsFov = std::clamp(p.handsFov, Defaults::FirstPersonFOV.min, Defaults::FirstPersonFOV.max);
            p.repulse     = std::clamp(p.repulse, 0.0f, 3.0f);
            p.repulseFeel = std::clamp(p.repulseFeel, 0.0f, 1.0f);
            clampNoise(p.noise);
        };
        clampFp(firstPersonGlobal);
        for (auto& [_, p] : stateFirstPerson) clampFp(p);
        for (auto& [_, ov] : stateFpMeleeOverrides)
            for (auto& p : ov.perWeapon) clampFp(p);
        firstPersonTransitionSpeed = std::clamp(firstPersonTransitionSpeed, 0.02f, 10.0f);
    }

    void SettingsManager::ResetAllToVanilla()
    {
        cameraCollision = {};
        disableCollisionTrees  = false;
        disableCollisionProps  = false;
        disableCollisionActors = false;
        transitionMulRotation = 0.5f;
        transitionMulPitch    = 0.5f;
        transitionMulPosition = 0.5f;
        transitionMulZoom     = 0.5f;
        transitionMulFOV      = 0.5f;
        transitionWeight      = 0.0f;
        cameraLooseness       = 0.0f;
        dragonShakeBreathEnabled    = false;
        dragonShakeBreathAmp        = 0.0f;
        dragonShakeBreathSpeed      = 1.0f;
        dragonShakeBreathRange      = 3000.0f;
        dragonShakeProjectileEnabled = false;
        dragonShakeProjectileAmp    = 0.0f;
        dragonShakeProjectileSpeed  = 1.0f;
        dragonShakeProjectileRange  = 3000.0f;
        dragonShakeBiteEnabled      = false;
        dragonShakeBiteAmp          = 0.0f;
        dragonShakeBiteSpeed        = 1.0f;
        dragonShakeBiteRange        = 3000.0f;
        dragonShakeTailEnabled      = false;
        dragonShakeTailAmp          = 0.0f;
        dragonShakeTailSpeed        = 1.0f;
        dragonShakeTailRange        = 3000.0f;
        dragonShakeWingEnabled      = false;
        dragonShakeWingAmp          = 0.0f;
        dragonShakeWingSpeed        = 1.0f;
        dragonShakeWingRange        = 3000.0f;
        dragonShakeLandingEnabled   = false;
        dragonShakeLandingAmp      = 0.0f;
        dragonShakeLandingSpeed    = 1.0f;
        dragonShakeLandingRange    = 3000.0f;
        dragonShakeTakeoffEnabled  = false;
        dragonShakeTakeoffAmp      = 0.0f;
        dragonShakeTakeoffSpeed    = 1.0f;
        dragonShakeTakeoffRange    = 3000.0f;
        centurionShakeWalkEnabled  = false;
        centurionShakeWalkAmp      = 0.0f;
        centurionShakeWalkSpeed    = 1.0f;
        centurionShakeWalkRange    = 3000.0f;
        centurionShakeMeleeEnabled = false;
        centurionShakeMeleeAmp     = 0.0f;
        centurionShakeMeleeSpeed   = 1.0f;
        centurionShakeMeleeRange   = 3000.0f;
        centurionShakeSteamEnabled = false;
        centurionShakeSteamAmp     = 0.0f;
        centurionShakeSteamSpeed   = 1.0f;
        centurionShakeSteamRange   = 3000.0f;
        werewolfTransformIntensity    = 0.0f;
        werewolfTransformSpeed        = 1.6f;
        vampireLordTransformIntensity = 0.0f;
        vampireLordTransformSpeed     = 0.7f;
        vampireLordBatsIntensity      = 0.0f;
        vampireLordBatsSpeed          = 1.4f;
        reanimateShakeIntensity       = 0.0f;
        reanimateShakeSpeed           = 0.9f;
        reanimateShakeRange           = 1200.0f;
        summonShakeIntensity          = 0.0f;
        summonShakeSpeed              = 1.1f;
        summonShakeRange              = 1500.0f;
        // First-person halves of the creature shakes.
        dragonShakeBreathAmpFp = 0.0f; dragonShakeBreathSpeedFp = 1.0f; dragonShakeBreathRangeFp = 3000.0f;
        dragonShakeBreathCharFp = kCharSustained;
        dragonShakeProjectileAmpFp = 0.0f; dragonShakeProjectileSpeedFp = 1.0f; dragonShakeProjectileRangeFp = 3000.0f;
        dragonShakeProjectileCharFp = kCharSharp;
        dragonShakeBiteAmpFp = 0.0f; dragonShakeBiteSpeedFp = 1.0f; dragonShakeBiteRangeFp = 3000.0f;
        dragonShakeBiteCharFp = kCharSharp;
        dragonShakeTailAmpFp = 0.0f; dragonShakeTailSpeedFp = 1.0f; dragonShakeTailRangeFp = 3000.0f;
        dragonShakeTailCharFp = kCharSharp;
        dragonShakeWingAmpFp = 0.0f; dragonShakeWingSpeedFp = 1.0f; dragonShakeWingRangeFp = 3000.0f;
        dragonShakeWingCharFp = kCharSharp;
        dragonShakeLandingAmpFp = 0.0f; dragonShakeLandingSpeedFp = 1.0f; dragonShakeLandingRangeFp = 3000.0f;
        dragonShakeLandingCharFp = kCharHeavy;
        dragonShakeTakeoffAmpFp = 0.0f; dragonShakeTakeoffSpeedFp = 1.0f; dragonShakeTakeoffRangeFp = 3000.0f;
        dragonShakeTakeoffCharFp = kCharHeavy;
        centurionShakeWalkAmpFp = 0.0f; centurionShakeWalkSpeedFp = 1.0f; centurionShakeWalkRangeFp = 3000.0f;
        centurionShakeWalkCharFp = kCharHeavy;
        centurionShakeMeleeAmpFp = 0.0f; centurionShakeMeleeSpeedFp = 1.0f; centurionShakeMeleeRangeFp = 3000.0f;
        centurionShakeMeleeCharFp = kCharHeavy;
        centurionShakeSteamAmpFp = 0.0f; centurionShakeSteamSpeedFp = 1.0f; centurionShakeSteamRangeFp = 3000.0f;
        centurionShakeSteamCharFp = kCharSustained;
        reanimateShakeIntensityFp = 0.0f; reanimateShakeSpeedFp = 0.9f; reanimateShakeRangeFp = 1200.0f;
        reanimateShakeCharFp      = kCharReanimate;
        summonShakeIntensityFp    = 0.0f; summonShakeSpeedFp    = 1.1f; summonShakeRangeFp    = 1500.0f;
        summonShakeCharFp         = kCharSummon;
        slowTimeNoiseStrength         = 0.0f;
        weaponDrawNoiseIntensity      = 0.0f;
        weaponDrawNoiseDuration       = 0.30f;
        weaponDrawNoiseSpeed          = 1.0f;
        weaponDrawNoiseIntensityFp    = 0.0f;
        weaponDrawNoiseDurationFp     = 0.30f;
        weaponDrawNoiseSpeedFp        = 1.0f;
        attackLagMagic                = 0.0f;
        attackLagArchery              = 0.0f;
        projectileRepulseArchery      = 0.0f;
        projectileRepulseMagic        = 0.0f;
        fleeFramingStrength           = 0.0f;
        jumpNoiseAmp                  = 0.0f;
        fallNoiseAmp                  = 0.0f;
        jumpRepulse                   = 0.0f;
        jumpRepulseFeel               = 0.5f;
        jumpNoiseAmpFp                = 0.0f;
        fallNoiseAmpFp                = 0.0f;
        jumpRepulseFp                 = 0.0f;
        jumpRepulseFeelFp             = 0.5f;
        paraglideEnabled              = false;
        paraglideLandFade             = 0.5f;
        paraglideProfile              = CameraProfile{};
        paraglideTLProfile            = CameraProfile{};
        paraglideNoise                = NoiseProfile{};
        paraglideNoiseIndoor          = NoiseProfile{};
        headBobIntensity              = 0.0f;
        headBobIntensityFp            = 0.0f;
        stairSmoothStrength           = 0.0f;
        stairSmoothLimit              = 24.0f;
        dragonShakeBreathChar     = kCharSustained;
        dragonShakeProjectileChar = kCharSharp;
        dragonShakeBiteChar       = kCharSharp;
        dragonShakeTailChar       = kCharSharp;
        dragonShakeWingChar       = kCharSharp;
        dragonShakeLandingChar    = kCharHeavy;
        dragonShakeTakeoffChar    = kCharHeavy;
        centurionShakeWalkChar    = kCharHeavy;
        centurionShakeMeleeChar   = kCharHeavy;
        centurionShakeSteamChar   = kCharSustained;
        werewolfTransformChar     = kCharWerewolf;
        vampireLordTransformChar  = kCharVampLord;
        vampireLordBatsChar       = kCharBats;
        reanimateShakeChar        = kCharReanimate;
        summonShakeChar           = kCharSummon;
        weaponDrawNoiseChar       = kCharDraw;
        weaponDrawNoiseCharFp     = kCharDraw;
        werewolfRevertIntensity    = 0.0f;
        werewolfRevertSpeed        = 1.6f;
        werewolfRevertChar         = kCharWerewolf;
        vampireLordRevertIntensity = 0.0f;
        vampireLordRevertSpeed     = 0.7f;
        vampireLordRevertChar      = kCharVampLord;
        eventBeats = MakeDefaultBeatTunings();
        npcNoiseIntensity = 0.0f;
        npcNoiseIntensityFp = 0.0f;
        npcShoutNoiseIntensity = 0.0f;
        npcShoutNoiseIntensityFp = 0.0f;
        npcMeleeNoiseIntensity = 0.0f;
        npcMeleeNoiseIntensityFp = 0.0f;
        npcArcheryNoiseIntensity = 0.0f;
        npcArcheryNoiseIntensityFp = 0.0f;
        npcTransformNoiseIntensity = 0.0f;
        npcTransformNoiseIntensityFp = 0.0f;
        combatPulseIntensity = 0.0f;
        combatPulseDuration  = 0.8f;
        disableVanityCamera = false;
        showPlayerInInventory = ShowPlayerInMenuEntry{};
        showPlayerInContainer = ShowPlayerInMenuEntry{};
        showPlayerInBarter    = ShowPlayerInMenuEntry{};
        showPlayerInMagic     = ShowPlayerInMenuEntry{};
        showPlayerInTween     = ShowPlayerInMenuEntry{};
        showPlayerInWait      = ShowPlayerInMenuEntry{};
        showPlayerInFavorites = ShowPlayerInMenuEntry{};
        archeryTracingEnabled   = false;
        spellTracingEnabled     = false;
        projectileReticleColorR      = 1.0f;
        projectileReticleColorG      = 1.0f;
        projectileReticleColorB      = 1.0f;
        projectileReticleSizeScale   = 1.0f;
        projectileReticleThickness   = 2.0f;
        archeryTracingSmoothTau = 0.015f;
        sneakMeterOffsetX       = -500.0f;
        sneakMeterOffsetY       = -100.0f;
        firstPersonFovEnabled      = true;   // always-on (toggle removed)
        firstPersonNoiseEnabled    = true;   // always-on (toggle removed)
        firstPersonGlobal          = FirstPersonProfile{};
        stateFirstPerson.clear();
        stateFpMeleeOverrides.clear();
        firstPersonTransitionSpeed = 1.0f;

        dialogueEnabled            = true;   // always on (no toggle)
        dialogueProfile            = CameraProfile::VanillaCombat();
        dialogueFirstPersonEnabled = true;   // always on (no toggle)
        dialogueFirstPersonProfile = CameraProfile::VanillaDialogue1p();
        dialogueMovementEnabled    = false;
        dialogueSwitchOnNpcLine    = false;
        dialogueSkipShortNpcLines  = false;
        dialogueAutoSwitchMin      = 0.0f;
        dialogueAutoSwitchMax      = 0.0f;
        dialogueRandomEveryTime    = false;
        dialogueMinShotSec         = 0.0f;
        dialogueMulRotation = 0.2f;
        dialogueMulPitch    = 0.2f;
        dialogueMulPosition = 0.2f;
        dialogueMulZoom     = 0.2f;
        dialogueMulFOV      = 0.2f;
        dialogueRandomEnabled = false;
        for (std::size_t i = 0; i < dialogueRandomOnEntry.size(); ++i) {
            dialogueRandomOnEntry[i]  = false;
            dialogueRandomOnOption[i] = false;
        }
        for (auto& bucket : dialogueBuckets) {
            bucket = DialogueBucket{};
        }
        // Re-seed the catch-alls through the shared helper — dialogueProfile
        // and dialogueFirstPersonProfile were just reset to VanillaCombat
        // above, so the Defaults it creates match what the old inline seeds
        // pushed, and Horseback (which the inline seeds forgot) is covered.
        EnsureDialogueDefaultLooks();
        dialogueSchemaVersion = 3;
        // dialogueCycleNextKey / dialogueCyclePrevKey are global keybinds â€”
        // not reset here (see the hotkey note in the death-camera block).
        activeDialogueBucketIdx = -1;
        activeDialogueLookIdx   = -1;

        sheathed = sheathedSprint = sheathedSwim = sheathedSneak = CameraProfile::Default3p();

        weaponsMelee = weaponsMeleeSprint = weaponsMeleeSwim = weaponsMeleeAttack = weaponsMeleeSneak = CameraProfile::Default3p();
        weaponsMeleePowerAttack = weaponsMeleeSneakAttack = weaponsMeleeSneakPowerAttack =
            weaponsMeleeSprintAttack = weaponsMeleeSprintPowerAttack = CameraProfile::Default3p();
        weaponsMeleeOverrides        = MeleeWeaponOverrides{};
        weaponsMeleeSprintOverrides  = MeleeWeaponOverrides{};
        weaponsMeleeSwimOverrides    = MeleeWeaponOverrides{};
        weaponsMeleeAttackOverrides     = MeleeWeaponOverrides{};
        weaponsMeleeSneakOverrides      = MeleeWeaponOverrides{};
        weaponsMeleeShoutOverrides      = MeleeWeaponOverrides{};
        weaponsMeleeShoutSneakOverrides = MeleeWeaponOverrides{};
        weaponsMeleePowerAttackOverrides       = MeleeWeaponOverrides{};
        weaponsMeleeSneakAttackOverrides       = MeleeWeaponOverrides{};
        weaponsMeleeSneakPowerAttackOverrides  = MeleeWeaponOverrides{};
        weaponsMeleeSprintAttackOverrides      = MeleeWeaponOverrides{};
        weaponsMeleeSprintPowerAttackOverrides = MeleeWeaponOverrides{};
        for (auto& p : weaponsMeleePowerAttackDir) p = CameraProfile::Default3p();
        weaponsMeleePowerAttackDirEnabled.fill(false);
        for (auto& o : weaponsMeleePowerAttackDirOverrides) o = MeleeWeaponOverrides{};
        weaponBindings.clear();
        weaponsBlockingOneHanded =
            weaponsBlockingTwoHanded =
            weaponsBlockingShield =
            weaponsBlockingWard =
            weaponsBlockingOneHandedSneak =
            weaponsBlockingTwoHandedSneak =
            weaponsBlockingShieldSneak =
            weaponsBlockingWardSneak =
            weaponsBlockingShieldSprint = CameraProfile::Default3p();
        weaponsBlockingWardEnabled      = false;
        weaponsBlockingWardSneakEnabled = false;
        weaponsBow = weaponsBowSprint = weaponsBowSwim = weaponsBowDraw = weaponsBowSneak = weaponsBowSneakDraw = weaponsBowZoom = weaponsBowSneakZoom = CameraProfile::Default3p();
        weaponsCrossbow = weaponsCrossbowSprint = weaponsCrossbowSwim = weaponsCrossbowDraw = weaponsCrossbowSneak = weaponsCrossbowSneakDraw = weaponsCrossbowZoom = weaponsCrossbowSneakZoom = CameraProfile::Default3p();
        weaponsMagic = weaponsMagicSprint = weaponsMagicSwim = weaponsMagicSneak = CameraProfile::Default3p();

        magicAlterationConcentration = magicAlterationFireAndForget = magicAlterationRitual = CameraProfile::Default3p();
        magicConjurationConcentration = magicConjurationFireAndForget = magicConjurationRitual = CameraProfile::Default3p();
        magicDestructionConcentration = magicDestructionFireAndForget = magicDestructionRitual = CameraProfile::Default3p();
        magicIllusionConcentration = magicIllusionFireAndForget = magicIllusionRitual = CameraProfile::Default3p();
        magicRestorationConcentration = magicRestorationFireAndForget = magicRestorationRitual = CameraProfile::Default3p();

        magicAlterationConcentrationSneak = magicAlterationFireAndForgetSneak = magicAlterationRitualSneak = CameraProfile::Default3p();
        magicConjurationConcentrationSneak = magicConjurationFireAndForgetSneak = magicConjurationRitualSneak = CameraProfile::Default3p();
        magicDestructionConcentrationSneak = magicDestructionFireAndForgetSneak = magicDestructionRitualSneak = CameraProfile::Default3p();
        magicIllusionConcentrationSneak = magicIllusionFireAndForgetSneak = magicIllusionRitualSneak = CameraProfile::Default3p();
        magicRestorationConcentrationSneak = magicRestorationFireAndForgetSneak = magicRestorationRitualSneak = CameraProfile::Default3p();

        // Per-hand magic overrides: back to untouched + disabled.
        magicHandOverrides   = MagicHandGrid{};
        tlMagicHandOverrides = MagicHandGrid{};

        weaponsStaves = weaponsStavesSprint = weaponsStavesSwim = weaponsStavesSneak = CameraProfile::Default3p();

        stavesAlterationConcentration = stavesAlterationFireAndForget = stavesAlterationRitual = CameraProfile::Default3p();
        stavesConjurationConcentration = stavesConjurationFireAndForget = stavesConjurationRitual = CameraProfile::Default3p();
        stavesDestructionConcentration = stavesDestructionFireAndForget = stavesDestructionRitual = CameraProfile::Default3p();
        stavesIllusionConcentration = stavesIllusionFireAndForget = stavesIllusionRitual = CameraProfile::Default3p();
        stavesRestorationConcentration = stavesRestorationFireAndForget = stavesRestorationRitual = CameraProfile::Default3p();

        stavesAlterationConcentrationSneak = stavesAlterationFireAndForgetSneak = stavesAlterationRitualSneak = CameraProfile::Default3p();
        stavesConjurationConcentrationSneak = stavesConjurationFireAndForgetSneak = stavesConjurationRitualSneak = CameraProfile::Default3p();
        stavesDestructionConcentrationSneak = stavesDestructionFireAndForgetSneak = stavesDestructionRitualSneak = CameraProfile::Default3p();
        stavesIllusionConcentrationSneak = stavesIllusionFireAndForgetSneak = stavesIllusionRitualSneak = CameraProfile::Default3p();
        stavesRestorationConcentrationSneak = stavesRestorationFireAndForgetSneak = stavesRestorationRitualSneak = CameraProfile::Default3p();

        transformationsWerewolf = transformationsWerewolfSheathed = transformationsWerewolfSprint = transformationsWerewolfSwim =
            transformationsWerewolfAttack = transformationsWerewolfRoar = transformationsWerewolfFeeding =
            transformationsWerewolfPowerAttack = transformationsWerewolfSprintPowerAttack = CameraProfile::WerewolfDefault();
        // VL Reset to Vanilla = the pulled-back default that frames the
        // VL body, NOT engine-natural Vanilla() (which is too tight) or
        // VanillaCombat (which is also too tight). Same baseline as the
        // load-time migration.
        vampireLordSheathed = vampireLordSheathedLevitating = vampireLordMelee = vampireLordMagic =
            vampireLordConcentration = vampireLordFireAndForget = vampireLordSprint =
            vampireLordSprintLevitating =
            vampireLordMeleeAttack = vampireLordMeleePowerAttack = kVampireLordVanilla;

        mountsHorseback = mountsHorsebackSprint = mountsHorsebackSwim = mountsHorsebackMelee = mountsHorsebackArchery = mountsHorsebackArcheryDraw = mountsHorsebackArcheryZoom = mountsHorsebackMeleeLeft = mountsHorsebackMeleeRight = CameraProfile::VanillaHorseback();
        mountsDragonRiding = CameraProfile::VanillaDragonRiding();
        for (auto* p : DragonRidingSubStateProfiles()) *p = CameraProfile::VanillaDragonRiding();

        for (auto& p : shoutsBaseByState)      p = CameraProfile{};
        for (auto& p : shoutsBaseByStateSneak) p = CameraProfile{};
        for (auto& row : shoutOverrideByState)           for (auto& p : row) p = CameraProfile{};
        for (auto& row : shoutOverrideByStateSneak)      for (auto& p : row) p = CameraProfile{};
        for (auto& row : shoutOverrideByStateEnabled)      for (auto& b : row) b = false;
        for (auto& row : shoutOverrideByStateEnabledSneak) for (auto& b : row) b = false;
        // Target-lock parallel tree â€” GetAllProfiles already covers every
        // tl* slot, but it returns profiles by pointer so we can't use it
        // here to assign. Clearing the shared defaults by re-constructing
        // a fresh copy via swap keeps this short and mechanical.
        for (auto* p : GetAllProfiles()) {
            // Non-TL profiles got their own reset above (vanilla defaults).
            // TL profiles use CameraProfile{} default which is already
            // what we want, so we only clear the tl* subset. The easiest
            // way to detect a TL pointer without a separate registry is
            // to keep the existing per-category resets elsewhere and just
            // not touch them here â€” but since Reset runs rarely, zeroing
            // all unknown profiles is cheap.
            (void)p;
        }
        // Explicit tl* resets â€” mirrors the field list. Keep in sync when
        // adding TL profiles.
        tlSheathed = tlSheathedSprint = tlSheathedSwim = tlSheathedSneak = CameraProfile{};
        tlWeaponsMelee = tlWeaponsMeleeSprint = tlWeaponsMeleeSwim = tlWeaponsMeleeAttack = tlWeaponsMeleeSneak = CameraProfile{};
        tlWeaponsMeleePowerAttack = tlWeaponsMeleeSneakAttack = tlWeaponsMeleeSneakPowerAttack =
            tlWeaponsMeleeSprintAttack = tlWeaponsMeleeSprintPowerAttack = CameraProfile{};
        for (auto& p : tlWeaponsMeleePowerAttackDir) p = CameraProfile{};
        tlWeaponsMeleePowerAttackDirEnabled.fill(false);
        for (auto& o : tlWeaponsMeleePowerAttackDirOverrides) o = MeleeWeaponOverrides{};
        tlWeaponsMeleeOverrides        = MeleeWeaponOverrides{};
        tlWeaponsMeleeSprintOverrides  = MeleeWeaponOverrides{};
        tlWeaponsMeleeSwimOverrides    = MeleeWeaponOverrides{};
        tlWeaponsMeleeAttackOverrides     = MeleeWeaponOverrides{};
        tlWeaponsMeleeSneakOverrides      = MeleeWeaponOverrides{};
        tlWeaponsMeleeShoutOverrides      = MeleeWeaponOverrides{};
        tlWeaponsMeleeShoutSneakOverrides = MeleeWeaponOverrides{};
        tlWeaponsMeleePowerAttackOverrides       = MeleeWeaponOverrides{};
        tlWeaponsMeleeSneakAttackOverrides       = MeleeWeaponOverrides{};
        tlWeaponsMeleeSneakPowerAttackOverrides  = MeleeWeaponOverrides{};
        tlWeaponsMeleeSprintAttackOverrides      = MeleeWeaponOverrides{};
        tlWeaponsMeleeSprintPowerAttackOverrides = MeleeWeaponOverrides{};
        tlWeaponsBlockingOneHanded =
            tlWeaponsBlockingTwoHanded =
            tlWeaponsBlockingShield =
            tlWeaponsBlockingWard =
            tlWeaponsBlockingOneHandedSneak =
            tlWeaponsBlockingTwoHandedSneak =
            tlWeaponsBlockingShieldSneak =
            tlWeaponsBlockingWardSneak =
            tlWeaponsBlockingShieldSprint = CameraProfile{};
        tlWeaponsBow = tlWeaponsBowSprint = tlWeaponsBowSwim = tlWeaponsBowDraw = tlWeaponsBowSneak = tlWeaponsBowSneakDraw = tlWeaponsBowZoom = tlWeaponsBowSneakZoom = CameraProfile{};
        tlWeaponsCrossbow = tlWeaponsCrossbowSprint = tlWeaponsCrossbowSwim = tlWeaponsCrossbowDraw = tlWeaponsCrossbowSneak = tlWeaponsCrossbowSneakDraw = tlWeaponsCrossbowZoom = tlWeaponsCrossbowSneakZoom = CameraProfile{};
        tlWeaponsMagic = tlWeaponsMagicSprint = tlWeaponsMagicSwim = tlWeaponsMagicSneak = CameraProfile{};
        tlMagicAlterationConcentration = tlMagicAlterationFireAndForget = tlMagicAlterationRitual = CameraProfile{};
        tlMagicConjurationConcentration = tlMagicConjurationFireAndForget = tlMagicConjurationRitual = CameraProfile{};
        tlMagicDestructionConcentration = tlMagicDestructionFireAndForget = tlMagicDestructionRitual = CameraProfile{};
        tlMagicIllusionConcentration = tlMagicIllusionFireAndForget = tlMagicIllusionRitual = CameraProfile{};
        tlMagicRestorationConcentration = tlMagicRestorationFireAndForget = tlMagicRestorationRitual = CameraProfile{};
        tlMagicAlterationConcentrationSneak = tlMagicAlterationFireAndForgetSneak = tlMagicAlterationRitualSneak = CameraProfile{};
        tlMagicConjurationConcentrationSneak = tlMagicConjurationFireAndForgetSneak = tlMagicConjurationRitualSneak = CameraProfile{};
        tlMagicDestructionConcentrationSneak = tlMagicDestructionFireAndForgetSneak = tlMagicDestructionRitualSneak = CameraProfile{};
        tlMagicIllusionConcentrationSneak = tlMagicIllusionFireAndForgetSneak = tlMagicIllusionRitualSneak = CameraProfile{};
        tlMagicRestorationConcentrationSneak = tlMagicRestorationFireAndForgetSneak = tlMagicRestorationRitualSneak = CameraProfile{};
        tlWeaponsStaves = tlWeaponsStavesSprint = tlWeaponsStavesSwim = tlWeaponsStavesSneak = CameraProfile{};
        tlStavesAlterationConcentration = tlStavesAlterationFireAndForget = tlStavesAlterationRitual = CameraProfile{};
        tlStavesConjurationConcentration = tlStavesConjurationFireAndForget = tlStavesConjurationRitual = CameraProfile{};
        tlStavesDestructionConcentration = tlStavesDestructionFireAndForget = tlStavesDestructionRitual = CameraProfile{};
        tlStavesIllusionConcentration = tlStavesIllusionFireAndForget = tlStavesIllusionRitual = CameraProfile{};
        tlStavesRestorationConcentration = tlStavesRestorationFireAndForget = tlStavesRestorationRitual = CameraProfile{};
        tlStavesAlterationConcentrationSneak = tlStavesAlterationFireAndForgetSneak = tlStavesAlterationRitualSneak = CameraProfile{};
        tlStavesConjurationConcentrationSneak = tlStavesConjurationFireAndForgetSneak = tlStavesConjurationRitualSneak = CameraProfile{};
        tlStavesDestructionConcentrationSneak = tlStavesDestructionFireAndForgetSneak = tlStavesDestructionRitualSneak = CameraProfile{};
        tlStavesIllusionConcentrationSneak = tlStavesIllusionFireAndForgetSneak = tlStavesIllusionRitualSneak = CameraProfile{};
        tlStavesRestorationConcentrationSneak = tlStavesRestorationFireAndForgetSneak = tlStavesRestorationRitualSneak = CameraProfile{};
        tlTransformationsWerewolf = tlTransformationsWerewolfSheathed = tlTransformationsWerewolfSprint = tlTransformationsWerewolfSwim = CameraProfile{};
        tlTransformationsWerewolfAttack = tlTransformationsWerewolfRoar = tlTransformationsWerewolfFeeding = CameraProfile{};
        tlTransformationsWerewolfPowerAttack = tlTransformationsWerewolfSprintPowerAttack = CameraProfile{};
        tlVampireLordSheathed = tlVampireLordSheathedLevitating = tlVampireLordMelee = tlVampireLordMagic = CameraProfile{};
        tlVampireLordMeleeAttack = tlVampireLordMeleePowerAttack = CameraProfile{};
        tlVampireLordConcentration = tlVampireLordFireAndForget = tlVampireLordSprint = tlVampireLordSprintLevitating = CameraProfile{};
        tlMountsHorseback = tlMountsHorsebackSprint = tlMountsHorsebackSwim = CameraProfile{};
        tlMountsHorsebackMelee = tlMountsHorsebackArchery = tlMountsHorsebackArcheryDraw = tlMountsHorsebackArcheryZoom = tlMountsHorsebackMeleeLeft = tlMountsHorsebackMeleeRight = CameraProfile{};
        for (auto& p : tlShoutsBaseByState)      p = CameraProfile{};
        for (auto& p : tlShoutsBaseByStateSneak) p = CameraProfile{};
        for (auto& row : tlShoutOverrideByState)           for (auto& p : row) p = CameraProfile{};
        for (auto& row : tlShoutOverrideByStateSneak)      for (auto& p : row) p = CameraProfile{};
        for (auto& row : tlShoutOverrideByStateEnabled)      for (auto& b : row) b = false;
        for (auto& row : tlShoutOverrideByStateEnabledSneak) for (auto& b : row) b = false;
        targetLockAimBias = 1.0f;
        targetLockTrackSeconds = 0.0f;
        targetLockAcquireSwingSeconds = 0.20f;
        targetLockSwitchSpeed = 300.0f;
        for (auto& row : enemyOverrides)       for (auto& o : row) o = EnemyFieldOverride{};
        for (auto& row : enemyOverridesIndoor) for (auto& o : row) o = EnemyFieldOverride{};
        for (auto& row : tlEnemyOverridesPowerAttackDir)       for (auto& o : row) o = EnemyFieldOverride{};
        for (auto& row : tlEnemyOverridesPowerAttackDirIndoor) for (auto& o : row) o = EnemyFieldOverride{};
        customEnemyOverrides.clear();
        customWeaponTypes.clear();

        vanityCamera = CameraProfile::Default3p();
        vanityIdleSeconds = 120.0f;
        deathCameraFov              = 90.0f;
        deathCameraHoldDuration     = 5.0f;
        deathCameraInfiniteDuration = false;
        deathCameraFreeLook         = false;
        deathCameraSlowmoStrength   = 0.0f;
        deathCameraSlowmoDuration   = 0.0f;
        ragdollCamFreeLook          = false;
        ragdollCamFov               = 90.0f;
        ragdollCamSlowmoStrength    = 0.0f;
        ragdollCamSlowmoDuration    = 0.0f;
        ragdollCamHoldParentState   = false;
        // Hotkeys (quick tune, preset cycle, shoulder swap, dialogue cycle,
        // death-cam skip) are global keybinds untied to presets â€” a settings
        // reset must NOT clear them. They persist via Save() / global prefs.

        // Location overrides â€” fully preset-scoped (user ruling 2026-08-15:
        // "location overrides seem to be transferring between presets").
        // The old rule kept the PLACES across a reset ("a fact about the
        // world, not a camera setting"), but every preset load runs
        // reset-then-apply, so carried places leaked into any preset saved
        // or updated afterward. A reset now clears the list outright; a
        // preset load repopulates it from the incoming file alone.
        locationOverridesEnabled = true;
        locationOverrides.clear();
        LocationDetector::GetSingleton().Invalidate();
        activeLocationChain.clear();
        activeLocationIdx  = -1;
        locationEditIdx    = 0;
        locationEditActive = -1;

        animationCameras.clear();
        AnimationCameraController::GetSingleton().RebuildMatchIndex();

        noiseEnabled = true;
        globalNoise        = NoiseProfile{};
        globalNoiseIndoor  = NoiseProfile{};
        stateNoise.clear();
        stateNoiseIndoor.clear();
        weaponsMeleeNoiseOverrides        = MeleeWeaponNoiseOverrides{};
        weaponsMeleeSprintNoiseOverrides  = MeleeWeaponNoiseOverrides{};
        weaponsMeleeSwimNoiseOverrides    = MeleeWeaponNoiseOverrides{};
        weaponsMeleeAttackNoiseOverrides      = MeleeWeaponNoiseOverrides{};
        weaponsMeleeSneakNoiseOverrides       = MeleeWeaponNoiseOverrides{};
        weaponsMeleeShoutNoiseOverrides       = MeleeWeaponNoiseOverrides{};
        weaponsMeleeShoutSneakNoiseOverrides  = MeleeWeaponNoiseOverrides{};
        weaponsMeleePowerAttackNoiseOverrides       = MeleeWeaponNoiseOverrides{};
        weaponsMeleeSneakAttackNoiseOverrides       = MeleeWeaponNoiseOverrides{};
        weaponsMeleeSneakPowerAttackNoiseOverrides  = MeleeWeaponNoiseOverrides{};
        weaponsMeleeSprintAttackNoiseOverrides      = MeleeWeaponNoiseOverrides{};
        weaponsMeleeSprintPowerAttackNoiseOverrides = MeleeWeaponNoiseOverrides{};
        for (auto& o : weaponsMeleePowerAttackDirNoiseOverrides) o = MeleeWeaponNoiseOverrides{};

        // Re-seed indoor variants from the just-vanilla outdoor values so
        // both sets are vanilla after a Reset All.
        InitIndoorOverrides();

        // Clear the active-preset marker â€” a "Reset to Vanilla" leaves no
        // preset semantically active, since the values no longer match any
        // saved preset.
        activePresetName.clear();

        // Snap the camera spring so the visible reset is immediate. Without
        // this, the spring EMAs from the prior custom values to vanilla over
        // ~1s and (with similar values) the user can't tell the reset took
        // effect. Per-panel reset buttons call this through RenderResetAllButton;
        // the global reset path was missing the call.
        CameraController::GetSingleton().ResetTransitionState();

        spdlog::info("All camera settings reset to vanilla");
    }

    CameraProfile* SettingsManager::PickMagicProfile(MagicSchool school, CastType castType, bool isSneaking)
    {
        // Base case: no active cast or no detected school â†’ use the default
        // magic profile. This is the "spell equipped, idle" camera. Honour
        // sneak here so the Sneak sub-state can defer to this picker for
        // Magic without re-overriding back to the non-school slot.
        if (school == MagicSchool::None || castType == CastType::None) {
            return isSneaking ? &weaponsMagicSneak : &weaponsMagic;
        }

        // Per-hand override splice: when the resolver attributes the active
        // cast to a hand (Left / Both / Right) and that hand's slot on this
        // school entry is enabled, the hand profile replaces the base slot.
        // Lives inside the picker so every caller (CameraController and
        // HookManager run parallel pickers) routes identically.
        if (auto* hs = GetMagicHandSet(school, castType, isSneaking, /*a_targetLock=*/false)) {
            const int h = static_cast<int>(StateResolver::GetSingleton().GetCastingHand());
            if (h >= 0 && h < static_cast<int>(kMagicHandCount) && hs->enabled[h]) {
                return &hs->profiles[h];
            }
        }

        // Active cast â€” pick the school+type-specific override slot. When the
        // player is sneaking, a parallel set of *Sneak slots overrides the
        // default cast camera so stealth casters can keep the closer angle.
        switch (school) {
        case MagicSchool::Alteration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &magicAlterationConcentrationSneak : &magicAlterationConcentration;
            case CastType::FireAndForget: return isSneaking ? &magicAlterationFireAndForgetSneak : &magicAlterationFireAndForget;
            case CastType::Ritual:        return isSneaking ? &magicAlterationRitualSneak        : &magicAlterationRitual;
            default: break;
            }
            break;
        case MagicSchool::Conjuration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &magicConjurationConcentrationSneak : &magicConjurationConcentration;
            case CastType::FireAndForget: return isSneaking ? &magicConjurationFireAndForgetSneak : &magicConjurationFireAndForget;
            case CastType::Ritual:        return isSneaking ? &magicConjurationRitualSneak        : &magicConjurationRitual;
            default: break;
            }
            break;
        case MagicSchool::Destruction:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &magicDestructionConcentrationSneak : &magicDestructionConcentration;
            case CastType::FireAndForget: return isSneaking ? &magicDestructionFireAndForgetSneak : &magicDestructionFireAndForget;
            case CastType::Ritual:        return isSneaking ? &magicDestructionRitualSneak        : &magicDestructionRitual;
            default: break;
            }
            break;
        case MagicSchool::Illusion:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &magicIllusionConcentrationSneak : &magicIllusionConcentration;
            case CastType::FireAndForget: return isSneaking ? &magicIllusionFireAndForgetSneak : &magicIllusionFireAndForget;
            case CastType::Ritual:        return isSneaking ? &magicIllusionRitualSneak        : &magicIllusionRitual;
            default: break;
            }
            break;
        case MagicSchool::Restoration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &magicRestorationConcentrationSneak : &magicRestorationConcentration;
            case CastType::FireAndForget: return isSneaking ? &magicRestorationFireAndForgetSneak : &magicRestorationFireAndForget;
            case CastType::Ritual:        return isSneaking ? &magicRestorationRitualSneak        : &magicRestorationRitual;
            default: break;
            }
            break;
        default:
            break;
        }
        return isSneaking ? &weaponsMagicSneak : &weaponsMagic;
    }

    std::array<CameraProfile*, 10> SettingsManager::DragonRidingSubStateProfiles()
    {
        return { &mountsDragonRidingPerched,  &mountsDragonRidingHovering,
                 &mountsDragonRidingTakeoff,  &mountsDragonRidingLanding,
                 &mountsDragonRidingAttackGrounded, &mountsDragonRidingAttackHovering,
                 &mountsDragonRidingAttackFlying,
                 &mountsDragonRidingBreathGrounded, &mountsDragonRidingBreathHovering,
                 &mountsDragonRidingBreathFlying };
    }

    CameraProfile* SettingsManager::DragonRidingProfileFor(DragonAction a_action)
    {
        // Cruising IS the base slot, and the base is also the fallback for
        // anything unmapped â€” so this never returns null and the caller can
        // assign the result straight into `selected`.
        switch (a_action) {
        case DragonAction::Perched:
            return &mountsDragonRidingPerched;
        case DragonAction::Hovering:
            return &mountsDragonRidingHovering;
        case DragonAction::Takeoff:
            return &mountsDragonRidingTakeoff;
        case DragonAction::Landing:
            return &mountsDragonRidingLanding;
        case DragonAction::AttackGrounded:
            return &mountsDragonRidingAttackGrounded;
        case DragonAction::AttackHovering:
            return &mountsDragonRidingAttackHovering;
        case DragonAction::AttackFlying:
            return &mountsDragonRidingAttackFlying;
        case DragonAction::BreathGrounded:
            return &mountsDragonRidingBreathGrounded;
        case DragonAction::BreathHovering:
            return &mountsDragonRidingBreathHovering;
        case DragonAction::BreathFlying:
            return &mountsDragonRidingBreathFlying;
        case DragonAction::Cruising:
        default:
            return &mountsDragonRiding;
        }
    }

    CameraProfile* SettingsManager::PickStavesProfile(MagicSchool school, CastType castType, bool isSneaking)
    {
        if (school == MagicSchool::None || castType == CastType::None) {
            return isSneaking ? &weaponsStavesSneak : &weaponsStaves;
        }

        switch (school) {
        case MagicSchool::Alteration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &stavesAlterationConcentrationSneak : &stavesAlterationConcentration;
            case CastType::FireAndForget: return isSneaking ? &stavesAlterationFireAndForgetSneak : &stavesAlterationFireAndForget;
            case CastType::Ritual: return isSneaking ? &stavesAlterationRitualSneak : &stavesAlterationRitual;
            default: break;
            }
            break;
        case MagicSchool::Conjuration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &stavesConjurationConcentrationSneak : &stavesConjurationConcentration;
            case CastType::FireAndForget: return isSneaking ? &stavesConjurationFireAndForgetSneak : &stavesConjurationFireAndForget;
            case CastType::Ritual: return isSneaking ? &stavesConjurationRitualSneak : &stavesConjurationRitual;
            default: break;
            }
            break;
        case MagicSchool::Destruction:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &stavesDestructionConcentrationSneak : &stavesDestructionConcentration;
            case CastType::FireAndForget: return isSneaking ? &stavesDestructionFireAndForgetSneak : &stavesDestructionFireAndForget;
            case CastType::Ritual: return isSneaking ? &stavesDestructionRitualSneak : &stavesDestructionRitual;
            default: break;
            }
            break;
        case MagicSchool::Illusion:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &stavesIllusionConcentrationSneak : &stavesIllusionConcentration;
            case CastType::FireAndForget: return isSneaking ? &stavesIllusionFireAndForgetSneak : &stavesIllusionFireAndForget;
            case CastType::Ritual: return isSneaking ? &stavesIllusionRitualSneak : &stavesIllusionRitual;
            default: break;
            }
            break;
        case MagicSchool::Restoration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &stavesRestorationConcentrationSneak : &stavesRestorationConcentration;
            case CastType::FireAndForget: return isSneaking ? &stavesRestorationFireAndForgetSneak : &stavesRestorationFireAndForget;
            case CastType::Ritual: return isSneaking ? &stavesRestorationRitualSneak : &stavesRestorationRitual;
            default: break;
            }
            break;
        default:
            break;
        }
        return isSneaking ? &weaponsStavesSneak : &weaponsStaves;
    }

    CameraProfile& SettingsManager::GetTLProfileBySlot(TLSlot s)
    {
        switch (s) {
#define DDC_X(name, key, field) case TLSlot::name: return field;
            DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X
            default: break;
        }
        static CameraProfile dummy{};
        return dummy;
    }

    std::optional<SettingsManager::TLSlot> SettingsManager::SlotFromTLProfile(CameraProfile* p)
    {
        if (!p) return std::nullopt;
#define DDC_X(name, key, field) if (p == &field) return TLSlot::name;
        DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X
        return std::nullopt;
    }

    CameraProfile* SettingsManager::TLProfileFromSlot(TLSlot a_slot)
    {
        switch (a_slot) {
#define DDC_X(name, key, field) case TLSlot::name: return &field;
            DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X
            default: return nullptr;
        }
    }

    std::optional<SettingsManager::TLSlot> SettingsManager::TLSlotForBinding(
        const WeaponBinding& a_binding, int a_subStateIdx)
    {
        if (a_subStateIdx < 0) return std::nullopt;
        const auto i = static_cast<std::size_t>(a_subStateIdx);
        switch (a_binding.category) {
        case BindingCategory::Melee: {
            // Order matches GetBindingSubStateName's melee table. 10-14 are the
            // directional power attacks â€” no TLSlot, dedicated storage.
            static constexpr TLSlot k[] = {
                TLSlot::WeaponsMelee,                 TLSlot::WeaponsMeleeSprint,
                TLSlot::WeaponsMeleeSwim,             TLSlot::WeaponsMeleeAttack,
                TLSlot::WeaponsMeleeSneak,            TLSlot::WeaponsMeleePowerAttack,
                TLSlot::WeaponsMeleeSprintAttack,     TLSlot::WeaponsMeleeSprintPowerAttack,
                TLSlot::WeaponsMeleeSneakAttack,      TLSlot::WeaponsMeleeSneakPowerAttack,
            };
            return i < std::size(k) ? std::optional<TLSlot>(k[i]) : std::nullopt;
        }
        case BindingCategory::Bow: {
            static constexpr TLSlot k[] = {
                TLSlot::WeaponsBow,      TLSlot::WeaponsBowSprint, TLSlot::WeaponsBowDraw,
                TLSlot::WeaponsBowSwim,  TLSlot::WeaponsBowSneak,  TLSlot::WeaponsBowSneakDraw,
                TLSlot::WeaponsBowZoom,  TLSlot::WeaponsBowSneakZoom,
            };
            return i < std::size(k) ? std::optional<TLSlot>(k[i]) : std::nullopt;
        }
        case BindingCategory::Crossbow: {
            static constexpr TLSlot k[] = {
                TLSlot::WeaponsCrossbow,     TLSlot::WeaponsCrossbowSprint,
                TLSlot::WeaponsCrossbowDraw, TLSlot::WeaponsCrossbowSwim,
                TLSlot::WeaponsCrossbowSneak, TLSlot::WeaponsCrossbowSneakDraw,
                TLSlot::WeaponsCrossbowZoom, TLSlot::WeaponsCrossbowSneakZoom,
            };
            return i < std::size(k) ? std::optional<TLSlot>(k[i]) : std::nullopt;
        }
        case BindingCategory::Spell: {
            // 0 Unsheathed, 1 Sprinting, 2 Swimming, 3 Casting, 4 Sneaking.
            switch (i) {
            case 0: return TLSlot::WeaponsMagic;
            case 1: return TLSlot::WeaponsMagicSprint;
            case 2: return TLSlot::WeaponsMagicSwim;
            case 4: return TLSlot::WeaponsMagicSneak;
            case 3: break;   // resolved from the spell below
            default: return std::nullopt;
            }
            // Casting resolves to the school x cast-type cell the spell would
            // otherwise route to. The bind names an exact spell, so the school
            // is knowable here; the cast type was captured at bind time. The
            // NON-sneak variant is the one reported â€” a bind's Casting
            // sub-state is a single entry, so it can only stand for one of the
            // pair, and standing is the ordinary case.
            RE::MagicItem* spell = nullptr;
            if (a_binding.formID != 0 && !a_binding.pluginName.empty()) {
                if (auto* dh = RE::TESDataHandler::GetSingleton())
                    spell = dh->LookupForm<RE::SpellItem>(a_binding.formID, a_binding.pluginName);
            }
            if (!spell) return TLSlot::WeaponsMagic;
            const bool conc = a_binding.castType == SpellCastType::Concentration;
            switch (spell->GetAssociatedSkill()) {
            case RE::ActorValue::kAlteration:
                return conc ? TLSlot::MagicAlterationConcentration : TLSlot::MagicAlterationFireAndForget;
            case RE::ActorValue::kConjuration:
                return conc ? TLSlot::MagicConjurationConcentration : TLSlot::MagicConjurationFireAndForget;
            case RE::ActorValue::kDestruction:
                return conc ? TLSlot::MagicDestructionConcentration : TLSlot::MagicDestructionFireAndForget;
            case RE::ActorValue::kIllusion:
                return conc ? TLSlot::MagicIllusionConcentration : TLSlot::MagicIllusionFireAndForget;
            case RE::ActorValue::kRestoration:
                return conc ? TLSlot::MagicRestorationConcentration : TLSlot::MagicRestorationFireAndForget;
            default:
                return TLSlot::WeaponsMagic;
            }
        }
        case BindingCategory::Staff: {
            static constexpr TLSlot k[] = {
                TLSlot::WeaponsStaves,     TLSlot::WeaponsStavesSprint,
                TLSlot::WeaponsStavesSwim, TLSlot::WeaponsStavesSneak,
            };
            return i < std::size(k) ? std::optional<TLSlot>(k[i]) : std::nullopt;
        }
        case BindingCategory::Shield: {
            static constexpr TLSlot k[] = {
                TLSlot::WeaponsBlockingShield, TLSlot::WeaponsBlockingShieldSneak,
                TLSlot::WeaponsBlockingShieldSprint,
            };
            return i < std::size(k) ? std::optional<TLSlot>(k[i]) : std::nullopt;
        }
        case BindingCategory::Shout:
            // A shout's slot is shouts.base.<state>, and which state that is
            // depends on what the player happens to be holding when they shout
            // â€” not on the bind. No stable answer.
            return std::nullopt;
        }
        return std::nullopt;
    }

    const SettingsManager::EnemyFieldOverride* SettingsManager::ResolveEnemyOverride(
        std::size_t enemyIdx, CameraProfile* resolved, std::optional<TLSlot> slot,
        const WeaponBinding* binding, int bindingSlot)
    {
        if (enemyIdx >= kEnemyOverrideEnemies) return nullptr;
        // 1. Specific-weapon binding wins over EVERYTHING when it carries any
        //    enabled override (a bound weapon/spell is the most specific thing
        //    the player can equip). bindingSlot is the matched sub-state from
        //    the picker, independent of whether the binding's base profile is
        //    enabled, so the enemy override fires even with no base camera set.
        if (binding && bindingSlot >= 0 && bindingSlot < (int)kWeaponBindingSubStates) {
            const auto& o = binding->TlEnemyOverridesFor(RuntimeEnv())[enemyIdx][bindingSlot];
            if (EnemyOverrideHasContent(o)) return &o;
            // Directional power-attack slots (10-14) cascade to the binding's
            // base Power Attack slot (5) when the direction has no override of
            // its own â€” mirrors the "Base blankets the directions" convention
            // and the camera profile's own dir->base-PA fallback.
            if (bindingSlot >= 10) {
                const auto& basePA = binding->TlEnemyOverridesFor(RuntimeEnv())[enemyIdx][5];
                if (EnemyOverrideHasContent(basePA)) return &basePA;
            }
            // A matched binding's decision STANDS: it does not cascade to the
            // category slot below it. That has always been the behaviour â€”
            // before the binding path started publishing a TLSlot anchor, the
            // slot lookup below simply had nothing to look up while a bound
            // weapon was equipped. Keeping it explicit means adding the anchor
            // (which exists so the CUSTOM enemy overrides can find their cell)
            // doesn't quietly change what the five built-in categories do.
            return nullptr;
        }
        // 2. Directional power attacks carry their own storage (no TLSlot).
        //    ResolveByEnv is a no-op for TL profiles so this pointer is the
        //    outdoor dir slot when a directional PA is active while locked;
        //    the ENEMY column it selects is env-split (2026-09-07) and picks
        //    off the live environment, not the edit tab.
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            if (resolved == &tlWeaponsMeleePowerAttackDir[d])
                return &TlEnemyOverridesPaDirFor(RuntimeEnv())[enemyIdx][d];
        }
        // 3. Plain TL slot.
        if (slot) {
            const auto slotIdx = static_cast<std::size_t>(*slot);
            if (slotIdx < kTLSlotCount)
                return &EnemyOverridesFor(RuntimeEnv())[enemyIdx][slotIdx];
        }
        return nullptr;
    }

    // Per-enemy transition-speed override splice (2026-08-14). Same contract
    // as a location entry's Transitions popup: gated by the enemy profile's
    // OWN transitionOverride flag, independent of the camera-fields master
    // toggle â€” an enemy can override only how the camera MOVES. Writing into
    // the resolved profile is what CameraController's perEntryTrans reads.
    static void SpliceEnemyTransitionOverride(CameraProfile& dst, const CameraProfile& src)
    {
        if (!src.TransitionAnySet()) return;
        // Splice PER SETTING (2026-08-19): a channel the enemy override does
        // not tick is left exactly as the resolved profile had it, so an enemy
        // can slow only the zoom without also seizing rotation and FOV from
        // whatever the entry (or the global) had already decided.
        if (src.transitionSetRotation)  { dst.transitionSetRotation  = true; dst.transitionRotation  = src.transitionRotation; }
        if (src.transitionSetPitch)     { dst.transitionSetPitch     = true; dst.transitionPitch     = src.transitionPitch; }
        if (src.transitionSetPosition)  { dst.transitionSetPosition  = true; dst.transitionPosition  = src.transitionPosition; }
        if (src.transitionSetZoom)      { dst.transitionSetZoom      = true; dst.transitionZoom      = src.transitionZoom; }
        if (src.transitionSetFOV)       { dst.transitionSetFOV       = true; dst.transitionFOV       = src.transitionFOV; }
        if (src.transitionSetLooseness) { dst.transitionSetLooseness = true; dst.transitionLooseness = src.transitionLooseness; }
        if (src.transitionSetWeight)    { dst.transitionSetWeight    = true; dst.transitionWeight    = src.transitionWeight; }
        // Aim Bias rides the same per-channel splice, which is the whole
        // reason it lives in this block: the resolved profile arrives carrying
        // the ENTRY's value, this overlays the ENEMY's when the enemy sets one,
        // and the lock-aim solve reads the winner off the composed profile.
        // Enemy > entry > global, with no separate resolution path.
        if (src.transitionSetAimBias)  { dst.transitionSetAimBias  = true; dst.transitionAimBias  = src.transitionAimBias; }
        dst.SyncTransitionOverride();
    }

    void SettingsManager::ApplyEnemyOverrideResolved(CameraProfile& dst, std::size_t enemyIdx,
                                                     CameraProfile* resolved, std::optional<TLSlot> slot,
                                                     const WeaponBinding* binding, int bindingSlot)
    {
        const auto* o = ResolveEnemyOverride(enemyIdx, resolved, slot, binding, bindingSlot);
        if (!o) return;
        // Master toggle: the camera fields apply together (like a weapon-type
        // override's profile). Aim bias is handled separately by the lock-aim
        // solve.
        if (o->fieldsEnabled) {
            dst.sideOffset  = o->profile.sideOffset;
            dst.height      = o->profile.height;
            dst.zoom        = o->profile.zoom;
            dst.fov         = o->profile.fov;
            dst.rotation    = o->profile.rotation;
            dst.pitchOffset = o->profile.pitchOffset;
        }
        SpliceEnemyTransitionOverride(dst, o->profile);
    }

    // â”€â”€ Custom (player-bound) enemy overrides â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    int SettingsManager::FindCustomEnemyForActor(RE::Actor* actor) const
    {
        if (!actor || customEnemyOverrides.empty()) return -1;
        // TESForm::GetLocalFormID() dereferences a null source file for dynamic
        // (0xFF...) runtime forms (e.g. a leveled creature's generated base),
        // so resolve identity via GetFile(0) and treat file-less forms as
        // unbindable by exact form (plugin stays empty -> never matches).
        auto identity = [](RE::TESForm* f, std::uint32_t& localID, std::string& plugin) {
            localID = 0; plugin.clear();
            if (!f) return;
            if (auto* file = f->GetFile(0)) { localID = f->GetLocalFormID(); plugin = file->fileName; }
        };
        auto lower = [](std::string str) {
            for (auto& ch : str) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return str;
        };
        RE::TESRace* race = actor->GetRace();
        std::uint32_t raceLocal = 0, npcLocal = 0;
        std::string   racePlugin, npcPlugin;
        identity(race,                  raceLocal, racePlugin);
        identity(actor->GetActorBase(), npcLocal,  npcPlugin);
        std::string raceEdid;
        if (race) { if (const char* e = race->GetFormEditorID()) raceEdid = lower(e); }
        const char* actorName = actor->GetName();

        // Pick the most specific match: NPC(5) > Name(4) > Race(3) > Faction(2) >
        // RaceFamily(1) > Keyword(0). Scan all binds and keep the highest-priority
        // hit.
        int best = -1, bestPrio = -1;
        for (std::size_t i = 0; i < customEnemyOverrides.size(); ++i) {
            const auto& c = customEnemyOverrides[i];
            int prio = -1;
            switch (c.matchType) {
            case CustomEnemyOverride::MatchType::NPC:
                if (!npcPlugin.empty() && c.formID == npcLocal && c.pluginName == npcPlugin) prio = 5;
                break;
            case CustomEnemyOverride::MatchType::Name:
                if (!c.matchKey.empty() && actorName && c.matchKey == actorName) prio = 4;
                break;
            case CustomEnemyOverride::MatchType::Race:
                if (!racePlugin.empty() && c.formID == raceLocal && c.pluginName == racePlugin) prio = 3;
                break;
            case CustomEnemyOverride::MatchType::Faction:
                if (!c.pluginName.empty()) {
                    if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                        if (auto* fac = dh->LookupForm<RE::TESFaction>(c.formID, c.pluginName);
                            fac && actor->IsInFaction(fac)) prio = 2;
                    }
                }
                break;
            case CustomEnemyOverride::MatchType::RaceFamily:
                if (!raceEdid.empty() && !c.matchKey.empty() &&
                    raceEdid.find(lower(c.matchKey)) != std::string::npos) prio = 1;
                break;
            case CustomEnemyOverride::MatchType::Keyword:
                if (!c.matchKey.empty()) {
                    if (auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(c.matchKey);
                        kw && actor->HasKeyword(kw)) prio = 0;
                }
                break;
            }
            if (prio > bestPrio) { bestPrio = prio; best = static_cast<int>(i); }
        }
        return best;
    }

    std::string SettingsManager::FpBindingLocationKey(const WeaponBinding& a_binding,
                                                      int a_subStateIdx)
    {
        return ItemBindings::LocationKey(a_binding.bindingScope,
            {a_binding.pluginName, a_binding.formID}, a_binding.enchantmentKey, a_subStateIdx);
    }

    std::string SettingsManager::BindingCamLocationKey(const WeaponBinding& a_binding,
                                                       int a_subStateIdx, bool a_targetLock)
    {
        return FpBindingLocationKey(a_binding, a_subStateIdx) +
               (a_targetLock ? "|tl" : "|cat");
    }

    std::string SettingsManager::BindingNoiseLocationKey(const WeaponBinding& a_binding,
                                                         int a_subStateIdx)
    {
        return FpBindingLocationKey(a_binding, a_subStateIdx) + "|noise";
    }

    CameraProfile* SettingsManager::ActiveLocationBindingCam(const std::string& a_key)
    {
        if (!locationOverridesEnabled || a_key.empty()) return nullptr;
        for (int idx : activeLocationChain) {
            if (idx < 0 || idx >= static_cast<int>(locationOverrides.size())) continue;
            auto& lo = locationOverrides[static_cast<std::size_t>(idx)];
            if (!lo.enabled) continue;
            const auto it = lo.bindingCam.find(a_key);
            if (it != lo.bindingCam.end()) return &it->second;
        }
        return nullptr;
    }

    const std::string* SettingsManager::CurrentCustomMeleeKeyword() const
    {
        if (customWeaponTypes.empty()) return nullptr;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        for (const auto& cwt : customWeaponTypes) {
            if (PlayerMeleeWeaponHasKeyword(player, cwt.keyword.c_str()))
                return &cwt.keyword;
        }
        return nullptr;
    }

    std::string SettingsManager::CustomEnemyIdentityKey(const CustomEnemyOverride& c)
    {
        // Match the dedupe rule the bind UI uses: Name/Keyword/RaceFamily
        // binds are identified by their key string, everything else by
        // (plugin, local form id).
        const auto mt = static_cast<int>(c.matchType);
        const bool keyed = c.matchType == CustomEnemyOverride::MatchType::Name ||
                           c.matchType == CustomEnemyOverride::MatchType::Keyword ||
                           c.matchType == CustomEnemyOverride::MatchType::RaceFamily;
        char buf[32];
        if (keyed) {
            std::snprintf(buf, sizeof(buf), "%d|", mt);
            return std::string(buf) + c.matchKey;
        }
        std::snprintf(buf, sizeof(buf), "%d|%08X|", mt, c.formID);
        return std::string(buf) + c.pluginName;
    }

    const SettingsManager::EnemyFieldOverride* SettingsManager::ResolveCustomEnemyOverride(
        int customIdx, const CameraProfile* resolved, std::optional<TLSlot> slot,
        const WeaponBinding* binding, int bindingSlot) const
    {
        if (customIdx < 0 || customIdx >= static_cast<int>(customEnemyOverrides.size())) return nullptr;
        const auto& c = customEnemyOverrides[static_cast<std::size_t>(customIdx)];
        // An active weapon binding owns its custom-enemy overrides outright:
        // a fresh bind starts with NONE and never inherits the slot-keyed
        // cells tuned for its parent category (user ruling 2026-08-15). Same
        // no-cascade contract as the built-in categories' binding branch.
        if (binding && bindingSlot >= 0 && bindingSlot < (int)kWeaponBindingSubStates) {
            const std::string key = CustomEnemyIdentityKey(c);
            const int benv = RuntimeEnv();
            for (const auto& g : binding->customEnemyGrids) {
                if (g.identityKey != key) continue;
                const auto& arr = g.SlotsFor(benv);
                const auto& o = arr[static_cast<std::size_t>(bindingSlot)];
                if (EnemyOverrideHasContent(o)) return &o;
                // Melee directional PA slots (10-14) cascade to the binding's
                // base Power Attack slot (5), mirroring the built-in grid.
                if (bindingSlot >= 10) {
                    const auto& basePA = arr[5];
                    if (EnemyOverrideHasContent(basePA)) return &basePA;
                }
                return nullptr;
            }
            return nullptr;
        }
        // Custom enemy overrides are env-split: use the twin matching the
        // live environment (interior / outdoor).
        const int env = RuntimeEnv();
        // Directional power attacks carry their own storage (no TLSlot).
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            if (resolved == &tlWeaponsMeleePowerAttackDir[d])
                return &c.PaDirFor(env)[d];
        }
        if (slot) {
            const auto slotIdx = static_cast<std::size_t>(*slot);
            if (slotIdx < kTLSlotCount)
                return &c.SlotsFor(env)[slotIdx];
        }
        return nullptr;
    }

    void SettingsManager::ApplyCustomEnemyOverrideResolved(
        CameraProfile& dst, int customIdx, const CameraProfile* resolved, std::optional<TLSlot> slot,
        const WeaponBinding* binding, int bindingSlot) const
    {
        const auto* o = ResolveCustomEnemyOverride(customIdx, resolved, slot, binding, bindingSlot);
        if (!o) return;
        if (o->fieldsEnabled) {
            dst.sideOffset  = o->profile.sideOffset;
            dst.height      = o->profile.height;
            dst.zoom        = o->profile.zoom;
            dst.fov         = o->profile.fov;
            dst.rotation    = o->profile.rotation;
            dst.pitchOffset = o->profile.pitchOffset;
        }
        SpliceEnemyTransitionOverride(dst, o->profile);
    }

    CameraProfile* SettingsManager::PickTLMagicProfile(MagicSchool school, CastType castType, bool isSneaking,
                                                       bool a_ignoreHandOverride)
    {
        if (school == MagicSchool::None || castType == CastType::None) {
            return isSneaking ? &tlWeaponsMagicSneak : &tlWeaponsMagic;
        }
        // Per-hand override splice against the TL grid â€” see PickMagicProfile.
        // Callers that need the slot this hand profile hangs off (enemy-override
        // keying) pass a_ignoreHandOverride and get the un-spliced pointer.
        if (auto* hs = a_ignoreHandOverride
                ? nullptr
                : GetMagicHandSet(school, castType, isSneaking, /*a_targetLock=*/true)) {
            const int h = static_cast<int>(StateResolver::GetSingleton().GetCastingHand());
            if (h >= 0 && h < static_cast<int>(kMagicHandCount) && hs->enabled[h]) {
                return &hs->profiles[h];
            }
        }
        switch (school) {
        case MagicSchool::Alteration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlMagicAlterationConcentrationSneak : &tlMagicAlterationConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlMagicAlterationFireAndForgetSneak : &tlMagicAlterationFireAndForget;
            case CastType::Ritual:        return isSneaking ? &tlMagicAlterationRitualSneak        : &tlMagicAlterationRitual;
            default: break;
            }
            break;
        case MagicSchool::Conjuration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlMagicConjurationConcentrationSneak : &tlMagicConjurationConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlMagicConjurationFireAndForgetSneak : &tlMagicConjurationFireAndForget;
            case CastType::Ritual:        return isSneaking ? &tlMagicConjurationRitualSneak        : &tlMagicConjurationRitual;
            default: break;
            }
            break;
        case MagicSchool::Destruction:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlMagicDestructionConcentrationSneak : &tlMagicDestructionConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlMagicDestructionFireAndForgetSneak : &tlMagicDestructionFireAndForget;
            case CastType::Ritual:        return isSneaking ? &tlMagicDestructionRitualSneak        : &tlMagicDestructionRitual;
            default: break;
            }
            break;
        case MagicSchool::Illusion:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlMagicIllusionConcentrationSneak : &tlMagicIllusionConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlMagicIllusionFireAndForgetSneak : &tlMagicIllusionFireAndForget;
            case CastType::Ritual:        return isSneaking ? &tlMagicIllusionRitualSneak        : &tlMagicIllusionRitual;
            default: break;
            }
            break;
        case MagicSchool::Restoration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlMagicRestorationConcentrationSneak : &tlMagicRestorationConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlMagicRestorationFireAndForgetSneak : &tlMagicRestorationFireAndForget;
            case CastType::Ritual:        return isSneaking ? &tlMagicRestorationRitualSneak        : &tlMagicRestorationRitual;
            default: break;
            }
            break;
        default:
            break;
        }
        return isSneaking ? &tlWeaponsMagicSneak : &tlWeaponsMagic;
    }

    CameraProfile* SettingsManager::PickTLStavesProfile(MagicSchool school, CastType castType, bool isSneaking)
    {
        if (school == MagicSchool::None || castType == CastType::None) {
            return isSneaking ? &tlWeaponsStavesSneak : &tlWeaponsStaves;
        }
        switch (school) {
        case MagicSchool::Alteration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlStavesAlterationConcentrationSneak : &tlStavesAlterationConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlStavesAlterationFireAndForgetSneak : &tlStavesAlterationFireAndForget;
            case CastType::Ritual: return isSneaking ? &tlStavesAlterationRitualSneak : &tlStavesAlterationRitual;
            default: break;
            }
            break;
        case MagicSchool::Conjuration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlStavesConjurationConcentrationSneak : &tlStavesConjurationConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlStavesConjurationFireAndForgetSneak : &tlStavesConjurationFireAndForget;
            case CastType::Ritual: return isSneaking ? &tlStavesConjurationRitualSneak : &tlStavesConjurationRitual;
            default: break;
            }
            break;
        case MagicSchool::Destruction:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlStavesDestructionConcentrationSneak : &tlStavesDestructionConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlStavesDestructionFireAndForgetSneak : &tlStavesDestructionFireAndForget;
            case CastType::Ritual: return isSneaking ? &tlStavesDestructionRitualSneak : &tlStavesDestructionRitual;
            default: break;
            }
            break;
        case MagicSchool::Illusion:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlStavesIllusionConcentrationSneak : &tlStavesIllusionConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlStavesIllusionFireAndForgetSneak : &tlStavesIllusionFireAndForget;
            case CastType::Ritual: return isSneaking ? &tlStavesIllusionRitualSneak : &tlStavesIllusionRitual;
            default: break;
            }
            break;
        case MagicSchool::Restoration:
            switch (castType) {
            case CastType::Concentration: return isSneaking ? &tlStavesRestorationConcentrationSneak : &tlStavesRestorationConcentration;
            case CastType::FireAndForget: return isSneaking ? &tlStavesRestorationFireAndForgetSneak : &tlStavesRestorationFireAndForget;
            case CastType::Ritual: return isSneaking ? &tlStavesRestorationRitualSneak : &tlStavesRestorationRitual;
            default: break;
            }
            break;
        default:
            break;
        }
        return isSneaking ? &tlWeaponsStavesSneak : &tlWeaponsStaves;
    }

    std::vector<CameraProfile*> SettingsManager::GetCategoryProfiles()
    {
        std::vector<CameraProfile*> out = {
            &sheathed, &sheathedSprint, &sheathedSwim, &sheathedSneak,
            &weaponsMelee, &weaponsMeleeSprint, &weaponsMeleeSwim, &weaponsMeleeAttack, &weaponsMeleeSneak,
            &weaponsMeleePowerAttack, &weaponsMeleeSneakAttack, &weaponsMeleeSneakPowerAttack,
            &weaponsMeleeSprintAttack, &weaponsMeleeSprintPowerAttack,
            &weaponsBlockingOneHanded, &weaponsBlockingTwoHanded, &weaponsBlockingShield, &weaponsBlockingWard,
            &weaponsBlockingOneHandedSneak, &weaponsBlockingTwoHandedSneak, &weaponsBlockingShieldSneak, &weaponsBlockingWardSneak,
            &weaponsBlockingShieldSprint,
            &weaponsBow, &weaponsBowSprint, &weaponsBowSwim, &weaponsBowDraw, &weaponsBowSneak, &weaponsBowSneakDraw, &weaponsBowZoom, &weaponsBowSneakZoom,
            &weaponsCrossbow, &weaponsCrossbowSprint, &weaponsCrossbowSwim, &weaponsCrossbowDraw, &weaponsCrossbowSneak, &weaponsCrossbowSneakDraw, &weaponsCrossbowZoom, &weaponsCrossbowSneakZoom,
            &weaponsMagic, &weaponsMagicSprint, &weaponsMagicSwim, &weaponsMagicSneak,
            &magicAlterationConcentration, &magicAlterationFireAndForget, &magicAlterationRitual,
            &magicConjurationConcentration, &magicConjurationFireAndForget, &magicConjurationRitual,
            &magicDestructionConcentration, &magicDestructionFireAndForget, &magicDestructionRitual,
            &magicIllusionConcentration, &magicIllusionFireAndForget, &magicIllusionRitual,
            &magicRestorationConcentration, &magicRestorationFireAndForget, &magicRestorationRitual,
            &magicAlterationConcentrationSneak, &magicAlterationFireAndForgetSneak, &magicAlterationRitualSneak,
            &magicConjurationConcentrationSneak, &magicConjurationFireAndForgetSneak, &magicConjurationRitualSneak,
            &magicDestructionConcentrationSneak, &magicDestructionFireAndForgetSneak, &magicDestructionRitualSneak,
            &magicIllusionConcentrationSneak, &magicIllusionFireAndForgetSneak, &magicIllusionRitualSneak,
            &magicRestorationConcentrationSneak, &magicRestorationFireAndForgetSneak, &magicRestorationRitualSneak,
            &weaponsStaves, &weaponsStavesSprint, &weaponsStavesSwim, &weaponsStavesSneak,
            &stavesAlterationConcentration, &stavesAlterationFireAndForget, &stavesAlterationRitual,
            &stavesConjurationConcentration, &stavesConjurationFireAndForget, &stavesConjurationRitual,
            &stavesDestructionConcentration, &stavesDestructionFireAndForget, &stavesDestructionRitual,
            &stavesIllusionConcentration, &stavesIllusionFireAndForget, &stavesIllusionRitual,
            &stavesRestorationConcentration, &stavesRestorationFireAndForget, &stavesRestorationRitual,
            &stavesAlterationConcentrationSneak, &stavesAlterationFireAndForgetSneak, &stavesAlterationRitualSneak,
            &stavesConjurationConcentrationSneak, &stavesConjurationFireAndForgetSneak, &stavesConjurationRitualSneak,
            &stavesDestructionConcentrationSneak, &stavesDestructionFireAndForgetSneak, &stavesDestructionRitualSneak,
            &stavesIllusionConcentrationSneak, &stavesIllusionFireAndForgetSneak, &stavesIllusionRitualSneak,
            &stavesRestorationConcentrationSneak, &stavesRestorationFireAndForgetSneak, &stavesRestorationRitualSneak,
            &transformationsWerewolf, &transformationsWerewolfSheathed, &transformationsWerewolfSprint, &transformationsWerewolfSwim,
            &transformationsWerewolfAttack, &transformationsWerewolfPowerAttack, &transformationsWerewolfSprintPowerAttack, &transformationsWerewolfRoar,
            &transformationsWerewolfFeeding,
            &vampireLordSheathed, &vampireLordSheathedLevitating,
            &vampireLordMelee, &vampireLordMeleeAttack, &vampireLordMeleePowerAttack, &vampireLordMagic,
            &vampireLordConcentration, &vampireLordFireAndForget, &vampireLordSprint, &vampireLordSprintLevitating,
            &mountsHorseback, &mountsHorsebackSprint, &mountsHorsebackSwim, &mountsHorsebackMelee, &mountsHorsebackArchery, &mountsHorsebackArcheryZoom, &mountsHorsebackArcheryDraw, &mountsHorsebackMeleeLeft, &mountsHorsebackMeleeRight, &mountsDragonRiding,
            &mountsDragonRidingPerched, &mountsDragonRidingHovering, &mountsDragonRidingTakeoff,
            &mountsDragonRidingLanding,
            &mountsDragonRidingAttackGrounded, &mountsDragonRidingAttackHovering,
            &mountsDragonRidingAttackFlying,
            &mountsDragonRidingBreathGrounded, &mountsDragonRidingBreathHovering,
            &mountsDragonRidingBreathFlying,
        };
        for (auto& p : shoutsBaseByState)      out.push_back(&p);
        for (auto& p : shoutsBaseByStateSneak) out.push_back(&p);
        for (auto& row : shoutOverrideByState)      for (auto& p : row) out.push_back(&p);
        for (auto& row : shoutOverrideByStateSneak) for (auto& p : row) out.push_back(&p);
        for (auto& p : weaponsMeleePowerAttackDir) out.push_back(&p);
        for (auto& sRow : magicHandOverrides)
            for (auto& cRow : sRow)
                for (auto& set : cRow)
                    for (auto& p : set.profiles) out.push_back(&p);
        return out;
    }

    std::vector<CameraProfile*> SettingsManager::GetTargetLockProfiles()
    {
        std::vector<CameraProfile*> out;
        CameraProfile* tlList[] = {
            &tlSheathed, &tlSheathedSprint, &tlSheathedSwim, &tlSheathedSneak,
            &tlWeaponsMelee, &tlWeaponsMeleeSprint, &tlWeaponsMeleeSwim, &tlWeaponsMeleeAttack, &tlWeaponsMeleeSneak,
            &tlWeaponsMeleePowerAttack, &tlWeaponsMeleeSneakAttack, &tlWeaponsMeleeSneakPowerAttack,
            &tlWeaponsMeleeSprintAttack, &tlWeaponsMeleeSprintPowerAttack,
            &tlWeaponsBlockingOneHanded, &tlWeaponsBlockingTwoHanded, &tlWeaponsBlockingShield, &tlWeaponsBlockingWard,
            &tlWeaponsBlockingOneHandedSneak, &tlWeaponsBlockingTwoHandedSneak, &tlWeaponsBlockingShieldSneak, &tlWeaponsBlockingWardSneak,
            &tlWeaponsBlockingShieldSprint,
            &tlWeaponsBow, &tlWeaponsBowSprint, &tlWeaponsBowSwim, &tlWeaponsBowDraw, &tlWeaponsBowSneak, &tlWeaponsBowSneakDraw, &tlWeaponsBowZoom, &tlWeaponsBowSneakZoom,
            &tlWeaponsCrossbow, &tlWeaponsCrossbowSprint, &tlWeaponsCrossbowSwim, &tlWeaponsCrossbowDraw, &tlWeaponsCrossbowSneak, &tlWeaponsCrossbowSneakDraw, &tlWeaponsCrossbowZoom, &tlWeaponsCrossbowSneakZoom,
            &tlWeaponsMagic, &tlWeaponsMagicSprint, &tlWeaponsMagicSwim, &tlWeaponsMagicSneak,
            &tlMagicAlterationConcentration, &tlMagicAlterationFireAndForget, &tlMagicAlterationRitual,
            &tlMagicConjurationConcentration, &tlMagicConjurationFireAndForget, &tlMagicConjurationRitual,
            &tlMagicDestructionConcentration, &tlMagicDestructionFireAndForget, &tlMagicDestructionRitual,
            &tlMagicIllusionConcentration, &tlMagicIllusionFireAndForget, &tlMagicIllusionRitual,
            &tlMagicRestorationConcentration, &tlMagicRestorationFireAndForget, &tlMagicRestorationRitual,
            &tlMagicAlterationConcentrationSneak, &tlMagicAlterationFireAndForgetSneak, &tlMagicAlterationRitualSneak,
            &tlMagicConjurationConcentrationSneak, &tlMagicConjurationFireAndForgetSneak, &tlMagicConjurationRitualSneak,
            &tlMagicDestructionConcentrationSneak, &tlMagicDestructionFireAndForgetSneak, &tlMagicDestructionRitualSneak,
            &tlMagicIllusionConcentrationSneak, &tlMagicIllusionFireAndForgetSneak, &tlMagicIllusionRitualSneak,
            &tlMagicRestorationConcentrationSneak, &tlMagicRestorationFireAndForgetSneak, &tlMagicRestorationRitualSneak,
            &tlWeaponsStaves, &tlWeaponsStavesSprint, &tlWeaponsStavesSwim, &tlWeaponsStavesSneak,
            &tlStavesAlterationConcentration, &tlStavesAlterationFireAndForget, &tlStavesAlterationRitual,
            &tlStavesConjurationConcentration, &tlStavesConjurationFireAndForget, &tlStavesConjurationRitual,
            &tlStavesDestructionConcentration, &tlStavesDestructionFireAndForget, &tlStavesDestructionRitual,
            &tlStavesIllusionConcentration, &tlStavesIllusionFireAndForget, &tlStavesIllusionRitual,
            &tlStavesRestorationConcentration, &tlStavesRestorationFireAndForget, &tlStavesRestorationRitual,
            &tlStavesAlterationConcentrationSneak, &tlStavesAlterationFireAndForgetSneak, &tlStavesAlterationRitualSneak,
            &tlStavesConjurationConcentrationSneak, &tlStavesConjurationFireAndForgetSneak, &tlStavesConjurationRitualSneak,
            &tlStavesDestructionConcentrationSneak, &tlStavesDestructionFireAndForgetSneak, &tlStavesDestructionRitualSneak,
            &tlStavesIllusionConcentrationSneak, &tlStavesIllusionFireAndForgetSneak, &tlStavesIllusionRitualSneak,
            &tlStavesRestorationConcentrationSneak, &tlStavesRestorationFireAndForgetSneak, &tlStavesRestorationRitualSneak,
            &tlTransformationsWerewolf, &tlTransformationsWerewolfSheathed, &tlTransformationsWerewolfSprint, &tlTransformationsWerewolfSwim,
            &tlTransformationsWerewolfPowerAttack, &tlTransformationsWerewolfSprintPowerAttack, &tlTransformationsWerewolfFeeding,
            &tlVampireLordSheathed, &tlVampireLordMelee, &tlVampireLordMeleeAttack, &tlVampireLordMeleePowerAttack, &tlVampireLordMagic,
            &tlVampireLordConcentration, &tlVampireLordFireAndForget, &tlVampireLordSprint, &tlVampireLordSprintLevitating,
            &tlMountsHorseback, &tlMountsHorsebackSprint, &tlMountsHorsebackSwim, &tlMountsHorsebackMelee, &tlMountsHorsebackArchery, &tlMountsHorsebackArcheryZoom, &tlMountsHorsebackArcheryDraw, &tlMountsHorsebackMeleeLeft, &tlMountsHorsebackMeleeRight,
        };
        for (auto* p : tlList) out.push_back(p);
        for (auto& p : tlShoutsBaseByState)      out.push_back(&p);
        for (auto& p : tlShoutsBaseByStateSneak) out.push_back(&p);
        for (auto& row : tlShoutOverrideByState)      for (auto& p : row) out.push_back(&p);
        for (auto& row : tlShoutOverrideByStateSneak) for (auto& p : row) out.push_back(&p);
        for (auto& p : tlWeaponsMeleePowerAttackDir) out.push_back(&p);
        for (auto& sRow : tlMagicHandOverrides)
            for (auto& cRow : sRow)
                for (auto& set : cRow)
                    for (auto& p : set.profiles) out.push_back(&p);
        return out;
    }

    std::vector<CameraProfile*> SettingsManager::GetAllProfiles()
    {
        std::vector<CameraProfile*> out = GetCategoryProfiles();
        // Not shoulder-framed, so they live here rather than in either half â€”
        // which is exactly why the swap can iterate the halves safely.
        out.push_back(&vanityCamera);
        out.push_back(&dialogueProfile);
        out.push_back(&dialogueFirstPersonProfile);
        const auto tl = GetTargetLockProfiles();
        out.insert(out.end(), tl.begin(), tl.end());
        return out;
    }

    void SettingsManager::SwapCategoriesShoulders()
    {
        // Every profile that frames the camera over a shoulder gets its side
        // offset negated. Both halves of the profile tree, from the SHARED
        // enumeration â€” hand-copying the Categories list here is what left
        // Target Lock out of the swap entirely until 2026-08-17, so the lists
        // live in one place now (GetCategoryProfiles / GetTargetLockProfiles)
        // and a profile added to either is swapped automatically.
        //
        // Dialogue, vanity and first-person are deliberately absent: they are
        // not shoulder-framed, so there is nothing to mirror.
        for (auto* p : GetCategoryProfiles())   p->sideOffset = -p->sideOffset;
        for (auto* p : GetTargetLockProfiles()) p->sideOffset = -p->sideOffset;
        // Aim Bias is deliberately NOT touched here.
        //
        // It looks like it should be â€” it is the one Target Lock value that
        // sounds like it names a side. It isn't. The lock solve is
        //     desiredLockAimYaw = bias * asin(-side / distance)
        // so the DIRECTION of the swing comes entirely from the side offset,
        // which this function has just negated; bias is a pure magnitude
        // multiplied on top of it. The aim therefore already mirrors itself
        // the moment the offsets flip, and negating the bias as well flips it
        // twice â€” back to the shoulder the camera just left.
        //
        // (Tried and reverted 2026-08-10. The sliders keep the widened -1.50
        // range from that attempt: a negative bias is a legitimate framing on
        // its own, pushing the target past centre instead of pulling it in.)
        // Indoor variants are mirror images of the outdoor set in the UI sense.
        // Negate them too so the swap stays consistent across the cell-driven
        // auto-switch (otherwise the player would walk inside and have the
        // shoulder snap back to the un-swapped offset).
        for (auto& ip : indoorProfilesStorage) ip.sideOffset = -ip.sideOffset;
        for (auto& lo : locationOverrides)
            for (auto& lp : lo.profiles) lp.sideOffset = -lp.sideOffset;

        // WEAPON BINDINGS (Specific Weapons) â€” all four grids. A bound weapon
        // substitutes its own profile for the state's, so a binding left
        // un-negated puts the shoulder straight back the moment that weapon is
        // drawn. Both environments, and both the Categories and Target Lock
        // halves, for the same reason the base tree does both.
        for (auto& b : weaponBindings) {
            for (auto& p : b.profiles)         p.sideOffset = -p.sideOffset;
            for (auto& p : b.profilesIndoor)   p.sideOffset = -p.sideOffset;
            for (auto& p : b.tlProfiles)       p.sideOffset = -p.sideOffset;
            for (auto& p : b.tlProfilesIndoor) p.sideOffset = -p.sideOffset;
        }

        // ENEMY OVERRIDES â€” target-lock only, and they SPLICE their side
        // offset onto the resolved profile, so an un-negated one un-swaps the
        // shoulder for exactly the enemies the player cared enough to tune.
        // Negated unconditionally: a disabled entry contributes nothing today,
        // and this way it is already correct if it is enabled later.
        // aimBias is NOT negated, for the reason given above.
        auto swapEnemyGrid = [](auto& a_grid) {
            for (auto& o : a_grid) o.profile.sideOffset = -o.profile.sideOffset;
        };
        for (auto& row : enemyOverrides)                  swapEnemyGrid(row);
        for (auto& row : enemyOverridesIndoor)            swapEnemyGrid(row);
        for (auto& row : tlEnemyOverridesPowerAttackDir)  swapEnemyGrid(row);
        for (auto& row : tlEnemyOverridesPowerAttackDirIndoor) swapEnemyGrid(row);
        for (auto& ce : customEnemyOverrides) {
            swapEnemyGrid(ce.slots);       swapEnemyGrid(ce.slotsIndoor);
            swapEnemyGrid(ce.paDir);       swapEnemyGrid(ce.paDirIndoor);
        }
        for (auto& b : weaponBindings) {
            for (auto& row : b.tlEnemyOverrides)       swapEnemyGrid(row);
            for (auto& row : b.tlEnemyOverridesIndoor) swapEnemyGrid(row);
            for (auto& g : b.customEnemyGrids) {
                swapEnemyGrid(g.slots);
                swapEnemyGrid(g.slotsIndoor);
            }
        }

        spdlog::info("Shoulder swap fired (Categories + Target Lock + bindings + enemy overrides)");
    }

    // ============================================================
    // Indoor / Outdoor variant infrastructure
    // ============================================================

    // Base state profile for a (binding category, sub-state slot) pair â€” the
    // reverse of pickBinding's slot mapping. Lets a per-form binding's camera
    // profile redirect to its underlying state so noise resolves normally.
    // Returns nullptr for slots with no plain base (e.g. the spell "casting"
    // slot), which then fall back to global noise.
    static CameraProfile* BindingBaseProfile(SettingsManager& s,
                                             SettingsManager::BindingCategory cat, int slot)
    {
        using BC = SettingsManager::BindingCategory;
        switch (cat) {
        case BC::Melee:
            switch (slot) {
            case 0:  return &s.weaponsMelee;                  case 1:  return &s.weaponsMeleeSprint;
            case 2:  return &s.weaponsMeleeSwim;              case 3:  return &s.weaponsMeleeAttack;
            case 4:  return &s.weaponsMeleeSneak;             case 5:  return &s.weaponsMeleePowerAttack;
            case 6:  return &s.weaponsMeleeSprintAttack;      case 7:  return &s.weaponsMeleeSprintPowerAttack;
            case 8:  return &s.weaponsMeleeSneakAttack;       case 9:  return &s.weaponsMeleeSneakPowerAttack;
            case 10: return &s.weaponsMeleePowerAttackDir[0]; case 11: return &s.weaponsMeleePowerAttackDir[1];
            case 12: return &s.weaponsMeleePowerAttackDir[2]; case 13: return &s.weaponsMeleePowerAttackDir[3];
            case 14: return &s.weaponsMeleePowerAttackDir[4]; default: break; } break;
        case BC::Bow:
            switch (slot) { case 0: return &s.weaponsBow;       case 1: return &s.weaponsBowSprint;
                            case 2: return &s.weaponsBowDraw;   case 3: return &s.weaponsBowSwim;
                            case 4: return &s.weaponsBowSneak;  case 5: return &s.weaponsBowSneakDraw;
                            case 6: return &s.weaponsBowZoom;   case 7: return &s.weaponsBowSneakZoom;
                            default: break; } break;
        case BC::Crossbow:
            switch (slot) { case 0: return &s.weaponsCrossbow;       case 1: return &s.weaponsCrossbowSprint;
                            case 2: return &s.weaponsCrossbowDraw;   case 3: return &s.weaponsCrossbowSwim;
                            case 4: return &s.weaponsCrossbowSneak;  case 5: return &s.weaponsCrossbowSneakDraw;
                            case 6: return &s.weaponsCrossbowZoom;   case 7: return &s.weaponsCrossbowSneakZoom;
                            default: break; } break;
        case BC::Spell:
            switch (slot) { case 0: return &s.weaponsMagic;      case 1: return &s.weaponsMagicSprint;
                            case 2: return &s.weaponsMagicSwim;  case 4: return &s.weaponsMagicSneak;
                            default: break; } break;
        case BC::Staff:
            switch (slot) { case 0: return &s.weaponsStaves;      case 1: return &s.weaponsStavesSprint;
                            case 2: return &s.weaponsStavesSwim;  case 3: return &s.weaponsStavesSneak;
                            default: break; } break;
        case BC::Shield:
            switch (slot) { case 0: return &s.weaponsBlockingShield;       case 1: return &s.weaponsBlockingShieldSneak;
                            case 2: return &s.weaponsBlockingShieldSprint; default: break; } break;
        case BC::Shout:
            // A shout's base depends on the live weapon state
            // (shoutsBaseByState) â€” no fixed profile to redirect to. Both
            // Shout slots are always-active anyway, so this path is
            // unreachable for them.
            break;
        }
        return nullptr;
    }

    void SettingsManager::SeedWeaponBinding(WeaponBinding& binding, const ItemBindings::EquippedItem& item)
    {
        using Scope = ItemBindings::Scope;
        WeaponBinding* parent = nullptr;
        for (auto& candidate : weaponBindings) {
            if (candidate.category != binding.category || candidate.fpOnly != binding.fpOnly ||
                !ItemBindings::SameForm({candidate.pluginName, candidate.formID}, item.base)) continue;
            if (candidate.bindingScope == Scope::BaseItem) { parent = &candidate; break; }
            if (candidate.bindingScope == Scope::ExactForm) parent = &candidate;
        }
        if (parent) {
            const auto formID = binding.formID;
            const auto pluginName = binding.pluginName;
            const auto displayName = binding.displayName;
            const auto scope = binding.bindingScope;
            const auto enchantmentKey = binding.enchantmentKey;
            binding = *parent;
            binding.formID = formID;
            binding.pluginName = pluginName;
            binding.displayName = displayName;
            binding.bindingScope = scope;
            binding.enchantmentKey = enchantmentKey;
            // Location rules are independent snapshots too. Copy by the saved
            // identity, never vector index; existing destination tuning wins.
            const auto copyKey = [](auto& map, const std::string& from, const std::string& to) {
                if (from == to) return;
                if (auto it = map.find(from); it != map.end()) {
                    const auto value = it->second;
                    map.try_emplace(to, value);
                }
            };
            for (auto& location : locationOverrides) {
                for (int slot = 0; slot < static_cast<int>(GetBindingSubStateCount(binding.category)); ++slot) {
                    if (binding.fpOnly) {
                        copyKey(location.fpState, FpBindingLocationKey(*parent, slot), FpBindingLocationKey(binding, slot));
                    } else {
                        for (bool tl : {false, true})
                            copyKey(location.bindingCam, BindingCamLocationKey(*parent, slot, tl), BindingCamLocationKey(binding, slot, tl));
                        copyKey(location.stateNoise, BindingNoiseLocationKey(*parent, slot), BindingNoiseLocationKey(binding, slot));
                    }
                }
            }
            return;
        }

        // Seed equipment from the category and applicable weapon-type values.
        // Each environment is read explicitly; opening the menu indoors cannot
        // accidentally copy indoor tuning into the outdoor slots.
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data && item.form.Valid() ? data->LookupForm(item.form.localID, item.form.plugin) : nullptr;
        const auto type = ClassifyMeleeWeaponForm(form);
        const auto wi = static_cast<std::size_t>(type);
        const char* keyword = nullptr;
        if (auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr) {
            for (const auto& custom : customWeaponTypes)
                if (weapon->HasKeywordString(custom.keyword)) { keyword = custom.keyword.c_str(); break; }
        }
        MeleeWeaponOverrides* cameraOverrides[] = {
            &weaponsMeleeOverrides, &weaponsMeleeSprintOverrides, &weaponsMeleeSwimOverrides,
            &weaponsMeleeAttackOverrides, &weaponsMeleeSneakOverrides, &weaponsMeleePowerAttackOverrides,
            &weaponsMeleeSprintAttackOverrides, &weaponsMeleeSprintPowerAttackOverrides,
            &weaponsMeleeSneakAttackOverrides, &weaponsMeleeSneakPowerAttackOverrides };
        MeleeWeaponOverrides* targetOverrides[] = {
            &tlWeaponsMeleeOverrides, &tlWeaponsMeleeSprintOverrides, &tlWeaponsMeleeSwimOverrides,
            &tlWeaponsMeleeAttackOverrides, &tlWeaponsMeleeSneakOverrides, &tlWeaponsMeleePowerAttackOverrides,
            &tlWeaponsMeleeSprintAttackOverrides, &tlWeaponsMeleeSprintPowerAttackOverrides,
            &tlWeaponsMeleeSneakAttackOverrides, &tlWeaponsMeleeSneakPowerAttackOverrides };
        MeleeWeaponNoiseOverrides* noiseOverrides[] = {
            &weaponsMeleeNoiseOverrides, &weaponsMeleeSprintNoiseOverrides, &weaponsMeleeSwimNoiseOverrides,
            &weaponsMeleeAttackNoiseOverrides, &weaponsMeleeSneakNoiseOverrides, &weaponsMeleePowerAttackNoiseOverrides,
            &weaponsMeleeSprintAttackNoiseOverrides, &weaponsMeleeSprintPowerAttackNoiseOverrides,
            &weaponsMeleeSneakAttackNoiseOverrides, &weaponsMeleeSneakPowerAttackNoiseOverrides };
        const auto eligible = GetIndoorEligibleProfiles();
        const int count = static_cast<int>(GetBindingSubStateCount(binding.category));
        for (int slot = 0; slot < count; ++slot) {
            auto* base = BindingBaseProfile(*this, binding.category, slot);
            if (!base) continue;
            std::string key;
            for (const auto& entry : eligible) if (entry.outdoor == base) { key = entry.tomlKey; break; }
            const bool melee = binding.category == BindingCategory::Melee;
            auto* camOv = melee ? (slot < 10 ? cameraOverrides[slot] : &weaponsMeleePowerAttackDirOverrides[slot - 10]) : nullptr;
            auto* tlOv = melee ? (slot < 10 ? targetOverrides[slot] : &tlWeaponsMeleePowerAttackDirOverrides[slot - 10]) : nullptr;
            auto* noiseOv = melee ? (slot < 10 ? noiseOverrides[slot] : &weaponsMeleePowerAttackDirNoiseOverrides[slot - 10]) : nullptr;
            if (melee && slot >= 10 && !binding.fpOnly) {
                binding.enabled[slot] = weaponsMeleePowerAttackDirEnabled[slot - 10];
                binding.tlEnabled[slot] = tlWeaponsMeleePowerAttackDirEnabled[slot - 10];
                bool noiseSet = noiseOv && wi < kMeleeWeaponCount && noiseOv->perWeaponSet[wi];
                for (int env = 0; env < kEnvCount; ++env) {
                    auto it = StateNoiseFor(env).find(key);
                    noiseSet = noiseSet || (it != StateNoiseFor(env).end() && it->second.enabled);
                }
                binding.noiseEnabled[slot] = noiseSet;
            }
            CameraProfile* tlBase = nullptr;
            if (melee && slot >= 10) tlBase = &tlWeaponsMeleePowerAttackDir[slot - 10];
            else if (auto tlSlot = TLSlotForBinding(binding, slot)) tlBase = TLProfileFromSlot(*tlSlot);
            for (int env = 0; env < kEnvCount; ++env) {
                const auto pickCamera = [&](CameraProfile* root, MeleeWeaponOverrides* overrides) {
                    if (!root) return BindingDefault(binding.category);
                    auto* source = overrides ? const_cast<CameraProfile*>(ResolveMeleeOverride(
                        *root, *overrides, type, keyword, env == kEnvIndoor)) : root;
                    return *VariantOf(source, env);
                };
                binding.ProfilesFor(env)[slot] = pickCamera(base, camOv);
                binding.TlProfilesFor(env)[slot] = pickCamera(tlBase, tlOv);
                auto noise = GlobalNoiseFor(env);
                if (auto it = stateNoise.find(key); it != stateNoise.end() && it->second.enabled) noise = it->second;
                if (auto it = StateNoiseFor(env).find(key); it != StateNoiseFor(env).end() && it->second.enabled) noise = it->second;
                if (noiseOv && wi < kMeleeWeaponCount && noiseOv->perWeaponSet[wi]) noise = noiseOv->perWeapon[wi];
                binding.NoiseProfilesFor(env)[slot] = noise;
            }
            auto fp = firstPersonGlobal;
            if (auto it = stateFirstPerson.find(key); it != stateFirstPerson.end()) fp = it->second;
            if (melee) if (auto it = stateFpMeleeOverrides.find(key); it != stateFpMeleeOverrides.end()) {
                const auto pickHalf = [&](bool fov) -> const FirstPersonProfile* {
                    if (keyword) for (const auto& custom : it->second.custom)
                        if (custom.keyword == keyword && (fov ? custom.setFov : custom.setNoise)) return &custom.profile;
                    if (wi < kMeleeWeaponCount && (fov ? it->second.perWeaponSetFov[wi] : it->second.perWeaponSetNoise[wi]))
                        return &it->second.perWeapon[wi];
                    return nullptr;
                };
                if (auto* source = pickHalf(true)) {
                    fp.worldFov = source->worldFov; fp.handsFov = source->handsFov; fp.transitionSpeed = source->transitionSpeed;
                }
                if (auto* source = pickHalf(false)) {
                    fp.noise = source->noise; fp.repulse = source->repulse; fp.repulseFeel = source->repulseFeel;
                    fp.fofFadeDuration = source->fofFadeDuration; fp.shoutFadeDuration = source->shoutFadeDuration;
                }
            }
            binding.fpProfiles[slot] = fp;
        }
        binding.settingsSeeded = true;
    }

    bool SettingsManager::FindBindingSlotForProfile(CameraProfile* a_profile,
                                                    WeaponBinding*& a_outBinding,
                                                    int& a_outSlot)
    {
        if (!a_profile) return false;
        for (auto& b : weaponBindings) {
            for (std::size_t sl = 0; sl < kWeaponBindingSubStates; ++sl) {
                // Every environment's copy maps back to the same (binding,
                // slot) coordinate â€” a lookup that covered only two of the
                // three would silently go dark in the missing one.
                bool hit = false;
                for (int e = 0; e < kEnvCount && !hit; ++e) {
                    hit = (&b.ProfilesFor(e)[sl]   == a_profile ||
                           &b.TlProfilesFor(e)[sl] == a_profile);
                    // Per-hand binding profiles belong to the same (binding,
                    // slot) coordinate as their base slot.
                    for (std::size_t h = 0; h < kMagicHandCount && !hit; ++h) {
                        hit = (&b.HandProfilesFor(e)[h][sl]   == a_profile ||
                               &b.HandTlProfilesFor(e)[h][sl] == a_profile);
                    }
                }
                if (hit) {
                    a_outBinding = &b;
                    a_outSlot    = static_cast<int>(sl);
                    return true;
                }
            }
        }
        return false;
    }

    const SettingsManager::NoiseProfile* SettingsManager::ResolveNpcBindingNoise(
        WeaponBinding* binding, int slot, std::string* outKey)
    {
        if (!binding || binding->fpOnly || slot < 0 || slot >= static_cast<int>(GetBindingSubStateCount(binding->category)))
            return nullptr;
        const auto key = BindingNoiseLocationKey(*binding, slot);
        if (auto* p = ActiveLocationStateNoise(key); p && p->enabled) {
            if (outKey) *outKey = key;
            return p;
        }
        // These two base entries have no enable toggle in Specific Weapons.
        if (slot == 0 || (binding->category == BindingCategory::Melee && slot == 5) || binding->noiseEnabled[slot]) {
            if (outKey) *outKey = key;
            return &binding->NoiseProfilesFor(RuntimeEnv())[slot];
        }
        return nullptr;
    }

    const SettingsManager::NoiseProfile* SettingsManager::FindNpcStateNoise(std::string_view key, std::string* outKey)
    {
        if (key.empty()) return nullptr;
        const std::string name(key);
        if (auto* p = ActiveLocationStateNoise(name); p && p->enabled) {
            if (outKey) *outKey = name;
            return p;
        }
        const auto& environment = StateNoiseFor(RuntimeEnv());
        if (auto it = environment.find(name); it != environment.end() && it->second.enabled) {
            if (outKey) *outKey = name;
            return &it->second;
        }
        if (auto it = stateNoise.find(name); it != stateNoise.end() && it->second.enabled) {
            if (outKey) *outKey = name;
            return &it->second;
        }
        return nullptr;
    }

    bool SettingsManager::IsHitShakeBindingSlot(const WeaponBinding& b, int slot)
    {
        return (b.category == BindingCategory::Melee && HitShake::IsMeleeSlot(slot)) ||
            ((b.category == BindingCategory::Bow || b.category == BindingCategory::Crossbow) && HitShake::IsArcherySlot(slot)) ||
            (b.category == BindingCategory::Spell && slot == 3 && b.castType != SpellCastType::Concentration) ||
            (b.category == BindingCategory::Staff && (slot == 0 || slot == 3));
    }

    bool SettingsManager::IsHitShakeLocationKey(std::string_view key) const
    {
        if (HitShake::IsAttackKey(key)) return true;
        if (!key.starts_with("binding.")) return false;
        for (const auto& b : weaponBindings) {
            for (int slot = 0; slot < static_cast<int>(GetBindingSubStateCount(b.category)); ++slot) {
                if (!IsHitShakeBindingSlot(b, slot)) continue;
                if (key == BindingNoiseLocationKey(b, slot) || key == FpBindingLocationKey(b, slot)) return true;
            }
        }
        return false;
    }

    SettingsManager::NoiseProfile* SettingsManager::ResolveHitShakeProfile(
        std::string_view key, bool firstPerson, MeleeWeaponType weapon,
        const ItemBindings::EquippedItem& item, std::string_view customKeyword, std::string* outKey, bool mountedArchery)
    {
        if (outKey) outKey->clear();
        if (!HitShake::IsAttackKey(key)) return nullptr;
        auto keys = HitShake::Parents(key);
        const int archery = HitShake::ArcheryIndex(key);
        const auto wi = static_cast<std::size_t>(weapon);
        auto result = [&](NoiseProfile* p, std::string_view name) {
            if (p && outKey) *outKey = name;
            return p;
        };
        if (HitShake::IsMagicKey(key)) {
            // Resolve the actual firing item/hand, never the dominant spell
            // selected by the camera state. A zero in an enabled override wins.
            const bool staff = key.starts_with("staves.");
            const bool schoolSpell = key.starts_with("magic.");
            const bool sneak = key.find(".sneak.") != key.npos;
            int hand = -1;
            for (int h = 0; h < static_cast<int>(kMagicHandCount); ++h)
                if (key.ends_with(std::string(".hand.") + GetMagicHandTomlKey(h))) hand = h;
            if (staff || schoolSpell) {
                const auto category = staff ? BindingCategory::Staff : BindingCategory::Spell;
                if (auto* b = ItemBindings::FindBest(weaponBindings, static_cast<int>(category), firstPerson, item)) {
                    for (const int slot : {staff ? (sneak ? 3 : 0) : 3, staff && sneak ? 0 : -1}) {
                        if (slot < 0) continue;
                        const auto name = firstPerson ? FpBindingLocationKey(*b, slot) : BindingNoiseLocationKey(*b, slot);
                        if (firstPerson) {
                            if (auto* p = ActiveLocationFpProfile(name, false)) return result(&p->noise, name);
                            if (b->settingsSeeded || !(b->fpProfiles[slot].noise == NoiseProfile{}))
                                return result(&b->fpProfiles[slot].noise, name);
                        } else {
                            if (auto* p = ActiveLocationStateNoise(name)) return result(p, name);
                            if (!staff && hand >= 0 && b->handNoiseEnabled[hand][slot])
                                return result(&b->HandNoiseFor(RuntimeEnv())[hand][slot], name + ".hand." + GetMagicHandTomlKey(hand));
                            if ((staff && slot == 0) || b->noiseEnabled[slot])
                                return result(&b->NoiseProfilesFor(RuntimeEnv())[slot], name);
                        }
                    }
                }
            }
            std::string base(key);
            if (const auto suffix = base.find(".hand."); suffix != base.npos) base.erase(suffix);
            std::string standing(base);
            if (const auto suffix = standing.find(".sneak."); suffix != standing.npos) standing.erase(suffix, 6);
            const std::string standingHand = hand >= 0 ? standing + ".hand." + GetMagicHandTomlKey(hand) : standing;
            for (const auto& name : {std::string(key), base, standingHand, standing}) {
                if (firstPerson) {
                    if (auto* p = ActiveLocationFpProfile(name, false)) return result(&p->noise, name);
                    if (auto it = stateFirstPerson.find(name); it != stateFirstPerson.end()) return result(&it->second.noise, name);
                } else {
                    if (auto* p = ActiveLocationStateNoise(name)) return result(p, name);
                    auto& env = StateNoiseFor(RuntimeEnv());
                    if (auto it = env.find(name); it != env.end() && it->second.enabled) return result(&it->second, name);
                    if (auto it = stateNoise.find(name); it != stateNoise.end() && it->second.enabled) return result(&it->second, name);
                }
            }
            return nullptr;
        }

        // Use the weapon that delivered the hit, including its enchantment
        // identity. This path never substitutes the other equipped hand.
        if (HitShake::MeleeIndex(key) >= 0 || (archery >= 0 && archery < 8)) {
            const auto category = archery < 0 ? BindingCategory::Melee :
                archery >= 4 ? BindingCategory::Crossbow : BindingCategory::Bow;
            if (auto* b = ItemBindings::FindBest(weaponBindings,
                    static_cast<int>(category), firstPerson, item)) {
                for (const auto candidate : keys) {
                    const int mi = HitShake::MeleeIndex(candidate);
                    const int ai = HitShake::ArcheryIndex(candidate);
                    if (mi < 0 && (ai < 0 || ai >= 8)) continue;
                    const int slot = mi >= 0 ? HitShake::kMeleeSlots[mi] : HitShake::kArcherySlots[ai % 4];
                    const auto locationKey = firstPerson ? FpBindingLocationKey(*b, slot) : BindingNoiseLocationKey(*b, slot);
                    if (firstPerson) {
                        if (auto* lp = ActiveLocationFpProfile(locationKey, false)) return result(&lp->noise, locationKey);
                        auto& p = b->fpProfiles[slot];
                        if (slot >= 10 && !HitShake::HasTuning(p.noise.hitShake)) continue;
                        if (b->settingsSeeded || !(p.noise == NoiseProfile{})) return result(&p.noise, locationKey);
                    } else {
                        if (auto* lp = ActiveLocationStateNoise(locationKey)) return result(lp, locationKey);
                        if ((category == BindingCategory::Melee && slot == 5) || b->noiseEnabled[slot])
                            return result(&b->NoiseProfilesFor(RuntimeEnv())[slot], locationKey);
                    }
                }
            }
        }

        // Specific bows/crossbows keep their item settings while mounted;
        // otherwise the rider's own archery entries supply the impact.
        if (mountedArchery && archery >= 0 && archery < 8)
            keys = HitShake::Parents(HitShake::kArcheryKeys[archery % 4 >= 2 ? 9 : 8]);

        std::array<MeleeWeaponNoiseOverrides*, 11> typeOverrides{
            &weaponsMeleeAttackNoiseOverrides, &weaponsMeleePowerAttackNoiseOverrides,
            &weaponsMeleeSprintAttackNoiseOverrides, &weaponsMeleeSprintPowerAttackNoiseOverrides,
            &weaponsMeleeSneakAttackNoiseOverrides, &weaponsMeleeSneakPowerAttackNoiseOverrides,
            &weaponsMeleePowerAttackDirNoiseOverrides[0], &weaponsMeleePowerAttackDirNoiseOverrides[1],
            &weaponsMeleePowerAttackDirNoiseOverrides[2], &weaponsMeleePowerAttackDirNoiseOverrides[3],
            &weaponsMeleePowerAttackDirNoiseOverrides[4]
        };
        for (const auto candidate : keys) {
            if (candidate.empty()) continue;
            const std::string name(candidate);
            if (firstPerson) {
                if (auto it = stateFpMeleeOverrides.find(name); it != stateFpMeleeOverrides.end()) {
                    if (!customKeyword.empty()) for (auto& custom : it->second.custom) {
                        if (custom.keyword == customKeyword && custom.setNoise) return result(&custom.profile.noise, name);
                    }
                    if (wi < kMeleeWeaponCount && it->second.perWeaponSetNoise[wi])
                        return result(&it->second.perWeapon[wi].noise, name);
                }
                if (auto* lp = ActiveLocationFpProfile(name, false)) return result(&lp->noise, name);
                if (auto it = stateFirstPerson.find(name); it != stateFirstPerson.end())
                    return result(&it->second.noise, name);
            } else {
                const int mi = HitShake::MeleeIndex(candidate);
                if (mi >= 0 && wi < kMeleeWeaponCount && typeOverrides[mi]->perWeaponSet[wi])
                    return result(&typeOverrides[mi]->perWeapon[wi], name);
                if (auto* lp = ActiveLocationStateNoise(name)) return result(lp, name);
                auto& env = StateNoiseFor(RuntimeEnv());
                if (auto it = env.find(name); it != env.end() && it->second.enabled) return result(&it->second, name);
                if (auto it = stateNoise.find(name); it != stateNoise.end() && it->second.enabled) return result(&it->second, name);
            }
        }
        return nullptr; // idle/global noise never supplies an impact
    }

    const SettingsManager::NoiseProfile* SettingsManager::ResolveNpcArcheryNoise(
        bool crossbow, bool sneak, bool mounted, const ItemBindings::EquippedItem& item, std::string* outKey)
    {
        if (outKey) outKey->clear();
        auto* binding = ItemBindings::FindBest(weaponBindings,
            static_cast<int>(crossbow ? BindingCategory::Crossbow : BindingCategory::Bow), false, item);
        for (int slot : {sneak ? 5 : -1, 2, sneak ? 4 : -1, 0})
            if (const auto* p = ResolveNpcBindingNoise(binding, slot, outKey)) return p;
        if (mounted) {
            if (auto* p = FindNpcStateNoise("mounts.horseback.archery.draw", outKey)) return p;
            if (auto* p = FindNpcStateNoise("mounts.horseback.archery", outKey)) return p;
        }
        const std::string base = crossbow ? "weapons.crossbow" : "weapons.bow";
        if (sneak) {
            if (auto* p = FindNpcStateNoise(base + ".sneak.draw", outKey)) return p;
        }
        if (auto* p = FindNpcStateNoise(base + ".draw", outKey)) return p;
        if (sneak) {
            if (auto* p = FindNpcStateNoise(base + ".sneak", outKey)) return p;
        }
        return FindNpcStateNoise(base, outKey);
    }

    const SettingsManager::NoiseProfile* SettingsManager::ResolveNpcShoutNoise(
        std::string_view bucket, std::string_view shoutKey, bool sneak,
        const ItemBindings::EquippedItem& shout, std::string* outKey)
    {
        if (outKey) outKey->clear();
        auto* binding = ItemBindings::FindBest(weaponBindings, static_cast<int>(BindingCategory::Shout), false, shout);
        for (int slot : {sneak ? 1 : -1, 0})
            if (const auto* p = ResolveNpcBindingNoise(binding, slot, outKey)) return p;
        const std::string base = "shouts." + std::string(bucket) + ".";
        if (!shoutKey.empty()) {
            if (sneak) if (auto* p = FindNpcStateNoise(base + std::string(shoutKey) + ".sneak", outKey)) return p;
            if (auto* p = FindNpcStateNoise(base + std::string(shoutKey), outKey)) return p;
        }
        if (sneak) if (auto* p = FindNpcStateNoise(base + "base.sneak", outKey)) return p;
        return FindNpcStateNoise(base + "base", outKey);
    }

    const SettingsManager::NoiseProfile* SettingsManager::ResolveNpcTransformationNoise(
        NpcNoise::Form form, NpcNoise::Action action)
    {
        for (const auto key : NpcNoise::TransformationKeys(form, action))
            if (auto* p = FindNpcStateNoise(key)) return p;
        return nullptr; // Beast forms never inherit generic melee/magic noise.
    }

    const SettingsManager::NoiseProfile* SettingsManager::ResolveNpcMeleeNoise(
        bool power, bool sneak, bool sprint, MeleeWeaponType weapon, int direction,
        const ItemBindings::EquippedItem& item, std::string* outKey)
    {
        if (outKey) outKey->clear();
        auto* binding = ItemBindings::FindBest(weaponBindings, static_cast<int>(BindingCategory::Melee), false, item);
        const int specific = sneak ? (power ? 9 : 8) : sprint ? (power ? 7 : 6) :
            power && direction >= 0 && direction < static_cast<int>(kPowerAttackDirectionCount) ? 10 + direction : -1;
        for (int slot : {specific, power ? 5 : -1, 3})
            if (const auto* p = ResolveNpcBindingNoise(binding, slot, outKey)) return p;
        const auto wi = static_cast<std::size_t>(weapon);
        auto find = [&](const std::string& key, const MeleeWeaponNoiseOverrides& overrides)
            -> const NoiseProfile* {
            // Match the ordinary melee resolver's weapon / location / indoor /
            // outdoor priority, but use the attacker's weapon and stance.
            if (wi < kMeleeWeaponCount && overrides.perWeaponSet[wi]) {
                if (outKey) *outKey = key + ".per_weapon." + MeleeWeaponTypeTomlKey(weapon);
                return &overrides.perWeapon[wi];
            }
            return FindNpcStateNoise(key, outKey);
        };
        if (sneak) {
            const auto* p = power
                ? find("weapons.melee.sneak_power_attack", weaponsMeleeSneakPowerAttackNoiseOverrides)
                : find("weapons.melee.sneak_attack", weaponsMeleeSneakAttackNoiseOverrides);
            if (p) return p;
        } else if (sprint) {
            const auto* p = power
                ? find("weapons.melee.sprint_power_attack", weaponsMeleeSprintPowerAttackNoiseOverrides)
                : find("weapons.melee.sprint_attack", weaponsMeleeSprintAttackNoiseOverrides);
            if (p) return p;
        } else if (power && direction >= 0 && direction < static_cast<int>(kPowerAttackDirectionCount)) {
            if (const auto* p = find(std::string("weapons.melee.power_attack.dir.") + kPADirKeys[direction],
                                    weaponsMeleePowerAttackDirNoiseOverrides[direction])) return p;
        }
        if (power) {
            if (const auto* p = find("weapons.melee.power_attack", weaponsMeleePowerAttackNoiseOverrides)) return p;
        }
        // An enabled zero-amount entry above is an intentional mute. Only an
        // absent/disabled entry inherits; ambient/idle noise never triggers a hit.
        return find("weapons.melee.attack", weaponsMeleeAttackNoiseOverrides);
    }

    SettingsManager::NoiseProfile* SettingsManager::ResolveStateNoise(CameraProfile* a_profile,
                                                                      std::string* a_outKey,
                                                                      int* a_outWeapon,
                                                                      std::string* a_outBindingLabel)
    {
        if (!a_profile) return nullptr;
        if (a_outWeapon) *a_outWeapon = -1;
        // Lazy pointerâ†’tomlKey cache built from GetIndoorEligibleProfiles.
        // Includes BOTH the outdoor profile pointer AND its indoor variant
        // pointer mapped to the same key â€” when the player is in an
        // interior, CameraController routes through ResolveByEnv() and
        // selects the indoor variant, which is a different address than
        // the outdoor profile. Without the indoor entries the lookup misses
        // for every per-state customization while indoors and noise falls
        // back to globalNoise. Both pointers share the key because users
        // configure noise once per state, not once per (state, environment).
        static std::unordered_map<CameraProfile*, std::string> sPtrToKey;
        if (sPtrToKey.empty()) {
            for (auto& e : GetIndoorEligibleProfiles()) {
                // The eligible list now includes the TL slots under
                // "target_lock."-prefixed keys (for the indoor-variant
                // system). Noise is NOT split by target lock â€” strip the
                // prefix so TL pointers (outdoor AND indoor variants) share
                // the Categories noise key. Inserting the prefixed key here
                // used to WIN over the plain-key TL block below (emplace
                // never overwrites), which silently dropped per-state and
                // per-weapon-type noise to global while locked.
                std::string key = e.tomlKey;
                if (key.rfind("target_lock.", 0) == 0) key = key.substr(12);
                // Per-hand magic camera profiles map to their BASE state's
                // noise key (strip the ".hand.<x>" tail): per-hand noise is
                // resolved by the hand-key redirect further down, gated on
                // its own enable â€” so a hand CAMERA override alone doesn't
                // reroute noise away from the state's tuning.
                if (const auto hp = key.find(".hand."); hp != std::string::npos) {
                    key.erase(hp);
                }
                // DRAGON RIDING HAS ONE NOISE CELL, NOT ELEVEN.
                //
                // The camera sub-states are about ANGLE â€” a breath from a
                // hover wants a different framing than a breath on a strafing
                // pass. The SHAKE does not follow that split: the dragon's
                // own noise is what riding one feels like, and the Cinematic
                // Effects dragon sources already fire for your mount (it sits
                // at distance ~0), so per-sub-state cells would only stack a
                // second shake on top of the one already playing. Every
                // sub-state therefore resolves to the base key and the base
                // blankets the whole ride (user ruling 2026-08-23).
                //
                // Same shape as the ".hand." strip above, and the same
                // reason: a CAMERA split that noise deliberately does not
                // inherit.
                static constexpr std::string_view kDragonRoot = "mounts.dragon_riding";
                if (key.size() > kDragonRoot.size() &&
                    key.compare(0, kDragonRoot.size(), kDragonRoot) == 0 &&
                    key[kDragonRoot.size()] == '.') {
                    key.erase(kDragonRoot.size());
                }
                sPtrToKey.emplace(e.outdoor, key);
                // Every environment's copy resolves to the same noise key.
                for (int env = kEnvIndoor; env < kEnvCount; ++env) {
                    if (auto* v = VariantOf(e.outdoor, env); v && v != e.outdoor) {
                        sPtrToKey.emplace(v, key);
                    }
                }
            }
            // Also map TL profile pointers to the same toml keys. When the
            // player is target-locked, CameraController switches `selected`
            // to a TL pointer; without these entries the noise lookup
            // misses and falls back to globalNoise â€” symptom: "camera
            // noise stops when target locked".
#define DDC_X(name, key, field) sPtrToKey.emplace(&field, key);
            DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X
            // Directional power-attack overrides â€” Categories pointers and
            // their TL counterparts share the directional noise key so the
            // user can configure each direction's noise once and it applies
            // regardless of target-lock state.
            for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                const std::string nk = std::string("weapons.melee.power_attack.dir.") + kPADirKeys[d];
                sPtrToKey.emplace(&weaponsMeleePowerAttackDir[d], nk);
                for (int env = kEnvIndoor; env < kEnvCount; ++env) {
                    if (auto* v = VariantOf(&weaponsMeleePowerAttackDir[d], env);
                        v && v != &weaponsMeleePowerAttackDir[d])
                    {
                        sPtrToKey.emplace(v, nk);
                    }
                }
                sPtrToKey.emplace(&tlWeaponsMeleePowerAttackDir[d], nk);
                // Per-weapon override profiles for this direction share the key.
                for (std::size_t w = 0; w < kMeleeWeaponCount; ++w) {
                    sPtrToKey.emplace(&weaponsMeleePowerAttackDirOverrides[d].perWeapon[w], nk);
                    sPtrToKey.emplace(&tlWeaponsMeleePowerAttackDirOverrides[d].perWeapon[w], nk);
                }
            }

            // Map per-weapon camera override profiles back to their base
            // noise key. When a position (weapon-type) override is active,
            // CameraController sets `selected` to the override profile; without
            // these entries the noise lookup missed and dropped to global,
            // silently ignoring the state's noise + noise-override. Both
            // Categories and TL override structs share the same base noise key
            // (noise isn't split by target lock).
            {
                struct OvKey { MeleeWeaponOverrides* ov; const char* key; };
                const OvKey ovKeys[] = {
                    { &weaponsMeleeOverrides,                  "weapons.melee" },
                    { &weaponsMeleeSprintOverrides,            "weapons.melee.sprint" },
                    { &weaponsMeleeSwimOverrides,              "weapons.melee.swim" },
                    { &weaponsMeleeAttackOverrides,            "weapons.melee.attack" },
                    { &weaponsMeleeSneakOverrides,             "weapons.melee.sneak" },
                    { &weaponsMeleeShoutOverrides,             "weapons.melee.shout" },
                    { &weaponsMeleeShoutSneakOverrides,        "weapons.melee.shout.sneak" },
                    { &weaponsMeleePowerAttackOverrides,       "weapons.melee.power_attack" },
                    { &weaponsMeleeSneakAttackOverrides,       "weapons.melee.sneak_attack" },
                    { &weaponsMeleeSneakPowerAttackOverrides,  "weapons.melee.sneak_power_attack" },
                    { &weaponsMeleeSprintAttackOverrides,      "weapons.melee.sprint_attack" },
                    { &weaponsMeleeSprintPowerAttackOverrides, "weapons.melee.sprint_power_attack" },
                    { &tlWeaponsMeleeOverrides,                  "weapons.melee" },
                    { &tlWeaponsMeleeSprintOverrides,            "weapons.melee.sprint" },
                    { &tlWeaponsMeleeSwimOverrides,              "weapons.melee.swim" },
                    { &tlWeaponsMeleeAttackOverrides,            "weapons.melee.attack" },
                    { &tlWeaponsMeleeSneakOverrides,             "weapons.melee.sneak" },
                    { &tlWeaponsMeleeShoutOverrides,             "weapons.melee.shout" },
                    { &tlWeaponsMeleeShoutSneakOverrides,        "weapons.melee.shout.sneak" },
                    { &tlWeaponsMeleePowerAttackOverrides,       "weapons.melee.power_attack" },
                    { &tlWeaponsMeleeSneakAttackOverrides,       "weapons.melee.sneak_attack" },
                    { &tlWeaponsMeleeSneakPowerAttackOverrides,  "weapons.melee.sneak_power_attack" },
                    { &tlWeaponsMeleeSprintAttackOverrides,      "weapons.melee.sprint_attack" },
                    { &tlWeaponsMeleeSprintPowerAttackOverrides, "weapons.melee.sprint_power_attack" },
                };
                for (const auto& ok : ovKeys) {
                    for (std::size_t w = 0; w < kMeleeWeaponCount; ++w) {
                        sPtrToKey.emplace(&ok.ov->perWeapon[w], ok.key);
                    }
                }
            }
        }
        // A location override's copy is a different address again. It is
        // deliberately NOT in the cache: unlike the env variants, a place can
        // be created or deleted mid-session, so cached addresses could dangle.
        // Mapping back to the outdoor pointer costs one small scan and can't go
        // stale. (Noise is configured once per state, not once per place â€” the
        // place's own entry is found by KEY further down.)
        if (sPtrToKey.find(a_profile) == sPtrToKey.end()) {
            if (auto* od = OutdoorOf(a_profile); od != a_profile) a_profile = od;
        }
        // [CLIPCAM] animation entries: their own cell when Customized (the
        // Specific Animations tab on the Camera Noise page), else the
        // pre-existing behaviour (fall through to global). Any of the
        // entry's four camera pointers may be the resolved one.
        for (auto& ae : animationCameras) {
            if (a_profile == &ae.profile || a_profile == &ae.profileIndoor ||
                a_profile == &ae.tlProfile || a_profile == &ae.tlProfileIndoor) {
                const std::string akey = "animcam." + std::to_string(ae.uid);
                if (a_outKey) *a_outKey = akey;
                return ResolveAnimationNoise(ae.uid);
            }
        }

        // Per-form weapon-binding camera profiles are dynamic (added/removed
        // at runtime) so they aren't in the static pointer map. If the active
        // profile is a binding's camera profile (Categories or TL slot),
        // resolve directly: the binding's own noise wins when enabled,
        // otherwise redirect to the binding's base state profile so the normal
        // per-state / weapon-override / global resolution below applies.
        // Without this, enabling a binding's camera profile dropped its noise
        // to global.
        if (sPtrToKey.find(a_profile) == sPtrToKey.end()) {
            for (auto& b : weaponBindings) {
                bool found = false;
                for (std::size_t sl = 0; sl < kWeaponBindingSubStates; ++sl) {
                    // Per-hand binding camera profiles count as their slot â€”
                    // the hand-noise layer below decides whether the noise
                    // follows the hand or the slot.
                    bool isHandPtr = false;
                    for (std::size_t h = 0; h < kMagicHandCount && !isHandPtr; ++h) {
                        isHandPtr = (&b.handProfiles[h][sl]       == a_profile ||
                                     &b.handProfilesIndoor[h][sl] == a_profile ||
                                     &b.handTlProfiles[h][sl]     == a_profile ||
                                     &b.handTlProfilesIndoor[h][sl] == a_profile);
                    }
                    if (!isHandPtr &&
                        &b.profiles[sl]       != a_profile && &b.tlProfiles[sl]       != a_profile &&
                        &b.profilesIndoor[sl] != a_profile && &b.tlProfilesIndoor[sl] != a_profile) continue;
                    // Directional power-attack NOISE decoupling (binding). The
                    // camera may route a bound weapon's power attack to the base
                    // PA slot (5), but the binding's DIRECTIONAL noise slot
                    // (10 + direction) should fire on the ACTUAL attack direction
                    // when it's enabled â€” noise is tuned independently of the
                    // camera's directional routing.
                    std::size_t useSl = sl;
                    if (b.category == BindingCategory::Melee && sl == 5) {
                        const auto di = static_cast<std::size_t>(
                            StateResolver::GetSingleton().GetPowerAttackDirection());
                        const std::size_t dirSl = 10 + di;
                        if (di < kPowerAttackDirectionCount && dirSl < kWeaponBindingSubStates &&
                            b.noiseEnabled[dirSl]) {
                            useSl = dirSl;
                        }
                    }
                    // A place's per-binding noise (the Location Override on
                    // the noise Specific Weapons toolbar) outranks the
                    // binding's own slot AND hand noise â€” location is the
                    // more specific layer (2026-08-15).
                    if (auto* lp = ActiveLocationStateNoise(
                            BindingNoiseLocationKey(b, static_cast<int>(useSl)))) {
                        if (a_outBindingLabel) {
                            *a_outBindingLabel =
                                (b.displayName.empty() ? std::string("Bound Weapon") : b.displayName) +
                                " - " + GetBindingSubStateName(b.category, static_cast<int>(useSl)) +
                                " (Location)";
                        }
                        return lp;
                    }
                    // Per-hand binding NOISE â€” its own layer above the slot
                    // noise, gated on the resolved hand + the hand slot's
                    // enable (independent of the base noise enable, like the
                    // camera-side hand overrides).
                    if (b.category == BindingCategory::Spell) {
                        const int h = ResolveSpellBindingHand(b, static_cast<int>(useSl));
                        if (h >= 0 && h < static_cast<int>(kMagicHandCount) &&
                            b.handNoiseEnabled[h][useSl]) {
                            if (a_outBindingLabel) {
                                *a_outBindingLabel =
                                    (b.displayName.empty() ? std::string("Bound Weapon") : b.displayName) +
                                    " - " + GetBindingSubStateName(b.category, static_cast<int>(useSl)) +
                                    " - " + GetMagicHandName(static_cast<std::size_t>(h));
                            }
                            return &b.HandNoiseFor(RuntimeEnv())[h][useSl];
                        }
                    }
                    // Every regular sub-state's noise is ALWAYS ACTIVE (flags
                    // normalized true on bind/load â€” the per-slot Enable
                    // toggles were removed); only the melee directional slots
                    // (10-14) stay flag-gated, redirecting to the base state
                    // so generic noise wins when off.
                    const bool baseNoise = (useSl == 0) ||
                        (b.category == BindingCategory::Melee && useSl == 5);
                    if (baseNoise || b.noiseEnabled[useSl]) {
                        // Report the noise STATE key for the slot this binding
                        // actually resolved, so the Quick Tune box names the
                        // exact override (e.g. the directional power attack)
                        // INSTEAD of falling back to the camera's state name â€”
                        // which is what made the box look Categories-gated.
                        if (a_outKey && b.category == BindingCategory::Melee) {
                            if (useSl >= 10 && useSl <= 14)
                                *a_outKey = std::string("weapons.melee.power_attack.dir.") + kPADirKeys[useSl - 10];
                            else if (useSl == 5)
                                *a_outKey = "weapons.melee.power_attack";
                        }
                        // Bound weapon: hand back a ready "<Binding> - <SubState>"
                        // label so the Quick Tune box names it by the weapon (and
                        // the NOISE's resolved slot), matching the camera box.
                        // Tag the base power-attack slot "(Base)" â€” it means no
                        // directional noise override applied, same as the camera.
                        if (a_outBindingLabel) {
                            *a_outBindingLabel =
                                (b.displayName.empty() ? std::string("Bound Weapon") : b.displayName) +
                                " - " + GetBindingSubStateName(b.category, static_cast<int>(useSl));
                            if (b.category == BindingCategory::Melee && useSl == 5)
                                *a_outBindingLabel += " (Base)";
                        }
                        return &b.NoiseProfilesFor(RuntimeEnv())[useSl];
                    }
                    if (auto* base = BindingBaseProfile(*this, b.category, static_cast<int>(sl))) {
                        a_profile = base;
                    } else if (b.category == BindingCategory::Spell && sl == 3) {
                        // The Casting slot has no fixed base state â€” its
                        // "base" is whatever school profile this cast would
                        // resolve without the binding. Redirect there so the
                        // generic school noise (including its per-hand
                        // variants via the redirect below) wins instead of
                        // silently dropping to global â€” which also blanked
                        // the Quick Tune noise box while casting a bound
                        // spell.
                        auto& srCast = StateResolver::GetSingleton();
                        a_profile = PickMagicProfile(srCast.GetSchool(), srCast.GetCastType(),
                                                     srCast.IsSneaking());
                    }
                    found = true;
                    break;
                }
                if (found) break;
            }
        }

        const auto it = sPtrToKey.find(a_profile);
        if (it == sPtrToKey.end()) return nullptr;
        // Report the resolved noise STATE key (e.g.
        // "weapons.melee.power_attack.dir.forward") so the Quick Tune box can
        // name the exact override it lands on â€” the noise tree resolves
        // independently of the camera (its own per-weapon / directional /
        // override enables), so the camera's state name can't stand in for it.
        if (a_outKey) *a_outKey = it->second;

        // Per-specific-form binding (shared across Categories/TL/Noise).
        // Sits above the per-weapon-type and base stateNoise layers: when
        // the player's currently-held form matches a binding and the
        // sub-state's noise slot is enabled, return that binding's
        // NoiseProfile. Maps the camera profile's tomlKey to (category,
        // sub-state slot) so the binding can be looked up.
        {
            static const std::unordered_map<std::string,
                std::pair<BindingCategory, int>> sBindingKeyMap = {
                { "weapons.melee",                  { BindingCategory::Melee,    0 } },
                { "weapons.melee.sprint",           { BindingCategory::Melee,    1 } },
                { "weapons.melee.swim",             { BindingCategory::Melee,    2 } },
                { "weapons.melee.attack",           { BindingCategory::Melee,    3 } },
                { "weapons.melee.sneak",            { BindingCategory::Melee,    4 } },
                { "weapons.melee.power_attack",          { BindingCategory::Melee, 5 } },
                { "weapons.melee.sprint_attack",         { BindingCategory::Melee, 6 } },
                { "weapons.melee.sprint_power_attack",   { BindingCategory::Melee, 7 } },
                { "weapons.melee.sneak_attack",          { BindingCategory::Melee, 8 } },
                { "weapons.melee.sneak_power_attack",    { BindingCategory::Melee, 9 } },
                { "weapons.melee.power_attack.dir.standing", { BindingCategory::Melee, 10 } },
                { "weapons.melee.power_attack.dir.forward",  { BindingCategory::Melee, 11 } },
                { "weapons.melee.power_attack.dir.back",     { BindingCategory::Melee, 12 } },
                { "weapons.melee.power_attack.dir.left",     { BindingCategory::Melee, 13 } },
                { "weapons.melee.power_attack.dir.right",    { BindingCategory::Melee, 14 } },
                { "weapons.bow",                    { BindingCategory::Bow,      0 } },
                { "weapons.bow.sprint",             { BindingCategory::Bow,      1 } },
                { "weapons.bow.draw",               { BindingCategory::Bow,      2 } },
                { "weapons.bow.swim",               { BindingCategory::Bow,      3 } },
                { "weapons.bow.sneak",              { BindingCategory::Bow,      4 } },
                { "weapons.bow.sneak.draw",         { BindingCategory::Bow,      5 } },
                { "weapons.bow.zoom",               { BindingCategory::Bow,      6 } },
                { "weapons.bow.sneak.zoom",         { BindingCategory::Bow,      7 } },
                { "weapons.crossbow",               { BindingCategory::Crossbow, 0 } },
                { "weapons.crossbow.sprint",        { BindingCategory::Crossbow, 1 } },
                { "weapons.crossbow.draw",          { BindingCategory::Crossbow, 2 } },
                { "weapons.crossbow.swim",          { BindingCategory::Crossbow, 3 } },
                { "weapons.crossbow.sneak",         { BindingCategory::Crossbow, 4 } },
                { "weapons.crossbow.sneak.draw",    { BindingCategory::Crossbow, 5 } },
                { "weapons.crossbow.zoom",          { BindingCategory::Crossbow, 6 } },
                { "weapons.crossbow.sneak.zoom",    { BindingCategory::Crossbow, 7 } },
                { "weapons.magic",                  { BindingCategory::Spell,    0 } },
                { "weapons.magic.sprint",           { BindingCategory::Spell,    1 } },
                { "weapons.magic.swim",             { BindingCategory::Spell,    2 } },
                { "weapons.magic.sneak",            { BindingCategory::Spell,    4 } },
                { "weapons.staves",                 { BindingCategory::Staff,    0 } },
                { "weapons.staves.sprint",          { BindingCategory::Staff,    1 } },
                { "weapons.staves.swim",            { BindingCategory::Staff,    2 } },
                { "weapons.staves.sneak",           { BindingCategory::Staff,    3 } },
                { "weapons.blocking.shield",        { BindingCategory::Shield,   0 } },
                { "weapons.blocking.shield.sneak",  { BindingCategory::Shield,   1 } },
                { "weapons.blocking.shield.sprint", { BindingCategory::Shield,   2 } },
            };
            const auto bit = sBindingKeyMap.find(it->second);
            if (bit != sBindingKeyMap.end()) {
                const auto cat = bit->second.first;
                const int  idx = bit->second.second;
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    // Shield bindings use the left hand exclusively;
                    // everything else tries the RIGHT hand first, then the
                    // LEFT whenever the right form has no matching binding
                    // (mirrors the camera picker) â€” a left-hand-bound spell
                    // under a right-hand weapon must still match. A matched
                    // binding's decision stands even when its noise slot is
                    // disabled (the inner `break` falls to generic).
                    bool bindingMatched = false;
                    const int first = cat == BindingCategory::Shield ? 1 : 0;
                    for (int c = first; c < 2 && !bindingMatched; ++c) {
                        const auto item = ItemBindings::DescribeEquipped(player, c == 1);
                        if (auto* match = ItemBindings::FindBest(weaponBindings, static_cast<int>(cat), false, item)) {
                            auto& b = *match;
                            bindingMatched = true;
                            // Location layer first â€” see the
                            // pointer-matched block above.
                            if (auto* lp = ActiveLocationStateNoise(
                                    BindingNoiseLocationKey(b, idx))) {
                                if (a_outBindingLabel) {
                                    *a_outBindingLabel =
                                        (b.displayName.empty() ? std::string("Bound Weapon") : b.displayName) +
                                        " - " + GetBindingSubStateName(b.category, idx) +
                                        " (Location)";
                                }
                                return lp;
                            }
                            // Per-hand binding noise layer â€” see the
                            // pointer-matched block above.
                            if (cat == BindingCategory::Spell) {
                                const int h = ResolveSpellBindingHand(b, idx);
                                if (h >= 0 && h < static_cast<int>(kMagicHandCount) &&
                                    b.handNoiseEnabled[h][idx]) {
                                    if (a_outBindingLabel) {
                                        *a_outBindingLabel =
                                            (b.displayName.empty() ? std::string("Bound Weapon") : b.displayName) +
                                            " - " + GetBindingSubStateName(b.category, idx) +
                                            " - " + GetMagicHandName(static_cast<std::size_t>(h));
                                    }
                                    return &b.HandNoiseFor(RuntimeEnv())[h][idx];
                                }
                            }
                            // Every regular sub-state's noise is
                            // ALWAYS ACTIVE (flags normalized true on
                            // bind/load); only melee directional slots
                            // stay flag-gated, falling through to the
                            // generic resolution below when off.
                            const bool baseNoise = (idx == 0) ||
                                (cat == BindingCategory::Melee && idx == 5);
                            if (baseNoise || b.noiseEnabled[idx]) {
                                if (a_outBindingLabel) {
                                    *a_outBindingLabel =
                                        (b.displayName.empty() ? std::string("Bound Weapon") : b.displayName) +
                                        " - " + GetBindingSubStateName(b.category, idx);
                                    if (cat == BindingCategory::Melee && idx == 5)
                                        *a_outBindingLabel += " (Base)";
                                }
                                return &b.NoiseProfilesFor(RuntimeEnv())[idx];
                            }
                            break;
                        }
                    }
                }
            }
        }

        // The remaining resolution works off a MUTABLE key so the directional
        // power-attack decoupling can redirect it.
        std::string key = it->second;

        // Directional power-attack NOISE decoupling. The camera may be
        // base-routed for a power attack (its per-direction toggle off), which
        // lands `selected` on the base power-attack profile and this key on
        // "weapons.melee.power_attack". The Camera Noise tree is tuned
        // independently of the camera, so a directional NOISE override must
        // still fire on the ACTUAL attack direction (StateResolver captured it
        // at the power-attack edge). Redirect the key to the directional one
        // when a directional noise override actually EXISTS for this direction
        // â€” either a per-weapon override or an enabled base directional noise â€”
        // so the normal resolution below applies it. Only redirect when an
        // override exists, so a plain base power attack with no directional
        // noise still uses the base power-attack noise.
        if (key == "weapons.melee.power_attack") {
            const auto di = static_cast<std::size_t>(
                StateResolver::GetSingleton().GetPowerAttackDirection());
            if (di < kPowerAttackDirectionCount) {
                const std::string dkey =
                    std::string("weapons.melee.power_attack.dir.") + kPADirKeys[di];
                const auto wi = static_cast<std::size_t>(
                    ClassifyPlayerMeleeWeapon(RE::PlayerCharacter::GetSingleton()));
                const bool perW = wi < kMeleeWeaponCount &&
                    weaponsMeleePowerAttackDirNoiseOverrides[di].perWeaponSet[wi];
                auto enabledIn = [&](const std::unordered_map<std::string, NoiseProfile>& m) {
                    auto i = m.find(dkey);
                    return i != m.end() && i->second.enabled;
                };
                const int  envN    = RuntimeEnv();
                const bool baseDir = enabledIn(stateNoise) ||
                                     (envN != kEnvOutdoor && enabledIn(StateNoiseFor(envN))) ||
                                     ActiveLocationStateNoise(dkey) != nullptr;
                if (perW || baseDir) key = dkey;
            }
        }

        // Per-hand magic NOISE decoupling. The camera may resolve the base
        // school profile (its per-hand camera override off) while a per-hand
        // NOISE entry is enabled â€” the Camera Noise tree is tuned
        // independently, so redirect the key to the hand variant whenever
        // the live cast-hand has an enabled entry. When the camera DID
        // route per-hand, sPtrToKey already mapped that pointer back to the
        // base key, so this single redirect covers both cases. School keys
        // are the only "magic."-prefixed ones (the base state is
        // "weapons.magic").
        if (key.rfind("magic.", 0) == 0 && key.find(".hand.") == std::string::npos) {
            const int h = static_cast<int>(StateResolver::GetSingleton().GetCastingHand());
            if (h >= 0 && h < static_cast<int>(kMagicHandCount)) {
                const std::string hkey = key + ".hand." + GetMagicHandTomlKey(h);
                auto enabledIn = [&](const std::unordered_map<std::string, NoiseProfile>& m) {
                    auto i = m.find(hkey);
                    return i != m.end() && i->second.enabled;
                };
                const int envH = RuntimeEnv();
                if (enabledIn(stateNoise) ||
                    (envH != kEnvOutdoor && enabledIn(StateNoiseFor(envH))) ||
                    ActiveLocationStateNoise(hkey) != nullptr) {
                    key = hkey;
                }
            }
        }

        // Per-weapon-type override layer for the 5 Melee buckets. Sits
        // above stateNoise: when the player's current weapon's slot is
        // toggled on, return that NoiseProfile regardless of whether
        // the base "Enable Customization" is also on. Falls through to
        // the base stateNoise lookup otherwise.
        MeleeWeaponNoiseOverrides* meleeOv = nullptr;
        if      (key == "weapons.melee")        meleeOv = &weaponsMeleeNoiseOverrides;
        else if (key == "weapons.melee.sprint") meleeOv = &weaponsMeleeSprintNoiseOverrides;
        else if (key == "weapons.melee.swim")   meleeOv = &weaponsMeleeSwimNoiseOverrides;
        else if (key == "weapons.melee.attack") meleeOv = &weaponsMeleeAttackNoiseOverrides;
        else if (key == "weapons.melee.sneak")  meleeOv = &weaponsMeleeSneakNoiseOverrides;
        else if (key == "weapons.melee.power_attack")        meleeOv = &weaponsMeleePowerAttackNoiseOverrides;
        else if (key == "weapons.melee.sneak_attack")        meleeOv = &weaponsMeleeSneakAttackNoiseOverrides;
        else if (key == "weapons.melee.sneak_power_attack")  meleeOv = &weaponsMeleeSneakPowerAttackNoiseOverrides;
        else if (key == "weapons.melee.sprint_attack")       meleeOv = &weaponsMeleeSprintAttackNoiseOverrides;
        else if (key == "weapons.melee.sprint_power_attack") meleeOv = &weaponsMeleeSprintPowerAttackNoiseOverrides;
        else {
            // Directional power-attack noise overrides share the directional
            // noise key with both Categories and Target Lock.
            for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
                if (key == std::string("weapons.melee.power_attack.dir.") + kPADirKeys[d]) {
                    meleeOv = &weaponsMeleePowerAttackDirNoiseOverrides[d];
                    break;
                }
            }
        }
        if (a_outKey) *a_outKey = key;   // reflect any directional redirect
        if (meleeOv) {
            const auto wt = ClassifyPlayerMeleeWeapon(RE::PlayerCharacter::GetSingleton());
            const auto wi = static_cast<std::size_t>(wt);
            if (wi < kMeleeWeaponCount && meleeOv->perWeaponSet[wi]) {
                if (a_outWeapon) *a_outWeapon = static_cast<int>(wi);
                return &meleeOv->perWeapon[wi];
            }
        }

        // Indoor variant takes precedence when the player is in an
        // interior cell. Falls through to the outdoor map if the indoor
        // entry is missing/disabled.
        // The active environment's variant takes precedence; a missing or
        // disabled entry falls through to the outdoor map, so a user who only
        // tuned Outdoor keeps that tuning everywhere.
        // A location override is more specific than the environment, so it is
        // asked first; a place with nothing to say about this key falls
        // through to the env map and then to plain stateNoise.
        if (auto* lp = ActiveLocationStateNoise(key)) return lp;
        if (const int envS = RuntimeEnv(); envS != kEnvOutdoor) {
            auto& envMap = StateNoiseFor(envS);
            const auto iit = envMap.find(key);
            if (iit != envMap.end() && iit->second.enabled) {
                return &iit->second;
            }
        }
        const auto sit = stateNoise.find(key);
        if (sit == stateNoise.end()) return nullptr;
        if (!sit->second.enabled) return nullptr;
        return &sit->second;
    }

    // Resolution result: a primary key plus an optional fallback. Most
    // states only set primary (fallback empty). Shouts emit both â€”
    // primary = per-shout override (shouts.<state>.<shoutTomlKey>[.sneak]),
    // fallback = per-state base (shouts.<state>.base[.sneak]). The
    // ResolveFirstPersonFovProfile / NoiseProfile callers walk primary
    // first, then fallback, then firstPersonGlobal.
    struct FirstPersonKeyPair
    {
        std::string primary;
        std::string fallback;
    };

    // Build the shouts key pair when the player is currently shouting.
    // Layout is state-agnostic: one global base for all shouts plus
    // 27 per-shout overrides (each with a sneak variant). Primary =
    // per-shout override, fallback = global base. Returns nullopt when
    // not shouting (or in a state that doesn't support shouting â€”
    // Werewolf, Transformations, etc.).
    static std::optional<FirstPersonKeyPair> ResolveFirstPersonShoutsKeys(bool a_sneaking)
    {
        const auto& sr = StateResolver::GetSingleton();
        if (sr.GetSubState() != CameraSubState::Shout) return std::nullopt;
        // Still require a shoutable weapon state â€” vampires/werewolves
        // hit ResolveSubState::Shout briefly during their roar wind-up
        // but we don't want to apply a shout profile then.
        if (!ShoutableIndexFor(sr.GetState())) return std::nullopt;

        const char* suffix = a_sneaking ? ".sneak" : "";

        FirstPersonKeyPair out;
        out.fallback = std::string("shouts.base") + suffix;
        if (auto shoutId = sr.GetActiveShoutId()) {
            const auto idx = static_cast<std::size_t>(*shoutId);
            if (idx < kShoutCount) {
                out.primary = std::string("shouts.") + kShouts[idx].tomlKey + suffix;
            }
        }
        return out;
    }

    static std::string ResolveFirstPersonKey()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return "sheathed";
        auto* asState = player->AsActorState();
        if (!asState) return "sheathed";

        // LATCHED/POLLED flags, never the raw actor flags: the engine
        // flaps IsSprinting mid-swing and mid-jump, and every flap flips
        // the FP key between two entries — invisible while their values
        // matched, a visible FOV pump the moment the user tunes them apart
        // ("first person sprinting is stuttering", root-caused
        // 2026-08-31). The 3p tree reads these same resolver signals, so
        // the FP key now changes exactly when the 3p state does.
        const auto& srKey = StateResolver::GetSingleton();
        const bool sneak  = srKey.IsAttackSneak();
        const bool sprint = srKey.IsAttackSprint();
        const bool swim   = asState->IsSwimming();

        // Shield-equipped detection (left-hand armor in shield slot). Used
        // BOTH to bias the blocking branch toward "shield" and to drive
        // bucket selection: when a shield is equipped without any weapon
        // in the main hand, treat the bucket as Melee so blocking still
        // routes through weapons.blocking.* and the user's shield-block
        // customization wins.
        auto* offHand    = player->GetEquippedObject(true);
        auto* offArmor   = offHand ? offHand->As<RE::TESObjectARMO>() : nullptr;
        const bool shieldEquipped =
            offArmor &&
            (offArmor->bipedModelData.bipedObjectSlots &
             RE::BGSBipedObjectForm::FirstPersonFlag::kShield)
                != RE::BGSBipedObjectForm::FirstPersonFlag::kNone;

        // Determine the "weapon bucket" the player is in. Defaults to
        // sheathed; gets reset to a weapons.* root if a weapon is drawn.
        // Priority: Spell > Staff > Bow/Crossbow > Melee. The spell
        // priority is essential â€” when the player holds a sword in the
        // right hand and a spell in the left, casting from the left
        // should route to Magic (and on through the school resolver),
        // not the Melee bucket. Either hand can carry the spell.
        enum class Bucket { Sheathed, Melee, Magic, Staves, Bow, Crossbow };
        Bucket bucket   = Bucket::Sheathed;
        bool   drawing  = false;  // bow/crossbow string taut
        // Eagle Eye / any mod's equivalent. Straight off the engine's own
        // PlayerCamera flag, set by its BowZoom anim-event handler â€” so a mod
        // that grants the zoom at level 1, or moves it behind another perk,
        // drives this exactly the same way a vanilla perk does.
        const bool zoomed =
            [] { auto* pc = RE::PlayerCamera::GetSingleton(); return pc && pc->bowZoomedIn; }();
        // [FP-FAST] DRAW BEGINS THE STATE. kDrawn only sets once the draw
        // animation FINISHES — the user watched the whole draw on sheathed
        // settings and got the weapon FOV as a pop at the end. kWantToDraw
        // fires the frame the button is pressed, so the transition and the
        // animation run together. Sheathing already reads as sheathed from
        // the first frame (kWantToSheathe leaves the set below).
        const auto fpWeapState = asState->GetWeaponState();
        if (fpWeapState == RE::WEAPON_STATE::kDrawn ||
            fpWeapState == RE::WEAPON_STATE::kDrawing ||
            fpWeapState == RE::WEAPON_STATE::kWantToDraw) {
            const bool isAttacking =
                asState->actorState1.meleeAttackState != RE::ATTACK_STATE_ENUM::kNone;

            auto* rhObj = player->GetEquippedObject(false);
            auto* lhObj = player->GetEquippedObject(true);

            auto isSpell = [](RE::TESForm* f) -> bool {
                return f && f->As<RE::SpellItem>() != nullptr;
            };
            auto getWeap = [](RE::TESForm* f) -> RE::TESObjectWEAP* {
                return f ? f->As<RE::TESObjectWEAP>() : nullptr;
            };
            auto* rhWep = getWeap(rhObj);
            auto* lhWep = getWeap(lhObj);
            auto isStaff = [](RE::TESObjectWEAP* w) { return w && w->IsStaff(); };

            // Melee always wins over an off-hand spell. A held spell only
            // claims the Magic bucket while the player is ACTUALLY casting it
            // (charging a fire-and-forget/ritual or streaming a concentration);
            // a spellsword holding a weapon stays in its weapon state otherwise
            // (no Magic fallback â€” an unconfigured weapon state goes to Global).
            // Magic without casting only happens when NO weapon claims the
            // state, i.e. a spell paired with a shield or another spell.
            const auto& resolver = StateResolver::GetSingleton();
            const bool casting  = resolver.IsChargingSpell() || resolver.IsCastingStream();
            const bool hasSpell = isSpell(rhObj) || isSpell(lhObj);

            if (casting && hasSpell) {
                bucket = Bucket::Magic;
            } else if (isStaff(rhWep) || isStaff(lhWep)) {
                bucket = Bucket::Staves;
            } else if (rhWep && rhWep->IsBow()) {
                bucket = Bucket::Bow;
                drawing = isAttacking;
            } else if (rhWep && rhWep->IsCrossbow()) {
                bucket = Bucket::Crossbow;
                drawing = isAttacking;
            } else if (rhWep || lhWep) {
                bucket = Bucket::Melee;
            } else if (hasSpell) {
                bucket = Bucket::Magic;
            }
        }

        // Melee attack sub-states. StateResolver resolves the Attack sub-state
        // (with the engine's attack timing + linger), and we mirror the 3p
        // attack matrix so the FP keys match: {sprint|sneak|none} x {power|
        // normal}. Standing power attacks route to the base power_attack key
        // (FP has no per-direction power-attack split). Runs before the
        // blocking gate so a bash from a blocked stance reads as an attack,
        // same as 3p (weaponsMeleeAttack).
        // [FP-FAST] ATTACK EXIT ON THE ENGINE'S RECOVERY EDGE (user
        // direction 2026-08-30: "make first person more responsive and
        // less sluggish to change states"). The resolver's Attack
        // sub-state is the animation BRACKET — it deliberately spans the
        // whole recovery for 3p framing, which in 1p reads as the FOV
        // outstaying the swing by the entire follow-through (0.5-1.5s on
        // modded power attacks). The engine's own meleeAttackState leaves
        // the active set (kDraw/kSwing/kHit/kBash) the moment contact
        // ends, on every attack framework — so 1p ANDs that in, with a
        // 150ms bridge so chained combo swings don't dip to the parent
        // between hits (each new swing re-enters kDraw and re-applies on
        // the same frame). Menu hold bypasses the gate entirely: the
        // hold's job is to keep the opened-in state on screen, and this
        // gate would strip the attack key out from under Quick Tune.
        // 3p keeps the bracket; this gate exists ONLY here.
        static bool sFpAtkLastWasPower = false;
        const bool fpAttackActive = [&]() -> bool {
            static std::chrono::steady_clock::time_point sLastActiveTp{};
            const auto atkSt = asState->actorState1.meleeAttackState;
            // [FPATK] capped transition walk — the ruler for every timing
            // decision in this gate. Change-gated (the resolver runs
            // several times a frame).
            {
                static auto sPrevSt  = RE::ATTACK_STATE_ENUM::kNone;
                static auto sPrevTp  = std::chrono::steady_clock::now();
                static int  sWalkLog = 0;
                if (atkSt != sPrevSt && sWalkLog < 80) {
                    ++sWalkLog;
                    const auto tpNow = std::chrono::steady_clock::now();
                    spdlog::debug("[FPATK] {} -> {} after {}ms",
                                 static_cast<int>(sPrevSt), static_cast<int>(atkSt),
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     tpNow - sPrevTp).count());
                    sPrevSt = atkSt;
                    sPrevTp = tpNow;
                }
            }
            // kNextAttack is in the ACTIVE set: it is the engine's
            // combo-CONTINUATION state — the chain lives there between
            // swings (adjudicated 2026-08-30: chained swings dipped to the
            // parent for the part of the gap the 150ms bridge couldn't
            // cover, and the FOV pumped once per swing). kFollowThrough
            // stays OUT: that is the winding-down tail with no follow-up,
            // i.e. the hand-back edge this gate exists to catch.
            const bool active =
                atkSt == RE::ATTACK_STATE_ENUM::kDraw       ||
                atkSt == RE::ATTACK_STATE_ENUM::kSwing      ||
                atkSt == RE::ATTACK_STATE_ENUM::kHit        ||
                atkSt == RE::ATTACK_STATE_ENUM::kNextAttack ||
                atkSt == RE::ATTACK_STATE_ENUM::kBash;
            const auto nowTp = CameraEffectClock::Now();
            if (active) {
                sLastActiveTp = nowTp;
                return true;
            }
            if (sLastActiveTp.time_since_epoch().count() == 0) return false;
            // POWER bridge 900ms vs 150ms ([FPATK] adjudication
            // 2026-08-30): light chains live in kNextAttack between
            // swings, but a power attack walks 3 -> 4 (~570ms) -> 0 and
            // the next PA's kDraw lands 110-900ms after the kNone — the
            // engine COMMITS the exit between chained PAs, so only the
            // bridge can span them ("back to back power attacks get a
            // bit jarring"). Single-PA exit becomes ~1.5s after contact —
            // half the old bracket-and-recovery wait.
            const auto kFpAttackBridge = sFpAtkLastWasPower
                ? std::chrono::milliseconds(900)
                : std::chrono::milliseconds(150);
            return (nowTp - sLastActiveTp) <= kFpAttackBridge;
        }();
        if (bucket == Bucket::Melee &&
            StateResolver::GetSingleton().GetSubState() == CameraSubState::Attack &&
            (fpAttackActive || StateResolver::GetSingleton().IsMenuHoldActive())) {
            const auto& sr    = StateResolver::GetSingleton();
            const bool  power = sr.IsPowerAttacking();
            // Latched for the swing, not live â€” the engine clears the sprint
            // flag partway through a sprint power attack, which flipped this
            // key mid-action and re-fired the attack beat on the new cell.
            std::string cand;
            if (sr.IsAttackSprint())
                cand = power ? "weapons.melee.sprint_power_attack"
                             : "weapons.melee.sprint_attack";
            else if (sr.IsAttackSneak())
                cand = power ? "weapons.melee.sneak_power_attack"
                             : "weapons.melee.sneak_attack";
            else
                cand = power ? "weapons.melee.power_attack"
                             : "weapons.melee.attack";
            // THE GRAPH ANNOUNCES THE WINDUP'S KIND (round 19, "power
            // attack to normal attack is lagging"). The engine's flag only
            // latches at RELEASE, so during any windup power=false and the
            // stickiness below held the previous power cell through a
            // whole light windup. But the start tag names the swing at
            // windup START — so a freshly announced kind owns the claim:
            // an announced POWER windup claims the power cell immediately
            // (also killing the initial-claim windup flash), an announced
            // LIGHT windup releases the power cell immediately (the lag
            // this round fixes). Untagged frameworks never advance the
            // counter and keep the flag-and-state rules below.
            static std::uint32_t sFpAtkSeenHintCtr   = 0;
            static bool          sFpAtkSwingHintPower = false;
            static bool          sFpAtkSwingHintValid = false;
            bool          dbgHintFresh = false;   // [FPATK] — see the probe below
            std::uint32_t dbgHintCtr   = 0;
            {
                std::uint32_t hintCtr = 0;
                const bool hintPower = sr.GetAttackSwingPowerHint(hintCtr);
                dbgHintCtr = hintCtr;
                if (hintCtr != sFpAtkSeenHintCtr) {
                    sFpAtkSeenHintCtr    = hintCtr;
                    sFpAtkSwingHintPower = hintPower;
                    sFpAtkSwingHintValid = true;
                    dbgHintFresh         = true;
                }
            }
            if (!power && sFpAtkSwingHintValid && sFpAtkSwingHintPower) {
                const auto posAtk = cand.rfind("attack");
                if (posAtk != std::string::npos &&
                    cand.find("power_") == std::string::npos)
                    cand.insert(posAtk, "power_");
            }
            // VARIANT DEBOUNCE + POWER STICKINESS (log-adjudicated
            // 2026-08-30, twice). First: each new swing in a chain flashed
            // power_attack -> attack for one 16ms resolve while the power
            // flag re-latched — any mid-claim change needs 120ms of
            // persistence. Second, the harder one: the engine only latches
            // kPowerAttack at RELEASE, so the whole windup (kDraw, ~1s on
            // some movesets) of a chained PA reports power=false and the
            // NORMAL attack cell flashed for the windup ("back to back
            // power attacks get a bit jarring"). So a power -> non-power
            // downgrade additionally needs PROOF that the current swing is
            // genuinely light: meleeAttackState in kSwing/kHit with the
            // power flag still down. A light swing chained after a PA
            // flips at its swing edge (stable, ~no flash); a chained PA
            // never downgrades at all. Upgrades (the flag arriving) stay
            // instant. Initial claims from idle are instant and
            // unguarded, as before.
            {
                static std::string sFpAtkLastKey;
                static std::string sFpAtkPendKey;
                static std::chrono::steady_clock::time_point sFpAtkPendSince{};
                static std::chrono::steady_clock::time_point sFpAtkLastTp{};
                const auto nowAtk = CameraEffectClock::Now();
                const bool midClaim = !sFpAtkLastKey.empty() &&
                    (nowAtk - sFpAtkLastTp) <= std::chrono::milliseconds(1000);
                if (midClaim && cand != sFpAtkLastKey) {
                    if (cand != sFpAtkPendKey) {
                        sFpAtkPendKey   = cand;
                        sFpAtkPendSince = nowAtk;
                    }
                    const bool lastPower =
                        sFpAtkLastKey.find("power") != std::string::npos;
                    const bool candPower =
                        cand.find("power") != std::string::npos;
                    const bool persisted =
                        (nowAtk - sFpAtkPendSince) >= std::chrono::milliseconds(120);
                    if (lastPower && !candPower) {
                        const auto stNow = asState->actorState1.meleeAttackState;
                        const bool provenLight =
                            stNow == RE::ATTACK_STATE_ENUM::kSwing ||
                            stNow == RE::ATTACK_STATE_ENUM::kHit;
                        // A graph-announced light windup is proof enough on
                        // its own — that is the entire point of the hint.
                        const bool announcedLight =
                            sFpAtkSwingHintValid && !sFpAtkSwingHintPower;
                        const bool held = !(announcedLight || (provenLight && persisted));
                        // [FPATK] THE POWER->LIGHT DOWNGRADE, printed with the
                        // evidence it rests on.
                        //
                        // MEASURED 2026-09-02, every power attack in the log,
                        // first person, authored melee cells:
                        //
                        //   20:11:56.711 [FP Noise] primary='weapons.melee.power_attack'
                        //   20:11:56.726 [FP Noise] primary='weapons.melee.attack'
                        //   20:11:56.981 [FP Noise] primary='weapons.melee.power_attack'
                        //
                        // One frame into the swing the power cell is released to
                        // the LIGHT cell, and 255ms later the engine's release
                        // flag takes it back — three cell changes for one swing,
                        // each one a step in the noise target. Same shape at
                        // 20:10:44/47/51, 20:11:03/12/22/28/30. The 120ms
                        // debounce cannot be what allowed it at 15ms, so the
                        // release came through `announcedLight`.
                        //
                        // THE QUESTION THIS ANSWERS: is that hint FRESH (the
                        // graph genuinely tagged this windup as light — then the
                        // tagging is the bug) or STALE (`sFpAtkSwingHintValid` is
                        // set once and never cleared, so the hint from a PREVIOUS
                        // swing still licenses a downgrade)? `fresh=1` means the
                        // counter advanced on this very resolve; `fresh=0` with
                        // `annLight=1` is the stale-hint case, and then the fix is
                        // to consume the hint per swing rather than to touch the
                        // debounce.
                        {
                            static int sAtkLogs = 0;
                            if (sAtkLogs < 60) {
                                ++sAtkLogs;
                                spdlog::debug("[FPATK] downgrade '{}' -> '{}' held={} annLight={} "
                                             "fresh={} hintCtr={} hintPower={} provenLight={} "
                                             "persisted={} atkState={}",
                                             sFpAtkLastKey, cand, held ? 1 : 0,
                                             announcedLight ? 1 : 0, dbgHintFresh ? 1 : 0,
                                             dbgHintCtr, sFpAtkSwingHintPower ? 1 : 0,
                                             provenLight ? 1 : 0, persisted ? 1 : 0,
                                             static_cast<int>(stNow));
                            }
                        }
                        if (held)
                            cand = sFpAtkLastKey;
                    } else if (!candPower || lastPower) {
                        // Sideways moves (sprint/sneak variants) debounce;
                        // upgrades to power fall through instantly.
                        if (!persisted)
                            cand = sFpAtkLastKey;
                    }
                } else {
                    sFpAtkPendKey.clear();
                }
                sFpAtkLastKey        = cand;
                sFpAtkLastTp         = nowAtk;
                sFpAtkLastWasPower   = cand.find("power") != std::string::npos;
            }
            return cand;
        }

        // Ward routing fires on the ward-active signal alone, BEFORE the
        // wantBlocking gate. Wards are Restoration concentration spells that
        // raise the block pose without the player ever pressing Block, so
        // gating on actorState2.wantBlocking misses them entirely. The
        // StateResolver sets blockKind=Ward whenever a hand caster is
        // streaming a ward spell (see ResolveBlocking).
        if (StateResolver::GetSingleton().GetBlockKind() == BlockKind::Ward) {
            std::string out = "weapons.blocking.ward";
            if (sneak) out += ".sneak";
            return out;
        }

        // Blocking â€” gated strictly on the input-driven wantBlocking flag.
        // BlockKind comes from direct equipment inspection so the resolver
        // stays robust when StateResolver bails out (e.g., when an off-hand
        // spell is mid-cast and ResolveBlocking yields to the cast).
        // Shield always wins over weapon kind because the shield is the
        // surface the player is presenting to the threat.
        if (asState->actorState2.wantBlocking) {
            const char* kindKey = "one_handed";
            if (shieldEquipped) {
                kindKey = "shield";
            } else {
                auto* rh    = player->GetEquippedObject(false);
                auto* rhWep = rh ? rh->As<RE::TESObjectWEAP>() : nullptr;
                if (rhWep && (rhWep->IsTwoHandedSword() || rhWep->IsTwoHandedAxe())) {
                    kindKey = "two_handed";
                }
            }
            // Shield-sprint composition (vanilla allows shield-sprinting;
            // weapon-block-sprint isn't a thing). Sneak applies on top of
            // any kind.
            if (std::strcmp(kindKey, "shield") == 0 && sprint) {
                return "weapons.blocking.shield.sprint";
            }
            std::string out = std::string("weapons.blocking.") + kindKey;
            if (sneak) out += ".sneak";
            return out;
        }

        // Magic / Staves school routing. When the player is casting (or
        // mid-charge) and the school + cast type are known, route to the
        // school-specific cell so per-school 1p customization applies.
        // Sneak suffix sits between school and cast type to match the
        // Categories TOML schema (magic.<school>.sneak.<casttype>).
        if (bucket == Bucket::Magic || bucket == Bucket::Staves) {
            const auto& sr = StateResolver::GetSingleton();
            const auto  school = sr.GetSchool();
            const auto  ct     = sr.GetCastType();
            auto schoolStr = [](MagicSchool s) -> const char* {
                switch (s) {
                case MagicSchool::Alteration:  return "alteration";
                case MagicSchool::Conjuration: return "conjuration";
                case MagicSchool::Destruction: return "destruction";
                case MagicSchool::Illusion:    return "illusion";
                case MagicSchool::Restoration: return "restoration";
                default:                       return nullptr;
                }
            };
            auto castStr = [&](CastType c) -> const char* {
                switch (c) {
                case CastType::Concentration: return "concentration";
                case CastType::FireAndForget: return "fire_and_forget";
                case CastType::Ritual:        return "ritual";
                default:                      return nullptr;
                }
            };
            const char* sch = schoolStr(school);
            const char* cst = castStr(ct);
            // THE CAST KEY ENDS AT THE RELEASE. GetCastType() keeps reporting
            // FireAndForget through the cast linger â€” a FRAMING device (it
            // parks the camera on the cast profile while the projectile
            // flies) â€” so the 1p noise stayed keyed to the school cell for
            // ~350ms after the spell left the hand, and the burst-tail
            // machinery in CameraNoiseController then pinned its texture for
            // over a second on top. Handing the KEY back at the release makes
            // the 1p texture crossfade arm the moment the spell leaves the
            // hand, with the cast texture as the outgoing layer riding down â€”
            // the same shape as the 3p release hand-off: a live cast (the
            // next charge building, or a concentration stream in the other
            // hand) keeps the cast key.
            //
            // DELIBERATE 1p/3p DIVERGENCE: 3p dropped its session stand-down
            // â€” it made an L-then-R double park the noise through the 1000ms
            // session linger while a single cast faded at once, reported as
            // "inconsistent". 1p KEEPS the session term below because the
            // user approved 1p's feel with it in place. If the same L-then-R
            // inconsistency is ever reported in first person, deleting
            // IsCastSessionActive from this condition is the whole fix.
            const auto isFafType = [](CastType c) {
                return c == CastType::FireAndForget || c == CastType::Ritual;
            };
            const bool fofReleased =
                isFafType(ct) && !sr.IsConcentrationCast() && sr.IsCastLingering() &&
                !sr.IsCastSessionActive() && !sr.IsLiveCasting();
            if (sch && cst && !swim && !sprint && !fofReleased) {
                // Swim/sprint take their bucket-root variant below â€” no
                // per-school swim/sprint cells exist in Categories.
                std::string root = (bucket == Bucket::Magic) ? "magic." : "staves.";
                root += sch;
                root += ".";
                if (sneak) root += "sneak.";
                root += cst;
                return root;
            }
        }

        // Build base + sub-state suffix.
        auto buildKey = [&](const char* root) -> std::string {
            if (swim)   return std::string(root) + ".swim";
            if (sprint) return std::string(root) + ".sprint";
            if (sneak)  return std::string(root) + ".sneak";
            return std::string(root);
        };

        switch (bucket) {
        case Bucket::Sheathed: return buildKey("sheathed");
        case Bucket::Melee:    return buildKey("weapons.melee");
        case Bucket::Magic:    return buildKey("weapons.magic");
        case Bucket::Staves:   return buildKey("weapons.staves");
        case Bucket::Bow:
            // ZOOM OUTRANKS DRAW, and it is checked FIRST for the same reason
            // it outranks it in the camera pickers: you can only zoom while
            // drawn, so the zoom cell is the narrower answer whenever it
            // applies. Without this the 1p FOV and the noise cell would both
            // stay keyed to the draw state through the whole Eagle Eye hold.
            if (zoomed && sneak)  return "weapons.bow.sneak.zoom";
            if (zoomed)           return "weapons.bow.zoom";
            // Drawing-bow + sneak composes: weapons.bow.sneak.draw.
            if (drawing && sneak) return "weapons.bow.sneak.draw";
            if (drawing)          return "weapons.bow.draw";
            return buildKey("weapons.bow");
        case Bucket::Crossbow:
            if (zoomed && sneak)  return "weapons.crossbow.sneak.zoom";
            if (zoomed)           return "weapons.crossbow.zoom";
            if (drawing && sneak) return "weapons.crossbow.sneak.draw";
            if (drawing)          return "weapons.crossbow.draw";
            return buildKey("weapons.crossbow");
        }
        return "sheathed";
    }

    // Composite of the shouts pair (if any) over the regular state key.
    // Walks {shoutPrimary, shoutFallback, regularKey} so the per-shout
    // override beats the per-state shouts base which beats the regular
    // state's profile. The walker (in the public Resolve functions
    // below) picks the first entry whose customize flag is on.
    static FirstPersonKeyPair ResolveFirstPersonKeyPair()
    {
        auto* asState = []() -> RE::ActorState* {
            auto* p = RE::PlayerCharacter::GetSingleton();
            return p ? p->AsActorState() : nullptr;
        }();
        // Same stabilized signal as ResolveFirstPersonKey below.
        const bool sneaking = StateResolver::GetSingleton().IsAttackSneak();
        (void)asState;

        FirstPersonKeyPair out;
        if (auto shoutPair = ResolveFirstPersonShoutsKeys(sneaking)) {
            // Shouts case: primary = per-shout override (may be empty),
            // fallback = per-state base. The regular state key is stored
            // as a deeper fallback by appending it onto the chain â€” but
            // since FirstPersonKeyPair only has two slots, we accept that
            // a customized weapons.<state> profile won't apply during a
            // shout (Categories' 3p shouts behave the same way).
            out.primary  = std::move(shoutPair->primary);
            out.fallback = std::move(shoutPair->fallback);
        } else {
            // Weapon states never fall back to Magic: a melee/staff held with
            // an off-hand spell stays in its weapon state, and if that exact
            // state (incl. sneak/sprint/swim sub-state) has no override it
            // falls through to Global â€” NOT magic. Magic only applies while
            // casting, or when paired with a shield / another spell (with no
            // weapon to claim the state), both handled in ResolveFirstPersonKey.
            out.primary  = ResolveFirstPersonKey();
            out.fallback.clear();
        }
        return out;
    }

    // The exact first-person state the player is physically in (the primary
    // key), independent of which overrides exist â€” used for the Quick Tune
    // header. Boxes label themselves from the profile each channel resolved to.
    std::string SettingsManager::ResolveFirstPersonStateKey()
    {
        const auto keys = ResolveFirstPersonKeyPair();
        return keys.primary.empty() ? keys.fallback : keys.primary;
    }

    SettingsManager::FirstPersonProfile* SettingsManager::ResolveFpMeleeOverride(const std::string& a_key, bool a_fov)
    {
        // Only melee state keys carry weapon overrides. The map is only
        // populated for melee entries, but guard on the prefix so we never
        // classify a weapon for a non-melee state.
        if (a_key.empty() || a_key.rfind("weapons.melee", 0) != 0) return nullptr;
        auto it = stateFpMeleeOverrides.find(a_key);
        if (it == stateFpMeleeOverrides.end()) return nullptr;
        // Custom (mod-added keyword) slot outranks the vanilla-type slot,
        // the 3p rule: more specific wins.
        if (const std::string* ck = CurrentCustomMeleeKeyword()) {
            for (auto& cs : it->second.custom) {
                if (cs.keyword != *ck) continue;
                if (a_fov ? cs.setFov : cs.setNoise) return &cs.profile;
                break;
            }
        }
        const auto wt = ClassifyPlayerMeleeWeapon(RE::PlayerCharacter::GetSingleton());
        const std::size_t w = static_cast<std::size_t>(wt);
        if (w >= kMeleeWeaponCount) return nullptr;
        const bool set = a_fov ? it->second.perWeaponSetFov[w]
                               : it->second.perWeaponSetNoise[w];
        if (!set) return nullptr;
        return &it->second.perWeapon[w];
    }

    SettingsManager::FirstPersonProfile* SettingsManager::ResolveFpBindingProfile(const std::string& a_primaryKey, bool a_fov)
    {
        // --- Shout bindings: while the Shout sub-state is live in a
        // shoutable weapon state, an active MOD-ADDED shout matches by
        // form identity (known shouts never have a binding â€” the bind
        // button refuses them). Sneaking selects the second slot.
        {
            const auto& sr = StateResolver::GetSingleton();
            // Shout Lag holds the sub-state for FRAMING only. The FOV half
            // of this resolver is framing and keeps the hold; the NOISE half
            // must fall through once the natural tail ends, or the shout's
            // own Fade Duration never gets to govern the tail.
            const bool shoutLive = sr.GetSubState() == CameraSubState::Shout &&
                                   (a_fov || !sr.IsShoutLagHolding());
            if (shoutLive && ShoutableIndexFor(sr.GetState())) {
                std::uint32_t fid = 0;
                std::string   plg;
                if (sr.GetActiveModShout(fid, plg)) {
                    auto* player  = RE::PlayerCharacter::GetSingleton();
                    auto* asState = player ? player->AsActorState() : nullptr;
                    const int slot = (asState && asState->IsSneaking()) ? 1 : 0;
                    for (auto& b : weaponBindings) {
                        if (!b.fpOnly || b.category != BindingCategory::Shout) continue;
                        if (b.formID != fid || b.pluginName != plg) continue;
                        auto& p = b.fpProfiles[slot];
                        {
                            // [FP-MERGE] the binding IS the enabler: a half the user touched
                            // applies; an untouched half falls through (so binding a weapon
                            // never pins fov 80).
                            const FirstPersonProfile fpBindDefaults{};
                            const bool touched = a_fov
                                ? (p.worldFov != fpBindDefaults.worldFov || p.handsFov != fpBindDefaults.handsFov ||
                                   p.transitionSpeed != fpBindDefaults.transitionSpeed)
                                : (!(p.noise == fpBindDefaults.noise) || p.repulse != fpBindDefaults.repulse ||
                                   p.repulseFeel != fpBindDefaults.repulseFeel ||
                                   p.fofFadeDuration != fpBindDefaults.fofFadeDuration ||
                                   p.shoutFadeDuration != fpBindDefaults.shoutFadeDuration);
                            return (b.settingsSeeded || touched) ? &p : nullptr;
                        }
                    }
                }
                return nullptr;
            }
        }

        if (a_primaryKey.empty()) return nullptr;

        // --- Equip-based bindings: FP state key -> (category, sub-state
        // slot). Mirrors ResolveStateNoise's binding key map; the magic /
        // staves SCHOOL keys (an active cast) route to the Spell binding's
        // Casting slot / the Staff binding's base slot.
        using BC = BindingCategory;
        struct CatSlot { BC cat; int slot; };
        static const std::unordered_map<std::string, CatSlot> kFpBindingKeyMap = {
            { "weapons.melee",                     { BC::Melee,    0 } },
            { "weapons.melee.sprint",              { BC::Melee,    1 } },
            { "weapons.melee.swim",                { BC::Melee,    2 } },
            { "weapons.melee.attack",              { BC::Melee,    3 } },
            { "weapons.melee.sneak",               { BC::Melee,    4 } },
            { "weapons.melee.power_attack",        { BC::Melee,    5 } },
            { "weapons.melee.sprint_attack",       { BC::Melee,    6 } },
            { "weapons.melee.sprint_power_attack", { BC::Melee,    7 } },
            { "weapons.melee.sneak_attack",        { BC::Melee,    8 } },
            { "weapons.melee.sneak_power_attack",  { BC::Melee,    9 } },
            { "weapons.bow",                       { BC::Bow,      0 } },
            { "weapons.bow.sprint",                { BC::Bow,      1 } },
            { "weapons.bow.draw",                  { BC::Bow,      2 } },
            { "weapons.bow.swim",                  { BC::Bow,      3 } },
            { "weapons.bow.sneak",                 { BC::Bow,      4 } },
            { "weapons.bow.sneak.draw",            { BC::Bow,      5 } },
            { "weapons.bow.zoom",                  { BC::Bow,      6 } },
            { "weapons.bow.sneak.zoom",            { BC::Bow,      7 } },
            { "weapons.crossbow",                  { BC::Crossbow, 0 } },
            { "weapons.crossbow.sprint",           { BC::Crossbow, 1 } },
            { "weapons.crossbow.draw",             { BC::Crossbow, 2 } },
            { "weapons.crossbow.swim",             { BC::Crossbow, 3 } },
            { "weapons.crossbow.sneak",            { BC::Crossbow, 4 } },
            { "weapons.crossbow.sneak.draw",       { BC::Crossbow, 5 } },
            { "weapons.crossbow.zoom",             { BC::Crossbow, 6 } },
            { "weapons.crossbow.sneak.zoom",       { BC::Crossbow, 7 } },
            { "weapons.magic",                     { BC::Spell,    0 } },
            { "weapons.magic.sprint",              { BC::Spell,    1 } },
            { "weapons.magic.swim",                { BC::Spell,    2 } },
            { "weapons.magic.sneak",               { BC::Spell,    4 } },
            { "weapons.staves",                    { BC::Staff,    0 } },
            { "weapons.staves.sprint",             { BC::Staff,    1 } },
            { "weapons.staves.swim",               { BC::Staff,    2 } },
            { "weapons.staves.sneak",              { BC::Staff,    3 } },
            { "weapons.blocking.shield",           { BC::Shield,   0 } },
            { "weapons.blocking.shield.sneak",     { BC::Shield,   1 } },
            { "weapons.blocking.shield.sprint",    { BC::Shield,   2 } },
        };
        BC  cat;
        int slot;
        if (const auto mit = kFpBindingKeyMap.find(a_primaryKey); mit != kFpBindingKeyMap.end()) {
            cat  = mit->second.cat;
            slot = mit->second.slot;
        } else if (a_primaryKey.rfind("magic.", 0) == 0) {
            cat  = BC::Spell;
            slot = 3;   // Casting
        } else if (a_primaryKey.rfind("staves.", 0) == 0) {
            // Staff bindings have no Casting slot (mirrors 3p: the base
            // slot stays authoritative through a staff fire).
            cat  = BC::Staff;
            slot = (a_primaryKey.find(".sneak.") != std::string::npos) ? 3 : 0;
        } else {
            return nullptr;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        // Shield bindings are left-hand-only; everything else tries the
        // RIGHT hand first, then the LEFT (mirrors the 3p binding picker).
        // A matched binding's decision stands even when the requested
        // subsystem isn't customized â€” don't fall through to the other hand.
        const int first = cat == BC::Shield ? 1 : 0;
        for (int c = first; c < 2; ++c) {
            const auto item = ItemBindings::DescribeEquipped(player, c == 1);
            if (auto* match = ItemBindings::FindBest(weaponBindings, static_cast<int>(cat), true, item)) {
                auto& b = *match;
                // Per-binding LOCATION layer (2026-08-15): a place that binds
                // this weapon's FP key outranks the binding's own profile â€”
                // same place-beats-plain precedence as the per-state path,
                // scoped inside the binding's own decision.
                if (auto* lp = ActiveLocationFpProfile(FpBindingLocationKey(b, slot), a_fov))
                    return lp;
                auto& p = b.fpProfiles[slot];
                {
                    // [FP-MERGE] the binding IS the enabler: a half the user touched
                    // applies; an untouched half falls through (so binding a weapon
                    // never pins fov 80).
                    const FirstPersonProfile fpBindDefaults{};
                    const bool touched = a_fov
                        ? (p.worldFov != fpBindDefaults.worldFov || p.handsFov != fpBindDefaults.handsFov ||
                           p.transitionSpeed != fpBindDefaults.transitionSpeed)
                        : (!(p.noise == fpBindDefaults.noise) || p.repulse != fpBindDefaults.repulse ||
                           p.repulseFeel != fpBindDefaults.repulseFeel ||
                           p.fofFadeDuration != fpBindDefaults.fofFadeDuration ||
                           p.shoutFadeDuration != fpBindDefaults.shoutFadeDuration);
                    return (b.settingsSeeded || touched) ? &p : nullptr;
                }
            }
        }
        return nullptr;
    }

    const char* SettingsManager::DescribeFpProfileSource(const FirstPersonProfile* a_p)
    {
        if (!a_p) return "none";
        if (a_p == &firstPersonGlobal) return "global";
        for (auto& [k, prof] : stateFirstPerson)
            if (&prof == a_p) return "state-entry";
        for (auto& [k, ov] : stateFpMeleeOverrides) {
            for (std::size_t w = 0; w < kMeleeWeaponCount; ++w)
                if (&ov.perWeapon[w] == a_p) return "weapon-type-override";
            for (auto& cs : ov.custom)
                if (&cs.profile == a_p) return "custom-type-override";
        }
        for (auto& b : weaponBindings)
            for (std::size_t sl = 0; sl < kWeaponBindingSubStates; ++sl)
                if (&b.fpProfiles[sl] == a_p) return "weapon-binding";
        for (auto& lo : locationOverrides)
            for (auto& [k, prof] : lo.fpState)
                if (&prof == a_p) return "location";
        return "unknown";
    }

    SettingsManager::FirstPersonProfile* SettingsManager::ResolveFirstPersonFovProfile()
    {
        const auto keys = ResolveFirstPersonKeyPair();
        // Specific-form binding wins over everything, then the per-weapon-
        // type override, then the per-state profile.
        if (auto* bp = ResolveFpBindingProfile(keys.primary, /*a_fov=*/true)) return bp;
        if (auto* ov = ResolveFpMeleeOverride(keys.primary,  /*a_fov=*/true)) return ov;
        if (auto* ov = ResolveFpMeleeOverride(keys.fallback, /*a_fov=*/true)) return ov;
        // Location layer: a place (or room) binding this FP key outranks the
        // plain per-state entry but not the specific bindings above â€” same
        // "specific beats place" precedence the 3p enemy-override splice
        // documents.
        if (auto* lp = ActiveLocationFpProfile(keys.primary,  /*a_fov=*/true)) return lp;
        if (auto* lp = ActiveLocationFpProfile(keys.fallback, /*a_fov=*/true)) return lp;
        // [FP-MERGE] presence is the bind — but a value-UNTOUCHED entry
        // resolves to the GLOBAL POINTER, exactly like pre-merge. Without
        // this, every state flip swapped the resolved pointer between
        // value-identical entries and everything keyed on profile identity
        // re-triggered ("transitions in first person are stuttering",
        // 2026-08-31).
        const auto fovEq = [](const FirstPersonProfile& a, const FirstPersonProfile& b) {
            return a.worldFov == b.worldFov && a.handsFov == b.handsFov;
        };
        FirstPersonProfile* prim = nullptr;
        FirstPersonProfile* fall = nullptr;
        if (!keys.primary.empty())
            if (auto it = stateFirstPerson.find(keys.primary); it != stateFirstPerson.end())
                prim = &it->second;
        if (!keys.fallback.empty())
            if (auto it = stateFirstPerson.find(keys.fallback); it != stateFirstPerson.end())
                fall = &it->second;
        // Collapse UP the chain while values agree, so a state-flag flap
        // between value-identical keys keeps ONE stable pointer (the
        // stutter rule, 2026-08-31).
        FirstPersonProfile* pick = prim ? prim : fall;
        if (pick == prim && prim && fall && fovEq(*prim, *fall)) pick = fall;
        if (pick && fovEq(*pick, firstPersonGlobal) &&
            pick->transitionSpeed == firstPersonGlobal.transitionSpeed)
            pick = &firstPersonGlobal;
        return pick ? pick : &firstPersonGlobal;
    }

    SettingsManager::FirstPersonProfile* SettingsManager::ResolveFirstPersonNoiseProfile()
    {
        const auto keys = ResolveFirstPersonKeyPair();
        // Specific-form binding wins over everything, then the per-weapon-
        // type override, then the per-state profile.
        if (auto* bp = ResolveFpBindingProfile(keys.primary, /*a_fov=*/false)) return bp;
        if (auto* ov = ResolveFpMeleeOverride(keys.primary,  /*a_fov=*/false)) return ov;
        if (auto* ov = ResolveFpMeleeOverride(keys.fallback, /*a_fov=*/false)) return ov;
        // Location layer â€” see the FOV twin above for the precedence note.
        if (auto* lp = ActiveLocationFpProfile(keys.primary,  /*a_fov=*/false)) return lp;
        if (auto* lp = ActiveLocationFpProfile(keys.fallback, /*a_fov=*/false)) return lp;
        // One-time-per-state log so we can verify shouts routing in
        // the field. Logs the key chain the resolver settled on and
        // which profile won (per-shout / per-state-base / global).
        static std::string sLastLoggedPrimary;
        static std::string sLastLoggedFallback;
        if (keys.primary != sLastLoggedPrimary || keys.fallback != sLastLoggedFallback) {
            sLastLoggedPrimary  = keys.primary;
            sLastLoggedFallback = keys.fallback;
            const char* winner = "Global";
            if (!keys.primary.empty()) {
                auto it = stateFirstPerson.find(keys.primary);
                if (it != stateFirstPerson.end()) winner = "Primary";
            }
            if (std::strcmp(winner, "Global") == 0 && !keys.fallback.empty()) {
                auto it = stateFirstPerson.find(keys.fallback);
                if (it != stateFirstPerson.end()) winner = "Fallback";
            }
            spdlog::debug("[FP Noise] primary='{}' fallback='{}' winner={}",
                         keys.primary, keys.fallback, winner);
        }
        // [FP-MERGE] presence is the bind; untouched noise resolves to the
        // GLOBAL pointer (see the FOV twin above for the stutter rationale).
        const auto noiseEq = [](const FirstPersonProfile& a, const FirstPersonProfile& b) {
            return a.noise == b.noise && a.repulse == b.repulse &&
                   a.repulseFeel == b.repulseFeel &&
                   a.fofFadeDuration == b.fofFadeDuration &&
                   a.shoutFadeDuration == b.shoutFadeDuration;
        };
        FirstPersonProfile* prim = nullptr;
        FirstPersonProfile* fall = nullptr;
        if (!keys.primary.empty())
            if (auto it = stateFirstPerson.find(keys.primary); it != stateFirstPerson.end())
                prim = &it->second;
        if (!keys.fallback.empty())
            if (auto it = stateFirstPerson.find(keys.fallback); it != stateFirstPerson.end())
                fall = &it->second;
        FirstPersonProfile* pick = prim ? prim : fall;
        if (pick == prim && prim && fall && noiseEq(*prim, *fall)) pick = fall;
        if (pick && noiseEq(*pick, firstPersonGlobal)) pick = &firstPersonGlobal;
        return pick ? pick : &firstPersonGlobal;
    }

    std::vector<SettingsManager::IndoorEligibleProfile>
    SettingsManager::GetIndoorEligibleProfiles()
    {
        // Categories core + Shouts. NO target-lock, dialogue, vanity, etc.
        // TOML keys mirror the outdoor save/load layout so the indoor table
        // is a structural twin under "indoor.*".
        std::vector<IndoorEligibleProfile> out;
        out.reserve(900);

        const auto add = [&](std::string k, CameraProfile* p) {
            out.push_back({ std::move(k), p });
        };

        // --- Sheathed ---
        add("sheathed",        &sheathed);
        add("sheathed.sprint", &sheathedSprint);
        add("sheathed.swim",   &sheathedSwim);
        add("sheathed.sneak",  &sheathedSneak);

        // --- Weapons: Melee ---
        add("weapons.melee",         &weaponsMelee);
        add("weapons.melee.sprint",  &weaponsMeleeSprint);
        add("weapons.melee.swim",    &weaponsMeleeSwim);
        add("weapons.melee.attack",  &weaponsMeleeAttack);
        add("weapons.melee.sneak",   &weaponsMeleeSneak);
        add("weapons.melee.power_attack",        &weaponsMeleePowerAttack);
        add("weapons.melee.sneak_attack",        &weaponsMeleeSneakAttack);
        add("weapons.melee.sneak_power_attack",  &weaponsMeleeSneakPowerAttack);
        add("weapons.melee.sprint_attack",       &weaponsMeleeSprintAttack);
        add("weapons.melee.sprint_power_attack", &weaponsMeleeSprintPowerAttack);

        // --- Weapons: Melee per-weapon-type overrides ---
        // 8 weapons * 7 sub-states = 56 profiles. Registering them here
        // makes the Outdoor / Indoor toggle apply to per-weapon overrides
        // too (UI edits route through EditTarget; runtime through
        // ResolveByEnv) â€” same semantics as the base melee profiles above.
        auto addMeleeOv = [&](const std::string& basePath, MeleeWeaponOverrides& ov) {
            for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
                const auto wt = static_cast<MeleeWeaponType>(i);
                add(basePath + ".overrides." + MeleeWeaponTypeTomlKey(wt),
                    &ov.perWeapon[i]);
            }
        };
        addMeleeOv("weapons.melee",             weaponsMeleeOverrides);
        addMeleeOv("weapons.melee.sprint",      weaponsMeleeSprintOverrides);
        addMeleeOv("weapons.melee.swim",        weaponsMeleeSwimOverrides);
        addMeleeOv("weapons.melee.attack",      weaponsMeleeAttackOverrides);
        addMeleeOv("weapons.melee.sneak",       weaponsMeleeSneakOverrides);
        addMeleeOv("weapons.melee.shout",       weaponsMeleeShoutOverrides);
        addMeleeOv("weapons.melee.shout.sneak", weaponsMeleeShoutSneakOverrides);
        addMeleeOv("weapons.melee.power_attack",        weaponsMeleePowerAttackOverrides);
        addMeleeOv("weapons.melee.sneak_attack",        weaponsMeleeSneakAttackOverrides);
        addMeleeOv("weapons.melee.sneak_power_attack",  weaponsMeleeSneakPowerAttackOverrides);
        addMeleeOv("weapons.melee.sprint_attack",       weaponsMeleeSprintAttackOverrides);
        addMeleeOv("weapons.melee.sprint_power_attack", weaponsMeleeSprintPowerAttackOverrides);

        // --- Directional power-attack overrides (Categories) ---
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            add(std::string("weapons.melee.power_attack.dir.") + kPADirKeys[d],
                &weaponsMeleePowerAttackDir[d]);
        }

        // --- Blocking (9 sub-states) ---
        add("weapons.blocking.one_handed",        &weaponsBlockingOneHanded);
        add("weapons.blocking.two_handed",        &weaponsBlockingTwoHanded);
        add("weapons.blocking.shield",            &weaponsBlockingShield);
        add("weapons.blocking.ward",              &weaponsBlockingWard);
        add("weapons.blocking.one_handed.sneak",  &weaponsBlockingOneHandedSneak);
        add("weapons.blocking.two_handed.sneak",  &weaponsBlockingTwoHandedSneak);
        add("weapons.blocking.shield.sneak",      &weaponsBlockingShieldSneak);
        add("weapons.blocking.ward.sneak",        &weaponsBlockingWardSneak);
        add("weapons.blocking.shield.sprint",     &weaponsBlockingShieldSprint);

        // --- Bow ---
        add("weapons.bow",            &weaponsBow);
        add("weapons.bow.sprint",     &weaponsBowSprint);
        add("weapons.bow.swim",       &weaponsBowSwim);
        add("weapons.bow.draw",       &weaponsBowDraw);
        add("weapons.bow.sneak",      &weaponsBowSneak);
        add("weapons.bow.sneak.draw", &weaponsBowSneakDraw);
        add("weapons.bow.zoom", &weaponsBowZoom);
        add("weapons.bow.sneak.zoom", &weaponsBowSneakZoom);

        // --- Crossbow ---
        add("weapons.crossbow",            &weaponsCrossbow);
        add("weapons.crossbow.sprint",     &weaponsCrossbowSprint);
        add("weapons.crossbow.swim",       &weaponsCrossbowSwim);
        add("weapons.crossbow.draw",       &weaponsCrossbowDraw);
        add("weapons.crossbow.sneak",      &weaponsCrossbowSneak);
        add("weapons.crossbow.sneak.draw", &weaponsCrossbowSneakDraw);
        add("weapons.crossbow.zoom", &weaponsCrossbowZoom);
        add("weapons.crossbow.sneak.zoom", &weaponsCrossbowSneakZoom);

        // --- Magic ---
        add("weapons.magic",        &weaponsMagic);
        add("weapons.magic.sprint", &weaponsMagicSprint);
        add("weapons.magic.swim",   &weaponsMagicSwim);
        add("weapons.magic.sneak",  &weaponsMagicSneak);

        add("magic.alteration.concentration",   &magicAlterationConcentration);
        add("magic.alteration.fire_and_forget", &magicAlterationFireAndForget);
        add("magic.alteration.ritual",          &magicAlterationRitual);
        add("magic.conjuration.concentration",   &magicConjurationConcentration);
        add("magic.conjuration.fire_and_forget", &magicConjurationFireAndForget);
        add("magic.conjuration.ritual",          &magicConjurationRitual);
        add("magic.destruction.concentration",   &magicDestructionConcentration);
        add("magic.destruction.fire_and_forget", &magicDestructionFireAndForget);
        add("magic.destruction.ritual",          &magicDestructionRitual);
        add("magic.illusion.concentration",   &magicIllusionConcentration);
        add("magic.illusion.fire_and_forget", &magicIllusionFireAndForget);
        add("magic.illusion.ritual",          &magicIllusionRitual);
        add("magic.restoration.concentration",   &magicRestorationConcentration);
        add("magic.restoration.fire_and_forget", &magicRestorationFireAndForget);
        add("magic.restoration.ritual",          &magicRestorationRitual);

        add("magic.alteration.sneak.concentration",   &magicAlterationConcentrationSneak);
        add("magic.alteration.sneak.fire_and_forget", &magicAlterationFireAndForgetSneak);
        add("magic.alteration.sneak.ritual",          &magicAlterationRitualSneak);
        add("magic.conjuration.sneak.concentration",   &magicConjurationConcentrationSneak);
        add("magic.conjuration.sneak.fire_and_forget", &magicConjurationFireAndForgetSneak);
        add("magic.conjuration.sneak.ritual",          &magicConjurationRitualSneak);
        add("magic.destruction.sneak.concentration",   &magicDestructionConcentrationSneak);
        add("magic.destruction.sneak.fire_and_forget", &magicDestructionFireAndForgetSneak);
        add("magic.destruction.sneak.ritual",          &magicDestructionRitualSneak);
        add("magic.illusion.sneak.concentration",   &magicIllusionConcentrationSneak);
        add("magic.illusion.sneak.fire_and_forget", &magicIllusionFireAndForgetSneak);
        add("magic.illusion.sneak.ritual",          &magicIllusionRitualSneak);
        add("magic.restoration.sneak.concentration",   &magicRestorationConcentrationSneak);
        add("magic.restoration.sneak.fire_and_forget", &magicRestorationFireAndForgetSneak);
        add("magic.restoration.sneak.ritual",          &magicRestorationRitualSneak);

        // --- Magic per-hand overrides (Categories grid) ---
        // Keyed "<schoolKey>[.sneak].<castKey>.hand.<left|both|right>" so the
        // hand profiles ride the same indoor-variant + noise-key machinery
        // as their base entries (ResolveStateNoise strips the ".hand." tail
        // when mapping these pointers to a noise state key).
        for (std::size_t si = 0; si < 5; ++si)
            for (std::size_t ci = 0; ci < 3; ++ci)
                for (std::size_t sn = 0; sn < 2; ++sn)
                    for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                        add(std::string("magic.") + GetMagicSchoolTomlKey(si) +
                                (sn ? ".sneak." : ".") + GetMagicCastTomlKey(ci) +
                                ".hand." + GetMagicHandTomlKey(h),
                            &magicHandOverrides[si][ci][sn].profiles[h]);
                    }

        // --- Staves ---
        add("weapons.staves",        &weaponsStaves);
        add("weapons.staves.sprint", &weaponsStavesSprint);
        add("weapons.staves.swim",   &weaponsStavesSwim);
        add("weapons.staves.sneak",  &weaponsStavesSneak);

        add("staves.alteration.concentration",   &stavesAlterationConcentration);
        add("staves.alteration.fire_and_forget", &stavesAlterationFireAndForget);
        add("staves.conjuration.concentration",   &stavesConjurationConcentration);
        add("staves.conjuration.fire_and_forget", &stavesConjurationFireAndForget);
        add("staves.destruction.concentration",   &stavesDestructionConcentration);
        add("staves.destruction.fire_and_forget", &stavesDestructionFireAndForget);
        add("staves.illusion.concentration",   &stavesIllusionConcentration);
        add("staves.illusion.fire_and_forget", &stavesIllusionFireAndForget);
        add("staves.restoration.concentration",   &stavesRestorationConcentration);
        add("staves.restoration.fire_and_forget", &stavesRestorationFireAndForget);

        add("staves.alteration.sneak.concentration",   &stavesAlterationConcentrationSneak);
        add("staves.alteration.sneak.fire_and_forget", &stavesAlterationFireAndForgetSneak);
        add("staves.conjuration.sneak.concentration",   &stavesConjurationConcentrationSneak);
        add("staves.conjuration.sneak.fire_and_forget", &stavesConjurationFireAndForgetSneak);
        add("staves.destruction.sneak.concentration",   &stavesDestructionConcentrationSneak);
        add("staves.destruction.sneak.fire_and_forget", &stavesDestructionFireAndForgetSneak);
        add("staves.illusion.sneak.concentration",   &stavesIllusionConcentrationSneak);
        add("staves.illusion.sneak.fire_and_forget", &stavesIllusionFireAndForgetSneak);
        add("staves.restoration.sneak.concentration",   &stavesRestorationConcentrationSneak);
        add("staves.restoration.sneak.fire_and_forget", &stavesRestorationFireAndForgetSneak);

        // --- Transformations ---
        add("transformations.werewolf",        &transformationsWerewolf);
        add("transformations.werewolf.sheathed", &transformationsWerewolfSheathed);
        add("transformations.werewolf.sprint", &transformationsWerewolfSprint);
        add("transformations.werewolf.swim",   &transformationsWerewolfSwim);
        add("transformations.werewolf.attack", &transformationsWerewolfAttack);
        add("transformations.werewolf.power_attack",        &transformationsWerewolfPowerAttack);
        add("transformations.werewolf.sprint_power_attack", &transformationsWerewolfSprintPowerAttack);
        add("transformations.werewolf.roar",   &transformationsWerewolfRoar);
        add("transformations.werewolf.feeding", &transformationsWerewolfFeeding);

        add("transformations.vampire_lord.sheathed",            &vampireLordSheathed);
        add("transformations.vampire_lord.sheathed.levitating", &vampireLordSheathedLevitating);
        add("transformations.vampire_lord.melee",               &vampireLordMelee);
        add("transformations.vampire_lord.melee.attack",        &vampireLordMeleeAttack);
        add("transformations.vampire_lord.melee.power_attack",  &vampireLordMeleePowerAttack);
        add("transformations.vampire_lord.magic",               &vampireLordMagic);
        add("transformations.vampire_lord.concentration",       &vampireLordConcentration);
        add("transformations.vampire_lord.fire_and_forget",     &vampireLordFireAndForget);
        add("transformations.vampire_lord.sprint",              &vampireLordSprint);
        add("transformations.vampire_lord.sprint.levitating",   &vampireLordSprintLevitating);

        // --- Mounts ---
        add("mounts.horseback",         &mountsHorseback);
        add("mounts.horseback.sprint",  &mountsHorsebackSprint);
        add("mounts.horseback.swim",    &mountsHorsebackSwim);
        add("mounts.horseback.melee",   &mountsHorsebackMelee);
        add("mounts.horseback.archery", &mountsHorsebackArchery);
        add("mounts.horseback.archery.zoom", &mountsHorsebackArcheryZoom);
        add("mounts.horseback.melee.attack_left",  &mountsHorsebackMeleeLeft);
        add("mounts.horseback.melee.attack_right", &mountsHorsebackMeleeRight);
        add("mounts.dragon_riding",     &mountsDragonRiding);
        // One add() each buys the indoor variant, the per-location override
        // slots AND the Camera Noise cell â€” see the note on this function.
        add("mounts.dragon_riding.perched",  &mountsDragonRidingPerched);
        add("mounts.dragon_riding.hovering", &mountsDragonRidingHovering);
        add("mounts.dragon_riding.takeoff",  &mountsDragonRidingTakeoff);
        add("mounts.dragon_riding.landing",  &mountsDragonRidingLanding);
        add("mounts.dragon_riding.attack.grounded", &mountsDragonRidingAttackGrounded);
        add("mounts.dragon_riding.attack.hovering", &mountsDragonRidingAttackHovering);
        add("mounts.dragon_riding.attack.flying",   &mountsDragonRidingAttackFlying);
        add("mounts.dragon_riding.breath.grounded", &mountsDragonRidingBreathGrounded);
        add("mounts.dragon_riding.breath.hovering", &mountsDragonRidingBreathHovering);
        add("mounts.dragon_riding.breath.flying",   &mountsDragonRidingBreathFlying);

        // --- Shouts (per-state base + per-(state, shout) overrides) ---
        // Indices match shoutsBaseByState[i] and the X-macro above.
        static constexpr const char* kShoutStateKeys[6] = {
            "sheathed", "melee", "bow", "crossbow", "magic", "staves"
        };
        for (std::size_t i = 0; i < kShoutableStateCount; ++i) {
            // Per-state base (non-sneak + sneak)
            add(std::string("shouts.base.") + kShoutStateKeys[i],          &shoutsBaseByState[i]);
            add(std::string("shouts.base.") + kShoutStateKeys[i] + ".sneak", &shoutsBaseByStateSneak[i]);

            // Per-(state, shout) overrides for both sneak modes.
            // KEYED BY NAME, NOT INDEX (2026-08-25). These tomlKeys are the
            // persisted identity of a shout's indoor twin, its per-location
            // override slot, its Camera Noise cell and its 1p slot. They used
            // to be the shout's position in kShouts, which is declared in
            // ALPHABETICAL order — so adding any shout would have landed it
            // mid-list and silently re-pointed all four of those onto a
            // different shout, while the outdoor profiles (already keyed by
            // entry.tomlKey in ApplyTable/BuildSaveTable) stayed put.
            // See PRESET-COMPAT.md.
            for (std::size_t s = 0; s < kShoutCount; ++s) {
                add(std::string("shouts.override.") + kShoutStateKeys[i] + "." + kShouts[s].tomlKey,
                    &shoutOverrideByState[i][s]);
                add(std::string("shouts.override.") + kShoutStateKeys[i] + ".sneak." + kShouts[s].tomlKey,
                    &shoutOverrideByStateSneak[i][s]);
            }
        }

        // --- Target Lock (tl* tree) ---
        // Same fallback chain as outdoor â€” every TL slot gets an indoor twin
        // so locked combat indoors can have its own tuning. Picker logic is
        // shared via ResolveByEnv: when indoorMode is true, the substitution
        // happens regardless of whether the resolved profile came from the
        // outdoor or TL tree.
#define DDC_X(name, key, field) if constexpr (TLSlot::name <= TLSlot::MountsHorsebackArcheryDraw) add(std::string("target_lock.") + key, &field);
        DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X

        // TL Magic per-hand overrides â€” same key scheme as the Categories
        // grid, under the target_lock. prefix (TL slots are env-eligible).
        for (std::size_t si = 0; si < 5; ++si)
            for (std::size_t ci = 0; ci < 3; ++ci)
                for (std::size_t sn = 0; sn < 2; ++sn)
                    for (std::size_t h = 0; h < kMagicHandCount; ++h) {
                        add(std::string("target_lock.magic.") + GetMagicSchoolTomlKey(si) +
                                (sn ? ".sneak." : ".") + GetMagicCastTomlKey(ci) +
                                ".hand." + GetMagicHandTomlKey(h),
                            &tlMagicHandOverrides[si][ci][sn].profiles[h]);
                    }

        // TL Shouts â€” per-state base + per-(state, shout) overrides.
        for (std::size_t i = 0; i < kShoutableStateCount; ++i) {
            // base already covered by the X-macro list above (ShoutsBaseSheathed
            // etc.). Per-shout overrides aren't in the X-macro, so append
            // them here.
            // Name-keyed for the same reason as the Categories twins above.
            for (std::size_t s = 0; s < kShoutCount; ++s) {
                add(std::string("target_lock.shouts.override.") + kShoutStateKeys[i] + "." + kShouts[s].tomlKey,
                    &tlShoutOverrideByState[i][s]);
                add(std::string("target_lock.shouts.override.") + kShoutStateKeys[i] + ".sneak." + kShouts[s].tomlKey,
                    &tlShoutOverrideByStateSneak[i][s]);
            }
        }

        // --- TL directional power-attacks (2026-08-16, "location overrides
        // on every single entry") â€” the Categories dir profiles were always
        // eligible; the TL twins weren't, so selecting a direction on the
        // Target Lock page hid its Location button. APPENDED AT THE END
        // deliberately: the indoor storage and every location profileSet are
        // index-mapped off this list, so appending keeps every existing
        // index stable (the preset-stability rule).
        for (std::size_t d = 0; d < kPowerAttackDirectionCount; ++d) {
            add(std::string("target_lock.weapons.melee.power_attack.dir.") + kPADirKeys[d],
                &tlWeaponsMeleePowerAttackDir[d]);
        }

        // --- Paragliding (2026-08-16, user request) -------------------------
        // The paraglide camera + target-lock profiles are ordinary
        // CameraProfiles that simply lived outside this list, so they had no
        // location slot and RenderLocationOverrideButton self-hid on them
        // (that outdoorIdxMap lookup IS the "why is the button missing"
        // mechanism). Registering them here grants BOTH the location slots and
        // an Outdoor/Indoor variant â€” the variant is seeded as a copy of the
        // outdoor profile in the rebuild below, so nothing changes for an
        // existing preset until the user diverges them.
        // APPENDED AT THE END, same reason as the TL directions above.
        add("cinematic.paraglide.camera",      &paraglideProfile);
        add("cinematic.paraglide.target_lock", &paraglideTLProfile);

        add("mounts.horseback.archery.draw", &mountsHorsebackArcheryDraw);

        // Format 2 staff rituals append after every format-1 environment
        // slot, keeping existing indoor/location indices stable.
        add("staves.alteration.ritual", &stavesAlterationRitual);
        add("staves.conjuration.ritual", &stavesConjurationRitual);
        add("staves.destruction.ritual", &stavesDestructionRitual);
        add("staves.illusion.ritual", &stavesIllusionRitual);
        add("staves.restoration.ritual", &stavesRestorationRitual);
        add("staves.alteration.sneak.ritual", &stavesAlterationRitualSneak);
        add("staves.conjuration.sneak.ritual", &stavesConjurationRitualSneak);
        add("staves.destruction.sneak.ritual", &stavesDestructionRitualSneak);
        add("staves.illusion.sneak.ritual", &stavesIllusionRitualSneak);
        add("staves.restoration.sneak.ritual", &stavesRestorationRitualSneak);
#define DDC_X(name, key, field) if constexpr (TLSlot::name > TLSlot::MountsHorsebackArcheryDraw) add(std::string("target_lock.") + key, &field);
        DDC_TL_SLOT_LIST(DDC_X)
#undef DDC_X

        return out;
    }

    void SettingsManager::InitIndoorOverrides()
    {
        CameraController::InvalidateProfileReferences();
        const auto eligible = GetIndoorEligibleProfiles();

        // Pre-reserve so addresses-of-elements stay stable. push_back to a
        // vector that has no spare capacity left would reallocate â€” and
        // that's exactly what we can't have, since outdoorIdxMap stores
        // indices into this storage that must remain dereferenceable.
        //
        // The indoor storage and every location override are built from the
        // SAME eligible list in the same order, so one index map addresses
        // all of them.
        indoorProfilesStorage.clear();
        indoorProfilesStorage.reserve(eligible.size());
        outdoorIdxMap.clear();
        outdoorIdxMap.reserve(eligible.size());
        idxToOutdoor.clear();
        idxToOutdoor.reserve(eligible.size());

        for (const auto& e : eligible) {
            outdoorIdxMap.emplace(e.outdoor, indoorProfilesStorage.size());
            idxToOutdoor.push_back(e.outdoor);
            // Default = a copy of the outdoor profile so a fresh install
            // behaves identically in every environment until the user
            // diverges them on the Indoor tab.
            indoorProfilesStorage.push_back(*e.outdoor);
        }
    }

    CameraProfile* SettingsManager::VariantOf(CameraProfile* outdoor, int a_env)
    {
        if (!outdoor) return outdoor;
        if (a_env != kEnvIndoor) return outdoor;
        const auto it = outdoorIdxMap.find(outdoor);
        if (it == outdoorIdxMap.end()) return outdoor;
        if (it->second >= indoorProfilesStorage.size()) return outdoor;   // pre-Init call
        return &indoorProfilesStorage[it->second];
    }

    CameraProfile* SettingsManager::OutdoorOf(CameraProfile* p)
    {
        if (!p) return p;
        // If p is already in the outdoor map, it's an outdoor profile.
        if (outdoorIdxMap.find(p) != outdoorIdxMap.end()) return p;
        return CanonicalLocationProfile(p, indoorProfilesStorage, locationOverrides, idxToOutdoor);
    }

    const Defaults::SliderRange& SettingsManager::ZoomRangeFor(CameraProfile* a_p)
    {
        if (!a_p) return Defaults::Zoom;
        // Normalise first: an indoor variant or a location override of a mount
        // profile is still a mount profile and must edit with the same range,
        // or the same entry would clamp differently on the Indoor tab than on
        // the Outdoor one.
        CameraProfile* base = OutdoorOf(a_p);

        if (base == &mountsDragonRiding) return Defaults::DragonZoom;
        for (auto* d : DragonRidingSubStateProfiles())
            if (base == d) return Defaults::DragonZoom;

        // Horseback: the six camera profiles and their target-lock twins. Any
        // mount profile added later must be listed here too â€” the range is not
        // derived from anything, so a missing row silently keeps the old floor.
        CameraProfile* const kMountProfiles[] = {
            &mountsHorseback,             &mountsHorsebackSprint,
            &mountsHorsebackSwim,         &mountsHorsebackMelee,
            &mountsHorsebackArchery,      &mountsHorsebackArcheryZoom, &mountsHorsebackArcheryDraw,
            &mountsHorsebackMeleeLeft,    &mountsHorsebackMeleeRight,
            &tlMountsHorseback,           &tlMountsHorsebackSprint,
            &tlMountsHorsebackSwim,       &tlMountsHorsebackMelee,
            &tlMountsHorsebackArchery,    &tlMountsHorsebackArcheryZoom, &tlMountsHorsebackArcheryDraw,
            &tlMountsHorsebackMeleeLeft,  &tlMountsHorsebackMeleeRight,
        };
        for (auto* m : kMountProfiles)
            if (base == m) return Defaults::MountZoom;

        return Defaults::Zoom;
    }

    CameraProfile* SettingsManager::ResolveByEnv(CameraProfile* outdoor)
    {
        // A location override is more specific than the environment, so it is
        // asked first. It answers per ENTRY, not per place: a place that has
        // nothing to say about this profile falls straight through to the
        // Outdoor/Indoor variant, which is what keeps a lightly-tuned place
        // from flattening everything else the user configured.
        if (auto* lp = ActiveLocationVariantOf(outdoor, RuntimeEnv())) return lp;
        return VariantOf(outdoor, RuntimeEnv());
    }

    CameraProfile* SettingsManager::LocationSlotOf(CameraProfile* outdoor, int a_idx, int a_env)
    {
        if (!outdoor) return nullptr;
        if (a_idx < 0 || a_idx >= static_cast<int>(locationOverrides.size())) return nullptr;
        const auto it = outdoorIdxMap.find(outdoor);
        if (it == outdoorIdxMap.end()) return nullptr;
        auto& lo = locationOverrides[static_cast<std::size_t>(a_idx)];
        auto& profs = lo.ProfilesFor(a_env);
        if (it->second >= profs.size()) return nullptr;
        return &profs[it->second];
    }

    bool SettingsManager::LocationSlotEnabled(CameraProfile* outdoor, int a_idx, int a_env)
    {
        if (!outdoor) return false;
        if (a_idx < 0 || a_idx >= static_cast<int>(locationOverrides.size())) return false;
        const auto it = outdoorIdxMap.find(outdoor);
        if (it == outdoorIdxMap.end()) return false;
        auto& lo = locationOverrides[static_cast<std::size_t>(a_idx)];
        const auto& set = lo.ProfileSetFor(a_env);
        return it->second < set.size() && set[it->second];
    }

    void SettingsManager::SetLocationSlotEnabled(CameraProfile* outdoor, int a_idx, bool a_on, int a_env)
    {
        if (!outdoor) return;
        if (a_idx < 0 || a_idx >= static_cast<int>(locationOverrides.size())) return;
        const auto it = outdoorIdxMap.find(outdoor);
        if (it == outdoorIdxMap.end()) return;
        auto& lo = locationOverrides[static_cast<std::size_t>(a_idx)];
        auto& set = lo.ProfileSetFor(a_env);
        if (it->second >= set.size()) return;
        if (set[it->second] != a_on) {
            set[it->second] = a_on;
            LocationDetector::GetSingleton().Invalidate();
        }
    }

    int SettingsManager::ActiveLocationOwnerOf(CameraProfile* outdoor, int a_env)
    {
        if (!locationOverridesEnabled || !outdoor) return -1;
        const auto it = outdoorIdxMap.find(outdoor);
        if (it == outdoorIdxMap.end()) return -1;
        // Narrowest first: the first place that BINDS this entry owns it.
        for (int idx : activeLocationChain) {
            if (idx < 0 || idx >= static_cast<int>(locationOverrides.size())) continue;
            auto& lo = locationOverrides[static_cast<std::size_t>(idx)];
            if (!lo.enabled) continue;
            const auto& set = lo.ProfileSetFor(a_env);
            if (it->second < set.size() && set[it->second]) return idx;
        }
        return -1;
    }

    int SettingsManager::LocationOwnerOfResolvedProfile(const CameraProfile* a_profile) const
    {
        return locationOverridesEnabled ? ResolvedLocationProfileOwner(a_profile,
            activeLocationChain, locationOverrides, indoorMode) : -1;
    }

    CameraProfile* SettingsManager::ActiveLocationVariantOf(CameraProfile* outdoor, int a_env)
    {
        const int idx = ActiveLocationOwnerOf(outdoor, a_env);
        if (idx < 0) return nullptr;
        const auto it = outdoorIdxMap.find(outdoor);
        return &locationOverrides[static_cast<std::size_t>(idx)].ProfilesFor(a_env)[it->second];
    }

    int SettingsManager::ActiveLocationNoiseOwnerOf(const std::string& a_key)
    {
        if (!locationOverridesEnabled) return -1;
        for (int idx : activeLocationChain) {
            if (idx < 0 || idx >= static_cast<int>(locationOverrides.size())) continue;
            auto& lo = locationOverrides[static_cast<std::size_t>(idx)];
            if (!lo.enabled) continue;
            const auto it = lo.stateNoise.find(a_key);
            if (it != lo.stateNoise.end() && it->second.enabled) return idx;
        }
        return -1;
    }

    SettingsManager::NoiseProfile* SettingsManager::ActiveLocationStateNoise(const std::string& key)
    {
        const int idx = ActiveLocationNoiseOwnerOf(key);
        if (idx < 0) return nullptr;
        return &locationOverrides[static_cast<std::size_t>(idx)].stateNoise[key];
    }

    SettingsManager::NoiseProfile* SettingsManager::ActiveLocationGlobalNoise()
    {
        auto* lo = ActiveLocation();
        if (!lo || !lo->globalNoiseSet) return nullptr;
        return &lo->globalNoise;
    }

    int SettingsManager::ActiveLocationFxBeatOwnerOf(const std::string& a_key)
    {
        if (!locationOverridesEnabled || a_key.empty()) return -1;
        for (int idx : activeLocationChain) {
            if (idx < 0 || idx >= static_cast<int>(locationOverrides.size())) continue;
            auto& lo = locationOverrides[static_cast<std::size_t>(idx)];
            if (!lo.enabled) continue;
            if (lo.fxBeats.find(a_key) != lo.fxBeats.end()) return idx;
        }
        return -1;
    }

    SettingsManager::BeatTuning* SettingsManager::ActiveLocationFxBeat(const std::string& a_key)
    {
        const int idx = ActiveLocationFxBeatOwnerOf(a_key);
        if (idx < 0) return nullptr;
        return &locationOverrides[static_cast<std::size_t>(idx)].fxBeats[a_key];
    }

    int SettingsManager::ActiveLocationFpOwnerOf(const std::string& a_key, bool a_fov)
    {
        if (!locationOverridesEnabled || a_key.empty()) return -1;
        for (int idx : activeLocationChain) {
            if (idx < 0 || idx >= static_cast<int>(locationOverrides.size())) continue;
            auto& lo = locationOverrides[static_cast<std::size_t>(idx)];
            if (!lo.enabled) continue;
            const auto it = lo.fpState.find(a_key);
            if (it == lo.fpState.end()) continue;
            // The channel gate keeps its usual meaning: a place that only
            // customizes noise must not claim the FOV channel, and vice
            // versa â€” the other channel falls through to the plain entry.
            // [FP-MERGE] presence claims BOTH channels now (entries are
            // whole fov+noise bindings, seeded on creation).
            return idx;
        }
        return -1;
    }

    SettingsManager::FirstPersonProfile*
    SettingsManager::ActiveLocationFpProfile(const std::string& a_key, bool a_fov)
    {
        const int idx = ActiveLocationFpOwnerOf(a_key, a_fov);
        if (idx < 0) return nullptr;
        return &locationOverrides[static_cast<std::size_t>(idx)].fpState[a_key];
    }

    void SettingsManager::SizeLocationOverride(LocationOverride& a_lo)
    {
        // Sized off outdoorIdxMap, which InitIndoorOverrides has already built
        // from the eligible list. Only grows, so calling it again after a
        // reload can't drop values the user has already set.
        const std::size_t n = indoorProfilesStorage.size();
        if (a_lo.profiles.size()         < n) a_lo.profiles.resize(n);
        if (a_lo.profileSet.size()       < n) a_lo.profileSet.resize(n, false);
        if (a_lo.profilesIndoor.size()   < n) a_lo.profilesIndoor.resize(n);
        if (a_lo.profileSetIndoor.size() < n) a_lo.profileSetIndoor.resize(n, false);
    }

    SettingsManager::NoiseProfile& SettingsManager::GlobalNoiseFor(int a_env)
    {
        return a_env == kEnvIndoor ? globalNoiseIndoor : globalNoise;
    }

    std::unordered_map<std::string, SettingsManager::NoiseProfile>&
    SettingsManager::StateNoiseFor(int a_env)
    {
        return a_env == kEnvIndoor ? stateNoiseIndoor : stateNoise;
    }

    SettingsManager::NoiseProfile& SettingsManager::EditTargetGlobalNoise()
    {
        if (locationEditActive >= 0 &&
            locationEditActive < static_cast<int>(locationOverrides.size())) {
            auto& lo = locationOverrides[static_cast<std::size_t>(locationEditActive)];
            lo.globalNoiseSet = true;
            return lo.globalNoise;
        }
        return GlobalNoiseFor(categoriesEditTab);
    }

    SettingsManager::NoiseProfile& SettingsManager::EditTargetStateNoise(const std::string& key)
    {
        if (locationEditActive >= 0 &&
            locationEditActive < static_cast<int>(locationOverrides.size())) {
            return locationOverrides[static_cast<std::size_t>(locationEditActive)].stateNoise[key];
        }
        return StateNoiseFor(categoriesEditTab)[key];
    }

    CameraProfile* SettingsManager::EditTarget(CameraProfile* outdoor)
    {
        // While a per-entry Location popup is rendering, every slider in it
        // must write into that place's copy instead of the Outdoor/Indoor
        // variant. Routing it here rather than at each call site is what lets
        // the popup reuse RenderProfileBlock (and the transition-override
        // button under it) unchanged.
        if (locationEditActive >= 0) {
            if (auto* lp = LocationSlotOf(outdoor, locationEditActive, categoriesEditTab)) return lp;
        }
        return VariantOf(outdoor, categoriesEditTab);
    }

    int SettingsManager::FindLocationOverride(LocationOverride::Kind a_kind,
                                              const std::string& a_plugin,
                                              std::uint32_t a_formID,
                                              const std::string& a_keyword) const
    {
        for (std::size_t i = 0; i < locationOverrides.size(); ++i) {
            const auto& lo = locationOverrides[i];
            if (lo.kind != a_kind) continue;
            if (a_kind == LocationOverride::Kind::Keyword) {
                if (_stricmp(lo.keyword.c_str(), a_keyword.c_str()) == 0) return static_cast<int>(i);
            } else {
                if (lo.formID == a_formID &&
                    _stricmp(lo.plugin.c_str(), a_plugin.c_str()) == 0) return static_cast<int>(i);
            }
        }
        return -1;
    }

    int SettingsManager::EnsureLocationOverride(LocationOverride::Kind a_kind, const std::string& a_name,
                                                const std::string& a_plugin, std::uint32_t a_formID,
                                                const std::string& a_keyword)
    {
        if (const int found = FindLocationOverride(a_kind, a_plugin, a_formID, a_keyword); found >= 0) {
            auto& lo = locationOverrides[static_cast<std::size_t>(found)];
            if (!lo.enabled) {
                lo.enabled = true;
                LocationDetector::GetSingleton().Invalidate();
            }
            return found;
        }
        LocationOverride lo;
        lo.kind    = a_kind;
        lo.name    = a_name;
        lo.plugin  = a_plugin;
        lo.formID  = a_formID;
        lo.keyword = a_keyword;
        SizeLocationOverride(lo);
        locationOverrides.push_back(std::move(lo));
        LocationDetector::GetSingleton().Invalidate();
        CameraController::InvalidateProfileReferences();
        spdlog::info("[LOC] created place \"{}\" ({})", a_name,
                     a_kind == LocationOverride::Kind::Keyword ? a_keyword
                                                               : a_plugin + "|" + std::to_string(a_formID));
        return static_cast<int>(locationOverrides.size()) - 1;
    }

    // Which eligible slots belong to the Target Lock tree. Built once from the
    // same list the storage is indexed by, so a slot index answers "which
    // section is this?" without another key lookup.
    static const std::vector<bool>& LocationSlotIsTL(SettingsManager& a_s)
    {
        static std::vector<bool> isTL;
        static bool built = false;
        if (!built) {
            for (const auto& e : a_s.GetIndoorEligibleProfiles())
                isTL.push_back(e.tomlKey.rfind("target_lock.", 0) == 0);
            built = true;
        }
        return isTL;
    }

    // Binding-camera keys carry their section as a suffix â€” "|tl" for the
    // Target Lock tree, "|cat" for Categories (see BindingCamLocationKey).
    static bool BindingCamKeyIsTL(const std::string& a_key)
    {
        return a_key.size() >= 3 &&
               a_key.compare(a_key.size() - 3, 3, "|tl") == 0;
    }

    bool SettingsManager::LocationHasSectionBindings(const LocationOverride& a_lo, int a_section)
    {
        if (a_section == kLocSecNoise)
            return !a_lo.stateNoise.empty() || a_lo.globalNoiseSet || !a_lo.fxBeats.empty();
        if (a_section == kLocSecFirstPerson) return !a_lo.fpState.empty();
        if (a_section == kLocSecDialogue)    return !a_lo.dlgLooks.empty();
        const auto& isTL = LocationSlotIsTL(*this);
        const bool wantTL = (a_section == kLocSecTargetLock);
        for (std::size_t i = 0; i < a_lo.profileSet.size() && i < isTL.size(); ++i) {
            if (a_lo.profileSet[i] && isTL[i] == wantTL) return true;
        }
        for (std::size_t i = 0; i < a_lo.profileSetIndoor.size() && i < isTL.size(); ++i) {
            if (a_lo.profileSetIndoor[i] && isTL[i] == wantTL) return true;
        }
        // Specific-weapon binding camera slots count toward their section.
        for (const auto& [k, p] : a_lo.bindingCam) {
            if (BindingCamKeyIsTL(k) == wantTL) return true;
        }
        return false;
    }

    bool SettingsManager::PruneLocationOverrideIfEmpty(int a_idx)
    {
        if (a_idx < 0 || a_idx >= static_cast<int>(locationOverrides.size())) return false;
        auto& lo = locationOverrides[static_cast<std::size_t>(a_idx)];
        if (lo.builtIn) return false;
        for (std::size_t i = 0; i < lo.profileSet.size(); ++i)
            if (lo.profileSet[i]) return false;
        for (std::size_t i = 0; i < lo.profileSetIndoor.size(); ++i)
            if (lo.profileSetIndoor[i]) return false;
        if (!lo.stateNoise.empty() || lo.globalNoiseSet) return false;
        if (!lo.fpState.empty()) return false;
        if (!lo.bindingCam.empty()) return false;
        if (!lo.fxBeats.empty()) return false;
        if (!lo.dlgLooks.empty()) return false;
        locationOverrides.erase(locationOverrides.begin() + a_idx);
        CameraController::InvalidateProfileReferences();
        if (locationEditIdx >= static_cast<int>(locationOverrides.size()))
            locationEditIdx = static_cast<int>(locationOverrides.size()) - 1;
        // Force a reclassify rather than trying to fix up the indices: the
        // erase shifted everything after it, and the detector re-derives the
        // right answer on the next frame anyway.
        activeLocationIdx = -1;
        activeLocationChain.clear();
        LocationDetector::GetSingleton().Invalidate();
        return true;
    }

}
