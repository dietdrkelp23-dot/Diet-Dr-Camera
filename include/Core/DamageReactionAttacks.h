#pragma once

#include "Core/DamageReaction.h"
#include <atomic>
#include <initializer_list>
#include <string_view>

namespace DietDrCamera::DamageReaction
{
    inline bool Contains(std::string_view text, std::string_view needle)
    {
        const auto lower = [](char c) { return c == '/' ? '\\' : c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c; };
        return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
            [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
    }

    enum class Family {
        Other, Giant, Centurion, Dragon, HeavyCreature, BitingCreature, Creature,
        FrostAtronach, StoneConstruct, DwarvenSpider, DwarvenSphere, DwarvenBallista,
        HeavyClawed, SmallBiter, Mammoth, Tusked, Hoofed, Horned, Chaurus, Spider, LargeSpider,
        Mudcrab, Clawed, IceWraith, Spirit, Netch, Lurker, SpearUser, BoneColossus, Construct, Count
    };
    inline Family CreatureFamily(std::string_view race, std::string_view skeleton,
        bool giant, bool dragon, bool creature, bool construct = false)
    {
        const auto named = [&](std::initializer_list<std::string_view> names) {
            for (const auto name : names) if (Contains(race, name)) return true;
            return false;
        };
        const auto rig = [&](std::initializer_list<std::string_view> names) {
            for (const auto name : names) if (Contains(skeleton, name)) return true;
            return false;
        };
        // Race editor IDs and skeletons, not localized actor names. Specific
        // anatomies precede shared rigs/keywords (ballista is not a centurion;
        // lurkers and bone colossi share giant attacks without wielding clubs).
        // See DAMAGE-REACTION-COVERAGE.md for UESP and game-record provenance.
        if (named({"bonecolossus"}) || rig({"bonecolossus"})) return Family::BoneColossus;
        if (named({"lurker"}) || rig({"\\benthiclurker\\"})) return Family::Lurker;
        if (named({"dwarvenballista"}) || rig({"\\dwarvenballistacenturion\\"})) return Family::DwarvenBallista;
        if (named({"dwarvencenturion", "ld_forgemaster"}) || rig({"\\dwarvensteamcenturion\\"})) return Family::Centurion;
        if (named({"dwarvensphere"}) || rig({"\\dwarvenspherecenturion\\"})) return Family::DwarvenSphere;
        if (named({"dwarvenspider"}) || rig({"\\dwarvenspider\\", "\\dwarvenspidercenturion\\"})) return Family::DwarvenSpider;
        if (named({"atronachfrost", "frostatronach"}) || rig({"\\atronachfrost\\"})) return Family::FrostAtronach;
        if (named({"atronachstorm", "stormatronach", "ashguardian"}) || rig({"\\atronachstorm\\"})) return Family::StoneConstruct;
        if (giant || rig({"actors\\giant\\"}) || named({"giantrace"})) return Family::Giant;
        // Swarms and anomalies borrow animal rigs without their anatomy.
        if (named({"magicanomaly", "wisp", "witchlight", "wailingwraith"}) || rig({"\\witchlight\\", "\\wisp\\"})) return Family::Spirit;
        // The spectral dragon spell is an ice-wraith rig, not a grounded dragon.
        if (named({"icewraith", "spectraldragon"}) || rig({"\\icewraith\\"})) return Family::IceWraith;
        if (dragon || rig({"actors\\dragon\\"})) return Family::Dragon;
        if (named({"mammoth"}) || rig({"\\mammoth\\"})) return Family::Mammoth;
        if (named({"horker", "boarrace", "mountedriekling"}) || rig({"\\horker\\", "\\boarriekling\\"})) return Family::Tusked;
        if (named({"bear", "troll", "werewolf", "sabrecat", "gargoyle", "vampirebeast"}) ||
            rig({"\\bear\\", "\\troll\\", "\\werewolfbeast\\", "\\sabrecat\\", "\\vampirebrute\\", "\\vampirelord\\"})) return Family::HeavyClawed;
        if (named({"horse", "reindeer"}) || rig({"\\horse\\"})) return Family::Hoofed;
        if (named({"goatrace", "goatdomestic", "cowrace", "deerrace", "deerglow", "elkrace", "whitestag"}) ||
            rig({"\\goat\\", "\\cow\\", "\\deer\\"})) return Family::Horned;
        if (named({"netch"}) || rig({"\\netch\\"})) return Family::Netch;
        if (named({"skeever", "slaughterfish", "ashhopper"}) ||
            rig({"\\skeever\\", "\\slaughterfish\\", "\\scrib\\"})) return Family::SmallBiter;
        if (named({"wolf", "dog", "husky", "deathhound", "nixhound", "foxrace"}) ||
            rig({"\\canine\\", "\\wolf\\", "\\dog\\", "\\fox\\"})) return Family::BitingCreature;
        if (named({"chaurus"}) || rig({"\\chaurus\\", "\\chaurusflyer\\"})) return Family::Chaurus;
        if (named({"frostbitespiderracegiant", "frostbitespiderracelarge", "webmother"})) return Family::LargeSpider;
        if (named({"frostbitespider", "expspider", "elytra", "sse002_spiderrace"}) || rig({"\\frostbitespider\\"})) return Family::Spider;
        if (named({"mudcrab"}) || rig({"\\mudcrab\\"})) return Family::Mudcrab;
        if (named({"spriggan", "hagraven", "falmerrace", "falmerfrozen"}) || rig({"\\spriggan\\", "\\hagraven\\", "\\falmer\\"})) return Family::Clawed;
        if (named({"riekling", "goblin"}) || rig({"\\riekling\\"})) return Family::SpearUser;
        // These use ordinary weapons/spells. Their empty-hand fallback is a
        // humanoid strike; lacking ActorTypeNPC must not invent claws.
        if (named({"draugr", "skeleton", "boneman", "keeper", "zombie", "wightrace", "torturedshade", "ashspawn", "dragonpriest", "ayleidlich", "seeker", "atronachflame"}) ||
            rig({"\\draugr\\", "\\skeleton\\", "\\dragonpriest\\", "\\hmdaedra\\", "\\atronachflame\\"})) return Family::Other;
        if (construct) return Family::Construct;
        return creature ? Family::Creature : Family::Other;
    }

    inline bool NeedsAttackClip(Family family)
    {
        return family == Family::Centurion || family == Family::FrostAtronach;
    }

    inline Kind ClassifyAttack(Family family, Kind weapon, std::string_view event, std::string_view clip, bool bash)
    {
        if (weapon == Kind::Arrow && !bash) return family == Family::DwarvenBallista ? Kind::HeavyProjectile : weapon;
        const auto attack = clip.empty() ? event : clip;
        const auto has = [&](std::string_view name) { return Contains(clip, name) || Contains(event, name); };
        if (family == Family::Giant) {
            if (Contains(attack, "stomp")) return Kind::GiantStomp;
            if (bash || Contains(attack, "handswipe") || Contains(attack, "backattack")) return Kind::GiantSwipe;
            if (Contains(attack, "club") || Contains(attack, "forwardpower") || weapon != Kind::Unarmed) return Kind::GiantClub;
            return Kind::GiantSwipe;
        }
        if (family == Family::Centurion) {
            // Vanilla SteamCenturion.nif: left forearm = axe, right = hammer.
            // Shared Chop/Slash/Stab ATKE events select mirrored graph clips;
            // the active clip's NODE name supplies the actual striking hand.
            if (Contains(attack, "axe") || Contains(attack, "left")) return Kind::CenturionAxe;
            if (Contains(attack, "hammer") || Contains(attack, "right")) return Kind::CenturionHammer;
            if (Contains(event, "left")) return Kind::CenturionAxe;
            if (Contains(event, "right")) return Kind::CenturionHammer;
            return Kind::Centurion; // Unknown custom clips retain heavy mechanical recoil.
        }
        if (family == Family::FrostAtronach) {
            if (bash) return Kind::FrostAtronachBlunt;
            // Vanilla anatomy: right arm = ice spike; left arm = broad club.
            // Its ATKE events and clip nodes both retain the R1/L1 suffix.
            for (const auto name : {clip, event}) {
                if (Contains(name, "_r1") || Contains(name, "right")) return Kind::FrostAtronachSpike;
                if (Contains(name, "_l1") || Contains(name, "left")) return Kind::FrostAtronachBlunt;
            }
            return Kind::ConstructBlunt;
        }
        // Creature weapon proxies are sometimes authored as two-handed swords.
        // Interpret these through the actual giant-derived attack vocabulary.
        if (family == Family::Lurker || family == Family::BoneColossus) {
            if (has("stomp")) return Kind::CreatureStomp;
            return Kind::CreatureHeavy;
        }
        if (family == Family::Spirit && weapon == Kind::Unarmed) return Kind::SpiritTouch;
        if (bash) return family == Family::StoneConstruct ? Kind::ConstructBlunt :
            family == Family::HeavyClawed || family == Family::Mammoth ? Kind::CreatureHeavy : Kind::Blunt;
        if (family == Family::HeavyCreature) return Kind::CreatureHeavy;
        if (family == Family::SpearUser && (weapon == Kind::Unarmed || weapon == Kind::HeavyBlade)) return Kind::Spear;
        // A mounted riekling's spear proxy describes the boar charge graph.
        if (family == Family::Tusked && weapon == Kind::HeavyBlade) return Kind::Tusk;
        if (weapon != Kind::Unarmed) return weapon;
        if (family == Family::Dragon) {
            if (Contains(attack, "tail")) return Kind::DragonTail;
            if (Contains(attack, "wing")) return Kind::DragonWing;
            return Kind::DragonBite;
        }
        if (family == Family::HeavyClawed) return has("bite") ? Kind::HeavyBite : Kind::HeavyClaw;
        if (family == Family::StoneConstruct) return Kind::ConstructBlunt;
        if (family == Family::DwarvenSpider) return Kind::MechanicalPierce;
        if (family == Family::DwarvenSphere) return Kind::MechanicalBlade;
        if (family == Family::DwarvenBallista) return Kind::ConstructBlunt;
        if (family == Family::Construct) return Kind::Blunt;
        if (family == Family::Mammoth) return has("stomp") ? Kind::CreatureStomp : Kind::HeavyTusk;
        if (family == Family::Tusked) return Kind::Tusk;
        if (family == Family::Hoofed) return Kind::Hoof;
        if (family == Family::Horned) return Kind::Ram;
        if (family == Family::Netch) return Kind::Tentacle;
        if (family == Family::Spirit) return Kind::SpiritTouch;
        if (family == Family::SmallBiter) return Kind::SmallBite;
        if (family == Family::LargeSpider) return has("bite") ? Kind::HeavyBite : Kind::HeavyClaw;
        if (family == Family::Chaurus) {
            if (has("headbash")) return Kind::Blunt;
            return has("bite") || has("rtol") || has("ltor") || has("grounddive") || has("powerstart_powerattack") ?
                Kind::CreatureBite : Kind::CreatureClaw;
        }
        if (family == Family::BitingCreature || family == Family::IceWraith || has("bite")) return Kind::CreatureBite;
        if (family == Family::Spider || family == Family::Mudcrab || family == Family::Clawed) return Kind::CreatureClaw;
        return family == Family::Creature ? Kind::CreatureClaw : Kind::Unarmed;
    }

    inline Vector LocalStrike(Kind kind, std::string_view event, std::string_view clip)
    {
        const auto attack = clip.empty() ? event : clip;
        if (kind == Kind::GiantStomp || kind == Kind::CreatureStomp) return {0,0,1};
        if (kind == Kind::GiantClub && Contains(attack, "forwardpower")) return {0,.35f,-1};
        if (kind != Kind::CenturionAxe && kind != Kind::CenturionHammer) return {};
        const float across = kind == Kind::CenturionAxe ? 1.0f : -1.0f;
        if (Contains(attack, "chop")) return {.18f*across,.35f,-1};
        if (Contains(attack, "slash")) return {across,.45f,-.12f};
        // Stabs and unknown replacement motions keep the source-direction cue.
        return {};
    }

    inline const char* KindName(Kind kind)
    {
        constexpr const char* names[]{"unarmed", "blade", "blunt", "arrow", "fire", "frost", "shock", "poison",
            "magic", "environment", "heavy-blade", "heavy-blunt", "giant-club", "giant-swipe", "giant-stomp",
            "centurion-axe", "centurion-hammer", "centurion", "creature-bite", "creature-claw", "heavy-creature",
            "dragon-bite", "dragon-wing", "dragon-tail", "frost-atronach-spike", "frost-atronach-blunt",
            "heavy-claw", "heavy-bite", "small-bite", "tusk", "heavy-tusk", "hoof", "ram", "tentacle",
            "construct-blunt", "mechanical-pierce", "mechanical-blade", "heavy-projectile", "spirit-touch",
            "spear", "creature-stomp", "venom-spit", "web", "drain", "force"};
        static_assert(std::size(names) == static_cast<std::size_t>(Kind::Count));
        const auto index = static_cast<std::size_t>(kind);
        return index < std::size(names) ? names[index] : "unknown";
    }

    // The existing clip hooks run on animation workers. Only copied node names,
    // IDs and opaque identities cross to hit capture; never dereference a cached
    // actor, graph or clip. Main-thread discovery bounds this to nearby enemies
    // with asymmetric arms; all other families classify directly from hit data.
    class AttackClips
    {
    public:
        struct Scope { std::uintptr_t character = 0; std::uint32_t actor = 0; };
        struct Snapshot {
            std::array<char, 96> name{};
            std::string_view Name() const { return name.data(); }
        };
        bool Enabled() const { return enabled.load(std::memory_order_relaxed); }
        void Clear()
        {
            std::scoped_lock lock(mutex);
            entries = {}; enabled.store(false, std::memory_order_relaxed);
        }
        void SetScopes(const std::array<Scope, 16>& scopes, std::size_t count)
        {
            std::scoped_lock lock(mutex);
            std::array<Entry, 16> next{};
            count = (std::min)(count, next.size());
            for (std::size_t i = 0; i < count; ++i) {
                for (const auto& old : entries) if (old.scope.character == scopes[i].character && old.scope.actor == scopes[i].actor) {
                    next[i] = old; break;
                }
                next[i].scope = scopes[i];
            }
            entries = next;
            enabled.store(count != 0, std::memory_order_relaxed);
        }
        void Observe(std::uintptr_t character, std::uintptr_t clip, std::string_view name,
            bool active, bool refresh, double now)
        {
            if (!character || !clip || !std::isfinite(now) || !Enabled()) return;
            std::scoped_lock lock(mutex);
            for (auto& entry : entries) if (entry.scope.character == character) {
                Clip* found = nullptr;
                for (auto& c : entry.clips) if (c.identity == clip) { found = &c; break; }
                if (!active) { if (found) *found = {}; return; }
                if (!Contains(name, "attack")) return;
                if (!found) for (auto& c : entry.clips) if (!c.identity) { found = &c; break; }
                if (!found) return;
                if (!found->identity || !refresh) {
                    *found = {};
                    found->identity = clip;
                    found->order = ++sequence;
                }
                found->time = now;
                found->snapshot.name = {};
                std::copy_n(name.begin(), (std::min)(name.size(), found->snapshot.name.size()-1), found->snapshot.name.begin());
                return;
            }
        }
        Snapshot At(std::uint32_t actor, double now) const
        {
            std::scoped_lock lock(mutex);
            const Clip* newest = nullptr;
            for (const auto& entry : entries) if (actor && entry.scope.actor == actor)
                for (const auto& clip : entry.clips) if (clip.identity && std::isfinite(now) && now >= clip.time && now-clip.time <= .15 &&
                    (!newest || clip.order > newest->order)) newest = &clip;
            return newest ? newest->snapshot : Snapshot{};
        }
    private:
        struct Clip { Snapshot snapshot{}; std::uintptr_t identity = 0; std::uint64_t order = 0; double time = 0; };
        struct Entry { Scope scope{}; std::array<Clip, 4> clips{}; };
        mutable std::mutex mutex;
        std::atomic<bool> enabled{false};
        std::array<Entry, 16> entries{};
        std::uint64_t sequence = 0;
    };
}
