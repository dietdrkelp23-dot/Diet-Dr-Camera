#include "Camera/AnimationCatalog.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <regex>
#include <tuple>
#include <nlohmann/json.hpp>

namespace DietDrCamera
{
    namespace
    {
        std::string Utf8(const std::filesystem::path& p)
        {
            const auto u = p.generic_u8string();
            return {reinterpret_cast<const char*>(u.data()), u.size()};
        }
        std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        bool NpcOnlyLabel(const std::string& label)
        {
            const auto name = Lower(label);
            // Explicit pack labels cover auxiliary MCO clips with no identity
            // condition of their own. Do not blacklist creature words: e.g.
            // "Dragon's Dogma" can be a perfectly valid player moveset.
            return (name.starts_with("npc - ") || name.starts_with("npc only") || name.starts_with("npc-only")) &&
                   name.find("player") == std::string::npos;
        }
        using Json = nlohmann::json;
        struct Metadata { std::string name; Json conditions = Json::array(); bool hasConditions = false; };
        Metadata ReadMetadata(const std::filesystem::path& directory, std::size_t& skipped)
        {
            Metadata result{Utf8(directory.filename())};
            // user.json is OAR's local override of config.json.
            for (const auto* file : {"config.json", "user.json"}) {
                std::ifstream stream(directory / file);
                if (!stream) continue;
                const auto json = nlohmann::json::parse(stream, nullptr, false);
                if (json.is_discarded() || !json.is_object()) { ++skipped; continue; }
                if (json.contains("name") && json["name"].is_string()) {
                    const auto value = json["name"].get<std::string>();
                    if (!value.empty()) result.name = value;
                }
                if (json.contains("conditions") && json["conditions"].is_array()) {
                    result.conditions = json["conditions"];
                    result.hasConditions = true;
                }
            }
            return result;
        }

        // Static applicability only. Dynamic combat/equipment/target conditions
        // remain unknown so animations do not disappear when the player is idle.
        enum class Possible { No, Yes, Unknown };
        Possible Negate(Possible value)
        {
            return value == Possible::Yes ? Possible::No : value == Possible::No ? Possible::Yes : value;
        }
        bool Flag(const Json& node, const char* key)
        {
            return node.contains(key) && node[key].is_boolean() && node[key].get<bool>();
        }
        Possible FormMatches(const Json& value, const AnimationFormIdentity& form)
        {
            if (!value.is_object()) return Possible::Unknown;
            if (value.contains("editorID") && value["editorID"].is_string()) {
                if (form.editorID.empty()) return Possible::Unknown;
                return Lower(value["editorID"].get<std::string>()) == Lower(form.editorID) ? Possible::Yes : Possible::No;
            }
            if (!value.contains("pluginName") || !value["pluginName"].is_string() ||
                !value.contains("formID") || !value["formID"].is_string() || form.plugin.empty()) return Possible::Unknown;
            auto id = value["formID"].get<std::string>();
            // Some installed configs contain an embedded NUL in a hex form ID.
            std::erase(id, '\0');
            if (id.starts_with("0x") || id.starts_with("0X")) id.erase(0, 2);
            std::uint32_t number = 0;
            const auto parsed = std::from_chars(id.data(), id.data() + id.size(), number, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size()) return Possible::Unknown;
            return Lower(value["pluginName"].get<std::string>()) == Lower(form.plugin) && number == form.localID
                ? Possible::Yes : Possible::No;
        }
        Possible EvaluateSet(const Json&, const AnimationPlayerContext&, const AnimationFormIdentity*, bool, int);
        Possible EvaluateCondition(const Json& node, const AnimationPlayerContext& player,
            const AnimationFormIdentity* race, int depth)
        {
            if (depth > 32 || !node.is_object() || !node.contains("condition") || !node["condition"].is_string())
                return Possible::Unknown;
            const auto name = Lower(node["condition"].get<std::string>());
            Possible value = Possible::Unknown;
            if ((name == "and" || name == "or" || name == "player") && node.contains("Conditions"))
                value = EvaluateSet(node["Conditions"], player, race, name == "or", depth + 1);
            else if (name == "isactorbase" && node.contains("Actor base")) value = FormMatches(node["Actor base"], player.actorBase);
            else if (name == "isform" && node.contains("Form")) value = FormMatches(node["Form"], player.reference);
            else if (name == "israce" && node.contains("Race") && race) value = FormMatches(node["Race"], *race);
            // TARGET/MOUNT/custom wrappers must stay unknown: their children
            // describe another actor, not the player running this animation.
            return Flag(node, "negated") ? Negate(value) : value;
        }
        Possible EvaluateSet(const Json& nodes, const AnimationPlayerContext& player,
            const AnimationFormIdentity* race, bool any, int depth)
        {
            if (!nodes.is_array()) return Possible::Unknown;
            bool unknown = false;
            for (const auto& node : nodes) {
                if (Flag(node, "disabled")) continue;
                const auto value = EvaluateCondition(node, player, race, depth);
                if (any && value == Possible::Yes) return Possible::Yes;
                if (!any && value == Possible::No) return Possible::No;
                unknown |= value == Possible::Unknown;
            }
            return unknown ? Possible::Unknown : any ? Possible::No : Possible::Yes;
        }
        bool CanApplyToPlayer(const Json& conditions, const AnimationPlayerContext& player)
        {
            if (player.races.empty()) return EvaluateSet(conditions, player, nullptr, false, 0) != Possible::No;
            return std::any_of(player.races.begin(), player.races.end(), [&](const auto& race) {
                return EvaluateSet(conditions, player, &race, false, 0) != Possible::No;
            });
        }
        Json ReadLegacyConditions(const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            Json result = Json::array(), alternatives = Json::array();
            std::string line;
            static const std::regex identity(R"re(^\s*(NOT\s+)?(IsActorBase|IsForm|IsRace)\s*\(\s*"([^"]+)"\s*\|\s*(0[xX][0-9a-fA-F]+)\s*\))re", std::regex::icase);
            while (std::getline(stream, line)) {
                if (const auto comment = line.find(';'); comment != std::string::npos) line.resize(comment);
                const auto end = line.find_last_not_of(" \t\r\n");
                if (end == std::string::npos) continue;
                line.resize(end + 1);
                const bool moreAlternatives = line.ends_with("OR");
                Json node = {{"condition", "unknown"}};
                std::smatch match;
                if (std::regex_search(line, match, identity)) {
                    const auto name = Lower(match[2].str());
                    const char* key = name == "israce" ? "Race" : name == "isform" ? "Form" : "Actor base";
                    node = {{"condition", name}, {"negated", match[1].matched},
                            {key, {{"pluginName", match[3].str()}, {"formID", match[4].str()}}}};
                }
                if (moreAlternatives || !alternatives.empty()) {
                    alternatives.push_back(std::move(node));
                    if (!moreAlternatives) {
                        result.push_back({{"condition", "OR"}, {"Conditions", std::move(alternatives)}});
                        alternatives = Json::array();
                    }
                } else result.push_back(std::move(node));
            }
            if (!alternatives.empty()) result.push_back({{"condition", "OR"}, {"Conditions", std::move(alternatives)}});
            return result;
        }
    }

    std::string AnimationPathKey(std::string path)
    {
        path = Lower(std::move(path));
        std::replace(path.begin(), path.end(), '/', '\\');
        if (const auto at = path.find("meshes\\"); at != std::string::npos &&
            (at == 0 || path[at - 1] == '\\')) path.erase(0, at);
        return path;
    }

    std::string LegacyAnimationPathKey(std::string path)
    {
        path = AnimationPathKey(std::move(path));
        for (const auto* marker : {"openanimationreplacer\\", "dynamicanimationreplacer\\"})
            if (const auto at = path.find(marker); at != std::string::npos) return path.substr(at);
        return path;
    }

    std::string AnimationVariantKey(std::string path, const std::string& variant)
    {
        if (!variant.empty()) {
            if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '\\';
            path += variant;
        }
        return AnimationPathKey(std::move(path));
    }

    bool AnimationSearchMatches(const std::string& text, const std::string& query)
    {
        std::istringstream words(Lower(query));
        std::string word;
        while (words >> word) if (text.find(word) == std::string::npos) return false;
        return true;
    }

    bool IsPlayerAnimationPath(const std::string& path)
    {
        const auto key = AnimationPathKey(path);
        if (key.find("\\_1stperson\\") != std::string::npos ||
            key.find("\\1stperson\\") != std::string::npos) return false;
        for (const auto* root : {"meshes\\actors\\character\\animations\\",
                                "meshes\\actors\\werewolfbeast\\animations\\",
                                "meshes\\actors\\vampirelord\\animations\\"})
            if (key.starts_with(root)) return true;
        return false;
    }

    AnimationCatalogResult ScanAnimationCatalog(const std::filesystem::path& meshes, const AnimationPlayerContext& player)
    {
        namespace fs = std::filesystem;
        AnimationCatalogResult result;
        try {
            std::error_code ec;
            if (!fs::is_directory(meshes, ec)) {
                result.error = "The game's meshes folder could not be read.";
                return result;
            }
            std::map<std::string, Metadata> metadata;
            std::map<std::string, bool> applicable;
            std::set<std::string> keys;
            fs::recursive_directory_iterator it(meshes, fs::directory_options::skip_permission_denied, ec), end;
            while (it != end) {
                const auto entry = *it;
                if (entry.is_symlink(ec)) it.disable_recursion_pending();
                if (entry.is_regular_file(ec) && Lower(Utf8(entry.path().extension())) == ".hkx") {
                    const auto relative = entry.path().lexically_relative(meshes);
                    const auto key = AnimationPathKey("meshes/" + Utf8(relative));
                    std::vector<fs::path> parts;
                    for (const auto& part : relative) parts.push_back(part);
                    std::size_t marker = parts.size();
                    bool legacy = false;
                    for (std::size_t p = 0; p < parts.size(); ++p) {
                        const auto name = Lower(Utf8(parts[p]));
                        if (name == "openanimationreplacer" || name == "dynamicanimationreplacer") {
                            marker = p; legacy = name == "dynamicanimationreplacer"; break;
                        }
                    }
                    if (IsPlayerAnimationPath(key) && marker + 3 < parts.size()) {
                        auto modPath = meshes;
                        for (std::size_t p = 0; p <= marker + 1; ++p) modPath /= parts[p];
                        const auto subPath = modPath / parts[marker + 2];
                        const auto metadataFor = [&](const fs::path& path) -> const Metadata& {
                            const auto key = Utf8(path);
                            auto [pos, added] = metadata.try_emplace(key);
                            if (added) pos->second = ReadMetadata(path, result.skipped);
                            return pos->second;
                        };
                        CatalogAnimation animation;
                        animation.key = key;
                        animation.filename = Utf8(entry.path().filename());
                        animation.modKey = AnimationPathKey("meshes/" + Utf8(modPath.lexically_relative(meshes)));
                        animation.submodKey = AnimationPathKey("meshes/" + Utf8(subPath.lexically_relative(meshes)));
                        animation.mod = legacy ? "Legacy DAR - " + Utf8(parts[marker + 1]) : metadataFor(modPath).name;
                        animation.submod = legacy ? Utf8(parts[marker + 2]) : metadataFor(subPath).name;
                        auto [eligibility, newSubmod] = applicable.try_emplace(animation.submodKey, true);
                        if (newSubmod) {
                            const auto& meta = metadataFor(subPath);
                            Json conditions = meta.conditions;
                            if (legacy && !meta.hasConditions) {
                                if (Lower(Utf8(parts[marker + 1])) == "_customconditions")
                                    conditions = ReadLegacyConditions(subPath / "_conditions.txt");
                                else conditions = Json::array({{{"condition", "IsActorBase"},
                                    {"Actor base", {{"pluginName", Utf8(parts[marker + 1])}, {"formID", Utf8(parts[marker + 2])}}}}});
                            }
                            eligibility->second = !NpcOnlyLabel(animation.mod) && !NpcOnlyLabel(animation.submod) &&
                                CanApplyToPlayer(conditions, player);
                        }
                        animation.firstPerson = animation.key.find("\\_1stperson\\") != std::string::npos ||
                                                animation.key.find("\\1stperson\\") != std::string::npos;
                        animation.variant = Lower(Utf8(entry.path().parent_path().filename())).starts_with("_variants_");
                        if (animation.variant)
                            animation.filename = Utf8(entry.path().parent_path().filename()).substr(10) + " / " + animation.filename;
                        // OAR uses a variants directory instead of a sibling HKX
                        // with that animation's name, when both are installed.
                        const auto variants = entry.path().parent_path() / ("_variants_" + Utf8(entry.path().stem()));
                        const bool shadowed = !legacy && !animation.variant &&
                            fs::exists(variants, ec) && fs::is_directory(variants, ec);
                        if (eligibility->second && !shadowed && keys.insert(animation.key).second) {
                            animation.search = Lower(animation.mod + " " + animation.submod + " " +
                                                     animation.filename + " " + animation.key);
                            result.animations.push_back(std::move(animation));
                        }
                    }
                }
                if (ec) { ++result.skipped; ec.clear(); }
                it.increment(ec);
                if (ec) { ++result.skipped; ec.clear(); }
            }
            std::sort(result.animations.begin(), result.animations.end(), [](const auto& a, const auto& b) {
                return std::tuple(Lower(a.mod), a.modKey, Lower(a.submod), a.submodKey, Lower(a.filename), a.key) <
                       std::tuple(Lower(b.mod), b.modKey, Lower(b.submod), b.submodKey, Lower(b.filename), b.key);
            });
        } catch (const std::exception&) {
            result.error = "Some animation folders could not be read. Refresh to try again.";
        }
        return result;
    }

    void AnimationCatalog::Refresh(const std::filesystem::path& meshes, AnimationPlayerContext player)
    {
        if (_job.valid()) return;
        _started = true;
        _job = std::async(std::launch::async, [meshes, player = std::move(player)] { return ScanAnimationCatalog(meshes, player); });
    }
    void AnimationCatalog::Poll()
    {
        if (_job.valid() && _job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            _result = _job.get();
            ++_revision;
        }
    }
}
