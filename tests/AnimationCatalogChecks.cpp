#include "Camera/AnimationCatalog.h"
#include "Camera/AnimationMatching.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace DietDrCamera;
namespace fs = std::filesystem;
static void Require(bool pass, const char* text) { if (!pass) throw std::runtime_error(text); }
static void Write(const fs::path& path, const char* content = "hkx fixture")
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path); file << content;
    Require(static_cast<bool>(file), "fixture write failed");
}
int main(int argc, char** argv)
{
    AnimationPlayerContext player;
    player.races = {{"Skyrim.esm", 0x13746, "NordRace"}, {"Skyrim.esm", 0xCDD84, "WerewolfBeastRace"},
                    {"Dawnguard.esm", 0x283A, "DLC1VampireBeastRace"}};
    if (argc > 1) {
        const auto started = std::chrono::steady_clock::now();
        const auto scan = ScanAnimationCatalog(fs::path(argv[1]), player);
        std::cout << scan.animations.size() << " files, " << scan.skipped << " skipped, "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                  << " seconds; " << scan.error << '\n';
        for (std::size_t i = 0; i < std::min(std::size_t{4}, scan.animations.size()); ++i)
            std::cout << scan.animations[i].mod << " / " << scan.animations[i].submod << " / "
                      << scan.animations[i].filename << '\n';
        return scan.error.empty() ? 0 : 1;
    }
    // An exclusively created test directory is the only cleanup target.
    const auto root = fs::temp_directory_path() / ("ddc-animation-catalog-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        Require(fs::create_directory(root), "fixture directory collision");
        const auto meshes = root / "meshes";
        const auto oar = meshes / "actors/character/animations/OpenAnimationReplacer";
        Write(oar / "Moves/config.json", R"({"name":"Combat Moves"})");
        Write(oar / "Moves/100/config.json", R"({"name":"Fire and Forget"})");
        Write(oar / "Moves/100/user.json", R"({"name":"My Casting"})");
        Write(oar / "Moves/100/Fire.HKX");
        Write(oar / "Moves/100/_variants_idle/a.hkx");
        Write(oar / "Moves/100/_variants_idle/b.hkx");
        Write(oar / "Moves/100/idle.hkx"); // shadowed by variants directory
        Write(oar / "Moves/100/not-an-animation.txt");
        Write(oar / "Moves/200/config.json", "{broken");
        Write(oar / "Moves/200/Fire.hkx"); // same filename, different submod
        Write(oar / "Moves/200/nested/Turn.hkx");
        Write(oar / "Moves/200/_1stperson/Arms.hkx");
        Write(meshes / "actors/werewolfbeast/animations/OpenAnimationReplacer/Moves/100/Fire.hkx");
        Write(meshes / "actors/vampirelord/animations/OpenAnimationReplacer/Moves/100/Cast.hkx");
        Write(meshes / "actors/dragon/animations/OpenAnimationReplacer/Dragon/100/Land.hkx");
        Write(meshes / "actors/draugr/animations/OpenAnimationReplacer/Undead/100/Attack.hkx");
        Write(meshes / "actors/character/animations/DynamicAnimationReplacer/_CustomConditions/42/Idle.hkx");
        Write(meshes / "actors/character/animations/DynamicAnimationReplacer/Skyrim.esm/000007/Idle.hkx");
        Write(meshes / "actors/character/animations/ordinary.hkx");
        const auto result = ScanAnimationCatalog(meshes, player);
        Require(result.error.empty() && result.animations.size() == 9, "catalog inventory/shadowing");
        Require(result.skipped == 1, "malformed metadata must be reported and recoverable");
        const auto fire = std::find_if(result.animations.begin(), result.animations.end(), [](const auto& a) {
            return a.mod == "Combat Moves" && a.filename == "Fire.HKX";
        });
        Require(fire != result.animations.end() && fire->submod == "My Casting", "OAR names/user override");
        Require(AnimationSearchMatches(fire->search, "FIRE combat") && !AnimationSearchMatches(fire->search, "fire missing"),
                "search words should match across mod, submod and filename");
        Require(fire->key == AnimationVariantKey("Data/Meshes/Actors/Character/Animations/OpenAnimationReplacer/Moves/100/Fire.HKX", ""),
                "browser and runtime file identity differ");
        const auto variantKey = AnimationVariantKey("data/meshes/actors/character/animations/OpenAnimationReplacer/Moves/100/_variants_idle", "B.HKX");
        Require(std::count_if(result.animations.begin(), result.animations.end(), [&](const auto& a) {
            return a.variant && a.key == variantKey;
        }) == 1, "variant file identity");
        Require(std::all_of(result.animations.begin(), result.animations.end(), [](const auto& a) {
            return IsPlayerAnimationPath(a.key) && !a.firstPerson;
        }), "creature or first-person arm file leaked into player-body library");
        Require(!IsPlayerAnimationPath("meshes/actors/dragon/animations/OpenAnimationReplacer/A/B/character.hkx") &&
                !IsPlayerAnimationPath("meshes/actors/characterOther/animations/OpenAnimationReplacer/A/B/a.hkx"),
                "player filter matched a filename or partial actor folder");
        Require(LegacyAnimationPathKey(fire->key) == "openanimationreplacer\\moves\\100\\fire.hkx", "old preset key compatibility");
        Require(AnimationPathKey("C:/Game/Data/MESHES/Actors/Character/Animations/X.HKX") == "meshes\\actors\\character\\animations\\x.hkx", "absolute path normalization");
        Require(!ScanAnimationCatalog(root / "missing").error.empty(), "missing root must report an error");

        // NPC animations can live in the character folder. Filter only static
        // identity restrictions, preserving dynamic/unknown conditions and ORs.
        const auto restricted = root / "restricted/meshes";
        const auto packs = restricted / "actors/character/animations/OpenAnimationReplacer";
        const auto pack = [&](const char* name, const char* conditions) {
            Write(packs / name / "1/config.json", conditions);
            Write(packs / name / "1/idle.hkx");
        };
        pack("DraugrSleep", R"({"conditions":[{"condition":"OR","Conditions":[{"condition":"IsRace","Race":{"pluginName":"Magink Bosses.esp","formID":"819\u000072"}}]}]})");
        pack("Excludes player", R"({"conditions":[{"condition":"IsActorBase","negated":true,"Actor base":{"pluginName":"Skyrim.esm","formID":"000007"}}]})");
        pack("Follower only", R"({"conditions":[{"condition":"IsForm","Form":{"pluginName":"Skyrim.esm","formID":"A2C94"}}]})");
        pack("beta", R"({"conditions":[{"condition":"OR","Conditions":[{"condition":"IsRace","Race":{"editorID":"DraugrRace"}},{"condition":"IsRace","Race":{"editorID":"NordRace"}}]}]})");
        pack("Alpha", R"({"conditions":[{"condition":"IsEquippedHasKeyword","Keyword":{"editorID":"WeapTypeBow"}}]})");
        pack("Negated group", R"({"conditions":[{"condition":"AND","negated":true,"Conditions":[{"condition":"IsActorBase","Actor base":{"pluginName":"Skyrim.esm","formID":"7"}}]}]})");
        pack("Target draugr", R"({"conditions":[{"condition":"TARGET","Conditions":[{"condition":"IsRace","Race":{"editorID":"DraugrRace"}}]}]})");
        pack("User override", R"({"conditions":[{"condition":"IsActorBase","negated":true,"Actor base":{"pluginName":"Skyrim.esm","formID":"7"}}]})");
        Write(packs / "User override/1/user.json", R"({"conditions":[]})");
        pack("Disabled condition", R"({"conditions":[{"condition":"IsActorBase","disabled":true,"negated":true,"Actor base":{"pluginName":"Skyrim.esm","formID":"7"}}]})");
        pack("Impossible races", R"({"conditions":[{"condition":"IsRace","Race":{"editorID":"NordRace"}},{"condition":"IsRace","Race":{"editorID":"WerewolfBeastRace"}}]})");
        pack("Transform", R"({"conditions":[{"condition":"IsRace","Race":{"editorID":"DLC1VampireBeastRace"}}]})");
        pack("NPC - auxiliary attack", R"({"conditions":[{"condition":"IsSprinting"}]})");
        pack("Dragon's Dogma Player", R"({"conditions":[]})");
        const auto dar = restricted / "actors/character/animations/DynamicAnimationReplacer";
        Write(dar / "_CustomConditions/1/idle.hkx");
        Write(dar / "_CustomConditions/1/_conditions.txt", "NOT IsActorBase(\"Skyrim.esm\"|0x000007)\n");
        Write(dar / "_CustomConditions/2/idle.hkx");
        Write(dar / "_CustomConditions/2/_conditions.txt", "IsRace(\"NPC.esp\"|0x858) OR\nIsRace(\"Skyrim.esm\"|0x13746)\nIsInCombat()\n");
        Write(dar / "_CustomConditions/3/idle.hkx");
        Write(dar / "_CustomConditions/3/_conditions.txt", "IsRace(\"Humanoid Dragon Priest.esp\"|0x858)\n");
        Write(dar / "Skyrim.esm/000007/idle.hkx");
        Write(dar / "Skyrim.esm/00A2C94/idle.hkx");
        const auto filtered = ScanAnimationCatalog(restricted, player);
        Require(filtered.error.empty() && filtered.animations.size() == 9, "static player eligibility inventory");
        Require(std::none_of(filtered.animations.begin(), filtered.animations.end(), [](const auto& a) {
            return a.mod == "DraugrSleep" || a.mod == "Excludes player" || a.mod == "Follower only" ||
                   a.mod == "Negated group" || a.mod == "Impossible races";
        }), "non-player conditions survived filtering");
        Require(filtered.animations[0].mod == "Alpha" && filtered.animations[1].mod == "beta",
                "alphabetical order must ignore capitalization");
        std::cout << "PASS static player conditions: OAR/DAR, race alternatives, NPC exclusions, overrides, target isolation, alphabetic sorting\n";
        std::unordered_map<std::string, std::uint32_t> bindings;
        const auto family = AnimationPathKey("Data/meshes/actors/character/animations/OpenAnimationReplacer/Moves/100/_variants_idle");
        bindings[LegacyAnimationPathKey(family)] = 10;
        bindings[variantKey] = 20;
        Require(FindAnimationBinding(bindings, variantKey, family) == 20, "exact variant must beat legacy family");
        Require(FindAnimationBinding(bindings, AnimationVariantKey(family, "a.hkx"), family) == 10, "legacy family fallback");
        std::vector<ActiveAnimationMatch> active;
        int idleClip, attackClip;
        Require(UpdateAnimationMatches(active, &idleClip, 10, false) == 10, "idle activation");
        Require(UpdateAnimationMatches(active, &attackClip, 20, false) == 20, "latest attack wins");
        Require(UpdateAnimationMatches(active, &idleClip, 10, true) == 20, "unchanged refresh stole attack priority");
        Require(UpdateAnimationMatches(active, &attackClip, 30, true) == 30, "variant changed without activation");
        Require(UpdateAnimationMatches(active, &attackClip, 0, true) == 10, "unmatched replacement must release to prior clip");
        Require(UpdateAnimationMatches(active, &idleClip, 0, false) == 0, "last clip must release camera");
        Require(fs::canonical(root).parent_path() == fs::canonical(fs::temp_directory_path()) &&
                root.filename().string().starts_with("ddc-animation-catalog-"), "cleanup escaped fixture root");
        fs::remove_all(root);
        std::cout << "PASS OAR/DAR catalog, names, variants, duplicate filenames/projects, fallback metadata and path compatibility\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        // Keep a failed fixture for inspection.
        return 1;
    }
}
