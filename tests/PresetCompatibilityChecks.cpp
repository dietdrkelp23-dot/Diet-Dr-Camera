#include "PCH.h"
#include "Camera/StateResolver.h"
#include "Camera/ProfileSnapshot.h"
#include "Settings/SettingsManager.h"
#include "Settings/PresetManager.h"
#include "Settings/PresetTable.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <nlohmann/json.hpp>

using namespace DietDrCamera;
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;

Json Snapshot(const CameraProfile& p)
{
    Json out = Json::object();
#define DDC_FIELD(name) out[#name] = p.name;
#include "fixtures/PresetV1Profile.inc"
#undef DDC_FIELD
    return out;
}

Json Snapshot(const SettingsManager::NoiseProfile& p)
{
    Json out = Json::object();
#define DDC_FIELD(name) out[#name] = p.name;
#include "fixtures/PresetV1NoiseProfile.inc"
#undef DDC_FIELD
    return out;
}

Json Snapshot(const SettingsManager::FirstPersonProfile& p)
{
    Json out = Json::object();
#define DDC_FIELD(name) out[#name] = p.name;
#include "fixtures/PresetV1FirstPersonProfile.inc"
#undef DDC_FIELD
    out["noise"] = Snapshot(p.noise);
    return out;
}

Json Snapshot(SettingsManager& s)
{
    Json out = Json::object();
#define DDC_FIELD(name) out["settings"][#name] = s.name;
#include "fixtures/PresetV1Settings.inc"
#undef DDC_FIELD
    out["settings"].erase("activePresetName");
    for (const auto& e : s.GetIndoorEligibleProfiles()) {
        out["outdoor"][e.tomlKey] = Snapshot(*e.outdoor);
        out["indoor"][e.tomlKey] = Snapshot(*s.IndoorVariantOf(e.outdoor));
    }
    out["vanity"] = Snapshot(s.vanityCamera);
    out["dialogue"] = Snapshot(s.dialogueProfile);
    out["dialogue1p"] = Snapshot(s.dialogueFirstPersonProfile);
    out["paraglide"] = Snapshot(s.paraglideProfile);
    out["noise"] = Snapshot(s.globalNoise);
    out["noiseIndoor"] = Snapshot(s.globalNoiseIndoor);
    out["firstPerson"] = Snapshot(s.firstPersonGlobal);
    for (const auto& [key, p] : s.stateNoise) out["stateNoise"][key] = Snapshot(p);
    for (const auto& [key, p] : s.stateNoiseIndoor) out["stateNoiseIndoor"][key] = Snapshot(p);
    for (const auto& [key, p] : s.stateFirstPerson) out["stateFirstPerson"][key] = Snapshot(p);
    for (size_t i = 0; i < s.locationOverrides.size(); ++i) {
        const auto& lo = s.locationOverrides[i];
        auto& dst = out["locations"][std::to_string(i)];
        dst["name"] = lo.name;
        dst["plugin"] = lo.plugin;
        dst["formID"] = lo.formID;
        dst["enabled"] = lo.enabled;
        const auto eligible = s.GetIndoorEligibleProfiles();
        for (size_t k = 0; k < eligible.size(); ++k) {
            if (lo.profileSet[k]) dst["outdoor"][eligible[k].tomlKey] = Snapshot(lo.profiles[k]);
            if (lo.profileSetIndoor[k]) dst["indoor"][eligible[k].tomlKey] = Snapshot(lo.profilesIndoor[k]);
        }
        if (lo.globalNoiseSet) dst["globalNoise"] = Snapshot(lo.globalNoise);
        for (const auto& [key, p] : lo.stateNoise) dst["stateNoise"][key] = Snapshot(p);
        for (const auto& [key, p] : lo.fpState) dst["firstPerson"][key] = Snapshot(p);
        for (const auto& [key, p] : lo.bindingCam) dst["bindingCam"][key] = Snapshot(p);
        for (const auto& [key, p] : lo.dlgLooks) dst["dialogue"][key] = Snapshot(p);
    }
    for (size_t i = 0; i < s.dialogueBuckets.size(); ++i)
        for (const auto& look : s.dialogueBuckets[i].looks)
            out["looks"][std::to_string(look.uid)] = Json{{"bucket", i}, {"name", look.name}, {"profile", Snapshot(look.profile)}};
    for (const auto& anim : s.animationCameras) {
        auto& dst = out["animations"][std::to_string(anim.uid)];
        dst["path"] = anim.animationPath;
        dst["profile"] = Snapshot(anim.profile);
        dst["indoor"] = Snapshot(anim.profileIndoor);
        dst["noise"] = Snapshot(anim.noise);
        dst["noiseIndoor"] = Snapshot(anim.noiseIndoor);
        dst["tl"] = Snapshot(anim.tlProfile);
        dst["tlIndoor"] = Snapshot(anim.tlProfileIndoor);
    }
    for (size_t i = 0; i < s.weaponBindings.size(); ++i) {
        const auto& b = s.weaponBindings[i];
        auto& dst = out["bindings"][std::to_string(i)];
        dst["plugin"] = b.pluginName;
        dst["formID"] = b.formID;
        dst["category"] = static_cast<int>(b.category);
        for (size_t k = 0; k < SettingsManager::kWeaponBindingSubStates; ++k) {
            auto& slot = dst["slots"][std::to_string(k)];
            slot["profile"] = Snapshot(b.profiles[k]);
            slot["indoor"] = Snapshot(b.profilesIndoor[k]);
            slot["tl"] = Snapshot(b.tlProfiles[k]);
            slot["tlIndoor"] = Snapshot(b.tlProfilesIndoor[k]);
            slot["noise"] = Snapshot(b.noiseProfiles[k]);
            slot["noiseIndoor"] = Snapshot(b.noiseProfilesIndoor[k]);
            slot["enabled"] = b.enabled[k];
            slot["tlEnabled"] = b.tlEnabled[k];
            slot["noiseEnabled"] = b.noiseEnabled[k];
        }
    }
    // Normalize hash-map iteration order for stable reference files/comparison.
    return Json(nlohmann::json::parse(out.dump()));
}

void Require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}

void Compare(const Json& expected, const Json& actual, const std::string& label)
{
    if (expected == actual) return;
    const auto diff = Json::diff(expected, actual);
    std::ofstream("preset-check-diff.json") << diff.dump(2);
    throw std::runtime_error(label + ": " + diff.dump().substr(0, 1800));
}

void CompareV1(const Json& expected, Json actual, const std::string& label)
{
    // Format 2 adds exactly these twenty profiles. Compare every original
    // value unchanged, and validate the new profiles separately below.
    for (const auto* environment : {"outdoor", "indoor"})
        for (const auto* prefix : {"staves.", "target_lock.staves."})
            for (const auto* school : {"alteration", "conjuration", "destruction", "illusion", "restoration"})
                for (const auto* suffix : {".ritual", ".sneak.ritual"}) {
                    const auto key = std::string(prefix) + school + suffix;
                    Require(actual[environment].erase(key) == 1, "Missing new staff profile: " + key);
                }
    Compare(expected, actual, label);
}

void RetuneScalars(toml::node& node)
{
    if (auto* table = node.as_table()) {
        for (auto&& [key, child] : *table)
            if (key != "meta") RetuneScalars(child);
    } else if (auto* array = node.as_array()) {
        for (auto& child : *array) RetuneScalars(child);
    } else if (auto* value = node.as_floating_point()) {
        *value = value->get() + 0.125;
    } else if (auto* value = node.as_boolean()) {
        *value = !value->get();
    } else if (auto* value = node.as_integer()) {
        *value = value->get() + 1;
    }
}

void AuthorFixture(SettingsManager& s)
{
    s.ResetAllToVanilla();
    CameraProfile p{ .sideOffset = 42, .height = 12, .zoom = 23, .fov = 92, .rotation = 7, .pitchOffset = 8 };
    p.SetTransitionAll(true);
    p.transitionSetAimBias = true;
    p.transitionAimBias = 0.75f;
    p.transitionZoom = 0.25f;
    p.transitionWeight = 0.2f;
    p.shoutLag = 0.4f;
    p.parentSeeded = true;
    for (auto e : s.GetIndoorEligibleProfiles()) *e.outdoor = p;
    s.InitIndoorOverrides();
    for (auto e : s.GetIndoorEligibleProfiles()) *s.IndoorVariantOf(e.outdoor) = CameraProfile{};
    s.vanityCamera = s.dialogueProfile = s.dialogueFirstPersonProfile = s.paraglideProfile = p;
    SettingsManager::NoiseProfile noise;
    noise.enabled = true; noise.amp = 0.4f; noise.speed = 0.6f; noise.sway = 0.2f;
    noise.tilt = 0.3f; noise.wobble = 0.7f; noise.driftJitter = 0.8f; noise.roughness = 0.9f;
    noise.repulse = 0.5f; noise.repulseFeel = 0.25f; noise.shoutFadeDuration = 0.8f;
    noise.attackDuration = 0.5f; noise.parentSeeded = true;
    s.globalNoise = noise;
    s.globalNoise.enabled = false; // global is always on; this gate is for states
    s.globalNoiseIndoor = {};
    s.stateNoise["shouts.melee.fire_breath"] = noise;
    s.stateNoiseIndoor["shouts.melee.fire_breath"] = {};
    s.stateNoiseIndoor["shouts.melee.fire_breath"].enabled = true;
    s.stateNoiseIndoor["shouts.melee.fire_breath"].parentSeeded = true;
    s.firstPersonGlobal.worldFov = 93;
    s.firstPersonGlobal.handsFov = 87;
    s.firstPersonGlobal.noise = noise;
    s.firstPersonGlobal.repulse = 0.7f;
    s.stateFirstPerson["weapons.bow.draw"] = s.firstPersonGlobal;
    SettingsManager::LocationOverride lo;
    lo.name = "Preset compatibility fixture"; lo.plugin = "Skyrim.esm"; lo.formID = 0x12345;
    s.SizeLocationOverride(lo);
    std::fill(lo.profiles.begin(), lo.profiles.end(), CameraProfile{});
    std::fill(lo.profilesIndoor.begin(), lo.profilesIndoor.end(), p);
    std::fill(lo.profileSet.begin(), lo.profileSet.end(), true);
    std::fill(lo.profileSetIndoor.begin(), lo.profileSetIndoor.end(), true);
    lo.globalNoiseSet = true; lo.globalNoise = {};
    lo.stateNoise["shouts.melee.fire_breath"] = noise;
    lo.fpState["weapons.bow.draw"] = {};
    for (auto& bucket : s.dialogueBuckets) for (auto& look : bucket.looks) {
        look.profile = p;
        lo.dlgLooks[std::to_string(look.uid)] = {};
    }
    SettingsManager::AnimationCameraEntry anim;
    anim.uid = 1234; anim.name = "Fixture"; anim.animationPath = "fixture/animation.hkx";
    anim.profile = anim.tlProfile = p; anim.profileIndoor = anim.tlProfileIndoor = {};
    anim.noise = noise; anim.noiseIndoor = {};
    s.animationCameras.push_back(anim);
    SettingsManager::WeaponBinding binding;
    binding.pluginName = "Skyrim.esm"; binding.formID = 0x1397E;
    binding.category = SettingsManager::BindingCategory::Melee;
    for (size_t k = 0; k < SettingsManager::kWeaponBindingSubStates; ++k) {
        binding.profiles[k] = binding.tlProfiles[k] = p;
        binding.profilesIndoor[k] = binding.tlProfilesIndoor[k] = {};
        // Regular binding slots are always active; directional attacks alone
        // have independent enable switches in the released interface.
        binding.enabled[k] = binding.tlEnabled[k] = binding.noiseEnabled[k] = (k < 10 || k % 2 == 0);
        binding.profilesIndoor[k].parentSeeded = binding.enabled[k];
        binding.tlProfilesIndoor[k].parentSeeded = binding.tlEnabled[k];
        binding.noiseProfilesIndoor[k].parentSeeded = binding.noiseEnabled[k];
        binding.noiseProfiles[k] = noise;
        lo.bindingCam[SettingsManager::BindingCamLocationKey(binding, static_cast<int>(k), false)] = {};
    }
    s.weaponBindings.push_back(binding);
    s.locationOverrides.push_back(lo);
    for (size_t i = 0; i < SettingsManager::kPowerAttackDirectionCount; ++i) {
        s.weaponsMeleePowerAttackDirEnabled[i] = (i % 2 == 0);
        s.tlWeaponsMeleePowerAttackDirEnabled[i] = (i % 2 != 0);
    }
}

std::string ReadBytes(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void CheckPresetFiles(SettingsManager& s)
{
    const auto originalDirectory = fs::current_path();
    const auto testDirectory = originalDirectory / ("preset-io-check-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(fs::create_directory(testDirectory), "Test directory already exists");
    fs::current_path(testDirectory);
    auto& pm = PresetManager::GetSingleton();
    const std::string name = "Preset-\xC3\xA9-\xE6\xA3\xAE";
    const auto path = fs::path("Data/SKSE/Plugins/DietDrCamera/Presets") / fs::u8path(name + ".toml");
    AuthorFixture(s);
    s.npcMeleeNoiseIntensity = 0.65f;
    s.npcMeleeNoiseIntensityFp = 1.35f;
    s.npcArcheryNoiseIntensity = 0.45f;
    s.npcArcheryNoiseIntensityFp = 0.9f;
    s.npcTransformNoiseIntensity = 1.25f;
    s.npcTransformNoiseIntensityFp = 2.0f;
    s.npcShoutNoiseIntensity = 0.35f;
    s.npcShoutNoiseIntensityFp = 1.75f;
    s.quickTuneHotkey = 99;
    Require(pm.SavePreset(name), "UTF-8 preset save failed");
    Require(fs::exists(path), "UTF-8 preset name was changed");
    Require(pm.ListPresets() == std::vector<std::string>{name}, "UTF-8 preset list changed the name");
    Require(!pm.SavePreset(name), "Save replaced an existing preset");
    Require(pm.SetDescription(name, "Fixture description"), "Description save failed");
    s.sheathed.zoom = 31;
    Require(pm.UpdatePreset(name), "Preset update failed");
    Require(pm.GetDescription(name) == "Fixture description", "Update erased author description");
    // A spell/shout may still be fading while the paused menu loads a preset.
    // Use the production effect capture against real preset-owned map/vector
    // entries, then replace those entries through the real file load path.
    auto& casting = s.stateFirstPerson["magic.destruction.fire_and_forget"];
    casting.noise.amp = 0.73f;
    casting.repulseFeel = 0.81f;
    auto spellTail = SnapshotProfile(&casting);
    SettingsManager::WeaponBinding shoutBinding{};
    shoutBinding.fpProfiles[0].noise.amp = 0.64f;
    shoutBinding.fpProfiles[0].shoutFadeDuration = 3.2f;
    s.weaponBindings.push_back(std::move(shoutBinding));
    auto shoutTail = SnapshotProfile(&s.weaponBindings.back().fpProfiles[0]);
    casting.noise.amp = 0.12f;
    Require(spellTail->noise.amp == 0.73f, "Live editing changed an already captured spell tail");
    s.stateFirstPerson.erase("magic.destruction.fire_and_forget");
    Require(spellTail->repulseFeel == 0.81f, "Deleting an entry invalidated the spell tail");
    Require(pm.LoadPreset(name), "UTF-8 preset load failed");
    Require(spellTail->noise.amp == 0.73f && spellTail->repulseFeel == 0.81f,
            "Preset reload invalidated the spell tail");
    Require(shoutTail->noise.amp == 0.64f && shoutTail->shoutFadeDuration == 3.2f,
            "Preset reload invalidated the bound shout tail");
    spellTail = SnapshotProfile<SettingsManager::FirstPersonProfile>(nullptr);
    shoutTail.reset();
    Require(!spellTail && !shoutTail, "Finished or cancelled effects retained a profile");
    Require(s.sheathed.zoom == 31, "Preset file lost tuning");
    Require(s.npcMeleeNoiseIntensity == 0.65f && s.npcMeleeNoiseIntensityFp == 1.35f,
            "Preset file lost independent NPC melee tuning");
    Require(s.npcShoutNoiseIntensity == 0.35f && s.npcShoutNoiseIntensityFp == 1.75f,
            "Preset file lost independent NPC shout tuning");
    Require(s.npcArcheryNoiseIntensity == 0.45f && s.npcArcheryNoiseIntensityFp == 0.9f &&
            s.npcTransformNoiseIntensity == 1.25f && s.npcTransformNoiseIntensityFp == 2.0f,
            "Preset file lost independent NPC archery/transformation tuning");
    Require(s.quickTuneHotkey == 99, "Preset replaced a global hotkey");
    const auto validFile = ReadBytes(path);
    s.sheathed.zoom = std::numeric_limits<float>::quiet_NaN();
    Require(!pm.UpdatePreset(name), "Invalid current tuning was saved");
    Require(ReadBytes(path) == validFile, "Invalid tuning replaced the original preset");
    s.sheathed.zoom = 31;
    const auto current = Snapshot(s);
    const auto fullCurrent = s.BuildSaveTable();
    const auto active = s.activePresetName;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "[meta]\nformat = " << kCurrentPresetFormat + 1 << "\ndescription = 'Future author'\n[new_feature]\nstrength = 0.9\n";
    }
    const auto future = ReadBytes(path);
    Require(!pm.LoadPreset(name), "Future preset was loaded");
    Require(!pm.UpdatePreset(name), "Future preset was overwritten");
    Compare(current, Snapshot(s), "Rejected preset changed current settings");
    Require(fullCurrent == s.BuildSaveTable(), "Rejected preset changed later-format settings");
    Require(s.activePresetName == active, "Rejected preset changed selected preset");
    Require(ReadBytes(path) == future, "Rejected update modified the file");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "invalid = [";
    }
    Require(!pm.LoadPreset(name), "Malformed preset was loaded");
    Require(!pm.UpdatePreset(name), "Malformed preset was overwritten");
    Compare(current, Snapshot(s), "Malformed preset changed tuning");
    Require(ReadBytes(path) == "invalid = [", "Failed save changed the original file");
    Require(pm.RenamePreset(name, "Renamed"), "Preset rename failed");
    Require(pm.DeletePreset("Renamed"), "Preset delete failed");
    const auto sparsePath = path.parent_path() / "Sparse.toml";
    std::ofstream(sparsePath) << "[meta]\nformat = 2\n";
    s.tlTransformationsWerewolfFeeding.zoom = 67;
    s.animationCameras.push_back(SettingsManager::AnimationCameraEntry{});
    Require(pm.LoadPreset("Sparse"), "Sparse preset failed to load");
    Require(s.tlTransformationsWerewolfFeeding == CameraProfile{},
            "Previous preset's werewolf feeding camera leaked into a sparse preset");
    Require(s.animationCameras.empty(), "Previous animation binding survived a sparse preset");
    Require(s.npcMeleeNoiseIntensity == 0 && s.npcMeleeNoiseIntensityFp == 0,
            "Previous NPC melee tuning leaked into a format-2 preset");
    Require(s.npcShoutNoiseIntensity == 0 && s.npcShoutNoiseIntensityFp == 0,
            "Previous NPC shout tuning leaked into a sparse preset");
    Require(s.npcArcheryNoiseIntensity == 0 && s.npcArcheryNoiseIntensityFp == 0 &&
            s.npcTransformNoiseIntensity == 0 && s.npcTransformNoiseIntensityFp == 0,
            "Previous NPC archery/transformation tuning leaked into a sparse preset");
    Require(s.quickTuneHotkey == 99, "Sparse preset replaced the global hotkey");
    Require(pm.DeletePreset("Sparse"), "Sparse test preset cleanup failed");
    fs::current_path(originalDirectory);
}

toml::table RoundTrip(SettingsManager& settings)
{
    const auto saved = settings.BuildSaveTable();
    std::ostringstream text;
    text << saved;
    settings.ResetAllToVanilla();
    Require(settings.ApplyTable(toml::parse(text.str())), "Load failed");
    return saved;
}

void CheckNpcMeleeNoise(SettingsManager& s)
{
    s.ResetAllToVanilla();
    Require(s.npcMeleeNoiseIntensity == 0 && s.npcMeleeNoiseIntensityFp == 0,
            "NPC melee must be opt-in for both views");
    s.npcNoiseIntensity = 0.4f;
    s.npcNoiseIntensityFp = 0.8f;
    s.npcMeleeNoiseIntensity = 1.25f;
    s.npcMeleeNoiseIntensityFp = 2.5f;
    const auto saved = RoundTrip(s);
    Require(saved["meta"]["format"].value_or(0) == kCurrentPresetFormat && kCurrentPresetFormat >= 3,
            "New NPC fields were written with an older preset format");
    Require(s.npcNoiseIntensity == 0.4f && s.npcNoiseIntensityFp == 0.8f &&
            s.npcMeleeNoiseIntensity == 1.25f && s.npcMeleeNoiseIntensityFp == 2.5f,
            "NPC magic/melee or POV amounts crossed during round trip");
    Require(saved == s.BuildSaveTable(), "NPC noise rewrite is unstable");
    s.ResetAllToVanilla();
    const auto defaults = s.BuildSaveTable();
    Require(!defaults["cinematic"]["npc_noise"]["melee_intensity"] &&
            !defaults["cinematic"]["npc_noise"]["melee_intensity_fp"], "Neutral melee amounts are not sparse");

    using Weapon = MeleeWeaponType;
    auto resolve = [&](bool power = false, bool sneak = false, bool sprint = false,
                       Weapon weapon = Weapon::Sword, int direction = -1) {
        return s.ResolveNpcMeleeNoise(power, sneak, sprint, weapon, direction);
    };
    s.globalNoise.amp = 3;
    s.stateNoise["weapons.melee"].enabled = true;
    s.stateNoise["weapons.melee"].amp = 2;
    Require(!resolve(), "Idle/global ambience becomes NPC attack noise");
    auto& normal = s.stateNoise["weapons.melee.attack"];
    normal.enabled = true; normal.amp = 0.5f;
    Require(resolve() == &normal && resolve(true) == &normal, "Missing power entry does not inherit attack");
    auto& power = s.stateNoise["weapons.melee.power_attack"];
    power.enabled = true; power.amp = 0;
    Require(resolve(true) == &power, "Explicit zero power entry falls through to audible normal attack");
    power.amp = 1;
    const char* variants[] = {"weapons.melee.sneak_attack", "weapons.melee.sprint_attack",
        "weapons.melee.sneak_power_attack", "weapons.melee.sprint_power_attack"};
    for (int i = 0; i < 4; ++i) {
        auto& p = s.stateNoise[variants[i]]; p.enabled = true;
        Require(resolve(i >= 2, i % 2 == 0, i % 2 == 1) == &p, "NPC attack stance routed to wrong entry");
    }
    auto& indoor = s.stateNoiseIndoor["weapons.melee.attack"]; indoor.enabled = true;
    s.indoorMode = true;
    Require(resolve() == &indoor, "NPC attack ignores indoor noise");
    SettingsManager::LocationOverride location;
    location.enabled = true;
    location.stateNoise["weapons.melee.attack"].enabled = true;
    s.locationOverrides.push_back(std::move(location));
    s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
    Require(resolve() == &s.locationOverrides[0].stateNoise.at("weapons.melee.attack"),
            "NPC attack ignores active location noise");
    for (std::size_t i = 0; i < kMeleeWeaponCount; ++i) {
        auto& overrides = s.weaponsMeleeAttackNoiseOverrides;
        overrides.perWeaponSet[i] = true;
        Require(resolve(false, false, false, static_cast<Weapon>(i)) == &overrides.perWeapon[i],
                "NPC weapon type override not honored (including unarmed)");
    }
    s.locationOverrides.clear(); s.activeLocationChain.clear(); s.indoorMode = false;
    for (int direction = 0; direction < 5; ++direction) {
        auto& overrides = s.weaponsMeleePowerAttackDirNoiseOverrides[direction];
        overrides.perWeaponSet[static_cast<int>(Weapon::Sword)] = true;
        Require(resolve(true, false, false, Weapon::Sword, direction) ==
                    &overrides.perWeapon[static_cast<int>(Weapon::Sword)], "Directional NPC power attack lost its weapon override");
    }
    Require(resolve(true, false, false, Weapon::Sword, 99) == &power, "Unknown direction fails to inherit base power attack");
    s.ResetAllToVanilla();
}

void CheckNpcWeaponNoiseOverrides(SettingsManager& s)
{
    s.ResetAllToVanilla();
    s.indoorMode = false;
    using Overrides = SettingsManager::MeleeWeaponNoiseOverrides;
    struct AttackCase {
        const char* key;
        Overrides* overrides;
        bool power, sneak, sprint;
        int direction = -1;
    };
    const AttackCase attacks[] = {
        {"weapons.melee.attack", &s.weaponsMeleeAttackNoiseOverrides, false, false, false},
        {"weapons.melee.power_attack", &s.weaponsMeleePowerAttackNoiseOverrides, true, false, false},
        {"weapons.melee.sneak_attack", &s.weaponsMeleeSneakAttackNoiseOverrides, false, true, false},
        {"weapons.melee.sneak_power_attack", &s.weaponsMeleeSneakPowerAttackNoiseOverrides, true, true, false},
        {"weapons.melee.sprint_attack", &s.weaponsMeleeSprintAttackNoiseOverrides, false, false, true},
        {"weapons.melee.sprint_power_attack", &s.weaponsMeleeSprintPowerAttackNoiseOverrides, true, false, true},
        {"weapons.melee.power_attack.dir.standing", &s.weaponsMeleePowerAttackDirNoiseOverrides[0], true, false, false, 0},
        {"weapons.melee.power_attack.dir.forward", &s.weaponsMeleePowerAttackDirNoiseOverrides[1], true, false, false, 1},
        {"weapons.melee.power_attack.dir.back", &s.weaponsMeleePowerAttackDirNoiseOverrides[2], true, false, false, 2},
        {"weapons.melee.power_attack.dir.left", &s.weaponsMeleePowerAttackDirNoiseOverrides[3], true, false, false, 3},
        {"weapons.melee.power_attack.dir.right", &s.weaponsMeleePowerAttackDirNoiseOverrides[4], true, false, false, 4},
    };
    auto resolve = [&](const AttackCase& attack, std::size_t weapon) {
        return s.ResolveNpcMeleeNoise(attack.power, attack.sneak, attack.sprint,
                                     static_cast<MeleeWeaponType>(weapon), attack.direction);
    };
    Json expected = Json::object();
    int variant = 0;
    for (const auto& attack : attacks) {
        auto& parent = s.stateNoise[attack.key]; parent.enabled = true; parent.parentSeeded = true; parent.amp = 3;
        auto& indoor = s.stateNoiseIndoor[attack.key]; indoor.enabled = true; indoor.parentSeeded = true; indoor.amp = 4;
        for (std::size_t weapon = 0; weapon < kMeleeWeaponCount; ++weapon) {
            auto& p = attack.overrides->perWeapon[weapon];
            attack.overrides->perWeaponSet[weapon] = true;
            p.enabled = true;
            p.parentSeeded = true;
            p.amp = weapon == 0 ? 0.0f : static_cast<float>(weapon) * 0.125f;
            p.speed = 0.5f + static_cast<float>(variant) * 0.125f;
            p.attackDuration = 0.25f + static_cast<float>(weapon) * 0.125f;
            p.sway = 0.25f; p.tilt = 0.75f; p.driftJitter = 0.5f; p.roughness = 0.875f;
            expected[attack.key][std::to_string(weapon)] = Snapshot(p);
        }
        ++variant;
    }
    auto verify = [&] {
        for (const auto& attack : attacks)
            for (std::size_t weapon = 0; weapon < kMeleeWeaponCount; ++weapon) {
                const auto* actual = resolve(attack, weapon);
                Require(actual == &attack.overrides->perWeapon[weapon] &&
                        Snapshot(*actual) == expected.at(attack.key).at(std::to_string(weapon)),
                        std::string("NPC weapon override changed or crossed attack/type: ") + attack.key);
            }
    };
    verify();
    const auto saved = RoundTrip(s);
    Require(saved == s.BuildSaveTable(), "NPC weapon override rewrite is unstable");
    verify();
    s.indoorMode = true;
    verify();
    SettingsManager::LocationOverride location;
    location.enabled = true;
    for (const auto& attack : attacks) {
        auto& p = location.stateNoise[attack.key]; p.enabled = true; p.amp = 5;
    }
    s.locationOverrides.push_back(std::move(location));
    s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
    verify();
    for (const auto& attack : attacks) {
        for (std::size_t weapon = 0; weapon < kMeleeWeaponCount; ++weapon) {
            attack.overrides->perWeaponSet[weapon] = false;
            Require(resolve(attack, weapon) == &s.locationOverrides[0].stateNoise.at(attack.key),
                    "Disabled NPC weapon override masks the location or inherits another weapon type");
            attack.overrides->perWeaponSet[weapon] = true;
        }
    }
    s.ResetAllToVanilla();
}

void CheckLegacyNpcWeaponNoiseOverrides(SettingsManager& s)
{
    s.ResetAllToVanilla(); s.indoorMode = false;
    Require(s.ApplyTable(toml::parse(
        "[noise.melee_overrides.weapons.melee.attack.per_weapon.sword]\n"
        "enabled = true\namp = 1.5\nspeed = 0.75")), "Nested legacy weapon noise table rejected");
    const auto* profile = s.ResolveNpcMeleeNoise(false, false, false, MeleeWeaponType::Sword);
    Require(profile && profile->amp == 1.5f && profile->speed == 0.75f,
            "Fixing literal weapon noise keys broke older nested keys");
    RoundTrip(s);
    profile = s.ResolveNpcMeleeNoise(false, false, false, MeleeWeaponType::Sword);
    Require(profile && profile->amp == 1.5f, "Nested legacy override is lost after conversion to literal keys");
    s.ResetAllToVanilla();
}

void CheckNpcSpecificBindings(SettingsManager& s)
{
    using Category = SettingsManager::BindingCategory;
    using Scope = ItemBindings::Scope;
    using Weapon = MeleeWeaponType;
    const ItemBindings::EquippedItem item{{"Weapons.esp", 0x123}, {"Weapons.esp", 0x100}, "frost"};
    const auto add = [&](Category category, Scope scope, bool fp) {
        SettingsManager::WeaponBinding binding;
        binding.category = category; binding.bindingScope = scope; binding.fpOnly = fp;
        binding.pluginName = "Weapons.esp";
        binding.formID = scope == Scope::ExactForm ? 0x123 : 0x100;
        binding.enchantmentKey = scope == Scope::Enchantment ? "frost" : "";
        binding.settingsSeeded = true;
        for (std::size_t slot = 0; slot < SettingsManager::GetBindingSubStateCount(category); ++slot) {
            binding.noiseEnabled[slot] = true;
            binding.noiseProfiles[slot].parentSeeded = true;
            binding.noiseProfilesIndoor[slot].parentSeeded = true;
            binding.noiseProfiles[slot].amp = 0.25f + static_cast<float>(slot) * 0.125f;
            binding.noiseProfilesIndoor[slot].amp = 0.5f + static_cast<float>(slot) * 0.125f;
        }
        s.weaponBindings.push_back(std::move(binding));
    };
    for (const auto category : {Category::Melee, Category::Bow, Category::Crossbow, Category::Shout}) {
        s.ResetAllToVanilla();
        s.indoorMode = false;
        add(category, Scope::BaseItem, false);
        add(category, Scope::ExactForm, false);
        add(category, Scope::Enchantment, false);
        add(category, Scope::Enchantment, true);
        const auto resolve = [&](const ItemBindings::EquippedItem& identity, int slot) -> const SettingsManager::NoiseProfile* {
            if (category == Category::Melee)
                return s.ResolveNpcMeleeNoise(slot == 5 || slot == 7 || slot == 9 || slot >= 10,
                    slot == 8 || slot == 9, slot == 6 || slot == 7, Weapon::Sword, slot >= 10 ? slot - 10 : -1, identity);
            if (category == Category::Shout) return s.ResolveNpcShoutNoise("melee", "unrelenting_force", slot == 1, identity);
            return s.ResolveNpcArcheryNoise(category == Category::Crossbow, slot == 5, true, identity);
        };
        const int slot = category == Category::Melee ? 3 : category == Category::Shout ? 0 : 2;
        Require(resolve(item, slot) == &s.weaponBindings[2].noiseProfiles[slot], "Enchantment-specific NPC binding does not win");
        auto other = item; other.enchantment = "fire";
        Require(resolve(other, slot) == &s.weaponBindings[1].noiseProfiles[slot], "NPC exact-form binding does not beat base item");
        other.form.localID = 0x456;
        Require(resolve(other, slot) == &s.weaponBindings[0].noiseProfiles[slot], "NPC base binding misses another item variant");
        other.base.plugin = other.form.plugin = "Other.esp";
        Require(!resolve(other, slot), "NPC binds a same-number item from another plugin");
        auto verify = [&] {
            const auto check = [&](int useSlot) {
                const auto* p = resolve(item, useSlot);
                Require(p == &s.weaponBindings[2].NoiseProfilesFor(s.RuntimeEnv())[useSlot],
                        "NPC specific binding crosses an attack, environment or POV");
                Require(p->amp == (s.indoorMode ? 0.5f : 0.25f) + static_cast<float>(useSlot) * 0.125f,
                        "NPC specific binding lost its saved amplitude");
            };
            if (category == Category::Melee) for (int useSlot : {3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}) check(useSlot);
            else if (category == Category::Shout) { check(0); check(1); }
            else { check(2); check(5); }
        };
        verify();
        RoundTrip(s);
        verify();
        s.indoorMode = true; verify();
        auto& bound = s.weaponBindings[2];
        SettingsManager::LocationOverride location; location.enabled = true;
        const auto locationKey = SettingsManager::BindingNoiseLocationKey(bound, slot);
        auto& mute = location.stateNoise[locationKey]; mute.enabled = true; mute.amp = 0;
        s.locationOverrides.push_back(std::move(location));
        s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
        Require(resolve(item, slot) == &s.locationOverrides[0].stateNoise.at(locationKey),
                "Specific NPC binding ignores an explicit location mute");
        s.locationOverrides.clear(); s.activeLocationChain.clear();
        bound.NoiseProfilesFor(s.RuntimeEnv())[slot].amp = 0;
        Require(resolve(item, slot)->amp == 0, "Specific NPC zero falls through to another source");
        if (category == Category::Melee) {
            auto& perType = s.weaponsMeleeAttackNoiseOverrides;
            perType.perWeaponSet[static_cast<int>(Weapon::Sword)] = true;
            perType.perWeapon[static_cast<int>(Weapon::Sword)].amp = 4;
            Require(resolve(item, 3)->amp == 0, "Weapon type overrides a specific weapon mute");
            bound.noiseEnabled[3] = false;
            Require(resolve(item, 3) == &perType.perWeapon[static_cast<int>(Weapon::Sword)],
                    "Disabled specific attack does not fall back to its weapon type");
            bound.noiseEnabled[11] = false;
            Require(resolve(item, 11) == &bound.NoiseProfilesFor(s.RuntimeEnv())[5],
                    "Disabled specific direction ignores the binding's base power attack");
        }
    }
    s.ResetAllToVanilla();
}

void CheckNpcShouts(SettingsManager& s)
{
    s.ResetAllToVanilla();
    s.indoorMode = false;
    Require(s.npcShoutNoiseIntensity == 0 && s.npcShoutNoiseIntensityFp == 0, "Shouts do not default to off");
    s.npcNoiseIntensity = 0.25f; s.npcNoiseIntensityFp = 0.5f;
    s.npcShoutNoiseIntensity = 0.75f; s.npcShoutNoiseIntensityFp = 1.25f;
    const auto saved = RoundTrip(s);
    Require(kCurrentPresetFormat >= 5 && saved == s.BuildSaveTable() &&
            s.npcShoutNoiseIntensity == 0.75f && s.npcShoutNoiseIntensityFp == 1.25f &&
            s.npcNoiseIntensity == 0.25f && s.npcNoiseIntensityFp == 0.5f, "Magic and Shouts cross on reload");
    Require(s.ApplyTable(toml::parse("[general]\ntransition_mul_zoom = 0.7")) && s.npcShoutNoiseIntensity == 0.75f,
            "Unrelated partial table migrates shout amounts");
    for (int format : {1, 2, 3, 4}) {
        s.ResetAllToVanilla();
        Require(s.ApplyTable(toml::parse("[meta]\nformat = " + std::to_string(format) +
            "\n[cinematic.npc_noise]\nintensity = 0.5\nintensity_fp = 1.5")) &&
            s.npcShoutNoiseIntensity == 0.5f && s.npcShoutNoiseIntensityFp == 1.5f,
            "Old combined Magic/Shouts loses its amount");
        RoundTrip(s);
        Require(s.npcShoutNoiseIntensity == 0.5f && s.npcShoutNoiseIntensityFp == 1.5f, "Migrated shout amounts do not persist");
    }
    s.ResetAllToVanilla();
    s.npcNoiseIntensity = 1; RoundTrip(s);
    Require(s.npcShoutNoiseIntensity == 0, "Current-format muted shouts inherit Magic");
    for (const char* bucket : {"sheathed", "melee", "bow", "crossbow", "magic", "staves"}) {
        const std::string root = std::string("shouts.") + bucket + ".";
        auto& base = s.stateNoise[root + "base"]; base.enabled = true; base.amp = 1;
        Require(s.ResolveNpcShoutNoise(bucket, "unrelenting_force", false) == &base, "Untuned NPC shout misses its state base");
        auto& named = s.stateNoise[root + "unrelenting_force"]; named.enabled = true; named.amp = 2;
        auto& sneak = s.stateNoise[root + "unrelenting_force.sneak"]; sneak.enabled = true; sneak.amp = 0;
        Require(s.ResolveNpcShoutNoise(bucket, "unrelenting_force", false) == &named &&
                s.ResolveNpcShoutNoise(bucket, "unrelenting_force", true) == &sneak,
                "Named NPC shout or sneak mute routed to another state");
    }
    s.ResetAllToVanilla();
}

void CheckNpcArcheryAndTransformations(SettingsManager& s)
{
    s.ResetAllToVanilla();
    Require(s.npcArcheryNoiseIntensity == 0 && s.npcArcheryNoiseIntensityFp == 0 &&
            s.npcTransformNoiseIntensity == 0 && s.npcTransformNoiseIntensityFp == 0,
            "New NPC controls must default to off");
    s.npcNoiseIntensity = 0.2f; s.npcNoiseIntensityFp = 0.3f;
    s.npcArcheryNoiseIntensity = 0.6f; s.npcArcheryNoiseIntensityFp = 0.7f;
    s.npcTransformNoiseIntensity = 1.1f; s.npcTransformNoiseIntensityFp = 1.2f;
    const auto saved = RoundTrip(s);
    Require(kCurrentPresetFormat >= 4 && saved == s.BuildSaveTable(), "Expanded NPC preset format is unstable");
    Require(s.npcNoiseIntensity == 0.2f && s.npcNoiseIntensityFp == 0.3f &&
            s.npcArcheryNoiseIntensity == 0.6f && s.npcArcheryNoiseIntensityFp == 0.7f &&
            s.npcTransformNoiseIntensity == 1.1f && s.npcTransformNoiseIntensityFp == 1.2f,
            "Archery/transformation amounts cross sources or POVs on reload");
    Require(s.ApplyTable(toml::parse("[general]\ntransition_mul_zoom = 0.7")) &&
            s.npcTransformNoiseIntensity == 1.1f, "Unrelated partial settings trigger legacy NPC migration");
    for (int format : {1, 2, 3}) {
        s.ResetAllToVanilla();
        Require(s.ApplyTable(toml::parse("[meta]\nformat = " + std::to_string(format) +
            "\n[cinematic.npc_noise]\nintensity = 0.4\nintensity_fp = 0.8")), "Older NPC preset was rejected");
        Require(s.npcNoiseIntensity == 0.4f && s.npcNoiseIntensityFp == 0.8f &&
                s.npcTransformNoiseIntensity == 0.4f && s.npcTransformNoiseIntensityFp == 0.8f &&
                s.npcArcheryNoiseIntensity == 0 && s.npcArcheryNoiseIntensityFp == 0,
                "Splitting transformations changes the old combined amount or enables archery");
        RoundTrip(s);
        Require(s.npcTransformNoiseIntensity == 0.4f && s.npcTransformNoiseIntensityFp == 0.8f,
                "Migrated transformation amounts do not survive a rewrite");
    }
    s.ResetAllToVanilla();
    s.npcNoiseIntensity = 1;
    RoundTrip(s);
    Require(s.npcTransformNoiseIntensity == 0 && s.npcTransformNoiseIntensityFp == 0,
            "Current-format muted transformations inherit Magic after reload");

    using Form = NpcNoise::Form;
    using Action = NpcNoise::Action;
    s.stateNoise["weapons.melee.attack"].enabled = true;
    s.stateNoise["weapons.melee.attack"].amp = 3;
    s.stateNoise["magic.destruction.fire_and_forget"].enabled = true;
    s.stateNoise["magic.destruction.fire_and_forget"].amp = 3;
    Require(!s.ResolveNpcTransformationNoise(Form::Werewolf, Action::Attack) &&
            !s.ResolveNpcTransformationNoise(Form::VampireLord, Action::FireAndForget),
            "Beast attacks inherit generic melee or magic noise");
    const struct { Form form; Action action; const char* key; } actions[] = {
        {Form::Werewolf, Action::Attack, "transformations.werewolf.attack"},
        {Form::Werewolf, Action::PowerAttack, "transformations.werewolf.power_attack"},
        {Form::Werewolf, Action::SprintPowerAttack, "transformations.werewolf.sprint_power_attack"},
        {Form::Werewolf, Action::Roar, "transformations.werewolf.roar"},
        {Form::VampireLord, Action::Attack, "transformations.vampire_lord.melee.attack"},
        {Form::VampireLord, Action::PowerAttack, "transformations.vampire_lord.melee.power_attack"},
        {Form::VampireLord, Action::Concentration, "transformations.vampire_lord.concentration"},
        {Form::VampireLord, Action::FireAndForget, "transformations.vampire_lord.fire_and_forget"}};
    for (auto [form, action, key] : actions) {
        auto& profile = s.stateNoise[key]; profile.enabled = true; profile.amp = 0;
        Require(s.ResolveNpcTransformationNoise(form, action) == &profile,
                "Transformation entry or explicit zero is ignored");
        auto& indoor = s.stateNoiseIndoor[key]; indoor.enabled = true; indoor.amp = 0.5f;
        s.indoorMode = true;
        Require(s.ResolveNpcTransformationNoise(form, action) == &indoor, "Transformation noise ignores indoor tuning");
        s.indoorMode = false;
    }
    auto& sprint = s.stateNoise["transformations.werewolf.sprint_power_attack"]; sprint.enabled = false;
    Require(s.ResolveNpcTransformationNoise(Form::Werewolf, Action::SprintPowerAttack) ==
            &s.stateNoise.at("transformations.werewolf.power_attack"), "Werewolf sprint power attack loses its parent");
    s.stateNoise["transformations.vampire_lord.concentration"].enabled = false;
    auto& vlMagic = s.stateNoise["transformations.vampire_lord.magic"]; vlMagic.enabled = true;
    Require(s.ResolveNpcTransformationNoise(Form::VampireLord, Action::Concentration) == &vlMagic,
            "Vampire Lord concentration loses its magic parent");
    SettingsManager::LocationOverride location;
    location.enabled = true;
    location.stateNoise["transformations.werewolf.attack"].enabled = true;
    s.locationOverrides.push_back(std::move(location));
    s.activeLocationChain = {0}; s.locationOverridesEnabled = true;
    Require(s.ResolveNpcTransformationNoise(Form::Werewolf, Action::Attack) ==
            &s.locationOverrides[0].stateNoise.at("transformations.werewolf.attack"),
            "Transformation noise ignores the active location's explicit zero");
    s.activeLocationChain.clear(); s.locationOverrides.clear();
    for (bool crossbow : {false, true}) {
        const std::string base = crossbow ? "weapons.crossbow" : "weapons.bow";
        auto& normal = s.stateNoise[base]; normal.enabled = true;
        Require(s.ResolveNpcArcheryNoise(crossbow, false, false) == &normal, "Archery base fallback is wrong");
        auto& drawing = s.stateNoise[base + ".draw"]; drawing.enabled = true;
        auto& sneak = s.stateNoise[base + ".sneak.draw"]; sneak.enabled = true;
        Require(s.ResolveNpcArcheryNoise(crossbow, false, false) == &drawing &&
                s.ResolveNpcArcheryNoise(crossbow, true, false) == &sneak, "Bow/crossbow release loses its tuned entry");
    }
    auto& mounted = s.stateNoise["mounts.horseback.archery.draw"]; mounted.enabled = true;
    Require(s.ResolveNpcArcheryNoise(false, true, true) == &mounted, "Mounted NPC archer uses on-foot settings");
    s.ResetAllToVanilla();
}

void CheckStaffRitualProfiles(SettingsManager& s)
{
    s.ResetAllToVanilla();
    const auto eligible = s.GetIndoorEligibleProfiles();
    const auto categoryProfiles = s.GetCategoryProfiles();
    const auto targetLockProfiles = s.GetTargetLockProfiles();
    SettingsManager::LocationOverride location;
    location.name = "Ritual staff location";
    location.plugin = "Skyrim.esm";
    location.formID = 0x12345;
    s.SizeLocationOverride(location);
    SettingsManager::CustomEnemyOverride enemy;
    enemy.displayName = "Ritual staff target";
    enemy.pluginName = "Skyrim.esm";
    enemy.formID = 0x12345;
    std::size_t cell = 0;
    for (int school = 1; school <= 5; ++school) {
        for (const bool sneak : {false, true}) {
            const auto key = std::string("staves.") + SettingsManager::GetMagicSchoolTomlKey(school - 1) +
                (sneak ? ".sneak.ritual" : ".ritual");
            const auto schoolType = static_cast<MagicSchool>(school);
            auto* normal = s.PickStavesProfile(schoolType, CastType::Ritual, sneak);
            auto* locked = s.PickTLStavesProfile(schoolType, CastType::Ritual, sneak);
            Require(normal != s.PickStavesProfile(schoolType, CastType::FireAndForget, sneak), "Staff ritual aliases Fire & Forget");
            Require(locked != s.PickTLStavesProfile(schoolType, CastType::FireAndForget, sneak), "Locked staff ritual aliases Fire & Forget");
            Require(*normal == CameraProfile::Default3p() && *locked == CameraProfile{}, "Staff ritual defaults changed");
            const auto slot = s.SlotFromTLProfile(locked);
            Require(slot.has_value(), "Staff ritual missing enemy override slot");
            const auto slotIndex = static_cast<std::size_t>(*slot);
            Require(slotIndex > static_cast<std::size_t>(SettingsManager::TLSlot::MountsHorsebackArcheryDraw), "New staff slot renumbered old slots");
            Require(&s.GetTLProfileBySlot(*slot) == locked, "Staff ritual TL slot resolves incorrectly");
            for (bool targetLock : {false, true}) {
                auto* profile = targetLock ? locked : normal;
                const auto fullKey = targetLock ? "target_lock." + key : key;
                const auto entry = std::find_if(eligible.begin(), eligible.end(), [&](const auto& e) { return e.tomlKey == fullKey; });
                Require(entry != eligible.end() && entry->outdoor == profile, "Staff ritual missing from environment/Quick Tune registry: " + fullKey);
                const auto& inventory = targetLock ? targetLockProfiles : categoryProfiles;
                Require(std::count(inventory.begin(), inventory.end(), profile) == 1, "Staff ritual missing/duplicated in copy/reset inventory: " + fullKey);
                const auto index = static_cast<std::size_t>(entry - eligible.begin());
                Require(index >= 1221, "New ritual entry renumbered a format-1 indoor/location slot");
                profile->zoom = float(30 + cell);
                profile->transitionZoom = 0.3f;
                profile->transitionSetZoom = true;
                profile->SyncTransitionOverride();
                s.IndoorVariantOf(profile)->zoom = float(60 + cell);
                location.profileSet[index] = location.profileSetIndoor[index] = true;
                location.profiles[index].zoom = float(90 + cell);
                location.profilesIndoor[index].zoom = float(120 + cell);
                ++cell;
            }
            s.stateNoise[key].enabled = true;
            s.stateNoise[key].parentSeeded = true;
            s.stateNoise[key].repulse = 0.75f;
            s.stateNoise[key].shoutFadeDuration = 0.6f;
            s.stateNoiseIndoor[key].enabled = true;
            s.stateNoiseIndoor[key].parentSeeded = true;
            s.stateNoiseIndoor[key].repulse = 0.25f;
            s.stateFirstPerson[key].worldFov = 103;
            s.stateFirstPerson[key].repulse = 0.5f;
            location.stateNoise[key] = s.stateNoise[key];
            location.fpState[key] = s.stateFirstPerson[key];
            s.enemyOverrides[0][slotIndex] = {CameraProfile{.zoom = 42}, true};
            s.enemyOverridesIndoor[0][slotIndex] = {CameraProfile{.zoom = 43}, true};
            enemy.slots[slotIndex] = {CameraProfile{.zoom = 44}, true};
            enemy.slotsIndoor[slotIndex] = {CameraProfile{.zoom = 45}, true};
        }
    }
    Require(cell == 20, "Incomplete staff ritual matrix");
    s.locationOverrides.push_back(std::move(location));
    s.customEnemyOverrides.push_back(std::move(enemy));
    const auto expected = Snapshot(s);
    const auto saved = RoundTrip(s);
    Require(saved["meta"]["format"].value_or(0) == kCurrentPresetFormat && kCurrentPresetFormat >= 2,
            "Staff tuning was stamped with an incorrect format");
    Compare(expected, Snapshot(s), "Staff ritual camera/noise/first-person/location settings changed on reload");
    for (int school = 1; school <= 5; ++school)
        for (const bool sneak : {false, true}) {
            const auto slot = s.SlotFromTLProfile(s.PickTLStavesProfile(static_cast<MagicSchool>(school), CastType::Ritual, sneak));
            const auto i = static_cast<std::size_t>(*slot);
            Require(s.enemyOverrides[0][i].fieldsEnabled && s.enemyOverrides[0][i].profile.zoom == 42 &&
                s.enemyOverridesIndoor[0][i].fieldsEnabled && s.enemyOverridesIndoor[0][i].profile.zoom == 43,
                "Built-in enemy staff ritual override lost on reload");
            Require(s.customEnemyOverrides.size() == 1 && s.customEnemyOverrides[0].slots[i].fieldsEnabled &&
                s.customEnemyOverrides[0].slots[i].profile.zoom == 44 && s.customEnemyOverrides[0].slotsIndoor[i].fieldsEnabled &&
                s.customEnemyOverrides[0].slotsIndoor[i].profile.zoom == 45, "Custom enemy staff ritual override lost on reload");
        }
    Require(saved == s.BuildSaveTable(), "Staff ritual preset rewrite is unstable");
    s.ResetAllToVanilla();
    Require(s.stateNoise.empty() && s.stateFirstPerson.empty() && s.locationOverrides.empty(), "Staff ritual reset retained authored overrides");
    Require(s.PickStavesProfile(MagicSchool::None, CastType::Ritual, false) == &s.weaponsStaves, "School-less staff fallback changed");
}

#include "HitShakePresetChecks.inc"
#include "MagicHitShakePresetChecks.inc"

int main(int argc, char** argv) try
{
    auto& s = SettingsManager::GetSingleton();
    s.InitIndoorOverrides();
    s.EnsureDialogueDefaultLooks();
    const auto freshInstall = Snapshot(s);
    s.ResetAllToVanilla();
    const auto defaultSnapshot = Snapshot(s);
    Compare(freshInstall, defaultSnapshot, "Fresh install and Reset All disagree");
    AuthorFixture(s);
    s.ResetAllToVanilla();
    Compare(defaultSnapshot, Snapshot(s), "Reset All retained authored camera settings");
    const auto defaults = s.BuildSaveTable();
    auto scalarEdits = defaults;
    RetuneScalars(scalarEdits);
    Require(s.ApplyTable(scalarEdits), "Retuned scalar fixture failed to load");
    s.ResetAllToVanilla();
    Compare(defaultSnapshot, Snapshot(s), "Reset All retained scalar tuning");
    Require(defaults == s.BuildSaveTable(), "Reset All retained serialized settings outside the frozen snapshot");
    RoundTrip(s);
    if (defaults != s.BuildSaveTable()) {
        std::ofstream("preset-before.toml") << defaults;
        std::ofstream("preset-after.toml") << s.BuildSaveTable();
        throw std::runtime_error("Default preset is unstable after reload (preset-before/after.toml)");
    }

    // Every registered category, Target Lock and shout profile. Exercise values
    // equal to a different family's baseline: the sparse writer must keep them.
    const CameraProfile samples[] = { CameraProfile{}, CameraProfile::VanillaHorseback(),
        CameraProfile::WerewolfDefault(), { .sideOffset = 42, .height = 12, .zoom = 23, .fov = 92 },
        [] { CameraProfile p; p.transitionSetPitchBias = true; p.transitionPitchBias = -0.75f;
            p.SyncTransitionOverride(); return p; }() };
    for (const auto& sample : samples) {
        s.ResetAllToVanilla();
        for (auto e : s.GetIndoorEligibleProfiles()) *e.outdoor = sample;
        s.InitIndoorOverrides();
        RoundTrip(s);
        for (auto e : s.GetIndoorEligibleProfiles())
            Require(*e.outdoor == sample, "Outdoor roundtrip: " + e.tomlKey);
    }

    // A tuned outdoor parent and an independent all-default indoor/location
    // snapshot must not turn into an inheritance relationship during loading.
    s.ResetAllToVanilla();
    for (auto e : s.GetIndoorEligibleProfiles()) *e.outdoor = samples[3];
    s.InitIndoorOverrides();
    for (auto e : s.GetIndoorEligibleProfiles()) *s.IndoorVariantOf(e.outdoor) = CameraProfile{};
    RoundTrip(s);
    for (auto e : s.GetIndoorEligibleProfiles())
        Require(*s.IndoorVariantOf(e.outdoor) == CameraProfile{}, "Indoor roundtrip: " + e.tomlKey);

    s.sheathed.transitionAimBias = 0.65f;
    s.sheathed.transitionSetAimBias = false;
    s.sheathed.SyncTransitionOverride();
    RoundTrip(s);
    Require(s.sheathed.transitionAimBias == 0.65f && !s.sheathed.transitionSetAimBias,
        "Disabled Aim Bias lost its stored tuning");

    s.tlSheathed.transitionPitchBias = -0.65f;
    s.tlSheathed.transitionSetPitchBias = false;
    s.tlSheathed.SyncTransitionOverride();
    s.enemyOverrides[0][0].profile.transitionSetPitchBias = true;
    s.enemyOverrides[0][0].profile.transitionPitchBias = 0.0f;
    s.enemyOverrides[0][0].profile.SyncTransitionOverride();
    RoundTrip(s);
    Require(s.tlSheathed.transitionPitchBias == -0.65f && !s.tlSheathed.transitionSetPitchBias,
        "Disabled Pitch Bias lost its stored tuning");
    Require(s.enemyOverrides[0][0].profile.transitionSetPitchBias &&
        s.enemyOverrides[0][0].profile.transitionPitchBias == 0 && s.enemyOverrides[0][0].profile.TransitionAnySet(),
        "An explicit neutral enemy Pitch Bias was pruned");
    s.ResetAllToVanilla();
    Require(s.tlSheathed.transitionPitchBias == 0 && !s.tlSheathed.transitionSetPitchBias,
        "Pitch Bias did not reset to neutral");

    AuthorFixture(s);
    const auto authored = Snapshot(s);
    const auto authoredTable = RoundTrip(s);
    Compare(authored, Snapshot(s), "Authored preset changed on reload");
    const auto firstReload = s.BuildSaveTable();
    if (authoredTable != firstReload) {
        std::ofstream("preset-before.toml") << authoredTable;
        std::ofstream("preset-after.toml") << firstReload;
        throw std::runtime_error("Authored preset table changed after loading (preset-before/after.toml)");
    }
    RoundTrip(s);
    Require(firstReload == s.BuildSaveTable(), "Authored preset rewrite is unstable");

    const fs::path fixtures = argc > 1 ? argv[1] : "tests/fixtures";
    const bool record = argc > 2 && std::string_view(argv[2]) == "--record-v1";
    if (record) {
        Require(kCurrentPresetFormat == 1, "Never regenerate the 1.0 fixtures for a later format");
        std::ofstream(fixtures / "preset-v1-defaults.json") << defaultSnapshot.dump(2) << '\n';
        std::ofstream(fixtures / "preset-v1-authored.toml") << authoredTable;
        std::ofstream(fixtures / "preset-v1-authored.json") << authored.dump(2) << '\n';
    } else {
        std::ifstream defaultFile(fixtures / "preset-v1-defaults.json");
        Json pinnedDefaults; defaultFile >> pinnedDefaults;
        CompareV1(pinnedDefaults, defaultSnapshot, "Released 1.0 defaults changed");
        s.ResetAllToVanilla();
        Require(s.ApplyTable(toml::parse_file((fixtures / "preset-v1-authored.toml").string())), "Frozen preset failed to load");
        std::ifstream authoredFile(fixtures / "preset-v1-authored.json");
        Json pinnedAuthored; authoredFile >> pinnedAuthored;
        CompareV1(pinnedAuthored, Snapshot(s), "Released 1.0 preset changed meaning");
    }

    // Unknown future formats must leave current tuning untouched.
    const auto before = s.BuildSaveTable();
    Require(!s.ApplyTable(toml::parse("[meta]\nformat = " + std::to_string(kCurrentPresetFormat + 1) +
        "\n[general]\ntransition_mul_zoom = 0.1")), "Future format accepted");
    Require(before == s.BuildSaveTable(), "Rejected format changed current tuning");
    Require(!CanReadPreset(toml::parse("[meta]\nformat = 4294967297")), "Format integer truncated");
    Require(!CanReadPreset(toml::parse("[meta]\nformat = '1'")), "Wrong format type accepted");
    Require(!CanReadPreset(toml::parse("[meta]\nformat = -1")), "Negative format accepted");
    Require(!CanReadPreset(toml::parse("[meta]\nformat = 1.0")), "Float format accepted");
    Require(!CanReadPreset(toml::parse("meta = false")), "Malformed metadata accepted");
    Require(!s.ApplyTable(toml::parse("[sheathed]\nzoom_offset = nan")), "NaN camera value accepted");
    Require(!CanReadPreset(toml::parse("[first_person]\nworld_fov = inf")), "Infinite FOV accepted");
    Require(!CanReadPreset(toml::parse("[first_person]\nworld_fov = 1e100")), "Float overflow accepted");
    toml::table collision;
    InsertPresetTable(collision, "archery", toml::table{{"zoom_offset", 23.0}});
    InsertPresetTable(collision, "archery.zoom", toml::table{{"zoom_offset", 12.0}});
    Require(collision["archery"]["zoom_offset"].value_or(0.0) == 23, "Zoomed child replaced parent zoom");
    bool rejected = false;
    try { InsertPresetTable(collision, "archery.zoom_offset", toml::table{{"set", true}}); }
    catch (const std::logic_error&) { rejected = true; }
    Require(rejected, "Scalar/table key collision silently accepted");
    CheckStaffRitualProfiles(s);
    CheckNpcMeleeNoise(s);
    CheckNpcWeaponNoiseOverrides(s);
    CheckLegacyNpcWeaponNoiseOverrides(s);
    CheckNpcSpecificBindings(s);
    CheckNpcShouts(s);
    CheckNpcArcheryAndTransformations(s);
    CheckHitShakePresets(s);
    CheckMagicHitShakePresets(s);
    CheckPresetFiles(s);
    std::cout << "Preset compatibility checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
