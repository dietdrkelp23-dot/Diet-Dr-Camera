#pragma once
#include "Camera/AnimationMatching.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace OAR_API::Animations
{
    class IAnimationsInterface1;
    using IAnimationsInterface = IAnimationsInterface1;
}

namespace DietDrCamera
{
    // ===== [CLIPCAM] per-animation camera overrides ========================
    //
    // The user binds a CameraProfile to an INDIVIDUAL animation file — the
    // headline case being OAR replacement animations, where a moveset author
    // ships dozens of clips and each deserves its own framing. Detection is
    // engine-level (compatibility-first): a vtable hook on
    // hkbClipGenerator::Activate/Update/Deactivate sees every clip the behavior
    // graph starts, scoped to the PLAYER by pointer-comparing the context's
    // hkbCharacter against the player's own graph instances (cached per
    // frame with the same manager-pointer-change invalidation the graph
    // sinks use, so it survives werewolf/VL transforms and Nemesis/Pandora
    // reloads). When Open Animation Replacer is installed, its Animations
    // API names the replacement file the clip actually resolved to; without
    // OAR the clip's own animationName path serves as the identity.
    //
    // THREADING: Activate/Deactivate fire on behavior-graph worker threads,
    // for every actor. The player check is two relaxed atomic pointer
    // compares before anything else; only player clips ever take the mutex.
    // The controller never reads SettingsManager from the hook — it matches
    // against its own snapshot (_matchIndex), rebuilt by the menu/load code
    // via RebuildMatchIndex() whenever the entry list changes.
    class AnimationCameraController
    {
    public:
        static AnimationCameraController& GetSingleton();

        // Installs the hkbClipGenerator vtable hooks and requests the OAR
        // Animations API. Called from HookManager::Install() (kPostLoad, so
        // OpenAnimationReplacer.dll is already loaded if present).
        void InstallHook();

        // Per-frame (StateResolver::Update — runs in 1p AND 3p): re-cache
        // the player's hkbCharacter pointers when the graph manager changes.
        void EnsurePlayerScope(RE::PlayerCharacter* a_player);

        // Rebuild the path→uid match snapshot from SettingsManager's entry
        // list. Call after ANY mutation of the entries (add/remove/toggle/
        // preset load). Also drops active matches whose entry vanished.
        void RebuildMatchIndex();

        // The uid of the entry whose clip is currently active (most recent
        // activation wins), or 0. The resolver re-derives entry pointers
        // from this uid every frame — never cache a profile pointer across
        // frames, the entries vector reallocates on menu edits.
        [[nodiscard]] std::uint32_t ActiveEntryUid() const;

        // Was OAR's Animations API acquired at install time?
        [[nodiscard]] bool OarAvailable() const { return _oar != nullptr; }

        // ----- capture ("Listen") support for the menu ---------------------
        struct CapturedClip
        {
            std::string key;       // the match key (lowercased path)
            std::string display;   // file stem, for the row label
            std::string subMod;    // OAR sub-mod name ("" without OAR)
            std::string mod;       // OAR mod name ("" without OAR)
            std::string clipName;  // behavior-graph node name, for the tooltip
        };
        void SetCaptureArmed(bool a_armed);

        [[nodiscard]] bool CaptureArmed() const { return _captureArmed.load(std::memory_order_relaxed); }
        // Snapshot copy, newest first. Menu-thread only.
        [[nodiscard]] std::vector<CapturedClip> GetCapturedClips() const;
        void ClearCapture();

        // Lowercase-normalize a match key (paths are case-insensitive on
        // disk; capture and storage must agree).
        static std::string NormalizeKey(const char* a_path);

    private:
        AnimationCameraController() = default;

        static void HookedClipActivate(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context);
        static void HookedClipUpdate(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context, float a_delta);
        static void HookedClipDeactivate(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context);
        void OnPlayerClip(RE::hkbClipGenerator* a_clip, bool a_activate, bool a_refresh = false);

        using ClipFn = void (*)(RE::hkbClipGenerator*, const RE::hkbContext&);
        static inline ClipFn _origActivate = nullptr;
        static inline ClipFn _origDeactivate = nullptr;
        using UpdateFn = void (*)(RE::hkbClipGenerator*, const RE::hkbContext&, float);
        static inline UpdateFn _origUpdate = nullptr;

        // Player scope: the embedded hkbCharacter of each of the player's
        // graphs (3p + 1p). Written on the main thread in EnsurePlayerScope,
        // read on graph worker threads — plain pointer payloads, relaxed is
        // fine, a one-frame-stale scope only delays a capture row.
        std::atomic<const void*> _playerChar3p{ nullptr };
        std::atomic<const void*> _playerChar1p{ nullptr };
        void* _lastGraphMgr = nullptr;

        mutable std::mutex _mutex;  // guards everything below
        std::unordered_map<std::string, std::uint32_t> _matchIndex;  // key → entry uid
        std::unordered_map<const void*, std::string> _observedPaths;
        using ActiveClip = ActiveAnimationMatch;
        std::vector<ActiveClip> _active;  // most recent activation at the back
        std::atomic<std::uint32_t> _activeUid{ 0 };  // published for the resolver

        std::atomic<bool> _captureArmed{ false };
        std::vector<CapturedClip> _capture;  // newest first, deduped by key
        static constexpr std::size_t kCaptureCap = 24;

        OAR_API::Animations::IAnimationsInterface* _oar = nullptr;
        bool _hookInstalled = false;
    };
}
