#include "PCH.h"
#include "Camera/CameraController.h"
#define NOMINMAX
#include <Windows.h>
#include "vendor/OpenAnimationReplacerAPI-Animations.h"
#include "Camera/AnimationCameraController.h"
#include "Camera/DamageReactionController.h"
#include "Camera/AnimationCatalog.h"
#include "Settings/SettingsManager.h"

#include <algorithm>
#include <cctype>

namespace DietDrCamera
{
    AnimationCameraController& AnimationCameraController::GetSingleton()
    {
        static AnimationCameraController instance;
        return instance;
    }

    namespace
    {
        // Insert word breaks a human would: underscores/hyphens, camelCase
        // boundaries ("WalkForward"), and letter<->digit seams
        // ("TurnRight60Slow" -> "Turn Right 60 Slow").
        std::string SplitAnimWords(std::string_view a_src)
        {
            std::string out;
            char prev = 0;
            for (std::size_t i = 0; i < a_src.size(); ++i) {
                const char ch = a_src[i];
                if (ch == '_' || ch == '-' || ch == ' ') {
                    if (!out.empty() && out.back() != ' ') out += ' ';
                    prev = ch;
                    continue;
                }
                const unsigned char uc = static_cast<unsigned char>(ch);
                const unsigned char up = static_cast<unsigned char>(prev);
                const char nxt = (i + 1 < a_src.size()) ? a_src[i + 1] : 0;
                bool boundary = false;
                if (!out.empty() && out.back() != ' ' && prev) {
                    if (std::isupper(uc) &&
                        (std::islower(up) || std::isdigit(up)))
                        boundary = true;   // walk|Forward
                    else if (std::isupper(uc) && std::isupper(up) && nxt &&
                             std::islower(static_cast<unsigned char>(nxt)))
                        boundary = true;   // MTIdle -> MT|Idle
                    else if (std::isdigit(uc) && std::isalpha(up))
                        boundary = true;   // Right|60
                    else if (std::isalpha(uc) && std::isdigit(up))
                        boundary = true;   // 60|Slow
                }
                if (boundary) out += ' ';
                out += ch;
                prev = ch;
            }
            return out;
        }

        // Bethesda's animation-file prefixes, translated. These are the
        // engine's own vocabulary ("mt" IS movement in every vanilla
        // path), so the translation names the file more honestly for a
        // human than the raw token does; the tooltip keeps the exact path.
        const char* AnimPrefixGloss(std::string a_first)
        {
            for (auto& c : a_first)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (a_first == "mt")        return "Movement";
            if (a_first == "sneakmt")   return "Sneak Movement";
            if (a_first == "1hm")       return "One-Handed";
            if (a_first == "sneak1hm")  return "Sneak One-Handed";
            if (a_first == "2hm")       return "Two-Handed";
            if (a_first == "2hw")       return "Two-Handed";
            if (a_first == "h2h")       return "Unarmed";
            if (a_first == "bow")       return "Bow";
            if (a_first == "crossbow")  return "Crossbow";
            if (a_first == "mlh")       return "Magic (Left Hand)";
            if (a_first == "mrh")       return "Magic (Right Hand)";
            if (a_first == "staff")     return "Staff";
            if (a_first == "shout")     return "Shout";
            if (a_first == "swim")      return "Swimming";
            if (a_first == "horse")     return "Horseback";
            return nullptr;
        }
    }

    std::string AnimationCameraController::NormalizeKey(const char* a_path)
    {
        return AnimationPathKey(a_path ? a_path : "");
    }

    void AnimationCameraController::InstallHook()
    {
        if (_hookInstalled) return;

        // OAR first — kPostLoad, so the DLL is loaded if the user has it.
        _oar = OAR_API::Animations::GetAPI();
        spdlog::debug("[CLIPCAM] Open Animation Replacer Animations API: {}",
                     _oar ? "acquired" : "not present (falling back to raw clip paths)");

        // hkbClipGenerator::Activate is vtable slot 0x4, Deactivate 0x7
        // (CommonLib RE/H/hkbClipGenerator.h declares both with their slots).
        // OAR hooks the same slots; both sides chain through the displaced
        // pointer, so install order does not matter.
        REL::Relocation<std::uintptr_t> clipVtbl{ RE::VTABLE_hkbClipGenerator[0] };
        _origActivate = reinterpret_cast<ClipFn>(clipVtbl.write_vfunc(0x4, &HookedClipActivate));
        _origUpdate = reinterpret_cast<UpdateFn>(clipVtbl.write_vfunc(0x5, &HookedClipUpdate));
        _origDeactivate = reinterpret_cast<ClipFn>(clipVtbl.write_vfunc(0x7, &HookedClipDeactivate));
        _hookInstalled = true;
        spdlog::debug("[CLIPCAM] hkbClipGenerator::Activate/Update/Deactivate hooked (animation cameras)");

        RebuildMatchIndex();
    }

    void AnimationCameraController::EnsurePlayerScope(RE::PlayerCharacter* a_player)
    {
        if (!a_player) return;
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
        if (!a_player->GetAnimationGraphManager(mgr) || !mgr) return;
        void* curMgr = static_cast<void*>(mgr.get());
        if (curMgr == _lastGraphMgr) return;

        // Manager changed (first frame, transformation, or a behavior-
        // framework reload): re-cache the embedded hkbCharacter of each
        // player graph and drop the active stack — the old clips died with
        // the old graphs and their Deactivate will never come.
        const void* char3p = nullptr;
        const void* char1p = nullptr;
        int slot = 0;
        for (auto& g : mgr->graphs) {
            if (auto* graph = g.get()) {
                if (slot == 0)      char3p = &graph->characterInstance;
                else if (slot == 1) char1p = &graph->characterInstance;
                ++slot;
            }
        }
        _playerChar3p.store(char3p, std::memory_order_relaxed);
        _playerChar1p.store(char1p, std::memory_order_relaxed);
        {
            std::scoped_lock lk(_mutex);
            _active.clear();
            _observedPaths.clear();
        }
        _activeUid.store(0, std::memory_order_relaxed);
        _lastGraphMgr = curMgr;
        spdlog::debug("[CLIPCAM] player graph scope (re)cached ({} graph(s), mgr=0x{:x})",
                     slot, reinterpret_cast<std::uintptr_t>(curMgr));
    }

    void AnimationCameraController::RebuildMatchIndex()
    {
        CameraController::InvalidateProfileReferences();
        auto& settings = SettingsManager::GetSingleton();
        std::scoped_lock lk(_mutex);
        _matchIndex.clear();
        _observedPaths.clear();
        for (const auto& e : settings.animationCameras) {
            if (!e.animationPath.empty())
                _matchIndex.emplace(NormalizeKey(e.animationPath.c_str()), e.uid);
        }
        // Drop active matches whose entry no longer exists / is disabled —
        // a deleted entry must release its override immediately, not at the
        // next Deactivate.
        std::erase_if(_active, [&](const ActiveClip& a) {
            for (const auto& [key, uid] : _matchIndex)
                if (uid == a.uid) return false;
            return true;
        });
        _activeUid.store(_active.empty() ? 0 : _active.back().uid, std::memory_order_relaxed);
    }

    std::uint32_t AnimationCameraController::ActiveEntryUid() const
    {
        return _activeUid.load(std::memory_order_relaxed);
    }

    void AnimationCameraController::SetCaptureArmed(bool a_armed)
    {
        if (a_armed) {
            std::scoped_lock lk(_mutex);
            _observedPaths.clear();
        }
        const bool was = _captureArmed.exchange(a_armed, std::memory_order_relaxed);
        if (was != a_armed)
            spdlog::debug("[CLIPCAM] capture {}", a_armed ? "armed — listening for player clips" : "disarmed");
    }

    std::vector<AnimationCameraController::CapturedClip> AnimationCameraController::GetCapturedClips() const
    {
        std::scoped_lock lk(_mutex);
        return _capture;
    }

    void AnimationCameraController::ClearCapture()
    {
        std::scoped_lock lk(_mutex);
        _capture.clear();
    }

    void AnimationCameraController::HookedClipActivate(RE::hkbClipGenerator* a_this,
                                                       const RE::hkbContext& a_context)
    {
        _origActivate(a_this, a_context);
        DamageReactionController::ObserveClip(a_this, a_context.character, true);
        auto& c = GetSingleton();
        const void* ch = a_context.character;
        // 3p graph ONLY (user ruling 2026-08-30: "first person animations
        // shouldn't be seen at all"). The 1p graph is the arms/camera-bob
        // rig, and it plays TWINS of shared paths — the 22:09 log shows it
        // running mt_idle/mt_walkforward while the 3p body did the visible
        // work — so admitting it would both litter Listen and flap a bound
        // shared key's override from a clip nobody can see. The 3p graph
        // keeps animating while the player views 1p, so 1p gameplay still
        // captures and matches through it.
        if (ch && ch == c._playerChar3p.load(std::memory_order_relaxed))
            c.OnPlayerClip(a_this, true);
    }

    void AnimationCameraController::HookedClipDeactivate(RE::hkbClipGenerator* a_this,
                                                         const RE::hkbContext& a_context)
    {
        DamageReactionController::ObserveClip(a_this, a_context.character, false);
        auto& c = GetSingleton();
        const void* ch = a_context.character;
        if (ch && ch == c._playerChar3p.load(std::memory_order_relaxed))
            c.OnPlayerClip(a_this, false);
        _origDeactivate(a_this, a_context);
    }

    void AnimationCameraController::HookedClipUpdate(RE::hkbClipGenerator* a_this,
        const RE::hkbContext& a_context, float a_delta)
    {
        _origUpdate(a_this, a_context, a_delta);
        DamageReactionController::ObserveClip(a_this, a_context.character, true, true);
        auto& c = GetSingleton();
        if (a_context.character && a_context.character == c._playerChar3p.load(std::memory_order_relaxed))
            c.OnPlayerClip(a_this, true, true);
    }

    void AnimationCameraController::OnPlayerClip(RE::hkbClipGenerator* a_clip, bool a_activate, bool a_refresh)
    {
        if (!a_activate) {
            // Always process removals, even with capture off and no index —
            // an entry bound mid-play must not leave a stale stack element.
            std::scoped_lock lk(_mutex);
            const auto before = _active.size();
            _observedPaths.erase(a_clip);
            std::erase_if(_active, [&](const ActiveClip& a) { return a.clip == a_clip; });
            if (_active.size() != before) {
                _activeUid.store(_active.empty() ? 0 : _active.back().uid,
                                 std::memory_order_relaxed);
                static int sRel = 0;
                if (sRel < 40) {
                    ++sRel;
                    spdlog::debug("[CLIPCAM] clip deactivated — override {}",
                                 _active.empty() ? "released" : "falls back to previous match");
                }
            }
            return;
        }

        const bool armed = _captureArmed.load(std::memory_order_relaxed);
        {
            std::scoped_lock lk(_mutex);
            if (!armed && _matchIndex.empty()) return;
        }

        // Identity: OAR's resolved replacement path when it has one, else the
        // clip's own animationName. Both are queried post-original, so OAR's
        // pick for this very activation is current regardless of hook order.
        // OAR returns owning BSStrings. Keep our own copy before `info` is
        // destroyed; retaining its c_str() left capture/matching reading freed
        // memory instead of the replacement's HKX path.
        std::string rawPath = a_clip->animationName.c_str()
                                  ? a_clip->animationName.c_str() : "";
        std::string subMod, mod, variant;
        if (_oar) {
            const auto info = _oar->GetCurrentReplacementAnimationInfo(a_clip);
            if (info.animationPath.length() > 0)
                rawPath = info.animationPath.c_str();
            if (info.subModName.length() > 0) subMod = info.subModName.c_str();
            if (info.modName.length() > 0)    mod = info.modName.c_str();
            if (info.variantFilename.length() > 0) variant = info.variantFilename.c_str();
        }
        if (rawPath.empty()) return;
        const auto familyKey = NormalizeKey(rawPath.c_str());
        std::string key = AnimationVariantKey(rawPath, variant);

        std::scoped_lock lk(_mutex);
        // OAR can change replacements/variants on a loop without a new
        // Activate. Recheck after Update, but never reorder unchanged clips:
        // otherwise locomotion would steal priority from the latest attack.
        auto previous = _observedPaths.find(a_clip);
        if (a_refresh && previous != _observedPaths.end() && previous->second == key) return;
        _observedPaths[a_clip] = key;
        // New bindings identify the exact file, including variant and actor
        // directory. Old presets can still bind an entire variants family or
        // a path beginning at the OAR directory. Prefer the precise binding.
        const auto uid = FindAnimationBinding(_matchIndex, key, familyKey);
        _activeUid.store(UpdateAnimationMatches(_active, a_clip, uid, a_refresh), std::memory_order_relaxed);
        if (uid) {
            static int sHit = 0;
            if (sHit < 40) {
                ++sHit;
                spdlog::debug("[CLIPCAM] clip '{}' matched entry uid {}", key, uid);
            }
        }

        if (armed) {
            // Name every accepted row while the cap lasts — if Listen still
            // lists clips nobody recognizes, THIS line says exactly which
            // files and OAR mods they are, and the next filter gets decided
            // on names instead of guesses. (Everything here is 3p-graph by
            // construction — see the hook-side scope note.)
            static int sCapLog = 0;
            if (sCapLog < 60) {
                ++sCapLog;
                spdlog::debug("[CLIPCAM] listen '{}' oar='{}{}{}' node='{}'",
                             key,
                             mod, (!mod.empty() && !subMod.empty()) ? " / " : "", subMod,
                             a_clip->name.c_str());
            }
            // Dedupe by key, newest first.
            std::erase_if(_capture, [&](const CapturedClip& cc) { return cc.key == key; });
            CapturedClip cc;
            cc.key = key;
            cc.subMod = std::move(subMod);
            cc.mod = std::move(mod);
            if (const char* nodeName = a_clip->name.c_str(); nodeName && *nodeName)
                cc.clipName = nodeName;
            // Display, from the most case-rich source available ("a lot of
            // these names are just impossible to make out", 2026-08-30).
            // NormalizeKey lowercases, so the KEY's stem cannot be split
            // into words — derive from the RAW path's stem instead, and
            // when even that is caseless fall back to the graph NODE name,
            // which Bethesda authored in CamelCase (the log that decided
            // this: key 'mt_turnright60.hkx' vs node 'MT_TurnRight60Slow').
            // Then split word seams and translate the engine's own file
            // prefix (mt/1hm/h2h/...). The exact path stays in the tooltip.
            std::string stem;
            {
                std::string_view rp{ rawPath };
                if (const auto sl = rp.find_last_of("\\/"); sl != std::string_view::npos)
                    rp.remove_prefix(sl + 1);
                if (rp.size() > 4 && (rp.ends_with(".hkx") || rp.ends_with(".HKX")))
                    rp.remove_suffix(4);
                stem.assign(rp);
            }
            const bool stemCaseless = std::none_of(stem.begin(), stem.end(),
                [](char c2) { return std::isupper(static_cast<unsigned char>(c2)); });
            if (stemCaseless && a_clip->name.c_str() && *a_clip->name.c_str())
                stem = a_clip->name.c_str();
            stem = SplitAnimWords(stem);
            // Title-case the fully-lowercase words; already-cased words
            // (from camel splits) keep their own capitals.
            {
                bool newWord = true;
                for (auto& ch2 : stem) {
                    if (ch2 == ' ') { newWord = true; continue; }
                    if (newWord) ch2 = static_cast<char>(std::toupper(static_cast<unsigned char>(ch2)));
                    newWord = false;
                }
            }
            if (const auto sp = stem.find(' '); sp != std::string::npos) {
                if (const char* gloss = AnimPrefixGloss(stem.substr(0, sp)))
                    stem = std::string(gloss) + ": " + stem.substr(sp + 1);
            }
            cc.display = std::move(stem);
            _capture.insert(_capture.begin(), std::move(cc));
            if (_capture.size() > kCaptureCap) _capture.resize(kCaptureCap);
        }
    }
}
