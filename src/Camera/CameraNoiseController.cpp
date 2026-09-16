#include "PCH.h"
#include "Camera/HitShakeController.h"
#include "Hooks/RuntimeHooks.h"
#include "Camera/CameraNoiseController.h"
#include "Camera/CameraEffectClock.h"
#include "Camera/ProfileSnapshot.h"
#include "Camera/NpcNoise.h"
#include "Camera/NpcArchery.h"
#include "Core/EffectFrame.h"
#include "Camera/CameraController.h"
#include "Camera/AnimationCameraController.h"
#include "Camera/EventBeatDefs.h"
#include "Camera/EventBeatSources.h"
#include "Camera/StateResolver.h"
#include "Hooks/HookManager.h"    // EnforceShieldSprintCamera, from the camera-update thunk
#include "Settings/SettingsManager.h"
#include "Settings/EquippedItemBinding.h"
#include "Hooks/TweenCameraTrace.h"
#include "Shouts/ShoutRegistry.h"
#include "UI/CrosshairManager.h"  // GetLiveBowDrawAmount — bow draw ramp for 1p shake
#include "UI/MenuUI.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <cstdint>

#include <RE/B/BSTimer.h>
#include <RE/E/EffectSetting.h>
#include <RE/M/MagicCaster.h>
#include <RE/P/ProcessLists.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/ScriptEventSourceHolder.h>
#include <RE/T/TESPlayerBowShotEvent.h>
#include <RE/S/SpellItem.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESMagicEffectApplyEvent.h>
#include <RE/T/TESSpellCastEvent.h>
#include <unordered_map>
#include <unordered_set>

namespace DietDrCamera
{
    namespace
    {
        // Animation-graph event detection for player transformations.
        // The race-change StateResolver edge (TESSwitchRaceCompleteEvent)
        // fires at the END of the morph animation; we want a signal at
        // the START so the build-up envelope plays during the visible
        // morph. The sink subscribes to the player's graph and watches
        // for any tag containing "beast" / "vampire" / "transform" while
        // also logging every unique tag once for diagnostic reuse.
        static std::chrono::steady_clock::time_point sWWGraphStartTime{};
        static std::chrono::steady_clock::time_point sVLGraphStartTime{};
        static bool sWWGraphSignalLatch = false;
        static bool sVLGraphSignalLatch = false;
        // Lifted out of the if-blocks so IsSuppressed can peek at them
        // and bypass kAnimated suppression while a transform envelope is
        // mid-flight (the morph animation rides on kAnimated).
        static bool sWWEnvelopeActive = false;
        static bool sVLEnvelopeActive = false;

        // Sheathing / Unsheathing shake (Cinematic Effects). One shared beat
        // for both directions. Armed by the weapon-state edge poll below,
        // shaped by PollWeaponDrawEnvelope, consumed by BOTH POV branches.
        // Kept as a latch rather than a per-frame test because the engine can
        // pass through the transient draw/sheathe states in a single frame on
        // fast animations.
        static bool sWeaponDrawLatch          = false;
        static bool sWeaponDrawEnvelopeActive = false;
        static std::chrono::steady_clock::time_point sWeaponDrawStart{};

        // Weapon draw / put-away edge. actorState2.weaponState walks
        // kSheathed -> kWantToDraw -> kDrawing -> kDrawn -> kWantToSheathe ->
        // kSheathing -> kSheathed, but not every animation set visits every
        // step, so key off "is the weapon out" (kDrawing OR kDrawn) instead of
        // one exact value: the transition into that set is the draw, out of it
        // is the put-away, and both land on the frame the animation starts.
        void PollWeaponDrawEdges(bool a_in1p, bool a_anyActive)
        {
            (void)a_in1p;   // the beat applies in both views now
            auto* player  = RE::PlayerCharacter::GetSingleton();
            auto* asState = player ? player->AsActorState() : nullptr;
            if (!asState) return;
            const auto ws = asState->actorState2.weaponState;
            const bool outNow = (ws == RE::WEAPON_STATE::kDrawing ||
                                 ws == RE::WEAPON_STATE::kDrawn);

            static bool sLastOut = false;
            static bool sInit    = false;
            if (!sInit) { sLastOut = outNow; sInit = true; return; }
            if (outNow != sLastOut) {
                sWeaponDrawLatch = true;
                sLastOut = outNow;
                // Diagnostic for "a 2nd repulse after the bow shot — maybe
                // the sheathe noise thinks I'm sheathing": if the engine's
                // weaponState flickers out of the drawn set after an arrow
                // release, this fires and names the culprit.
                spdlog::debug("[DrawBeat] armed ({} -> weaponState={})",
                             outNow ? "draw" : "put-away", static_cast<int>(ws));
            }
            // Drop the latch while the intensity is 0 — otherwise a draw
            // performed with the effect off would be sitting queued and fire
            // the moment it's enabled.
            if (!a_anyActive) sWeaponDrawLatch = false;
        }

        // Envelope for the sheathe / unsheathe beat, polled once per frame
        // ahead of the POV split so both views can consume it. Returns 0..1.
        //
        // Shape: smoothstep attack → HOLD at full for the user's Duration →
        // smoothstep decay over Fade Duration. The hold is the fix for "it
        // doesn't last long enough": the original had no hold at all, so the
        // entire beat was a 0.05s ramp into a 0.12s floor decay.
        //
        // No instant edges anywhere. Rotation angle scales linearly with amp,
        // so a 0 → peak step on the trigger frame is a visible pop no matter
        // how small the peak — the same failure mode as the 1p activation snap.
        //
        // One envelope for both directions, retriggered by either edge:
        // drawing out of a sheathe restarts the beat instead of stacking two.
        float PollWeaponDrawEnvelope(bool a_active, float a_hold)
        {
            const bool fired = sWeaponDrawLatch;
            sWeaponDrawLatch = false;
            if (!a_active) { sWeaponDrawEnvelopeActive = false; return 0.0f; }

            const auto now = CameraEffectClock::Now();
            if (fired) { sWeaponDrawEnvelopeActive = true; sWeaponDrawStart = now; }
            if (!sWeaponDrawEnvelopeActive) return 0.0f;

            constexpr float kAttack = 0.06f;
            // One slider shapes the whole beat (user 2026-09-05: "it should
            // just have a duration slider"): hold for Duration, then fade
            // over Duration again. The char's fadeDuration is no longer
            // read here; at the shipped 0.30 s this lands within 0.05 s of
            // the old 0.30 hold + 0.35 fade.
            //
            // Handed in rather than read from settings because the source has
            // a third- and a first-person Duration now (2026-09-06) and this
            // poll runs once, ahead of the POV split. The caller passes the
            // Duration of the view we are in; switching views mid-beat
            // re-shapes the tail it has left, which is the same thing every
            // other live slider edit does to a running envelope.
            const float hold  = std::clamp(a_hold, 0.0f, 3.0f);
            const float decay = std::max(0.15f, hold);
            const float t =
                std::chrono::duration<float>(now - sWeaponDrawStart).count();
            if (t >= kAttack + hold + decay) {
                sWeaponDrawEnvelopeActive = false;
                return 0.0f;
            }
            const auto smooth = [](float x) {
                x = std::clamp(x, 0.0f, 1.0f);
                return x * x * (3.0f - 2.0f * x);
            };
            if (t < kAttack)        return smooth(t / kAttack);
            if (t < kAttack + hold) return 1.0f;
            return 1.0f - smooth((t - kAttack - hold) / decay);
        }

        // Crossbows retain graph release timing. Bows use the engine's actual
        // shot event below: recovery/re-nocking can repeat these annotations
        // while the draw timer still contains the preceding shot's value.
        static std::uint32_t sArrowReleaseCounter = 0;
        // Debounce stamp — a vanilla graph annotates the same shot twice.
        static std::chrono::steady_clock::time_point sLastArrowReleaseTp{};

        struct PlayerBowShot
        {
            float power;
            std::chrono::steady_clock::time_point fired;
            bool zoomed;
            bool sneaking;
        };
        class PlayerBowShotSink final : public RE::BSTEventSink<RE::TESPlayerBowShotEvent>
        {
        public:
            static PlayerBowShotSink& Get() { static PlayerBowShotSink sink; return sink; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESPlayerBowShotEvent* event,
                RE::BSTEventSource<RE::TESPlayerBowShotEvent>*) override
            {
                auto* weapon = event ? RE::TESForm::LookupByID<RE::TESObjectWEAP>(event->weapon) : nullptr;
                if (weapon && weapon->GetWeaponType() == RE::WEAPON_TYPE::kBow) {
                    const float power = std::isfinite(event->shotPower) ? std::clamp(event->shotPower, 0.0f, 1.0f) : 0.0f;
                    const auto fired = CameraEffectClock::Now();
                    const auto& resolver = StateResolver::GetSingleton();
                    const auto* camera = RE::PlayerCamera::GetSingleton();
                    // Snapshot at release, before the next camera frame can
                    // drop zoom or change stance during the recovery animation.
                    const bool zoomed = (camera && camera->bowZoomedIn) || resolver.IsBowZoomed();
                    const bool sneaking = resolver.IsSneaking();
                    std::lock_guard lock(_mutex);
                    _pending = PlayerBowShot{power, fired, zoomed, sneaking};
                }
                return RE::BSEventNotifyControl::kContinue;
            }
            std::optional<PlayerBowShot> Take()
            {
                std::lock_guard lock(_mutex);
                return std::exchange(_pending, std::nullopt);
            }
        private:
            std::mutex _mutex;
            std::optional<PlayerBowShot> _pending;
        };
        void EnsureBowShotSubscription()
        {
            static bool subscribed = false;
            if (!subscribed) {
                if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                    holder->AddEventSink<RE::TESPlayerBowShotEvent>(&PlayerBowShotSink::Get());
                    subscribed = true;
                    spdlog::info("[CameraNoise] player bow-shot sink subscribed");
                }
            }
        }
        // Bumped on any hand-cast SPELL-FIRE graph event (MLh_SpellFire_Event
        // / MRh_SpellFire_Event on the human graph, and whatever analogue a
        // transformed graph emits — matched by substring so Vampire Lord /
        // modded animation sets participate without a name table). This is
        // the release-timing ANCHOR for the magic repulse: the event fires at
        // the visual release frame of whatever cast animation is playing, so
        // the kick self-aligns to any animation length. "voice" events are
        // excluded (shout fire, not a hand cast).
        static std::uint32_t sSpellFireCounter = 0;
        // Bumped on the player's "JumpUp" graph event (jump takeoff). Drives
        // the Jumping noise entry's takeoff burst — jumps only; walking off a
        // ledge never emits JumpUp, so falls get the airborne noise but no
        // launch spike.
        static std::uint32_t sJumpUpCounter = 0;
        // SkyParkour (SPPF) integration. Bumped on a SkyParkour_Start whose
        // payload is a "big" traversal (Vault / Climb* / Grab / Fail) so the
        // Cinematic Effects jump shake fires for parkour moves too. The small
        // step-ups (Step* payloads) are excluded per user request — and while
        // one is in progress they suppress the generic physics/fallback jump
        // triggers for a short window, so a mantle blip can never sneak a shake.
        static std::uint32_t sParkourJumpCounter = 0;
        static std::chrono::steady_clock::time_point sParkourStepSuppressUntil{};


        class TransformGraphSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
        {
        public:
            static TransformGraphSink& Get() { static TransformGraphSink s; return s; }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::BSAnimationGraphEvent* a_event,
                RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
            {
                if (!a_event || a_event->holder != RE::PlayerCharacter::GetSingleton())
                    return RE::BSEventNotifyControl::kContinue;
                const char* raw = a_event->tag.c_str();
                if (!raw || !raw[0]) return RE::BSEventNotifyControl::kContinue;

                std::string tag(raw);
                static std::unordered_set<std::string> sSeen;
                if (spdlog::should_log(spdlog::level::debug) && sSeen.size() < 300 && sSeen.insert(tag).second) {
                    spdlog::debug("[GraphEvent] '{}'", tag);
                }
                // Werewolf feed (2026-09-05): every tag while a feed is
                // latched goes to the resolver - it logs them and releases
                // the feed on the idle's end tag.
                if (StateResolver::GetSingleton().IsWerewolfFeeding()) {
                    StateResolver::GetSingleton().NotifyWerewolfFeedGraphTag(raw);
                }

                // [ANIMCAM] — the animated-camera window, every occurrence
                // (the log above is first-seen-only; this must not be).
                // 'StartAnimatedCameraDelta' / 'StartAnimatedCamera' open the
                // engine's animation-drives-the-camera mode; 'EndAnimatedCamera'
                // closes it. Exact, case-insensitive matches.
                if (_stricmp(raw, "StartAnimatedCameraDelta") == 0 ||
                    _stricmp(raw, "StartAnimatedCamera") == 0) {
                    HookManager::NotifyAnimatedCameraEvent(true);
                } else if (_stricmp(raw, "EndAnimatedCamera") == 0) {
                    HookManager::NotifyAnimatedCameraEvent(false);
                }

                std::string lower(tag);
                std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool isEnd = lower.find("end")    != std::string::npos ||
                                   lower.find("stop")   != std::string::npos ||
                                   lower.find("revert") != std::string::npos ||
                                   lower.find("done")   != std::string::npos;

                const bool ww = (lower.find("beastform")  != std::string::npos ||
                                 lower.find("werewolf")   != std::string::npos);
                // The vampire lord transformation's own tags carry no "vampire"
                // at all - the 2026-09-05 10:06:59 log reads, in order:
                // "VampireLord transformation detected (poll)" (race edge),
                // then 'cameraTransform' 0.5s later, then 'transformStop'.
                // So the name test below never latched the buildup for the
                // vampire lord; key it on 'cameraTransform' while the race is
                // the vampire lord instead (the werewolf's tags do say so).
                const bool vl = (lower.find("vampirelord") != std::string::npos ||
                                 (lower.find("vampire") != std::string::npos &&
                                  lower.find("transform") != std::string::npos) ||
                                 (lower == "cameratransform" &&
                                  StateResolver::GetSingleton().IsVampireLord()));

                if (ww && !isEnd) {
                    sWWGraphStartTime  = CameraEffectClock::Now();
                    sWWGraphSignalLatch = true;
                }
                if (vl && !isEnd) {
                    sVLGraphStartTime  = CameraEffectClock::Now();
                    sVLGraphSignalLatch = true;
                }
                // Accept either spelling, then DEBOUNCE. A vanilla graph
                // emits both per shot (release, then detach a few frames
                // later) and counting both would arm two recoil impulses
                // milliseconds apart, which the slot pool would sum into one
                // double-weight kick.
                //
                // This counter now drives crossbows only. Bow repulse requires
                // TESPlayerBowShotEvent and ignores all graph annotations.
                if (lower == "arrowrelease" || lower == "arrowdetach") {
                    const auto now = CameraEffectClock::Now();
                    constexpr auto kArrowDebounce = std::chrono::milliseconds(150);
                    if (now - sLastArrowReleaseTp >= kArrowDebounce) {
                        sLastArrowReleaseTp = now;
                        ++sArrowReleaseCounter;
                    }
                }
                if (lower.find("spellfire") != std::string::npos &&
                    lower.find("voice") == std::string::npos) {
                    ++sSpellFireCounter;
                }
                if (lower == "jumpup") {
                    ++sJumpUpCounter;
                }
                // SkyParkour v3 (SPPF) fires SkyParkour_Start on every parkour
                // move; the payload names the move — Vault, ClimbHighest/High/
                // Medium/Low, StepHighRight/Left, StepLowRight/Left, Grab, Fail.
                // Route the "big" moves into the jump-shake launch trigger and
                // hold the step-ups out (they read as a small stumble, not a
                // launch). Payload logged so the exact strings can be verified.
                if (lower == "skyparkour_start") {
                    const char* plRaw = a_event->payload.c_str();
                    std::string pl(plRaw ? plRaw : "");
                    std::string plLower(pl);
                    std::transform(plLower.begin(), plLower.end(), plLower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    // "step..." = the small step-up variants (High/Low, L/R).
                    const bool isStepUp = plLower.rfind("step", 0) == 0;
                    spdlog::debug("[GraphEvent] SkyParkour_Start payload='{}' -> {}",
                                 pl, isStepUp ? "STEP (no shake)" : "shake");
                    if (isStepUp) {
                        sParkourStepSuppressUntil = CameraEffectClock::Now() +
                                                    std::chrono::milliseconds(500);
                    } else {
                        ++sParkourJumpCounter;
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Track the last-seen graph manager pointer; re-subscribe when
        // it changes. Race-change events (player → werewolf, player →
        // vampire lord, player ← back to human) swap the player's
        // animation graph manager, and our previously attached sink
        // dangles on the deleted graph. Polling each tick and re-adding
        // the sink when the manager pointer changes keeps footstep
        // events firing across transformations.
        static void* sLastGraphMgr = nullptr;
        void EnsureTransformGraphSubscription()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
            if (!player->GetAnimationGraphManager(mgr) || !mgr) return;
            void* curMgr = static_cast<void*>(mgr.get());
            if (curMgr == sLastGraphMgr) return;
            for (auto& g : mgr->graphs) {
                if (auto* graph = g.get()) {
                    graph->AddEventSink<RE::BSAnimationGraphEvent>(&TransformGraphSink::Get());
                }
            }
            sLastGraphMgr = curMgr;
            spdlog::info("[CameraNoise] (re)subscribed graph sink (mgr=0x{:x})",
                         reinterpret_cast<std::uintptr_t>(curMgr));
        }

        // ===================================================================
        // Magic-effect beats: Vampire Lord Bats, Reanimation, Summoning.
        //
        // All three key off TESMagicEffectApplyEvent rather than animation
        // tags or a per-frame state probe. That matters for two different
        // reasons:
        //   - Reanimation and Summoning are matched on the effect's
        //     ARCHETYPE (kReanimate / kSummonCreature), which is a property
        //     of the record itself. Every necromancy and conjuration spell
        //     in the game — vanilla, DLC, or any mod's — carries one of
        //     those archetypes, so the sources work everywhere without a
        //     form list to maintain and without a single animation tag.
        //   - Bats is a one-frame power. A per-frame caster-slot probe can
        //     miss a cast that starts and finishes inside a dropped frame;
        //     the event cannot be missed.
        //
        // Each source latches (time, world position) and the per-frame
        // envelope below turns that into an amplitude. Position is latched
        // rather than tracked live because the interesting thing is where
        // the event HAPPENED — a summon that walks away shouldn't drag the
        // shake with it.
        // ===================================================================
        struct EventBeat
        {
            bool                                  active = false;
            std::chrono::steady_clock::time_point start{};
            RE::NiPoint3                          pos{};   // world position of the event
            bool                                  hasPos = false;
            NpcNoise::Source                      npcSource = NpcNoise::Source::None;
            // Floor on the distance falloff (0 = pure falloff). Player-owned
            // explosions set this so your own casts always register while
            // still scaling with how close they land.
            float                                 falloffFloor = 0.0f;
            // Set by the event sink, consumed by the envelope poll. The
            // re-arm can't just move `start` to now: a second event landing
            // while the first beat is still playing would drop the amplitude
            // to zero on that frame and rise again from nothing, which is a
            // hard step in the middle of an effect. The poll is the only
            // place that knows this source's hold and fade, so it is the only
            // place that can work out where the envelope currently IS and
            // restart from there.
            bool                                  rearmPending = false;
            // The envelope value this beat last RENDERED. The re-arm resumes
            // from this instead of recomputing where the old beat "should" be,
            // because the recomputation uses the NEW arm's timings — see the
            // note in EventBeatEnvelope.
            float                                 lastEnv = 0.0f;
            // Per-arm amplitude scale (default 1). Explosions use it to make
            // a firestorm blast outweigh a firebolt pop at the same sliders.
            float                                 armScale = 1.0f;
        };

        // Inverse of smoothstep, s(x) = 3x^2 - 2x^3, on [0,1]. Used to find
        // the point in a beat's attack that corresponds to an amplitude it has
        // already reached, so a re-arm can resume from there instead of
        // restarting at zero.
        float InvSmoothstep(float y)
        {
            y = std::clamp(y, 0.0f, 1.0f);
            return 0.5f - std::sin(std::asin(1.0f - 2.0f * y) / 3.0f);
        }
        static EventBeat sBatsBeat;
        static EventBeat sReanimateBeat;
        static EventBeat sSummonBeat;
        // The table-driven one-shots (EventBeatDefs.h) — armed from
        // EventBeatSources' drained queue, competing in the same
        // loudest-wins pool as the three bespoke sources above.
        static std::array<EventBeat, kEventBeatCount> sTableBeats;

        // Transformation build-up latches (archetype apply edge — engine
        // signal, set here by MagicBeatSink; the graph-tag latches above are
        // the secondary source). Consumed by the transform envelope.
        static std::chrono::steady_clock::time_point sWWArchetypeTp{};
        static std::chrono::steady_clock::time_point sVLArchetypeTp{};

        // ===================================================================
        // NPC noise (Cinematic Effects → NPC Noise). One master slider scales
        // every NPC-driven source; the entries themselves are the PLAYER's
        // tuned ones — an NPC shout resolves the shout entry for the player's
        // current state, an NPC concentration stream resolves the magic
        // school entry, an NPC transformation reuses the transform entry.
        // Ranges are internal (the slider is deliberately the only control).
        // SKSE's ActionEvent is player-only in practice, so NPC shouts come
        // from TESSpellCastEvent (a shout FIRES as its variation spell, for
        // every actor) with a HighProcessData::voiceState poll as fallback.
        // Dragons are excluded everywhere here — the dragon shake system
        // owns them, and double-driving breath/shouts would stack.
        // ===================================================================
        float ProximityFalloff(float a_d, float a_range);  // defined with the prox scanner below

        constexpr float kNpcNoiseRange     = 2000.0f;  // ~28 m base
        constexpr float kNpcConcRangeMul   = 0.7f;     // a lightning stream is close-quarters
        constexpr float kNpcShoutRangeMul  = 1.4f;     // Thu'um carries
        constexpr float kNpcShoutBeatScale = 0.20f;    // compensates the event layer's ×7:
                                                       // net peak ≈ 1.4 × entry amp × slider,
                                                       // between the player's word-2 and word-3 tiers

        struct NpcBeatParams
        {
            float                              intensity = 0.0f;
            float                              speed     = 1.0f;
            float                              range     = kNpcNoiseRange;
            float                              hold      = 1.0f;
            // Both halves are latched at ARM time and chosen at READ time
            // (2026-09-07). Latching only the view that happened to be active
            // when the NPC shouted would freeze the loudness of an event that
            // outlives a POV switch — and the read site is the one place that
            // knows which view is being rendered.
            float                              intensityFp = 0.0f;
            SettingsManager::CinematicShakeChar chr{};
        };
        static EventBeat     sNpcShoutBeat;
        static EventBeat     sNpcTransformBeat;
        static NpcBeatParams sNpcShoutParams;
        static NpcBeatParams sNpcTransformParams;

        struct NpcCasterMemory
        {
            EventBeat shoutBeat;
            NpcBeatParams shoutParams;
            RE::VOICE_STATE                       lastVoiceState = RE::VOICE_STATE::kNone;
            std::chrono::steady_clock::time_point lastShoutArmTp{};
            std::chrono::steady_clock::time_point lastSeen{};
        };
        static std::unordered_map<RE::FormID, NpcCasterMemory> sNpcMemory;

        struct NpcCombatMemory
        {
            NpcNoise::MeleeSwingTracker swings;
            bool archerySneak = false;
            bool archeryDrawing = false;
            RE::FormID archeryWeapon = 0;
            ItemBindings::EquippedItem archeryItem;
            EventBeat beat;
            NpcBeatParams params;
            EventBeat spellBeat;
            NpcBeatParams spellParams;
            std::chrono::steady_clock::time_point lastConcentrationArm{};
            NpcNoise::Form form = NpcNoise::Form::None;
            bool formKnown = false;
            bool voiceActive = false;
            bool seen = false;
        };
        static std::unordered_map<RE::FormID, NpcCombatMemory> sNpcCombatMemory;

        struct NpcArcheryShot
        {
            RE::ObjectRefHandle shooter;
            bool crossbow = false;
            RE::FormID weapon = 0;
            RE::FormID projectile = 0;
            RE::NiPoint3 pos{};
            std::chrono::steady_clock::time_point fired{};
        };
        NpcNoise::ArcheryShotQueue<NpcArcheryShot> sNpcArcheryShots;

        float NpcIntensity(NpcNoise::Source source, bool fp)
        {
            const auto& s = SettingsManager::GetSingleton();
            return NpcNoise::IntensityFor(source,
                fp ? s.npcNoiseIntensityFp : s.npcNoiseIntensity,
                fp ? s.npcMeleeNoiseIntensityFp : s.npcMeleeNoiseIntensity,
                fp ? s.npcArcheryNoiseIntensityFp : s.npcArcheryNoiseIntensity,
                fp ? s.npcTransformNoiseIntensityFp : s.npcTransformNoiseIntensity,
                fp ? s.npcShoutNoiseIntensityFp : s.npcShoutNoiseIntensity);
        }

        bool NpcSourceEnabled(NpcNoise::Source source)
        {
            return NpcNoise::EnabledForEitherView(NpcIntensity(source, false), NpcIntensity(source, true));
        }

        NpcNoise::Form NpcFormOf(RE::Actor* actor)
        {
            const auto* race = actor ? actor->GetRace() : nullptr;
            if (!race) return NpcNoise::Form::None;
            const char* id = race->GetFormEditorID();
            return NpcNoise::ClassifyForm(id ? id : "", race->HasKeywordString("ActorTypeWerewolf"));
        }

        NpcNoise::Source NpcMagicSource(RE::TESObjectREFR* caster)
        {
            if (caster && caster == RE::PlayerCharacter::GetSingleton()) return NpcNoise::Source::None;
            if (!caster) return NpcNoise::Source::Magic;
            return NpcFormOf(caster->As<RE::Actor>()) != NpcNoise::Form::None
                ? NpcNoise::Source::Transformations : NpcNoise::Source::Magic;
        }

        void ArmNpcProfile(EventBeat& beat, NpcBeatParams& params, const SettingsManager::NoiseProfile* profile,
                           NpcNoise::Source source, const RE::NiPoint3& pos, float range, float hold, float fade)
        {
            if (!profile || profile->amp <= 0.0001f || !NpcSourceEnabled(source)) { beat = {}; return; }
            params.intensity = profile->amp * kNpcShoutBeatScale;
            params.speed = std::max(0.1f, profile->speed);
            params.range = range;
            params.hold = hold;
            params.chr.rotShake = profile->tilt;
            params.chr.posShake = profile->sway;
            params.chr.driftJitter = profile->driftJitter;
            params.chr.roughness = profile->roughness;
            params.chr.fadeDuration = fade;
            beat.rearmPending = true;
            beat.hasPos = true;
            beat.pos = pos;
            beat.npcSource = source;
        }

        // The player's current shoutable-state bucket — an NPC shout inherits
        // whatever the user tuned for the state they are standing in
        // ("inherits all the player's settings", maximally).
        const char* NpcShoutStateBucket()
        {
            switch (StateResolver::GetSingleton().GetState()) {
            case CameraState::Melee:    return "melee";
            case CameraState::Bow:      return "bow";
            case CameraState::Crossbow: return "crossbow";
            case CameraState::Magic:    return "magic";
            case CameraState::Staves:   return "staves";
            default:                    return "sheathed";
            }
        }

        const char* NpcShoutStateBucket(RE::Actor* actor)
        {
            if (!actor || !actor->AsActorState()->IsWeaponDrawn()) return "sheathed";
            bool spell = false;
            for (bool left : {false, true}) {
                auto* form = actor->GetEquippedObject(left);
                if (auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr) {
                    if (weapon->IsCrossbow()) return "crossbow";
                    if (weapon->IsBow()) return "bow";
                    if (weapon->IsStaff()) return "staves";
                    return "melee";
                }
                spell = spell || (form && form->As<RE::SpellItem>());
            }
            return spell ? "magic" : "melee";
        }

        // The same location → env → plain chain the player's own shout/noise
        // resolution walks, as a key lookup usable at event time.
        // --- Attack Duration ------------------------------------------------
        // Per-entry cap on how long an attack cell's NOISE plays. The Attack
        // sub-state lasts as long as the animation bracket, and on a slow,
        // heavy or interrupted swing that outstays the moment the hit reads
        // for — the shake is still going long after the blow landed. With
        // Duration above 0 the cell instead runs for exactly that long from
        // the start of the swing and then hands back to the parent state
        // (sheathed, drawn, whatever you were in).
        //
        // NOISE ONLY, by request: camera framing keeps the full bracket, so
        // zoom and FOV still follow the animation. Only the texture expires.
        //
        // Duration changes exactly ONE thing: WHEN the hand-back to the parent
        // begins. It must not touch amplitude, character or fade rate. Up to
        // the hand-back a cell with a Duration is bit-identical to the same
        // cell at 0, and the hand-back itself is the ordinary state crossfade
        // that any state change already uses. "Fade out sooner", nothing else.
        // (An earlier pass shortened the crossfade in proportion to the window
        // so short Durations could still reach full amplitude — that changed
        // the feel of the swing itself, which is not what was asked for.)
        struct AttackNoiseWindow
        {
            std::chrono::steady_clock::time_point start{};
            bool armed = false;
        };
        AttackNoiseWindow sAtkWin;
        // The swing's captured cell + envelope. The attack cell is no longer a
        // STATE that blankets everything until you sheathe — it is a one-shot
        // fired by the action itself, so it needs a character snapshot and its
        // own envelope, exactly like the draw / transform / event beats.
        EventBeat                     sAtkBeat;
        SettingsManager::NoiseProfile sAtkCell{};
        bool                          sAtkCellValid    = false;
        bool                          sAtkArmedForSwing = false;
        // Set by the poll on each INDIVIDUAL swing and LATCHED until an arming
        // site consumes it. A latch, not a per-frame flag: the swing edge and
        // the frame the new attack cell resolves are not always the same frame
        // — the engine clears attackData for a frame or two between a power
        // attack and the normal attack that follows it, so the resolver reports
        // the new cell a beat late. With a per-frame flag that edge was simply
        // lost, and since the sub-state never drops inside a combo the
        // first-swing fallback could not fire either: the follow-up attack got
        // no beat at all. Cleared when the attack ends.
        bool                          sAtkSwingPending  = false;
        // Which attack cell the last beat was armed from. A change of key while
        // still attacking IS a new action (power -> normal in a combo), and is
        // a direct signal rather than an inferred one.
        std::string                   sAtkLastKey;
        // When the live beat was armed, and how long after that a key change is
        // read as the engine CORRECTING its own classification rather than as a
        // second swing.
        //
        // Every sprint power attack in the 18:57 log opens the same way:
        //   attack started (power=false)          <- attackData hasn't ruled yet
        //   power flag from graph tag -> true     <- 14ms later, the truth
        // The first line arms a beat on sprint_ATTACK; the second changes the
        // key, which re-armed the envelope AND started a texture crossfade to
        // sprint_POWER_attack. So the opening ~0.5s of every sprint power
        // attack was a muddle of two cells instead of one clean punch. Inside
        // the settle window the cell is simply adopted in place: same beat,
        // same envelope, no fade — the swing hasn't changed, only our name
        // for it has.
        constexpr float               kAtkSettleSec = 0.15f;
        std::chrono::steady_clock::time_point sAtkArmTp{};
        bool                          sAtkArmTpValid = false;
        bool                          sAtkKeyCorrected = false;  // consumed by the blend
        // Fade rate for the attack blend, sized when the cell changes.
        float                         sAtkBlendRate = 1.0f / 0.35f;
        // The live beat's envelope timing, fixed at ARM. See the note where
        // these are latched: BeatRampFor reads the ambient, the ambient moves,
        // and an envelope whose duration is redefined under it is not an
        // envelope.
        float                         sAtkRiseLatched = 0.06f;
        float                         sAtkFallLatched = 0.35f;
        bool                          sAtkRampValid   = false;
        // Ceiling for the OUTGOING layer, as an AMPLITUDE rather than an
        // envelope fraction. See the note where it is applied.
        float                         sAtkPrevAmpCap  = 0.0f;
        // Last frame's TOTAL applied 3p noise offset, and the fade that walks
        // it out when the camera enters a suppressed state. Entering kTween
        // (the menu cam) makes OnCameraUpdate early-return, so the offset that
        // was on the camera last frame simply is not written this frame — the
        // camera steps back to unshaken in ONE frame. That is the small snap
        // felt opening the Tween menu. The offset is FROZEN and decayed, never
        // re-sampled, so nothing evolves on the unstable menu cameraRoot and
        // it reaches exactly zero.
        RE::NiPoint3                  sTailTrans{};
        RE::NiPoint3                  sTailRotVec{};   // axis*theta, small-angle
        float                         sSuppressFade   = 0.0f;
        std::chrono::steady_clock::time_point sSuppressTick{};
        bool                          sSuppressHasTick = false;
        bool                          sSuppressWasSupp = false;
        // The cell each blend layer is built from. The OUTGOING one is kept as
        // a cell rather than as a finished layer so it can be rebuilt every
        // frame against the live envelope — otherwise its amplitude freezes at
        // whatever it was when the swap happened and the old texture hangs on
        // at full volume while the new one fades in underneath it.
        SettingsManager::NoiseProfile  sAtkCurCell{};
        SettingsManager::NoiseProfile  sAtkPrevCell{};
        bool                           sAtkPrevCellValid = false;


        // Seconds since the current swing began, or -1 when not attacking.
        //
        // MUST BE CALLED EVERY FRAME. It disarms on the falling edge, and the
        // only way it sees that edge is by being called on a non-attacking
        // frame. It used to be called lazily, from inside the "is this an
        // attack cell with a Duration" branch — which only runs WHILE
        // attacking. So it armed on the first swing and never disarmed: every
        // later swing measured from the first one ever, was instantly past any
        // Duration, and the attack noise was expired before it could be heard.
        // That is why even the maximum Duration read as almost no noise.
        float PollAttackNoiseElapsed()
        {
            // (sAtkSwingPending is a latch — it is NOT cleared here.)
            // PER-SWING edge, off the engine's melee attack state.
            //
            // The Attack sub-state is a BRACKET around the whole animation and
            // it does NOT close between chained swings — hold the button and
            // it stays true across the entire combo. So it yields exactly one
            // rising edge for the chain, and back-to-back power attacks fired
            // one beat between them instead of one each. The engine's attack
            // state re-enters kDraw/kSwing for every individual swing, which is
            // the signal an on-action effect actually needs, and it is an
            // engine field rather than an animation tag, so it reads the same
            // on any attack framework.
            static RE::ATTACK_STATE_ENUM sPrevAtkState = RE::ATTACK_STATE_ENUM::kNone;
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                if (auto* as = pl->AsActorState()) {
                    const auto cur = as->actorState1.meleeAttackState;
                    if (cur != sPrevAtkState &&
                        (cur == RE::ATTACK_STATE_ENUM::kDraw ||
                         cur == RE::ATTACK_STATE_ENUM::kSwing)) {
                        sAtkSwingPending = true;
                        static int sLogs = 0;
                        if (sLogs < 12) {
                            ++sLogs;
                            spdlog::debug("[ATKBEAT] swing edge: attackState {} -> {}",
                                         static_cast<int>(sPrevAtkState),
                                         static_cast<int>(cur));
                        }
                    }
                    sPrevAtkState = cur;
                }
            }
            const bool atk =
                StateResolver::GetSingleton().GetSubState() == CameraSubState::Attack;
            if (!atk) {
                sAtkWin.armed      = false;
                sAtkArmedForSwing  = false;   // next swing gets its own beat
                sAtkSwingPending   = false;   // don't carry a stale edge across
                // sAtkLastKey is deliberately NOT cleared. It names the cell the
                // BEAT is playing, and the beat outlives the attack state — its
                // decay runs on past the linger. Clearing it here changed the
                // crossfade's identity hash on the frame the attack ended, which
                // fired a texture refade onto a decorrelated clock right in the
                // middle of an audible decay: the snap felt after a sprint
                // attack. A new swing re-arms via !sAtkArmedForSwing above, so
                // nothing depends on this being cleared.
                return -1.0f;
            }
            const auto now = CameraEffectClock::Now();
            if (!sAtkWin.armed) { sAtkWin.armed = true; sAtkWin.start = now; }
            return std::chrono::duration<float>(now - sAtkWin.start).count();
        }

        // Rise AND fall times for a beat landing on an ambient, sized by how
        // big the change actually is. Same principle as the adaptive crossfade.
        //
        // Both arguments are ENERGY — amp x (tilt + sway) — not amp. Amp alone
        // is a trap: a real preset here has weapons.melee at amp 5 with
        // tilt/sway 0.5, and weapons.melee.power_attack at amp 5 with tilt/sway
        // 5. Identical amps, TEN TIMES the motion. Comparing amps called that a
        // small step and gave it a 60ms rise, which is exactly the snap being
        // reported. Energy sees it for what it is.
        void BeatRampFor(float a_beatEnergy, float a_baseEnergy,
                         float& a_outRise, float& a_outFall)
        {
            const float step = std::clamp(a_beatEnergy / (std::max)(a_baseEnergy, 0.5f),
                                          0.0f, 4.0f) * 0.25f;   // 0..1
            a_outRise = 0.06f + step * 0.44f;   // 0.06s .. 0.50s
            a_outFall = 0.30f + step * 0.50f;   // 0.30s .. 0.80s — the release
                                                // has as far to travel as the
                                                // attack did.
        }

        // The state the player was ACTUALLY in before the swing, remembered
        // rather than looked up. A key-string lookup of the parent misses the
        // per-weapon melee overrides and weapon-binding entries that the
        // pointer-keyed resolve applies, so it could hand back a different
        // profile than the one that had been playing — which is a signature
        // change, and therefore a transition, on every single attack.
        SettingsManager::NoiseProfile sLastNonAtkNoise{};
        bool                          sLastNonAtkValid = false;
        // ...and WHICH state it was. The remembered profile is only usable
        // while it still describes the state the swing's parent key names.
        // A sprint power attack is the case that proves it: the sprint ENDS
        // when the swing starts, so the remembered cell (weapons.melee.sprint,
        // energy 18) no longer describes anything, and holding it under the
        // beat for the whole attack meant an artificial plateau that dropped
        // to the real state (weapons.melee, energy 5) the moment the beat let
        // go. The parent key of sprint_power_attack is weapons.melee, which
        // does not match the remembered key — so fall back to a live lookup.
        std::string                   sLastNonAtkKey;

        bool IsAttackNoiseKey(std::string_view a_key)
        {
            const auto pos = a_key.rfind('.');
            if (pos == std::string_view::npos) return false;
            const auto tail = a_key.substr(pos + 1);
            // Every attack suffix in the key space, including the target_lock.
             // mirrors (same tails) and the werewolf / vampire-lord cells.
            return tail == "attack"        || tail == "power_attack"        ||
                   tail == "sneak_attack"  || tail == "sneak_power_attack"  ||
                   tail == "sprint_attack" || tail == "sprint_power_attack";
        }

        // The state an expired attack cell hands back to: the key with its
        // attack suffix removed.
        std::string AttackNoiseParentKey(std::string_view a_key)
        {
            const auto pos = a_key.rfind('.');
            return pos == std::string_view::npos ? std::string{}
                                                 : std::string(a_key.substr(0, pos));
        }

        const SettingsManager::NoiseProfile* ResolveNoiseByKey(const std::string& a_key)
        {
            auto& s = SettingsManager::GetSingleton();
            if (const auto* lp = s.ActiveLocationStateNoise(a_key); lp && lp->enabled)
                return lp;
            {
                auto& m = s.StateNoiseFor(s.RuntimeEnv());
                if (auto it = m.find(a_key); it != m.end() && it->second.enabled)
                    return &it->second;
            }
            {
                if (auto it = s.stateNoise.find(a_key); it != s.stateNoise.end() && it->second.enabled)
                    return &it->second;
            }
            return nullptr;
        }

        constexpr const char* kSchoolNames[5] = {
            "alteration", "conjuration", "destruction", "illusion", "restoration",
        };
        constexpr const char* kNpcSchoolConcKeys[5] = {
            "magic.alteration.concentration", "magic.conjuration.concentration",
            "magic.destruction.concentration", "magic.illusion.concentration",
            "magic.restoration.concentration",
        };
        int NpcSchoolIndex(RE::ActorValue a_av)
        {
            switch (a_av) {
            case RE::ActorValue::kAlteration:  return 0;
            case RE::ActorValue::kConjuration: return 1;
            case RE::ActorValue::kDestruction: return 2;
            case RE::ActorValue::kIllusion:    return 3;
            case RE::ActorValue::kRestoration: return 4;
            default:                           return -1;
            }
        }

        // Per-entry Projectile Repulse lookup: the same location → env →
        // plain chain the noise walks, returning the WINNING ENTRY so the
        // caller reads both `repulse` (strength) and `repulseFeel` (shape)
        // from one place. The terminal PLAIN entry is accepted without the
        // enabled gate — the base cells are this value's canonical home and
        // enabling them there would activate custom NOISE as a side effect.
        // a_enabledOnly = the hand/per-shout probe (returns nullptr when no
        // enabled entry exists so the caller can fall through to the base
        // key).
        const SettingsManager::NoiseProfile* RepulseEntryForKey(const std::string& a_key,
                                                                bool a_enabledOnly = false)
        {
            auto& s = SettingsManager::GetSingleton();
            if (const auto* lp = s.ActiveLocationStateNoise(a_key); lp && lp->enabled)
                return lp;
            {
                auto& m = s.StateNoiseFor(s.RuntimeEnv());
                if (auto it = m.find(a_key); it != m.end() && it->second.enabled)
                    return &it->second;
            }
            if (auto it = s.stateNoise.find(a_key); it != s.stateNoise.end()) {
                if (!a_enabledOnly || it->second.enabled) return &it->second;
            }
            return nullptr;
        }
        float RepulseForKey(const std::string& a_key, bool a_enabledOnly = false)
        {
            const auto* p = RepulseEntryForKey(a_key, a_enabledOnly);
            if (!p) return a_enabledOnly ? -1.0f : 0.0f;
            return p->repulse;
        }

        // Arm the NPC shout beat from the player's tuned entry. a_spell == 0
        // means the identity is unknown (voiceState fallback) — base entry
        // only. NoiseProfile → beat character maps field-for-field: the
        // profile IS the texture.
        void TryArmNpcShout(RE::Actor* actor, std::uint32_t spell)
        {
            if (!actor || !NpcSourceEnabled(NpcNoise::Source::Shouts)) return;
            auto* shout = actor->GetCurrentShout();
            if (shout && spell && std::none_of(std::begin(shout->variations), std::end(shout->variations),
                [spell](const auto& variation) { return variation.spell && variation.spell->GetFormID() == spell; }))
                shout = nullptr; // Never borrow a different shout's specific binding.
            auto& registry = ShoutRegistry::GetSingleton();
            if (!shout) shout = registry.FindFormBySpell(spell);
            const auto id = shout ? registry.Find(shout) : registry.FindBySpell(spell);
            const auto item = ItemBindings::DescribeForm(shout);
            std::string key;
            const auto* p = SettingsManager::GetSingleton().ResolveNpcShoutNoise(NpcShoutStateBucket(actor),
                id ? kShouts[static_cast<std::size_t>(*id)].tomlKey : "",
                actor->AsActorState()->IsSneaking(), item, &key);
            auto& memory = sNpcMemory[actor->GetFormID()];
            ArmNpcProfile(memory.shoutBeat, memory.shoutParams, p, NpcNoise::Source::Shouts,
                actor->GetPosition(), kNpcNoiseRange * kNpcShoutRangeMul, 1.0f,
                p ? std::max(0.3f, p->shoutFadeDuration) : 0.3f);
            static unsigned logged = 0;
            if (logged < 16) {
                ++logged;
                spdlog::debug("[NPCNOISE] shout actor={:08X} spell={:08X} entry={} amp={:.2f} armed={}",
                    actor->GetFormID(), spell, key.empty() ? "unset" : key, p ? p->amp : 0.0f, memory.shoutBeat.rearmPending);
            }
        }

        // NPC fire-and-forget release. Rides the shout beat slot: the two are
        // the same shape (a one-shot at the caster) and an NPC cannot shout and
        // sling a firebolt on the same frame, so one slot is enough and they
        // stay loudest-wins against each other exactly like every other beat.
        void TryArmNpcCast(int a_schoolIdx, const RE::NiPoint3& a_pos)
        {
            auto&       s      = SettingsManager::GetSingleton();
            const float npcInt = s.npcNoiseIntensity;
            if (!NpcNoise::EnabledForEitherView(npcInt, s.npcNoiseIntensityFp) ||
                a_schoolIdx < 0 || a_schoolIdx > 4) return;
            // The player's own cell for this school's fire-and-forget, so an
            // NPC's fireball reads like your fireball. Nothing tuned there =
            // nothing here, which is the required degrade.
            const std::string key = std::string("magic.") + kSchoolNames[a_schoolIdx] +
                                    ".fire_and_forget";
            const auto* p = ResolveNoiseByKey(key);
            if (!p || p->amp <= 0.0001f) return;
            sNpcShoutParams.intensity        = p->amp * npcInt * kNpcShoutBeatScale;
            sNpcShoutParams.intensityFp      = p->amp * kNpcShoutBeatScale *
                SettingsManager::GetSingleton().npcNoiseIntensityFp;
            sNpcShoutParams.speed            = std::max(0.1f, p->speed);
            sNpcShoutParams.range            = kNpcNoiseRange;
            // Short: a release is a crack, not a held note like a shout.
            sNpcShoutParams.hold             = 0.15f;
            sNpcShoutParams.chr.rotShake     = p->tilt;
            sNpcShoutParams.chr.posShake     = p->sway;
            sNpcShoutParams.chr.driftJitter  = p->driftJitter;
            sNpcShoutParams.chr.roughness    = p->roughness;
            sNpcShoutParams.chr.fadeDuration = 0.45f;
            sNpcShoutBeat.rearmPending = true;   // resume-not-restart
            sNpcShoutBeat.pos          = a_pos;
            sNpcShoutBeat.hasPos       = true;
            sNpcShoutBeat.npcSource    = NpcNoise::Source::Magic;
            static bool sLoggedCast = false;
            if (!sLoggedCast) {
                sLoggedCast = true;
                spdlog::debug("[NPCNOISE] cast beat armed key={} amp={:.2f}",
                             key, sNpcShoutParams.intensity);
            }
        }

        struct NpcCastResult
        {
            float amp = 0.0f, speed = 1.0f;
            float sway = 0.0f, tilt = 0.0f, dj = 0.35f, rgh = 0.45f;
            int   domKey = -1;
        };

        // Per-frame walk of the high-process actors: voiceState shout
        // fallback + concentration-cast accumulation. Deliberately a SECOND
        // walk beside the dragon scan — the two have disjoint gates and
        // filter chains, and coupling them saves only the handle iteration.
        // Does this race have its OWN Cinematic Effects source?
        //
        // Dragons and Dwarven Centurions each carry a per-source Intensity in
        // Cinematic Effects (per view since 2026-09-06), so the generic NPC
        // casting layer must not shake for them as well — its Intensity is a
        // single POV-independent slider and knows nothing about those sources,
        // so a centurion whose 1p Intensity is 0 still shook the first-person
        // view through it. The dragon half of this test was already at the NPC
        // scan; the centurion half was not, which is the bug (user, 2026-09-07:
        // "dwarven centurion noise seems to be active in 1st person even though
        // its 1st person intensity is 0" — the preset has no centurion _fp keys
        // at all, and npc_noise intensity = 1.0).
        //
        // Same detection the cinematic scanner uses: the vanilla race form plus
        // a name fallback for named/modded variants (Dawnguard's Forgemaster).
        bool RaceHasOwnCinematicShake(const RE::TESRace* a_race)
        {
            if (!a_race) return false;
            if (a_race->HasKeywordString("ActorTypeDragon")) return true;
            if (a_race->GetFormID() == 0x000241B4) return true;
            if (const char* rname = a_race->GetName(); rname && rname[0]) {
                std::string lower(rname);
                for (auto& ch : lower)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lower.find("centurion") != std::string::npos) return true;
            }
            return false;
        }

        void ArmNpcTransformationAction(RE::Actor* actor, NpcNoise::Action action)
        {
            if (!actor || !NpcSourceEnabled(NpcNoise::Source::Transformations)) return;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || actor == player || actor->IsDead() ||
                actor->GetPosition().GetDistance(player->GetPosition()) >= kNpcNoiseRange) return;
            const auto* profile = SettingsManager::GetSingleton().ResolveNpcTransformationNoise(NpcFormOf(actor), action);
            auto& memory = sNpcCombatMemory[actor->GetFormID()];
            if (action == NpcNoise::Action::Concentration) {
                const auto now = CameraEffectClock::Now();
                if (!NpcNoise::RefreshConcentration(memory.spellBeat.active || memory.spellBeat.rearmPending,
                    std::chrono::duration<float>(now - memory.lastConcentrationArm).count())) return;
                memory.lastConcentrationArm = now;
            }
            ArmNpcProfile(memory.spellBeat, memory.spellParams, profile, NpcNoise::Source::Transformations,
                          actor->GetPosition(), kNpcNoiseRange,
                          action == NpcNoise::Action::Concentration ? 0.3f : 1.0f,
                          profile ? std::max(0.3f, profile->shoutFadeDuration) : 0.3f);
        }

        void ArmNpcTransformationEffect(RE::Actor* actor, int locationSlot)
        {
            if (!actor || !NpcSourceEnabled(NpcNoise::Source::Transformations)) return;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || actor == player || actor->IsDead() ||
                actor->GetPosition().GetDistance(player->GetPosition()) >= kNpcNoiseRange) return;
            auto& s = SettingsManager::GetSingleton();
            auto& memory = sNpcCombatMemory[actor->GetFormID()];
            const auto* location = s.ActiveLocationFxBeat(SettingsManager::kFxBeatLocKeys[locationSlot]);
            auto& p = memory.spellParams;
            const auto& feed = s.EventBeatTuning(BeatId::WerewolfFeed);
            p.intensity = location ? location->intensity : locationSlot == 4 ? feed.intensity : s.vampireLordBatsIntensity;
            p.speed = location ? location->speed : locationSlot == 4 ? feed.speed : s.vampireLordBatsSpeed;
            p.chr = location ? location->chr : locationSlot == 4 ? feed.chr : s.vampireLordBatsChar;
            p.range = kNpcNoiseRange;
            p.hold = locationSlot == 4 ? 0.5f : 0.15f;
            if (p.intensity <= 0.0001f) return;
            memory.spellBeat.rearmPending = true;
            memory.spellBeat.hasPos = true;
            memory.spellBeat.pos = actor->GetPosition();
            memory.spellBeat.npcSource = NpcNoise::Source::Transformations;
        }

        void ScanNpcCombat(RE::PlayerCharacter* player)
        {
            auto& s = SettingsManager::GetSingleton();
            auto* lists = RE::ProcessLists::GetSingleton();
            if (!player || !lists ||
                !(NpcSourceEnabled(NpcNoise::Source::Melee) || NpcSourceEnabled(NpcNoise::Source::Archery) ||
                  NpcSourceEnabled(NpcNoise::Source::Transformations))) {
                sNpcCombatMemory.clear();
                return;
            }
            for (auto& [_, memory] : sNpcCombatMemory) memory.seen = false;
            using Phase = NpcNoise::MeleePhase;
            using State = RE::ATTACK_STATE_ENUM;
            for (auto& handle : lists->highActorHandles) {
                auto owner = handle.get();
                auto* actor = owner.get();
                if (!actor || actor == player || actor->IsDead() || !actor->Is3DLoaded() ||
                    RaceHasOwnCinematicShake(actor->GetRace()) ||
                    actor->GetPosition().GetDistance(player->GetPosition()) >= kNpcNoiseRange) continue;
                auto* state = actor->AsActorState();
                if (!state) continue;
                auto& memory = sNpcCombatMemory[actor->GetFormID()];
                memory.seen = true;
                const auto form = NpcFormOf(actor);
                if (memory.formKnown && memory.form != form)
                    CameraNoiseController::NotifyNpcRaceChange(actor);
                memory.form = form;
                memory.formKnown = true;
                const auto attackState = state->GetAttackState();
                Phase phase = Phase::Ranged;
                switch (attackState) {
                case State::kNone: phase = Phase::Idle; break;
                case State::kDraw: phase = Phase::Draw; break;
                case State::kSwing: phase = Phase::Swing; break;
                case State::kHit: phase = Phase::Hit; break;
                case State::kNextAttack:
                case State::kFollowThrough: phase = Phase::Recover; break;
                case State::kBash: phase = Phase::Bash; break;
                default: break;
                }
                RE::NiPointer<RE::BGSAttackData> attackData;
                auto* proc = actor->GetActorRuntimeData().currentProcess;
                if (proc && proc->high) attackData = proc->high->attackData;
                const bool voice = proc && proc->high && (proc->high->voiceState.get() == RE::VOICE_STATE::kStart ||
                                                          proc->high->voiceState.get() == RE::VOICE_STATE::kContinue);
                auto* right = actor->GetEquippedObject(false);
                auto* leftHand = actor->GetEquippedObject(true);
                const bool handSpell = (right && right->As<RE::SpellItem>()) || (leftHand && leftHand->As<RE::SpellItem>());
                if (!NpcNoise::MeleeAllowed(form, voice, handSpell)) phase = Phase::Idle;
                const char* eventName = attackData ? attackData->event.c_str() : nullptr;
                const std::string_view event = eventName ? eventName : "";
                const bool power = (attackData && attackData->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack)) ||
                    (form != NpcNoise::Form::None && NpcNoise::ContainsCI(event, "power"));
                const auto attack = memory.swings.Observe(phase, power, state->IsSneaking(), state->IsSprinting());
                if (attack) {
                    bool left = attackData && attackData->IsLeftAttack();
                    auto weapon = ClassifyMeleeWeaponForm(actor->GetEquippedObject(left));
                    if (weapon == MeleeWeaponType::Unarmed) {
                        const auto other = ClassifyMeleeWeaponForm(actor->GetEquippedObject(!left));
                        if (other != MeleeWeaponType::Unarmed) { weapon = other; left = !left; }
                    }
                    const auto item = s.weaponBindings.empty() ? ItemBindings::EquippedItem{} :
                        ItemBindings::DescribeEquipped(actor, left);
                    int direction = -1;
                    if (attack->power) {
                        if (NpcNoise::ContainsCI(event, "forward")) direction = 1;
                        else if (NpcNoise::ContainsCI(event, "backward")) direction = 2;
                        else if (NpcNoise::ContainsCI(event, "left")) direction = 3;
                        else if (NpcNoise::ContainsCI(event, "right")) direction = 4;
                        else if (NpcNoise::ContainsCI(event, "inplace")) direction = 0;
                    }
                    const auto action = attack->power ? (attack->sprint ? NpcNoise::Action::SprintPowerAttack :
                        NpcNoise::Action::PowerAttack) : NpcNoise::Action::Attack;
                    std::string key;
                    const auto* profile = form == NpcNoise::Form::None
                        ? s.ResolveNpcMeleeNoise(attack->power, attack->sneak, attack->sprint, weapon, direction, item, &key)
                        : s.ResolveNpcTransformationNoise(form, action);
                    ArmNpcProfile(memory.beat, memory.params, profile, NpcNoise::CombatSource(form, false),
                                  actor->GetPosition(), kNpcNoiseRange * 0.5f,
                                  profile ? profile->attackDuration : 0.0f, 0.3f);
                    static unsigned logged = 0;
                    if (form == NpcNoise::Form::None && NpcSourceEnabled(NpcNoise::Source::Melee) && logged < 16) {
                        ++logged;
                        spdlog::debug("[NPCNOISE] melee actor={:08X} weapon={} hand={} event={} entry={} amp={:.2f} armed={}",
                            actor->GetFormID(), MeleeWeaponTypeTomlKey(weapon), left ? "left" : "right", event,
                            key.empty() ? "unset" : key, profile ? profile->amp : 0.0f, memory.beat.rearmPending);
                    }
                }
                auto* equipped = actor->GetEquippedObject(false);
                const auto* bow = equipped ? equipped->As<RE::TESObjectWEAP>() : nullptr;
                const bool archery = bow && (bow->IsBow() || bow->IsCrossbow());
                const bool drawing = attackState == State::kBowDraw || attackState == State::kBowAttached ||
                                     attackState == State::kBowDrawn;
                if (archery && drawing && !s.weaponBindings.empty() &&
                    (!memory.archeryDrawing || memory.archeryWeapon != bow->GetFormID())) {
                    memory.archeryWeapon = bow->GetFormID();
                    memory.archeryItem = ItemBindings::DescribeEquipped(actor, false);
                }
                memory.archeryDrawing = archery && drawing;
                const bool release = attackState == State::kBowReleasing || attackState == State::kBowReleased ||
                                     attackState == State::kFire || attackState == State::kFiring || attackState == State::kFired;
                if (archery && drawing) memory.archerySneak = state->IsSneaking();
                else if (!release) memory.archerySneak = false;
                if (form != NpcNoise::Form::None && NpcSourceEnabled(NpcNoise::Source::Transformations)) {
                    if (voice && !memory.voiceActive && form == NpcNoise::Form::Werewolf)
                        ArmNpcTransformationAction(actor, NpcNoise::Action::Roar);
                    memory.voiceActive = voice;
                    if (form == NpcNoise::Form::VampireLord) {
                        for (int hand = 0; hand < 4; ++hand) {
                            auto* caster = actor->GetMagicCaster(static_cast<RE::MagicSystem::CastingSource>(hand));
                            if (caster && caster->state.get() == RE::MagicCaster::State::kCasting && caster->currentSpell &&
                                caster->currentSpell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                                ArmNpcTransformationAction(actor, NpcNoise::Action::Concentration);
                                break;
                            }
                        }
                    }
                }
            }
            std::erase_if(sNpcCombatMemory, [](const auto& entry) { return !entry.second.seen; });
        }

        void PollNpcArcheryShots(RE::PlayerCharacter* player)
        {
            std::array<NpcArcheryShot, 32> shots;
            const auto count = sNpcArcheryShots.Drain(shots);
            if (!player || !NpcSourceEnabled(NpcNoise::Source::Archery)) return;
            const auto now = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < count; ++i) {
                const auto& shot = shots[i];
                if (!NpcNoise::FreshArcheryShot(std::chrono::duration<float>(now - shot.fired).count())) continue;
                auto owner = shot.shooter.get();
                auto* actor = owner ? owner->As<RE::Actor>() : nullptr;
                if (!actor || actor == player || actor->IsDead() || !actor->Is3DLoaded() ||
                    NpcFormOf(actor) != NpcNoise::Form::None || RaceHasOwnCinematicShake(actor->GetRace()) ||
                    shot.pos.GetDistance(player->GetPosition()) >= kNpcNoiseRange) continue;
                auto& memory = sNpcCombatMemory[actor->GetFormID()];
                ItemBindings::EquippedItem item;
                if (!SettingsManager::GetSingleton().weaponBindings.empty()) {
                    if (memory.archeryWeapon == shot.weapon) item = memory.archeryItem;
                    else {
                        auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(shot.weapon);
                        item = ItemBindings::DescribeForm(weapon);
                        for (bool left : {false, true})
                            if (weapon && actor->GetEquippedObject(left) == weapon) {
                                item = ItemBindings::DescribeEquipped(actor, left);
                                break;
                            }
                    }
                }
                std::string key;
                const auto* profile = SettingsManager::GetSingleton().ResolveNpcArcheryNoise(
                    shot.crossbow, actor->AsActorState()->IsSneaking() || memory.archerySneak, actor->IsOnMount(), item, &key);
                ArmNpcProfile(memory.beat, memory.params, profile, NpcNoise::Source::Archery,
                              shot.pos, kNpcNoiseRange, 0.0f, 0.3f);
                static unsigned logged = 0;
                if (logged < 24) {
                    ++logged;
                    spdlog::debug("[NPCNOISE] archery actor={:08X} projectile={:08X} weapon={:08X} type={} entry={} amp={:.2f} armed={}",
                        actor->GetFormID(), shot.projectile, shot.weapon, shot.crossbow ? "crossbow" : "bow",
                        key.empty() ? "unset" : key, profile ? profile->amp : 0.0f, memory.beat.rearmPending);
                }
            }
        }

        NpcCastResult ScanNpcCasters(RE::PlayerCharacter* a_player, float a_dt)
        {
            auto& s = SettingsManager::GetSingleton();
            NpcCastResult out;
            static std::array<float, 5> sSchoolSmoothed{};
            // VIEW-AGNOSTIC (2026-09-07). The scan runs whenever EITHER half
            // is on and returns an UNSCALED amplitude; each view multiplies by
            // its own Intensity at read time. Gating the scan on one view's
            // slider would let a 0 there wipe sSchoolSmoothed — shared state
            // the other view is still using — which is the tear-down the
            // creature-shake and event-beat fixes both had to undo.
            if (!a_player || !(NpcSourceEnabled(NpcNoise::Source::Magic) || NpcSourceEnabled(NpcNoise::Source::Shouts))) {
                sSchoolSmoothed.fill(0.0f);
                return out;
            }
            const auto  now       = CameraEffectClock::Now();
            const auto  playerPos = a_player->GetPosition();
            const float concRange  = kNpcNoiseRange * kNpcConcRangeMul;
            const float shoutRange = kNpcNoiseRange * kNpcShoutRangeMul;
            std::array<float, 5> wMax{}, wSum{};

            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                for (auto& h : pl->highActorHandles) {
                    auto aptr = h.get();
                    RE::Actor* actor = aptr.get();
                    if (!actor || actor == a_player || actor->IsDead()) continue;
                    // Creatures with their own Cinematic Effects source are
                    // governed by THAT source, in whichever view — not by this
                    // generic layer on top of it.
                    if (RaceHasOwnCinematicShake(actor->GetRace())) continue;
                    if (NpcFormOf(actor) != NpcNoise::Form::None) continue;
                    const float d = actor->GetPosition().GetDistance(playerPos);
                    if (d >= shoutRange) continue;
                    auto& mem   = sNpcMemory[actor->GetFormID()];
                    mem.lastSeen = now;
                    // voiceState fallback: a shout start the spell-cast event
                    // missed (1 s shared debounce with the event path).
                    if (auto* proc = actor->GetActorRuntimeData().currentProcess;
                        proc && proc->high) {
                        const auto vs = proc->high->voiceState.get();
                        const bool starting =
                            vs == RE::VOICE_STATE::kStart || vs == RE::VOICE_STATE::kContinue;
                        const bool wasIdle =
                            mem.lastVoiceState != RE::VOICE_STATE::kStart &&
                            mem.lastVoiceState != RE::VOICE_STATE::kContinue;
                        if (starting && wasIdle &&
                            std::chrono::duration<double>(now - mem.lastShoutArmTp).count() > 1.0) {
                            mem.lastShoutArmTp = now;
                            TryArmNpcShout(actor, 0);
                        }
                        mem.lastVoiceState = vs;
                    }
                    if (d < concRange && NpcSourceEnabled(NpcNoise::Source::Magic)) {
                        constexpr RE::MagicSystem::CastingSource kSrcs[] = {
                            RE::MagicSystem::CastingSource::kLeftHand,
                            RE::MagicSystem::CastingSource::kRightHand,
                            RE::MagicSystem::CastingSource::kOther,
                            RE::MagicSystem::CastingSource::kInstant,
                        };
                        for (auto src : kSrcs) {
                            auto* caster = actor->GetMagicCaster(src);
                            if (!caster ||
                                caster->state.get() != RE::MagicCaster::State::kCasting)
                                continue;
                            auto* sp = caster->currentSpell
                                           ? caster->currentSpell->As<RE::SpellItem>()
                                           : nullptr;
                            if (!sp ||
                                sp->GetCastingType() !=
                                    RE::MagicSystem::CastingType::kConcentration)
                                continue;
                            int schoolIdx = -1;
                            if (auto* eff = sp->GetCostliestEffectItem();
                                eff && eff->baseEffect)
                                schoolIdx = NpcSchoolIndex(eff->baseEffect->GetMagickSkill());
                            if (schoolIdx < 0) continue;
                            const auto sidx = static_cast<std::size_t>(schoolIdx);
                            const float w = ProximityFalloff(d, concRange);
                            wMax[sidx] = std::max(wMax[sidx], w);
                            wSum[sidx] += w;
                        }
                    }
                }
            }
            if (sNpcMemory.size() > 96) {
                std::erase_if(sNpcMemory, [&](const auto& kv) {
                    return std::chrono::duration<double>(now - kv.second.lastSeen).count() > 30.0;
                });
            }

            // Per-school smoothing (rise 0.15 s / fall 0.35 s — kCasting
            // flicker between frames can't pop), then loudest school owns the
            // texture; the rest sum in at half weight, capped.
            const SettingsManager::NoiseProfile* winnerP   = nullptr;
            float                                winnerLoud = 0.0f;
            float                                othersSum  = 0.0f;
            int                                  winnerIdx  = -1;
            for (int i = 0; i < 5; ++i) {
                const auto  sidx   = static_cast<std::size_t>(i);
                const float target = std::min(wMax[sidx] + 0.35f * (wSum[sidx] - wMax[sidx]), 1.5f);
                const bool  rising = target > sSchoolSmoothed[sidx];
                const float tau    = rising ? 0.15f : 0.35f;
                sSchoolSmoothed[sidx] +=
                    (target - sSchoolSmoothed[sidx]) * (1.0f - std::exp(-a_dt / tau));
                if (sSchoolSmoothed[sidx] <= 0.001f) continue;
                const auto* p = ResolveNoiseByKey(kNpcSchoolConcKeys[i]);
                if (!p || p->amp <= 0.0001f) continue;
                const float loud = p->amp * sSchoolSmoothed[sidx];
                if (loud > winnerLoud) {
                    if (winnerLoud > 0.0f) othersSum += winnerLoud;
                    winnerLoud = loud;
                    winnerP    = p;
                    winnerIdx  = i;
                } else {
                    othersSum += loud;
                }
            }
            if (winnerP) {
                const float total = std::min(winnerLoud + 0.5f * othersSum, 1.5f * winnerLoud);
                out.amp    = total;   // unscaled — the view applies its own half
                out.speed  = std::max(0.1f, winnerP->speed);
                out.sway   = winnerP->sway;
                out.tilt   = winnerP->tilt;
                out.dj     = winnerP->driftJitter;
                out.rgh    = winnerP->roughness;
                out.domKey = winnerIdx;
                static bool sConcLogged = false;
                if (!sConcLogged && out.amp > 0.02f) {
                    sConcLogged = true;
                    spdlog::debug("[NPCNOISE] concentration engaged school={} amp={:.2f}",
                                 kNpcSchoolConcKeys[winnerIdx], out.amp);
                }
            }
            return out;
        }

        // Dawnguard's Vampire Lord "Bats" lesser power (DLC1VampireBats) and
        // the magic effect it applies (DLC1BatsEffect). Resolved through
        // TESDataHandler so the local IDs survive any load order, and simply
        // never resolving on an install without Dawnguard — which is how the
        // source no-ops there instead of misfiring. Same pattern as
        // ShoutRegistry's per-shout (plugin, localID) pairs.
        constexpr std::uint32_t kDLC1VampireBatsSpellID = 0x0038B9;
        constexpr std::uint32_t kDLC1BatsEffectID       = 0x01571A;
        static RE::FormID sBatsSpellFormID  = 0;
        static RE::FormID sBatsEffectFormID = 0;
        static bool       sBatsFormsResolved = false;

        void ResolveBatsForms()
        {
            if (sBatsFormsResolved) return;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            // The singleton exists before the load order is populated, so a
            // lookup here can fail for "not loaded yet" and be indistinguishable
            // from "not installed" — and latching on that would leave the source
            // permanently dead on a machine that does have Dawnguard. Probe with
            // a plugin that is always present first, and only commit once the
            // load order is actually readable.
            if (!dh->LookupModByName("Skyrim.esm")) return;
            sBatsFormsResolved = true;
            if (!dh->LookupModByName("Dawnguard.esm")) {
                spdlog::info("[CameraNoise] Dawnguard not in load order; "
                             "Vampire Lord Bats shake will never fire");
                return;
            }
            if (auto* sp = dh->LookupForm(kDLC1VampireBatsSpellID, "Dawnguard.esm"))
                sBatsSpellFormID = sp->GetFormID();
            if (auto* mg = dh->LookupForm(kDLC1BatsEffectID, "Dawnguard.esm"))
                sBatsEffectFormID = mg->GetFormID();
            spdlog::info("[CameraNoise] Bats forms: spell=0x{:08X} effect=0x{:08X}",
                         sBatsSpellFormID, sBatsEffectFormID);
        }

        class MagicBeatSink :
            public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>,
            public RE::BSTEventSink<RE::TESSpellCastEvent>
        {
        public:
            static MagicBeatSink& Get() { static MagicBeatSink s; return s; }

            // ---- Reanimate: the beat fires on the RAISE, not the hit ---------
            // A kReanimate apply lands on living targets and on corpses the
            // spell is too weak to raise (resisted by level). Neither gets up.
            // The hit only queues the actor; the poll below arms the beat the
            // frame the engine's own success signals flip, and forgets it
            // after kReanimateWait if nothing happened.
            //
            // 2026-09-05 11:32 log: a dead guard was raised and the beat
            // stayed silent - lifeState read 2 (kDead) for the whole 4 s
            // wait, so the life state is NOT the first thing to move. Three
            // signals are read now and every transition is logged, so the
            // next log says which one fires and when: the reanimate life
            // state, IsCommandedActor (the raise makes the corpse the
            // caster's commanded actor), and the reanimate effect still
            // being on the target after the resist window.
            struct PendingReanimate
            {
                RE::ActorHandle                       handle;
                std::chrono::steady_clock::time_point tp;
                bool                                  wasDead = false;
                RE::FormID                            mgef    = 0;
                int                                   lastSig = -1;
                NpcNoise::Source                      npcSource = NpcNoise::Source::None;
            };
            static inline std::vector<PendingReanimate> sPendingReanimates;
            static constexpr float kReanimateWait   = 8.0f;
            static constexpr float kResistWindowSec = 0.35f;

            static void QueueReanimate(RE::Actor* a_actor,
                                       std::chrono::steady_clock::time_point a_now,
                                       RE::FormID a_mgef, NpcNoise::Source a_npcSource)
            {
                if (!a_actor) return;
                const auto h = a_actor->GetHandle();
                const bool dead = a_actor->IsDead();
                for (auto& p : sPendingReanimates) {
                    if (p.handle == h) { p.tp = a_now; p.wasDead = dead; p.mgef = a_mgef; p.lastSig = -1; p.npcSource = a_npcSource; return; }
                }
                if (sPendingReanimates.size() >= 16) sPendingReanimates.erase(sPendingReanimates.begin());
                sPendingReanimates.push_back({ h, a_now, dead, a_mgef, -1, a_npcSource });
                static int sQueueLog = 12;
                if (sQueueLog > 0) {
                    --sQueueLog;
                    spdlog::info("[Beat] reanimate hit target=\"{}\" dead={} lifeState={} commanded={} -> waiting for the raise",
                                 a_actor->GetDisplayFullName(), dead,
                                 static_cast<int>(a_actor->AsActorState()->GetLifeState()),
                                 a_actor->IsCommandedActor());
                }
            }

            static void PollPendingReanimates()
            {
                if (sPendingReanimates.empty()) return;
                const auto now = CameraEffectClock::Now();
                static int sPollLog = 40;
                for (auto it = sPendingReanimates.begin(); it != sPendingReanimates.end();) {
                    auto* actor = it->handle.get().get();
                    const float age = std::chrono::duration<float>(now - it->tp).count();
                    if (!actor) { it = sPendingReanimates.erase(it); continue; }
                    const auto life      = actor->AsActorState()->GetLifeState();
                    const bool dead      = actor->IsDead();
                    const bool commanded = actor->IsCommandedActor();
                    auto*      mgef      = it->mgef ? RE::TESForm::LookupByID<RE::EffectSetting>(it->mgef) : nullptr;
                    const bool hasFx     = mgef && actor->AsMagicTarget() && actor->AsMagicTarget()->HasMagicEffect(mgef);
                    const int  sig       = (life == RE::ACTOR_LIFE_STATE::kReanimate ? 1 : 0) |
                                           (dead ? 0 : 2) | (commanded ? 4 : 0) | (hasFx ? 8 : 0);
                    if (sig != it->lastSig && sPollLog > 0) {
                        --sPollLog;
                        spdlog::info("[Beat] reanimate \"{}\" t+{:.2f}s lifeState={} dead={} commanded={} effect={}",
                                     actor->GetDisplayFullName(), age, static_cast<int>(life),
                                     dead, commanded, hasFx);
                    }
                    it->lastSig = sig;
                    // Only a corpse can be raised; a living target never arms.
                    const bool raised = it->wasDead &&
                                        (life == RE::ACTOR_LIFE_STATE::kReanimate || commanded ||
                                         (hasFx && age > kResistWindowSec));
                    if (raised) {
                        spdlog::info("[Beat] reanimate RAISED \"{}\" after {:.2f}s (lifeState={} commanded={} effect={}) -> armed",
                                     actor->GetDisplayFullName(), age, static_cast<int>(life), commanded, hasFx);
                        Arm(sReanimateBeat, now, actor->GetPosition(), "reanimate", 0.0f, it->npcSource);
                        it = sPendingReanimates.erase(it);
                        continue;
                    }
                    if (age > kReanimateWait) {
                        spdlog::info("[Beat] reanimate on \"{}\" never rose in {:.1f}s (dead={} lifeState={} commanded={} effect={}) -> silent",
                                     actor->GetDisplayFullName(), age, dead,
                                     static_cast<int>(life), commanded, hasFx);
                        it = sPendingReanimates.erase(it);
                        continue;
                    }
                    ++it;
                }
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESMagicEffectApplyEvent* a_event,
                RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                auto& s = SettingsManager::GetSingleton();
                // Location-effective beat intensities: the active place's
                // beat copy (Location Override on the row) replaces the
                // global slider, so a beat at 0 globally still arms where a
                // place turns it up. Index order = kFxBeatLocKeys.
                auto effInt = [&](int i, float g) {
                    auto* p = s.ActiveLocationFxBeat(SettingsManager::kFxBeatLocKeys[i]);
                    return p ? p->intensity : g;
                };
                const bool wantBats      = effInt(5, s.vampireLordBatsIntensity) > 0.0001f;
                const bool wantReanimate = s.reanimateShakeIntensity   > 0.0001f ||
                                           s.reanimateShakeIntensityFp > 0.0001f;
                const bool wantSummon    = s.summonShakeIntensity      > 0.0001f ||
                                           s.summonShakeIntensityFp    > 0.0001f;
                const bool wantFeed      = effInt(4, s.EventBeatTuning(BeatId::WerewolfFeed).intensity) > 0.0001f;
                const bool wantBuildup   = effInt(0, s.werewolfTransformIntensity)    > 0.0001f ||
                                           effInt(2, s.vampireLordTransformIntensity) > 0.0001f;
                // The Werewolf Feeding CAMERA entries (Third Person / Target
                // Lock) also latch off this sink, and they apply regardless
                // of any noise slider — so while the player is in beast form
                // the archetype check must run even with every beat at 0.
                // Outside beast form the flag is false and the cheap early-
                // outs keep their old shape.
                const bool wwForm = StateResolver::GetSingleton().IsWerewolf();
                if (!wantBats && !wantReanimate && !wantSummon && !wantFeed && !wantBuildup &&
                    !wwForm)
                    return RE::BSEventNotifyControl::kContinue;

                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) return RE::BSEventNotifyControl::kContinue;
                const auto now = CameraEffectClock::Now();

                // Bats — matched on the effect the power applies to the
                // player. Checked before the archetype tests because it's a
                // cheap FormID compare and it can't collide with them.
                if (wantBats && sBatsEffectFormID != 0 &&
                    a_event->magicEffect == sBatsEffectFormID &&
                    a_event->target.get() == player)
                {
                    Arm(sBatsBeat, now, player->GetPosition(), "bats");
                    return RE::BSEventNotifyControl::kContinue;
                }

                if (wantBats && sBatsEffectFormID != 0 && a_event->magicEffect == sBatsEffectFormID) {
                    auto* target = a_event->target.get();
                    auto* actor = target ? target->As<RE::Actor>() : nullptr;
                    if (actor != player && NpcFormOf(actor) == NpcNoise::Form::VampireLord) {
                        ArmNpcTransformationEffect(actor, 5);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }

                if (!wantReanimate && !wantSummon && !wantFeed && !wantBuildup && !wwForm)
                    return RE::BSEventNotifyControl::kContinue;

                auto* mgef = RE::TESForm::LookupByID<RE::EffectSetting>(a_event->magicEffect);
                if (!mgef) return RE::BSEventNotifyControl::kContinue;
                const auto arch = mgef->data.archetype;

                if (wantFeed && arch == RE::EffectSetting::Archetype::kWerewolfFeed) {
                    auto* caster = a_event->caster.get();
                    auto* actor = caster ? caster->As<RE::Actor>() : nullptr;
                    if (NpcFormOf(actor) != NpcNoise::Form::Werewolf) {
                        auto* target = a_event->target.get();
                        actor = target ? target->As<RE::Actor>() : nullptr;
                    }
                    if (actor != player && NpcFormOf(actor) == NpcNoise::Form::Werewolf) {
                        ArmNpcTransformationEffect(actor, 4);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }

                // Werewolf feeding has its OWN archetype — record property, so
                // any mod's feed drives it with zero support work.
                //
                // The effect is applied to the CORPSE, not to the werewolf:
                // the vanilla record is Skyrim.esm 0x00106395
                // `PlayerWerewolfVictimEffect` (verified with a raw MGEF scan
                // of the shipped ESMs — it is the only kWerewolfFeed effect in
                // the game, and its name says whose it is). Matching on
                // target == player therefore never fired once, which is why
                // the beat looked dead. The PLAYER is the caster, so accept
                // either end of the apply: caster covers vanilla and anything
                // shaped like it, target keeps working for a mod that flips
                // the effect around onto the feeder.
                if (arch == RE::EffectSetting::Archetype::kWerewolfFeed &&
                    (a_event->caster.get() == player || a_event->target.get() == player))
                {
                    // The camera state latch is unconditional — the Feeding
                    // profile applies whether or not the beat is turned up.
                    StateResolver::GetSingleton().NotifyWerewolfFeed(mgef->GetFormID());
                    if (wantFeed)
                        EventBeatSources::Enqueue(BeatId::WerewolfFeed, nullptr);
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Transformation build-up: the kWerewolf / kVampireLord
                // archetype applies at the START of the change, seconds before
                // the race-switch edge fires the climax beat. Latch the
                // timestamp; the transform envelope resumes from this ramp.
                if (wantBuildup && a_event->target.get() == player) {
                    if (arch == RE::EffectSetting::Archetype::kWerewolf) {
                        sWWArchetypeTp = now;
                    } else if (arch == RE::EffectSetting::Archetype::kVampireLord) {
                        sVLArchetypeTp = now;
                    }
                }

                // NPC-cast reanimate/summon are gated (not scaled) by the NPC
                // Noise slider: 0 = off, >0 = the shipped behaviour. The
                // slider's 1.0 default keeps every existing preset identical.
                const auto castSource = NpcMagicSource(a_event->caster.get());
                const bool npcCastOk = NpcSourceEnabled(castSource);

                if (wantReanimate && npcCastOk &&
                    arch == RE::EffectSetting::Archetype::kReanimate)
                {
                    // The apply event is the HIT, not the raise. It fires for
                    // every body the bolt lands on - living targets, and
                    // corpses too high in level to be raised, where the effect
                    // is resisted and nothing gets up (user, 2026-09-05:
                    // "successful reanimations only"). So the hit only queues
                    // the target; PollPendingReanimates fires the beat the
                    // frame the corpse's life state actually flips to
                    // reanimated, and drops it quietly if that never happens.
                    auto* at = a_event->target.get();
                    auto* atActor = at ? at->As<RE::Actor>() : nullptr;
                    if (atActor) QueueReanimate(atActor, now, mgef->GetFormID(), castSource);
                }
                else if (wantSummon && npcCastOk &&
                         arch == RE::EffectSetting::Archetype::kSummonCreature)
                {
                    // Summon effects land on the SUMMONER, and the portal
                    // opens in front of them — so the caster is both the
                    // right anchor and the one ref guaranteed to exist at
                    // this point (the creature isn't spawned yet).
                    //
                    // YOUR OWN SUMMON GETS NO DISTANCE FALLOFF. The anchor is
                    // where the player STOOD when casting, and the falloff is
                    // full-strength only inside a quarter of Range (375u at
                    // the default 1500) — so summoning and then moving, which
                    // is how a summon is actually used, walked the beat's
                    // amplitude off within a second or two of the cast:
                    // "summoning noise cuts out". A player-cast summon is a
                    // player-centric event exactly like Bats (which skips the
                    // falloff outright); the distance scaling is for OTHER
                    // people's summons, where "how close did that happen" is
                    // the right question. floor=1 keeps nearness at 1 too, so
                    // the far-event character shaping stays off for your own.
                    auto* ac = a_event->caster.get();
                    if (!ac) ac = a_event->target.get();
                    if (ac) {
                        const bool selfCast =
                            a_event->caster.get() ==
                            static_cast<RE::TESObjectREFR*>(player);
                        Arm(sSummonBeat, now, ac->GetPosition(),
                            selfCast ? "summon(self)" : "summon",
                            selfCast ? 1.0f : 0.0f, castSource);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            // Backstop for Bats: if a setup routes the power through a
            // different effect (or the effect apply is swallowed), the spell
            // cast itself still names it. Same beat, so a build that fires
            // both only ever gets one — Arm re-starts rather than stacking.
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESSpellCastEvent* a_event,
                RE::BSTEventSource<RE::TESSpellCastEvent>*) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                auto* caster = a_event->object.get();
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!caster || !player) return RE::BSEventNotifyControl::kContinue;
                if (caster != player) {
                    auto* actor = caster->As<RE::Actor>();
                    const auto form = NpcFormOf(actor);
                    if (form != NpcNoise::Form::None) {
                        if (actor->IsDead() || !NpcSourceEnabled(NpcNoise::Source::Transformations))
                            return RE::BSEventNotifyControl::kContinue;
                        if (form == NpcNoise::Form::VampireLord && sBatsSpellFormID != 0 && a_event->spell == sBatsSpellFormID)
                            ArmNpcTransformationEffect(actor, 5);
                        else if (auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell)) {
                            if (form == NpcNoise::Form::Werewolf && ShoutRegistry::GetSingleton().IsShoutSpell(a_event->spell))
                                ArmNpcTransformationAction(actor, NpcNoise::Action::Roar);
                            else if (form == NpcNoise::Form::VampireLord &&
                                     spell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget)
                                ArmNpcTransformationAction(actor, NpcNoise::Action::FireAndForget);
                        }
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }
                // Bats backstop (player-only). Deliberately NOT an early gate
                // for the whole handler any more — on a Dawnguard-less
                // install sBatsSpellFormID stays 0 and the NPC branch below
                // must still run.
                if (sBatsSpellFormID != 0 && a_event->spell == sBatsSpellFormID) {
                    auto& smB = SettingsManager::GetSingleton();
                    auto* lbB = smB.ActiveLocationFxBeat(SettingsManager::kFxBeatLocKeys[5]);
                    if ((lbB ? lbB->intensity : smB.vampireLordBatsIntensity) > 0.0001f &&
                        caster == static_cast<RE::TESObjectREFR*>(player)) {
                        Arm(sBatsBeat, CameraEffectClock::Now(),
                            player->GetPosition(), "bats(cast)");
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                // NPC shout — a shout FIRES as its per-word variation spell,
                // and this event carries that spell for every actor. The
                // player's shouts own the full 3p envelope, so they are
                // excluded here; dragons belong to the dragon shake system.
                if (!(NpcSourceEnabled(NpcNoise::Source::Magic) || NpcSourceEnabled(NpcNoise::Source::Shouts)))
                    return RE::BSEventNotifyControl::kContinue;
                if (caster == static_cast<RE::TESObjectREFR*>(player))
                    return RE::BSEventNotifyControl::kContinue;
                auto* act = caster->As<RE::Actor>();
                if (!act || act->IsDead()) return RE::BSEventNotifyControl::kContinue;
                if (const auto* race = act->GetRace();
                    race && race->HasKeywordString("ActorTypeDragon"))
                    return RE::BSEventNotifyControl::kContinue;
                const auto pos = act->GetPosition();
                if (!ShoutRegistry::GetSingleton().IsShoutSpell(a_event->spell)) {
                    if (!NpcSourceEnabled(NpcNoise::Source::Magic)) return RE::BSEventNotifyControl::kContinue;
                    // NPC FIRE-AND-FORGET cast.
                    //
                    // This was the hole: NPC noise only ever covered shouts and
                    // CONCENTRATION casts (the per-frame caster walk filters on
                    // kConcentration). Concentration is the sustained-beam
                    // family — Flames, Sparks, a healing stream. Every spell an
                    // enemy mage actually throws at you, firebolt through
                    // fireball, is fire-and-forget, so none of them made any
                    // noise at all even though the same spell from your own
                    // hands produces a charge ramp, a release burst and a kick.
                    //
                    // This event is the right place for it: it carries caster
                    // and spell for every actor, and a release is an EDGE, which
                    // is what a beat wants (the caster-state walk is a level and
                    // would need its own per-hand edge tracking to see one).
                    // Character comes from the player's own
                    // magic.<school>.fire_and_forget cell, so an NPC's fireball
                    // is shaped like yours and nothing new needs tuning.
                    if (pos.GetDistance(player->GetPosition()) > kNpcNoiseRange)
                        return RE::BSEventNotifyControl::kContinue;
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_event->spell);
                    if (!sp || sp->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget)
                        return RE::BSEventNotifyControl::kContinue;
                    int schoolIdx = -1;
                    if (auto* eff = sp->GetCostliestEffectItem(); eff && eff->baseEffect)
                        schoolIdx = NpcSchoolIndex(eff->baseEffect->GetMagickSkill());
                    if (schoolIdx < 0) return RE::BSEventNotifyControl::kContinue;
                    TryArmNpcCast(schoolIdx, pos);
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (pos.GetDistance(player->GetPosition()) > kNpcNoiseRange * kNpcShoutRangeMul)
                    return RE::BSEventNotifyControl::kContinue;
                auto& mem = sNpcMemory[act->GetFormID()];
                mem.lastShoutArmTp = CameraEffectClock::Now();  // debounces the voiceState poll
                TryArmNpcShout(act, a_event->spell);
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            static void Arm(EventBeat& a_beat,
                            std::chrono::steady_clock::time_point a_now,
                            const RE::NiPoint3& a_pos, const char* a_tag,
                            float a_falloffFloor = 0.0f, NpcNoise::Source a_npcSource = NpcNoise::Source::None)
            {
                // Re-arming an already-running beat REFRESHES it rather than
                // stacking a second one — reanimating a whole room raises
                // several corpses within a few frames, and a spell with more
                // than one summon effect fires more than one event for a
                // single cast. Each should keep the shake alive, not multiply
                // it and not restart it from silence. The envelope poll
                // resolves the actual restart point; see rearmPending.
                (void)a_now;
                a_beat.rearmPending = true;
                a_beat.pos          = a_pos;
                a_beat.hasPos       = true;
                // Assigned unconditionally: the floor belongs to THIS arm (a
                // player's summon after an NPC's must not inherit the NPC's
                // zero, and vice versa).
                a_beat.falloffFloor = a_falloffFloor;
                a_beat.npcSource = a_npcSource;
                spdlog::debug("[EventBeat] {} armed at ({:.0f},{:.0f},{:.0f}){}",
                             a_tag, a_pos.x, a_pos.y, a_pos.z,
                             a_beat.active ? " (refresh)" : "");
            }
        };

        static bool sMagicBeatSinkAdded = false;
        void EnsureMagicBeatSubscription()
        {
            ResolveBatsForms();
            if (sMagicBeatSinkAdded) return;
            auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
            if (!holder) return;
            holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&MagicBeatSink::Get());
            holder->AddEventSink<RE::TESSpellCastEvent>(&MagicBeatSink::Get());
            sMagicBeatSinkAdded = true;
            spdlog::info("[CameraNoise] magic-effect beat sink subscribed");
        }

        // Slow Time Noise (Cinematic Effects). Returns the factor to scale
        // every noise CLOCK by this frame — 1.0 when time is running normally.
        //
        // Camera noise advances on wall-clock time, deliberately: it has to
        // keep moving while the game is paused for a menu. The side effect is
        // that a Slow Time shout leaves the shake running at full tempo over a
        // world that has slowed to a crawl, which is exactly the tell that
        // breaks the effect. Scaling the clock (never the amplitude) puts the
        // shake back on the world's clock without changing how big it is.
        //
        // Read from the engine's live global time multiplier rather than from
        // shout detection. That is what makes it scale with the strength of
        // the slow for free — three words set a lower multiplier than one — and
        // it means any other slow-time source is handled identically, with no
        // per-source support. It also naturally covers the ramp in and out,
        // since the engine eases the multiplier itself.
        float SlowTimeNoiseFactor()
        {
            const float strength = std::clamp(
                SettingsManager::GetSingleton().slowTimeNoiseStrength, 0.0f, 1.0f);
            if (strength <= 0.0001f) return 1.0f;
            const float mul = RE::BSTimer::QGlobalTimeMultiplier();
            // Only ever SLOW the noise. Sped-up time (console SGTM, some mods'
            // haste effects) driving the shake faster than the user tuned it
            // is not something anyone asked for, and it reads as a bug.
            if (!std::isfinite(mul) || mul >= 1.0f) return 1.0f;
            const float clamped = std::clamp(mul, 0.05f, 1.0f);
            return 1.0f + (clamped - 1.0f) * strength;
        }

        // Shared envelope for the three event beats: smoothstep attack →
        // hold → smoothstep decay, exactly like the sheathe beat, and for
        // the same reason (no instant edges — a 0→peak step on the trigger
        // frame reads as a pop no matter how small the peak).
        //
        // a_hold is per-source: Bats is a dash, reanimation and summoning
        // are second-and-a-bit events, so they hold longer before the
        // source's own Fade Duration takes them down.
        // a_attack: how long the beat takes to reach full. 0.06s is right for
        // an IMPACT (a ward shattering, a summon landing) — it is meant to hit.
        // It is wrong for a beat whose amplitude is comparable to the ambient
        // it lands on top of, because then the composite doubles inside four
        // frames and that is a snap, not a punch. Callers that add a lot
        // relative to what is already playing pass a longer rise; see the
        // attack beat.
        float EventBeatEnvelope(EventBeat& a_beat, bool a_enabled,
                                float a_hold, float a_fade, float a_attack = 0.06f)
        {
            if (!a_enabled) {
                a_beat.active = false;
                a_beat.rearmPending = false;
                a_beat.lastEnv = 0.0f;
                return 0.0f;
            }
            const float kAttack = std::clamp(a_attack, 0.02f, 1.0f);
            const float hold  = std::clamp(a_hold, 0.0f, 3.0f);
            const float decay = std::max(0.15f, a_fade);
            const auto  now   = CameraEffectClock::Now();

            const auto smooth = [](float x) {
                x = std::clamp(x, 0.0f, 1.0f);
                return x * x * (3.0f - 2.0f * x);
            };
            const auto shapeAt = [&](float t) -> float {
                if (t >= kAttack + hold + decay) return -1.0f;   // finished
                if (t < kAttack)        return smooth(t / kAttack);
                if (t < kAttack + hold) return 1.0f;
                return 1.0f - smooth((t - kAttack - hold) / decay);
            };

            if (a_beat.rearmPending) {
                a_beat.rearmPending = false;
                // Restart the envelope from the amplitude it is ALREADY at, by
                // back-dating the start into the attack ramp to whatever point
                // produces that value. A beat that has decayed to 0.4 resumes
                // climbing from 0.4; a fresh one starts at 0. Either way the
                // output never steps, and it never goes backwards on a refresh.
                // Resume from what the beat was ACTUALLY RENDERING, not from a
                // recomputation of where it "should" be.
                //
                // shapeAt() here is built from the NEW arm's attack/hold/decay,
                // but tOld belongs to the OLD one. Feed one into the other and
                // the answer is meaningless — and specifically, whenever the
                // old elapsed time exceeds the new envelope's total length the
                // guard `t >= attack + hold + decay` reports FINISHED, so
                // resumeEnv came back 0 and the beat restarted from silence.
                // That is precisely the failure the struct comment warns
                // about, and it is what "the noise stops and starts again
                // within like 0.2 seconds" is: a still-audible sprint power
                // beat cut to zero the instant the next power attack armed,
                // because the two cells have different Duration and fade.
                //
                // lastEnv is the rendered value, so it needs no parameters and
                // cannot disagree with itself across an arm.
                const float resumeEnv = a_beat.active
                    ? std::clamp(a_beat.lastEnv, 0.0f, 1.0f)
                    : 0.0f;
                a_beat.start  = now - std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(InvSmoothstep(resumeEnv) * kAttack));
                a_beat.active = true;
            }

            if (!a_beat.active) { a_beat.lastEnv = 0.0f; return 0.0f; }
            const float t   = std::chrono::duration<float>(now - a_beat.start).count();
            const float env = shapeAt(t);
            if (env < 0.0f) { a_beat.active = false; a_beat.lastEnv = 0.0f; return 0.0f; }
            a_beat.lastEnv = env;
            return env;
        }

        // Distance falloff for the proximity beats, matching the dragon /
        // centurion sources: full strength up to a quarter of Range, then a
        // smooth roll-off to nothing at Range. Player-centric sources (Bats)
        // skip this entirely.
        float EventBeatFalloff(const EventBeat& a_beat, RE::PlayerCharacter* a_player,
                               float a_range)
        {
            if (!a_beat.hasPos || !a_player) return 1.0f;
            const float range = std::max(50.0f, a_range);
            const auto& pp = a_player->GetPosition();
            const float dx = a_beat.pos.x - pp.x;
            const float dy = a_beat.pos.y - pp.y;
            const float dz = a_beat.pos.z - pp.z;
            const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d >= range) return a_beat.falloffFloor;
            const float nearEdge = range * 0.25f;
            if (d <= nearEdge) return 1.0f;
            const float t = (d - nearEdge) / (range - nearEdge);  // 0 at nearEdge, 1 at range
            return std::max(a_beat.falloffFloor, 1.0f - (t * t * (3.0f - 2.0f * t)));
        }

        // RECONSTRUCTED 2026-08-09 (lost to a bad edit; rebuilt from HEAD's
        // three-source version plus the table/NPC pools the mega-batch added).
        // Loudest-wins across every one-shot source: the three bespoke beats,
        // the table-driven entries from EventBeatDefs, and the two NPC beats.
        struct EventBeatResult
        {
            float                                      amp   = 0.0f;
            float                                      speed = 1.0f;
            const SettingsManager::CinematicShakeChar* chr   = nullptr;
            // Which beat won, for [FPNOISE]. A bare amp column can say the
            // event layer is firing but not WHICH source, and six sources
            // reach this struct.
            const char*                                src   = "";
            // Where the winning event happened, carried so the render can make
            // the shake radiate from it. Captured whenever the source knew a
            // position — INDEPENDENT of the def's `positional` flag, which only
            // decides whether a distance falloff applies. A standing stone you
            // are touching has no falloff and a very definite location.
            RE::NiPoint3                               pos{};
            bool                                       hasPos   = false;
            // 1 at the source, falling to 0 at Range. Drives the character
            // shaping below; stays 1 for anything without a falloff.
            float                                      nearness = 1.0f;
            // How hard to bias the motion toward pos (the entry's Direction).
            float                                      dirBias  = 0.0f;
        };
        // a_fp: which half of Reanimation / Summoning this pass is sized with.
        EventBeatResult PollEventBeats(RE::PlayerCharacter* a_player, bool a_fp)
        {
            const auto& s = SettingsManager::GetSingleton();
            EventBeatResult out;
            MagicBeatSink::PollPendingReanimates();

            auto consider = [&](EventBeat& beat, float intensity, float speed,
                                float hold, const SettingsManager::CinematicShakeChar& chr,
                                float range, bool proximity, float dirBias = 0.0f,
                                const char* a_src = "?") {
                // NPC ownership follows each arm (including queued raises).
                // A muted POV still ages the event; returning to an enabled
                // view cannot start a spell that happened earlier.
                if (beat.npcSource != NpcNoise::Source::None) {
                    EventBeatEnvelope(beat, true, hold, chr.fadeDuration);
                    if (!NpcNoise::AllowMagicSource(true, NpcIntensity(beat.npcSource, a_fp))) return;
                }
                // A view that is switched OFF contributes nothing and TOUCHES
                // NOTHING. Calling the envelope with a_enabled=false tears the
                // beat down (active=false, lastEnv=0), and these beats are
                // shared between the two views — so a source whose 1p
                // Intensity is 0 would reach across and kill an event the 3p
                // view was still playing, the mirror of the leak fixed in the
                // creature-shake envelope on 2026-09-07. The amplitude itself
                // was never at risk here: `amp` below multiplies by THIS
                // view's intensity, so 0 renders 0 without any of this.
                const bool on = intensity > 0.0001f;
                if (!on) return;
                const float env = EventBeatEnvelope(beat, true, hold, chr.fadeDuration);
                if (env <= 0.0f) return;
                float amp  = env * intensity * beat.armScale;
                float proximityWeight = 1.0f;
                if (proximity) {
                    proximityWeight = EventBeatFalloff(beat, a_player, range);
                    amp *= proximityWeight;
                }
                if (amp > out.amp) {
                    out.src      = a_src;
                    out.amp      = amp;
                    out.speed    = std::max(0.1f, speed);
                    out.chr      = &chr;
                    out.pos      = beat.pos;
                    out.hasPos   = beat.hasPos;
                    out.nearness = proximityWeight;
                    out.dirBias  = dirBias;
                }
            };

            // Holds: the dash is over almost as soon as it starts; a raise
            // and a summon both take about a second and a half of screen
            // time, so they sit at full through the animation and let Fade
            // Duration carry the tail.
            // Bats rides the location layer: the active place's beat copy
            // (if bound) replaces the global tuning wholesale. The chr
            // reference points into the place's map entry, which is stable
            // for the frame (nothing mutates locationOverrides mid-poll).
            {
                auto* lb = SettingsManager::GetSingleton().ActiveLocationFxBeat(
                    SettingsManager::kFxBeatLocKeys[5]);
                if (lb) consider(sBatsBeat, lb->intensity, lb->speed,
                                 0.15f, lb->chr, 0.0f, /*proximity=*/false, 0.0f, "bats-loc");
                else    consider(sBatsBeat, s.vampireLordBatsIntensity, s.vampireLordBatsSpeed,
                                 0.15f, s.vampireLordBatsChar, 0.0f, /*proximity=*/false, 0.0f, "bats");
            }
            // Per-view halves (2026-09-06). Same shape as the creature shakes:
            // one detection pass, one envelope, two sets of numbers.
            consider(sReanimateBeat,
                     (a_fp ? s.reanimateShakeIntensityFp : s.reanimateShakeIntensity) *
                         (sReanimateBeat.npcSource == NpcNoise::Source::Transformations
                              ? NpcIntensity(NpcNoise::Source::Transformations, a_fp) : 1.0f),
                     a_fp ? s.reanimateShakeSpeedFp     : s.reanimateShakeSpeed,
                     1.20f,
                     a_fp ? s.reanimateShakeCharFp      : s.reanimateShakeChar,
                     a_fp ? s.reanimateShakeRangeFp     : s.reanimateShakeRange, /*proximity=*/true, 0.0f, "reanimate");
            consider(sSummonBeat,
                     (a_fp ? s.summonShakeIntensityFp : s.summonShakeIntensity) *
                         (sSummonBeat.npcSource == NpcNoise::Source::Transformations
                              ? NpcIntensity(NpcNoise::Source::Transformations, a_fp) : 1.0f),
                     a_fp ? s.summonShakeSpeedFp        : s.summonShakeSpeed,
                     0.90f,
                     a_fp ? s.summonShakeCharFp         : s.summonShakeChar,
                     a_fp ? s.summonShakeRangeFp        : s.summonShakeRange, /*proximity=*/true, 0.0f, "summon");

            // Table-driven entries. Hold and positional-ness come from the
            // static def; intensity / speed / range / character are the user's.
            // Feeding spans the feed (2026-09-05): while the resolver holds
            // the feed state the beat is re-armed every frame (resume-not-
            // restart, so the plateau just continues), and the frame the
            // feed releases its hold drops to zero so the tail is exactly
            // the beat's Fade Duration from that moment - no fixed 1.5 s
            // hold hanging past the end of the animation.
            const bool feedingNow = StateResolver::GetSingleton().IsWerewolfFeeding();
            for (std::size_t i = 0; i < kEventBeatCount; ++i) {
                const auto& d = kEventBeatDefs[i];
                const bool  isFeed = (i == static_cast<std::size_t>(BeatId::WerewolfFeed));
                // Feeding (the only table beat with a Location Override row)
                // takes the active place's copy when bound.
                const SettingsManager::BeatTuning* lb =
                    isFeed ? SettingsManager::GetSingleton().ActiveLocationFxBeat(
                                 SettingsManager::kFxBeatLocKeys[4])
                           : nullptr;
                const auto& t = lb ? *lb : s.eventBeats[i];
                float hold = d.hold;
                if (isFeed) {
                    // NOT every frame (12:53 log, "only a small burst after
                    // the feeding has already ended"): a re-arm resumes from
                    // lastEnv by moving the arm time to the matching point of
                    // the attack, so re-arming on the very frame the envelope
                    // is sampled pins it at that value - at 0 for the whole
                    // feed. Re-arm on a coarse interval instead (well inside
                    // the 1.5 s hold, so the plateau never lapses) and once
                    // more on the release edge, so the fade starts from full
                    // at the animation's end rather than partway through.
                    static auto sFeedLastRearm = std::chrono::steady_clock::time_point{};
                    static bool sFeedPrev      = false;
                    const auto  nowFeed        = CameraEffectClock::Now();
                    auto& fb = sTableBeats[i];
                    if (feedingNow && fb.active &&
                        std::chrono::duration<float>(nowFeed - sFeedLastRearm).count() > 0.5f) {
                        fb.rearmPending = true;
                        sFeedLastRearm  = nowFeed;
                    }
                    if (sFeedPrev && !feedingNow && fb.active) fb.rearmPending = true;
                    if (!feedingNow) sFeedLastRearm = {};
                    sFeedPrev = feedingNow;
                    hold = feedingNow ? d.hold : 0.0f;
                }
                consider(sTableBeats[i], t.intensity, t.speed, hold, t.chr,
                         t.range, d.positional, t.direction, d.tomlKey);
            }

            // NPC shouts / transforms — parameters latched by the sink that
            // armed them, so a beat keeps the character it was armed with.
            consider(sNpcShoutBeat,
                     a_fp ? sNpcShoutParams.intensityFp : sNpcShoutParams.intensity,
                     sNpcShoutParams.speed,
                     sNpcShoutParams.hold, sNpcShoutParams.chr,
                      sNpcShoutParams.range, /*proximity=*/true, 0.0f, "npc-magic");
            for (auto& [_, memory] : sNpcMemory) {
                const auto& p = memory.shoutParams;
                consider(memory.shoutBeat, p.intensity * NpcIntensity(NpcNoise::Source::Shouts, a_fp),
                         p.speed, p.hold, p.chr, p.range, true, 0.0f, "npc-shout");
            }
            consider(sNpcTransformBeat,
                     sNpcTransformParams.intensity * NpcIntensity(NpcNoise::Source::Transformations, a_fp),
                     sNpcTransformParams.speed, sNpcTransformParams.hold,
                     sNpcTransformParams.chr, sNpcTransformParams.range,
                      /*proximity=*/true, 0.0f, "npc-transform");

            for (auto& [_, memory] : sNpcCombatMemory) {
                const auto& p = memory.params;
                const char* source = memory.beat.npcSource == NpcNoise::Source::Archery ? "npc-archery" :
                    memory.beat.npcSource == NpcNoise::Source::Transformations ? "npc-beast-attack" : "npc-melee";
                consider(memory.beat, p.intensity * NpcIntensity(memory.beat.npcSource, a_fp), p.speed,
                         p.hold, p.chr, p.range, true, 0.0f, source);
                const auto& spell = memory.spellParams;
                consider(memory.spellBeat, spell.intensity * NpcIntensity(NpcNoise::Source::Transformations, a_fp),
                         spell.speed, spell.hold, spell.chr, spell.range, true, 0.0f, "npc-beast-magic");
            }

            return out;
        }

        // Distance falloff on a LIVE distance: flat inside a quarter of Range,
        // smoothstep to nothing at Range. Kept after the proximity-ambience
        // removal because the NPC concentration-casting scan still uses it.
        float ProximityFalloff(float a_d, float a_range)
        {
            const float range = std::max(50.0f, a_range);
            if (a_d >= range) return 0.0f;
            const float nearEdge = range * 0.25f;
            if (a_d <= nearEdge) return 1.0f;
            const float t = (a_d - nearEdge) / (range - nearEdge);
            return 1.0f - (t * t * (3.0f - 2.0f * t));
        }

        // Per-dragon state memory. The engine flags ahead of the
        // visible event: kTakeOff is set during the wind-up before the
        // wing-flap actually pushes off, kLanding is set during descent
        // before touchdown, meleeAttackState sustains across the full
        // swing→hit→followthrough animation (~0.5s). Reacting to the
        // raw states sustains the shake for too long. We track edges
        // and emit short impulse windows aligned with the actual
        // impact moments — the Sharp / Heavy decay tails do the rest.
        struct DragonMemory
        {
            RE::FLY_STATE                         lastFlyState    = RE::FLY_STATE::kNone;
            RE::ATTACK_STATE_ENUM                 lastAttackState = RE::ATTACK_STATE_ENUM::kNone;
            // Touchdown impulse: fires on airborne → grounded
            // transitions OR when z-velocity decelerates sharply
            // (the fallback path catches perch landings where
            // engine state lags visible touchdown).
            std::chrono::steady_clock::time_point landingImpulseEnd{};
            // Debounce so the fly-state edge and the z-velocity fallback can't
            // both fire one shake for a single touchdown.
            std::chrono::steady_clock::time_point landingCooldownEnd{};
            // Touchdown confirmation for the z-velocity fallback. A momentary
            // mid-air deceleration (dive → hover/approach) looks like a "stop"
            // but the dragon keeps descending afterward — that produced a false
            // shake high above the real landing. We hold the candidate and only
            // fire if the dragon actually STAYS put (didn't keep dropping).
            bool                                  touchdownPending = false;
            float                                 touchdownCandZ   = 0.0f;
            std::chrono::steady_clock::time_point touchdownCandTime{};
            // Take-off impulse: scheduled for partway through kTakeOff
            // so it lines up with the wing-flap pushing off the ground
            // rather than the wind-up animation start.
            std::chrono::steady_clock::time_point takeoffImpulseStart{};
            std::chrono::steady_clock::time_point takeoffImpulseEnd{};
            // Bite/swipe impulse: short burst on the rising edge into
            // an active strike state. Decays via Sharp tau. attackKind
            // is captured at the rising edge so the per-source slider
            // (Bite vs Tail vs Wing) is selected by the attack type
            // that fired, not whatever's live now.
            std::chrono::steady_clock::time_point biteImpulseEnd{};
            // 0 = Bite, 1 = Tail Swipe, 2 = Wing Buffet
            int                                   attackKind      = 0;
            // Z-velocity tracking for perch-landing detection. The
            // engine often clears kLanding noticeably after the
            // dragon's visible touchdown on a perch (slower, more
            // controlled approach). Watching the dragon's z-velocity
            // ourselves catches the actual contact moment.
            float                                 lastZ           = 0.0f;
            float                                 lastZVel        = 0.0f;
            bool                                  hasLastZ        = false;
            bool                                  hasLastZVel     = false;
            std::chrono::steady_clock::time_point lastZTime{};
            std::chrono::steady_clock::time_point lastSeen{};
        };
        std::unordered_map<RE::FormID, DragonMemory> sDragonMemory;
        // Result of the dragon-activity scan: the distance to the closest
        // nearby dragon doing something the camera should react to, plus a
        // per-source intensity weight so we can make landings hit harder
        // than a casual cruise-takeoff. distance < 0 means no source.
        //
        // Kind drives the decay shape — sharp impulses (bite, fireball
        // spit) snap on and fall fast for an "impact" feel; heavy events
        // (landing, take-off) hold their tail longer for ground-rumble;
        // sustained streams (breath beam) match the engine duration.
        enum class DragonShakeKind
        {
            None,
            Sustained,  // breath beam — matches engine cast duration
            Sharp,      // bite, fireball — short and snappy
            Heavy,      // landing / take-off — extended decay tail
        };
        struct DragonShakeSource
        {
            float           distance     = -1.0f;
            float           intensityMul = 0.0f;
            DragonShakeKind kind         = DragonShakeKind::None;
            float           speed        = 1.0f;
            float           range        = 3000.0f;
            SettingsManager::CinematicShakeChar chr{};  // per-source character
        };

        // Carries the winning source's character from UpdateDragonShake to the
        // apply paths (both in this TU), parallel to the dragonShakeKind member.
        SettingsManager::CinematicShakeChar sDragonShakeCharOut[2]{};

        // Walks the high-process actor list, identifies dragons (via the
        // ActorTypeDragon race keyword), and computes a per-source
        // intensity for whatever dragon-y thing each is doing this frame.
        // Picks the loudest activity the closest dragon has, then returns
        // that pair. Cinematic Effects → Dragon Breath Shake uses both.
        //
        // Sources (highest → lowest intensity):
        //   - Landing animation (kLanding): the slam-into-the-ground beat.
        //   - Concentration breath beam: sustained roar / firehose.
        //   - FireAndForget cast: fireball spit / shout.
        //   - Melee attack swing: bite / tail-swipe / wing-buffet.
        //   - Take-off: wings beating off the ground.
        // The "intensity" is a multiplier on the boost amp; falloff with
        // distance still applies on top.
        // a_fp picks WHICH half of every creature source this pass is sized
        // with (2026-09-06). One detection pass, one envelope, two sets of
        // numbers — amp, speed, range AND character all split, so a source
        // can be quieter, slower, tighter and shaped differently on the eye
        // than it is over the shoulder. Range included deliberately: a
        // rumble that reads well at 3000 units in 3p is often too much at
        // that distance in 1p.
        DragonShakeSource FindNearestDragonShakeSource(RE::PlayerCharacter* a_player, float a_range,
                                                       bool a_fp)
        {
            DragonShakeSource result{};
            if (!a_player) return result;
            auto* lists = RE::ProcessLists::GetSingleton();
            if (!lists) return result;
            const auto& s = SettingsManager::GetSingleton();

            // Prune stale per-dragon memory entries. Cheap erase-if
            // keyed on lastSeen — anything we haven't observed for
            // 30 seconds is either despawned, dead, or out of high
            // process. Bounds the map size across long sessions.
            {
                const auto cutoff = CameraEffectClock::Now() - std::chrono::seconds(30);
                for (auto it = sDragonMemory.begin(); it != sDragonMemory.end();) {
                    if (it->second.lastSeen < cutoff) it = sDragonMemory.erase(it);
                    else                              ++it;
                }
            }

            using Src    = RE::MagicSystem::CastingSource;
            using CType  = RE::MagicSystem::CastingType;
            using CState = RE::MagicCaster::State;
            using FState = RE::FLY_STATE;
            using AState = RE::ATTACK_STATE_ENUM;

            const auto& pp = a_player->GetPosition();
            const float maxRangeSq = a_range * a_range;

            for (auto& handle : lists->highActorHandles) {
                auto actorPtr = handle.get();
                auto* actor   = actorPtr.get();
                if (!actor || actor == a_player) continue;
                if (actor->IsDead()) continue;

                auto* base = actor->GetActorBase();
                if (!base || !base->race) continue;
                auto* race = base->race;
                const bool isDragon = race->HasKeywordString("ActorTypeDragon");
                // Centurion filter — vanilla DwarvenCenturionRace
                // (Skyrim.esm 0x000241B4) plus a name fallback to catch
                // named/modded variants like Dawnguard's Forgemaster.
                bool isCenturion = (race->GetFormID() == 0x000241B4);
                if (!isCenturion) {
                    const char* rname = race->GetName();
                    if (rname && rname[0]) {
                        std::string lower(rname);
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (lower.find("centurion") != std::string::npos) isCenturion = true;
                    }
                }
                if (!isDragon && !isCenturion) continue;

                const auto& ap = actor->GetPosition();
                const float dx = ap.x - pp.x;
                const float dy = ap.y - pp.y;
                const float dz = ap.z - pp.z;
                const float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 > maxRangeSq) continue;
                const float d = std::sqrt(d2);

                // Walk every source type for this dragon and pick the
                // loudest one. Each source carries its own kind so the
                // decay model matches the action — bite/fireball snap
                // off, landings rumble. considerSource also gates each
                // source on its own range so a long-range source (e.g.
                // landing) can fire while a short-range one (e.g. bite)
                // is filtered out for the same dragon.
                float           bestIntensity = 0.0f;
                DragonShakeKind bestKind      = DragonShakeKind::None;
                float           bestSpeed     = 1.0f;
                float           bestRange     = 3000.0f;
                SettingsManager::CinematicShakeChar bestChar{};
                // View selectors for the split halves.
                auto A = [&](float tp, float fp) { return a_fp ? fp : tp; };
                auto C = [&](const SettingsManager::CinematicShakeChar& tp,
                             const SettingsManager::CinematicShakeChar& fp)
                             -> const SettingsManager::CinematicShakeChar& {
                    return a_fp ? fp : tp;
                };
                auto considerSource = [&](float baseI, DragonShakeKind k,
                                          bool /*enabled*/, float amp,
                                          float speed, float range,
                                          const SettingsManager::CinematicShakeChar& chr) {
                    if (amp <= 0.0f) return;   // amp 0 = source off (no enable toggle)
                    if (d > range) return;
                    const float adjusted = baseI * std::max(0.0f, amp);
                    if (adjusted > bestIntensity) {
                        bestIntensity = adjusted;
                        bestKind      = k;
                        bestSpeed     = std::max(0.05f, speed);
                        bestRange     = std::max(50.0f, range);
                        bestChar      = chr;
                    }
                };

                // Cast-state probe (any caster slot in kCasting with a real spell).
                for (auto cs : { Src::kLeftHand, Src::kRightHand, Src::kOther, Src::kInstant }) {
                    auto* caster = actor->GetMagicCaster(cs);
                    if (!caster || caster->state.get() != CState::kCasting) continue;
                    auto* spell = caster->currentSpell ? caster->currentSpell->As<RE::SpellItem>() : nullptr;
                    if (!spell) continue;
                    const auto ct = spell->GetCastingType();
                    if (isDragon) {
                        if (ct == CType::kConcentration) {
                            considerSource(1.0f, DragonShakeKind::Sustained,
                                           s.dragonShakeBreathEnabled, A(s.dragonShakeBreathAmp, s.dragonShakeBreathAmpFp),
                                           A(s.dragonShakeBreathSpeed, s.dragonShakeBreathSpeedFp), A(s.dragonShakeBreathRange, s.dragonShakeBreathRangeFp), C(s.dragonShakeBreathChar, s.dragonShakeBreathCharFp));
                        } else if (ct == CType::kFireAndForget) {
                            considerSource(1.3f, DragonShakeKind::Sharp,
                                           s.dragonShakeProjectileEnabled, A(s.dragonShakeProjectileAmp, s.dragonShakeProjectileAmpFp),
                                           A(s.dragonShakeProjectileSpeed, s.dragonShakeProjectileSpeedFp), A(s.dragonShakeProjectileRange, s.dragonShakeProjectileRangeFp), C(s.dragonShakeProjectileChar, s.dragonShakeProjectileCharFp));
                        }
                    } else if (isCenturion) {
                        // Steam attack — Centurion's chest-vent burst is a
                        // Concentration cast. Sharp would feel too snappy
                        // for a sustained vent, so we use Sustained.
                        if (ct == CType::kConcentration) {
                            considerSource(1.4f, DragonShakeKind::Sustained,
                                           s.centurionShakeSteamEnabled, A(s.centurionShakeSteamAmp, s.centurionShakeSteamAmpFp),
                                           A(s.centurionShakeSteamSpeed, s.centurionShakeSteamSpeedFp), A(s.centurionShakeSteamRange, s.centurionShakeSteamRangeFp), C(s.centurionShakeSteamChar, s.centurionShakeSteamCharFp));
                        }
                    }
                }

                // Centurion-only triggers — walking and melee attack.
                if (isCenturion) {
                    if (auto* asState = actor->AsActorState()) {
                        const bool moving = asState->IsWalking() ||
                                            asState->actorState1.running != 0 ||
                                            asState->IsSprinting();
                        if (moving) {
                            considerSource(0.9f, DragonShakeKind::Heavy,
                                           s.centurionShakeWalkEnabled, A(s.centurionShakeWalkAmp, s.centurionShakeWalkAmpFp),
                                           A(s.centurionShakeWalkSpeed, s.centurionShakeWalkSpeedFp), A(s.centurionShakeWalkRange, s.centurionShakeWalkRangeFp), C(s.centurionShakeWalkChar, s.centurionShakeWalkCharFp));
                        }
                        const auto m = asState->actorState1.meleeAttackState;
                        if (m == AState::kSwing || m == AState::kHit ||
                            m == AState::kFollowThrough)
                        {
                            considerSource(2.0f, DragonShakeKind::Heavy,
                                           s.centurionShakeMeleeEnabled, A(s.centurionShakeMeleeAmp, s.centurionShakeMeleeAmpFp),
                                           A(s.centurionShakeMeleeSpeed, s.centurionShakeMeleeSpeedFp), A(s.centurionShakeMeleeRange, s.centurionShakeMeleeRangeFp), C(s.centurionShakeMeleeChar, s.centurionShakeMeleeCharFp));
                        }
                    }
                }

                if (isDragon) if (auto* asState = actor->AsActorState()) {
                    const auto fs    = asState->actorState1.flyState;
                    const auto m     = asState->actorState1.meleeAttackState;
                    const auto nowTp = CameraEffectClock::Now();
                    auto& mem        = sDragonMemory[actor->GetFormID()];
                    mem.lastSeen     = nowTp;

                    auto isAirborneState = [](FState s) {
                        return s == FState::kTakeOff || s == FState::kCruising ||
                               s == FState::kHovering || s == FState::kLanding;
                    };

                    // Fly-state edges. Take-off = entry into kTakeOff
                    // plus a 200ms delay so the impulse fires during
                    // the wing-flap rather than the wind-up. Touchdown
                    // = any airborne→grounded transition (covers
                    // ground landings AND perch landings whether the
                    // engine routes through kLanding or jumps directly
                    // into kPerching from kHovering/kCruising).
                    // One debounced landing fire. The fly-state edge and the
                    // z-velocity fallback can both report one touchdown (esp. on
                    // perches); the cooldown lets the first win so the shake
                    // doesn't double up.
                    auto fireLanding = [&mem](std::chrono::steady_clock::time_point now) -> bool {
                        if (now < mem.landingCooldownEnd) return false;
                        mem.landingImpulseEnd  = now + std::chrono::milliseconds(220);
                        // 2.5s cooldown: on perch landings (word walls, rocks) the
                        // engine's fly-state edge lags the real (z-velocity)
                        // touchdown by ~1.3s — long enough that a 1s cooldown let
                        // it fire a delayed second shake. 2.5s covers that lag so
                        // whichever signal lands first wins and the late one drops.
                        mem.landingCooldownEnd = now + std::chrono::milliseconds(2500);
                        return true;
                    };

                    if (mem.lastFlyState != fs) {
                        const bool wasAirborne = isAirborneState(mem.lastFlyState);
                        const bool nowAirborne = isAirborneState(fs);
                        if (wasAirborne && !nowAirborne) {
                            // Fly-state airborne→grounded is the authoritative
                            // touchdown — fire it and cancel any pending
                            // rest candidate so they don't double up.
                            fireLanding(nowTp);
                            mem.touchdownPending = false;
                        }
                        if (fs == FState::kTakeOff && mem.lastFlyState != FState::kTakeOff) {
                            mem.takeoffImpulseStart = nowTp + std::chrono::milliseconds(90);
                            mem.takeoffImpulseEnd   = nowTp + std::chrono::milliseconds(220);
                        }
                        mem.lastFlyState = fs;
                    }

                    // Touchdown detection — SURFACE-INDEPENDENT. Dragons land on
                    // anything (ground, rocks, word walls, towers, modded
                    // perches) with wildly different descent speeds, so any
                    // velocity threshold is fragile. Instead use the one thing
                    // every landing shares: while the engine reports kLanding,
                    // the dragon comes to REST and holds there. No speed
                    // threshold. Mid-air decelerations during cruise/hover are
                    // excluded by the kLanding gate; a flare-pause mid-descent
                    // re-arms (it moves again) so only the FINAL rest confirms.
                    // The fly-state airborne->grounded edge above backstops any
                    // landing that skips kLanding; the cooldown dedupes.
                    const float curZ = actor->GetPosition().z;
                    float zVel = 0.0f;
                    if (mem.hasLastZ) {
                        const float ftDt = std::chrono::duration<float>(nowTp - mem.lastZTime).count();
                        if (ftDt > 0.001f) zVel = (curZ - mem.lastZ) / ftDt;
                    }
                    mem.lastZ     = curZ;
                    mem.lastZTime = nowTp;
                    mem.hasLastZ  = true;

                    // Drop any pending candidate the moment the dragon is clearly
                    // flying (not in a landing descent) so a later hover can't
                    // confirm a phantom touchdown.
                    if (fs == FState::kTakeOff || fs == FState::kCruising ||
                        fs == FState::kHovering) {
                        mem.touchdownPending = false;
                    }
                    // Arm a rest candidate when essentially stationary vertically
                    // during a landing.
                    if (fs == FState::kLanding && !mem.touchdownPending &&
                        std::abs(zVel) < 10.0f) {
                        mem.touchdownPending  = true;
                        mem.touchdownCandZ    = curZ;
                        mem.touchdownCandTime = nowTp;
                    }
                    // Fire once it has held within 10u for 120ms (= truly at
                    // rest, not still gliding/settling); re-arm at the new height
                    // if it moves, so the shake lands on the real touchdown.
                    if (mem.touchdownPending) {
                        if (std::abs(curZ - mem.touchdownCandZ) > 10.0f) {
                            mem.touchdownCandZ    = curZ;
                            mem.touchdownCandTime = nowTp;
                        } else if (std::chrono::duration<float>(
                                       nowTp - mem.touchdownCandTime).count() >= 0.12f) {
                            fireLanding(nowTp);
                            mem.touchdownPending = false;
                        }
                    }

                    // Bite shape: a brief heavy peak on the rising edge into
                    // kHit — the CONTACT frame. We previously fired on the
                    // first active state (kSwing), which is the wind-up, so the
                    // shake landed too early; kHit lines it up with the actual
                    // impact. Snaps off via the Sharp decay tail.
                    if (m == AState::kHit && mem.lastAttackState != AState::kHit) {
                        mem.biteImpulseEnd = nowTp + std::chrono::milliseconds(60);
                        // Classify the attack from AIProcess::high->attackData->event.
                        // The animation event names dragons use embed
                        // "Tail" for tail swipes and "Wing" for wing
                        // buffets. Anything else is treated as Bite.
                        mem.attackKind = 0;
                        if (auto* proc = actor->GetActorRuntimeData().currentProcess) {
                            if (auto* hi = proc->high) {
                                auto& ad = hi->attackData;
                                if (ad) {
                                    const auto& ev = ad->event;
                                    auto contains = [&](const char* key) {
                                        const char* p = ev.c_str();
                                        if (!p) return false;
                                        for (; *p; ++p) {
                                            const char* k = key;
                                            const char* q = p;
                                            while (*k && *q && (std::tolower(static_cast<unsigned char>(*q)) == std::tolower(static_cast<unsigned char>(*k)))) { ++k; ++q; }
                                            if (!*k) return true;
                                        }
                                        return false;
                                    };
                                    if      (contains("tail")) mem.attackKind = 1;
                                    else if (contains("wing")) mem.attackKind = 2;
                                    // Wing Buffet reportedly never fires. The
                                    // match is a guess at what dragons put in
                                    // attackData->event, and nothing has ever
                                    // printed the real string — so print it,
                                    // once per distinct value, and let the log
                                    // say what the substring should be.
                                    {
                                        static std::unordered_set<std::string> sSeen;
                                        const char* raw = ev.c_str();
                                        if (raw && *raw && sSeen.size() < 24 &&
                                            sSeen.insert(raw).second) {
                                            spdlog::debug("[DRAGON] attack event '{}' -> kind {} "
                                                         "(0=bite 1=tail 2=wing)",
                                                         raw, mem.attackKind);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // Don't cancel the impulse window on the falling
                    // edge — letting it run through then decay via the
                    // (slightly slower) Sharp tau gives the gentle
                    // fade-out the bite needs to feel like the impact
                    // is settling rather than vanishing.
                    mem.lastAttackState = m;

                    // Apply active impulses.
                    if (nowTp < mem.landingImpulseEnd) {
                        considerSource(2.2f, DragonShakeKind::Heavy,
                                       s.dragonShakeLandingEnabled, A(s.dragonShakeLandingAmp, s.dragonShakeLandingAmpFp),
                                       A(s.dragonShakeLandingSpeed, s.dragonShakeLandingSpeedFp), A(s.dragonShakeLandingRange, s.dragonShakeLandingRangeFp), C(s.dragonShakeLandingChar, s.dragonShakeLandingCharFp));
                    }
                    if (nowTp >= mem.takeoffImpulseStart && nowTp < mem.takeoffImpulseEnd) {
                        considerSource(1.0f, DragonShakeKind::Heavy,
                                       s.dragonShakeTakeoffEnabled, A(s.dragonShakeTakeoffAmp, s.dragonShakeTakeoffAmpFp),
                                       A(s.dragonShakeTakeoffSpeed, s.dragonShakeTakeoffSpeedFp), A(s.dragonShakeTakeoffRange, s.dragonShakeTakeoffRangeFp), C(s.dragonShakeTakeoffChar, s.dragonShakeTakeoffCharFp));
                    }
                    if (nowTp < mem.biteImpulseEnd) {
                        if (mem.attackKind == 1) {
                            considerSource(1.8f, DragonShakeKind::Sharp,
                                           s.dragonShakeTailEnabled, A(s.dragonShakeTailAmp, s.dragonShakeTailAmpFp),
                                           A(s.dragonShakeTailSpeed, s.dragonShakeTailSpeedFp), A(s.dragonShakeTailRange, s.dragonShakeTailRangeFp), C(s.dragonShakeTailChar, s.dragonShakeTailCharFp));
                        } else if (mem.attackKind == 2) {
                            considerSource(1.8f, DragonShakeKind::Sharp,
                                           s.dragonShakeWingEnabled, A(s.dragonShakeWingAmp, s.dragonShakeWingAmpFp),
                                           A(s.dragonShakeWingSpeed, s.dragonShakeWingSpeedFp), A(s.dragonShakeWingRange, s.dragonShakeWingRangeFp), C(s.dragonShakeWingChar, s.dragonShakeWingCharFp));
                        } else {
                            considerSource(1.8f, DragonShakeKind::Sharp,
                                           s.dragonShakeBiteEnabled, A(s.dragonShakeBiteAmp, s.dragonShakeBiteAmpFp),
                                           A(s.dragonShakeBiteSpeed, s.dragonShakeBiteSpeedFp), A(s.dragonShakeBiteRange, s.dragonShakeBiteRangeFp), C(s.dragonShakeBiteChar, s.dragonShakeBiteCharFp));
                        }
                    }
                    // Anticipation tremor while the dragon is winding
                    // up an attack — runs through kDraw frames only so
                    // it doesn't bleed into the impact peak. Routes to
                    // the same source the impact will use.
                    else if (m == AState::kDraw) {
                        if (mem.attackKind == 1) {
                            considerSource(0.25f, DragonShakeKind::Sharp,
                                           s.dragonShakeTailEnabled, A(s.dragonShakeTailAmp, s.dragonShakeTailAmpFp),
                                           A(s.dragonShakeTailSpeed, s.dragonShakeTailSpeedFp), A(s.dragonShakeTailRange, s.dragonShakeTailRangeFp), C(s.dragonShakeTailChar, s.dragonShakeTailCharFp));
                        } else if (mem.attackKind == 2) {
                            considerSource(0.25f, DragonShakeKind::Sharp,
                                           s.dragonShakeWingEnabled, A(s.dragonShakeWingAmp, s.dragonShakeWingAmpFp),
                                           A(s.dragonShakeWingSpeed, s.dragonShakeWingSpeedFp), A(s.dragonShakeWingRange, s.dragonShakeWingRangeFp), C(s.dragonShakeWingChar, s.dragonShakeWingCharFp));
                        } else {
                            considerSource(0.25f, DragonShakeKind::Sharp,
                                           s.dragonShakeBiteEnabled, A(s.dragonShakeBiteAmp, s.dragonShakeBiteAmpFp),
                                           A(s.dragonShakeBiteSpeed, s.dragonShakeBiteSpeedFp), A(s.dragonShakeBiteRange, s.dragonShakeBiteRangeFp), C(s.dragonShakeBiteChar, s.dragonShakeBiteCharFp));
                        }
                    }
                }

                if (bestIntensity <= 0.0f) continue;

                if (result.distance < 0.0f || d < result.distance) {
                    result.distance     = d;
                    result.intensityMul = bestIntensity;
                    result.kind         = bestKind;
                    result.speed        = bestSpeed;
                    result.range        = bestRange;
                    result.chr          = bestChar;
                }
            }

            return result;
        }
    }

    CameraNoiseController::CameraNoiseController()
        : perlinTransX(1001)
        , perlinTransY(1002)
        , perlinTransZ(1003)
        , perlinRotX(2001)
        , perlinRotY(2002)
        , perlinRotZ(2003)
    {}

    CameraNoiseController& CameraNoiseController::GetSingleton()
    {
        static CameraNoiseController instance;
        return instance;
    }

    namespace
    {
        // This frame's total applied 3p camera offset (rot as a composed
        // matrix, trans as a summed world offset) — see the header's
        // GetAppliedCameraOffset3p. Reset each 3p frame before anything
        // applies; every site that mutates cameraRoot->local accumulates.
        // The engine aims (and launches projectiles) from the CLEAN basis,
        // so aim/trace consumers subtract this to match the actual flight.
        RE::NiMatrix3 sApplied3pRot{};
        RE::NiPoint3  sApplied3pTrans{};
        bool          sApplied3pAny = false;

        void ResetApplied3p()
        {
            sApplied3pRot.entry[0][0] = 1.0f;
            sApplied3pRot.entry[0][1] = 0.0f;
            sApplied3pRot.entry[0][2] = 0.0f;
            sApplied3pRot.entry[1][0] = 0.0f;
            sApplied3pRot.entry[1][1] = 1.0f;
            sApplied3pRot.entry[1][2] = 0.0f;
            sApplied3pRot.entry[2][0] = 0.0f;
            sApplied3pRot.entry[2][1] = 0.0f;
            sApplied3pRot.entry[2][2] = 1.0f;
            sApplied3pTrans = RE::NiPoint3{};
            sApplied3pAny   = false;
        }

        // Build a rotation matrix from axis-angle (same formula as doodlum).
        RE::NiMatrix3 MatrixFromAxisAngle(const RE::NiPoint3& axis, float theta)
        {
            const RE::NiPoint3& a = axis;
            const float c = std::cos(theta);
            const float s = std::sin(theta);
            const float ic = 1.0f - c;
            RE::NiMatrix3 m;
            m.entry[0][0] = c + a.x * a.x * ic;
            m.entry[0][1] = a.x * a.y * ic - a.z * s;
            m.entry[0][2] = a.x * a.z * ic + a.y * s;
            m.entry[1][0] = a.y * a.x * ic + a.z * s;
            m.entry[1][1] = c + a.y * a.y * ic;
            m.entry[1][2] = a.y * a.z * ic - a.x * s;
            m.entry[2][0] = a.z * a.x * ic - a.y * s;
            m.entry[2][1] = a.z * a.y * ic + a.x * s;
            m.entry[2][2] = c + a.z * a.z * ic;
            return m;
        }

        RE::NiCamera* FindNiCamera(RE::TESCamera* a_camera)
        {
            if (!a_camera || !a_camera->cameraRoot) return nullptr;
            for (auto& child : a_camera->cameraRoot->GetChildren()) {
                if (auto* ni = child.get()) {
                    if (auto* nc = skyrim_cast<RE::NiCamera*>(ni)) return nc;
                }
            }
            return nullptr;
        }

        // NiCamera view/projection rebuild (doodlum's RelocationID).
        void UpdateWorldToScreenMatrix(RE::NiCamera* a_niCamera)
        {
            using func_t = void(*)(RE::NiCamera*);
            REL::Relocation<func_t> func{ REL::RelocationID(69271, 70641) };
            func(a_niCamera);
        }

        // Trampoline target inside PlayerCamera::Update: the call to
        // TESCamera::Update at +0x1A6 (same site doodlum uses).
        struct TESCameraUpdateHook
        {
            static void thunk(RE::TESCamera* a_camera)
            {
                func(a_camera);
                // Shield-sprint injection cancel runs BEFORE the noise layers
                // so they compose on the corrected pose. Same stage as every
                // other 3p camera write in this mod — post TESCamera::Update,
                // pre scene-graph propagation — which renders world and sky
                // together (correcting the NiCamera's world transform after
                // propagation made the sky shake against the geometry).
                // (EnforceShieldSprintCamera stays disabled — superseded. The
                // source was found: the StartAnimatedCameraDelta anim event.
                // The fix is the [ANIMCAM] bone hold, input-side; this call
                // re-asserts the held pose after TESCamera::Update for any
                // consumer later in PlayerCamera::Update.)
                HookManager::ApplyAnimCamBoneHold(a_camera);
                CameraNoiseController::GetSingleton().OnCameraUpdate(a_camera);
                TweenCameraTrace::Sample("noise-after");
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void CameraNoiseController::InstallHook()
    {
        auto& trampoline = SKSE::GetTrampoline();
        const auto callSite = RuntimeHooks::Get().cameraUpdate;
        RuntimeHooks::RequireCall(callSite);
        TESCameraUpdateHook::func = trampoline.write_call<5>(
            callSite, &TESCameraUpdateHook::thunk);
        spdlog::info("CameraNoise: hooked TESCamera::Update call at 0x{:x}",
                     callSite);
    }

    namespace
    {
        // Query live caster state directly — no laundering through
        // StateResolver's 0.35s-1s linger.
        //
        // "Charging" (fire-and-forget / ritual wind-up, button held):
        // caster->state is non-kNone AND non-kCasting AND the spell is a
        // fire-and-forget or ritual. The exact sub-state varies (kUnk01,
        // kUnk02, kReady, kUnk04, kCharging — Skyrim cycles through
        // several), so we whitelist "anything that's not None and not
        // post-release kCasting".
        bool IsCasterCharging(RE::PlayerCharacter* p)
        {
            if (!p) return false;
            using Src   = RE::MagicSystem::CastingSource;
            using CType = RE::MagicSystem::CastingType;
            using CState = RE::MagicCaster::State;
            for (auto src : { Src::kLeftHand, Src::kRightHand }) {
                auto* caster = p->GetMagicCaster(src);
                if (!caster) continue;
                const auto st = caster->state.get();
                if (st == CState::kNone || st == CState::kCasting) continue;
                auto* spell = caster->currentSpell ? caster->currentSpell->As<RE::SpellItem>() : nullptr;
                if (!spell) continue;
                // kFireAndForget covers rituals too in Skyrim's enum —
                // they're distinguished elsewhere, not via CastingType.
                if (spell->GetCastingType() == CType::kFireAndForget) return true;
            }
            return false;
        }

        bool IsCasterStreaming(RE::PlayerCharacter* p)
        {
            if (!p) return false;
            using Src   = RE::MagicSystem::CastingSource;
            using CType = RE::MagicSystem::CastingType;
            using CState = RE::MagicCaster::State;
            for (auto src : { Src::kLeftHand, Src::kRightHand }) {
                auto* caster = p->GetMagicCaster(src);
                if (!caster) continue;
                if (caster->state.get() != CState::kCasting) continue;
                auto* spell = caster->currentSpell ? caster->currentSpell->As<RE::SpellItem>() : nullptr;
                if (!spell) continue;
                if (spell->GetCastingType() == CType::kConcentration) return true;
            }
            return false;
        }

        // ==== Jumping noise arc (Cinematic Effects) ====
        // Hardcoded three-phase airborne arc, one Intensity slider. The arc:
        //   1. LAUNCH — a hard burst at the jump (peak 2.6 at slider 1,
        //      quadratic decay over 0.30s) — "really jumping hard".
        //   2. APEX — the burst has decayed by the top of a typical jump, so
        //      it weens down to a quiet airborne base.
        //   3. FALL — a wind-rush that builds with AIRSPEED: amplitude grows
        //      toward 3.2 and the oscillation rate rises with it, so the
        //      longer/faster you fall, the harder and busier the shake — "air
        //      rushing into my face". Plain falls (no jump) skip the burst and
        //      go straight to this.
        //
        // Polled ONCE per frame, BEFORE the POV split, and consumed by both
        // views — 3p swaps the ambient source to a jump profile, 1p folds the
        // envelope into its own amp. Running the poll before the split is not
        // cosmetic: the launch trigger and the airborne latch are EDGE
        // detectors over per-frame statics (position delta, character-
        // controller state, graph-event counters). While this lived inside the
        // 3p branch it never ticked in first person, so the arc was not merely
        // unapplied there — it was never computed, and the first jump after a
        // POV switch also read a stale position delta.
        struct JumpArcState
        {
            float env       = 0.0f;   // amplitude envelope (burst + base + wind)
            float speedMul  = 1.0f;   // Perlin rate multiplier
            bool  inAir     = false;
            bool  windOpen  = false;  // wind ramp opened at some point this airtime
            bool  snapXfade = false;  // takeoff frame — 3p crossfade snap
            // Diagnostic only ([FPFALL]). env is three terms multiplied
            // together and the log could only see the product, so "falling
            // doesn't kick in until the very last second" could not be
            // pinned on the airspeed ramp or on the ground-clearance gate.
            float dbgSpeed  = 0.0f;
            float dbgRushF  = 0.0f;
            float dbgWindF  = 0.0f;
            float dbgClear  = 0.0f;   // clearGate, 0 = skim .. 1 = free air
        };

        // Set by the 3p consumer when a shout owned the ambient source for part
        // of an airtime; read by the [JUMPARC] diag on landing.
        bool sJumpArcShoutBlocked = false;

        // Landing thud (Cinematic Effects → Jumping → Repulse). Published by
        // PollJumpArc at the CONFIRMED touchdown, consumed by whichever view's
        // kick pool renders that frame (exactly one of the two branches runs).
        // Scale carries how hard the landing was, from the airtime's peak
        // descent speed — a hop taps, a plunge slams.
        static bool  sJumpLandKickPending = false;
        static float sJumpLandKickScale   = 1.0f;
        // True when the pending thud ends a PARAGLIDE airtime — that kick
        // reads the glide's own Fade Duration instead of the stock tempo.
        static bool  sJumpLandKickGlide   = false;

        // ===================================================================
        // Head Bobbing (Cinematic Effects)
        //
        // Deliberately NOT a noise layer. Every other source in this file is
        // Perlin — a texture with no beat to it — and a walk cycle is the
        // opposite of that: it is periodic, it is locked to the feet, and its
        // whole appeal is that you can feel the tempo change when you break
        // into a run. So this is a plain phase oscillator whose RATE comes
        // from the player's measured ground speed, which gets walk / run /
        // sprint / sneak cadences for free (and, on a mount, the horse's gait)
        // without asking the animation graph anything — the compatibility-
        // first route, since no animation set has to emit a footfall tag for
        // this to work.
        //
        // Shape: vertical dips at 2x the stride rate (one dip per FOOTFALL);
        // sideways sway and roll run at 1x, so consecutive steps lean opposite
        // ways. Both are pure sinusoids of one continuously-advancing phase,
        // so a speed change re-tempos the cycle without ever stepping the
        // rendered offset.
        //
        // Polled once per frame BEFORE the POV split, like the jump arc and
        // for the same reason: the phase is a per-frame integrator and the
        // speed comes from a position delta, so a poll that only ticked in one
        // view would restart mid-stride on every POV switch.
        struct HeadBobState
        {
            float vert   = 0.0f;   // -1..1, x Intensity downstream
            float side   = 0.0f;   // -1..1
            float roll   = 0.0f;   // -1..1
            float yaw    = 0.0f;   // -1..1 (first person only)
            float weight = 0.0f;   // 0..1 gait engagement (smoothed)
            float gait   = 1.0f;   // amplitude scale: <1 a walk, >1 a sprint
        };

        // ---- Footfall source -------------------------------------------------
        //
        // The bob is driven by the player's ACTUAL footsteps, not by a cadence
        // guessed from movement speed. Guessing was wrong in exactly the way it
        // was always going to be: stride length varies per animation set, per
        // gait and per armour weight, so a fixed units-per-stride constant
        // produced four dips per real step.
        //
        // BGSFootstepEvent is the engine event that drives footstep SOUNDS, so
        // it fires for every animation set, vanilla or not, and it fires on the
        // frame the foot actually plants. That makes it both the correct signal
        // and the compatibility-first one — nothing has to annotate anything for
        // this to work. Its `tag` names the foot ("Left"/"Right" appear in the
        // vanilla tags), which we use only to alternate the sway; the timing
        // comes from the event itself either way.
        struct FootstepSink : public RE::BSTEventSink<RE::BGSFootstepEvent>
        {
            static FootstepSink& Get() { static FootstepSink s; return s; }

            // Written on the event thread, read on the camera path. Plain
            // atomics: a missed or doubled read costs at most one step's
            // phase, and locking here would be absurd for two scalars.
            static inline std::atomic<std::uint32_t> sCounter{ 0 };
            static inline std::atomic<bool>          sRightFoot{ false };
            // Per-tag tallies for the beast-form cadence probe (2026-09-05).
            static inline std::atomic<std::uint32_t> sFrontSteps{ 0 };
            static inline std::atomic<std::uint32_t> sBackSteps{ 0 };
            static inline std::atomic<std::uint32_t> sSideSteps{ 0 };   // Left / Right / untagged
            // True while the most recent footfall was a quadruped (Front /
            // Back) plant - the werewolf is galloping, not walking upright.
            static inline std::atomic<bool>          sLastQuad{ false };

            RE::BSEventNotifyControl ProcessEvent(
                const RE::BGSFootstepEvent* a_event,
                RE::BSTEventSource<RE::BGSFootstepEvent>*) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) return RE::BSEventNotifyControl::kContinue;
                if (a_event->actor.get().get() != player)
                    return RE::BSEventNotifyControl::kContinue;
                // Tag naming is not guaranteed across animation sets, so treat
                // it as a hint only: when it says nothing useful we simply
                // alternate, which is what a gait does anyway.
                const std::string_view tag(a_event->tag.c_str() ? a_event->tag.c_str() : "");
                bool right;
                if (tag.find("Right") != std::string_view::npos ||
                    tag.find("right") != std::string_view::npos) {
                    right = true;
                } else if (tag.find("Left") != std::string_view::npos ||
                           tag.find("left") != std::string_view::npos) {
                    right = false;
                } else {
                    right = !sRightFoot.load(std::memory_order_relaxed);
                }
                // Quadruped gallop (werewolf sprint, 2026-09-05): the beast
                // graph plants FootFront and FootBack per bound, and counting
                // both made the bob dip twice per bound - fast enough that the
                // output low-pass flattened it to nothing ("doesn't feel like
                // it's doing anything"). A bound is one stride: count the
                // front pair, drop the back pair, so the lope reads at the
                // gallop's own tempo. Per-tag tallies feed the cadence probe.
                const bool front = tag.find("Front") != std::string_view::npos ||
                                   tag.find("front") != std::string_view::npos;
                const bool back  = tag.find("Back")  != std::string_view::npos ||
                                   tag.find("back")  != std::string_view::npos;
                if (front)      sFrontSteps.fetch_add(1, std::memory_order_relaxed);
                else if (back)  sBackSteps.fetch_add(1, std::memory_order_relaxed);
                else            sSideSteps.fetch_add(1, std::memory_order_relaxed);
                sLastQuad.store(front || back, std::memory_order_relaxed);
                if (back && StateResolver::GetSingleton().IsWerewolf())
                    return RE::BSEventNotifyControl::kContinue;
                sRightFoot.store(right, std::memory_order_relaxed);
                sCounter.fetch_add(1, std::memory_order_release);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void EnsureFootstepSink()
        {
            static bool sRegistered = false;
            if (sRegistered) return;
            auto* mgr = RE::BGSFootstepManager::GetSingleton();
            if (!mgr) return;
            mgr->AddEventSink(&FootstepSink::Get());
            sRegistered = true;
            spdlog::debug("[HeadBob] footstep sink registered");
        }

        // Head bob, built on the shape the genre settled on decades ago.
        //
        // Researched rather than invented, after two homegrown attempts felt
        // wrong. The three ideas that matter, and where they come from:
        //
        //  1. FIGURE OF EIGHT (Quake / Source). Lateral runs at the STRIDE rate
        //     and vertical at twice it, so the eye traces a lemniscate: one dip
        //     per foot, one sway per left+right pair. Vertical = sin(2*phi)
        //     against lateral = sin(phi) is the classic pairing — it puts the
        //     dip at the moment the sway crosses centre, which is what a real
        //     weight transfer does.
        //
        //  2. AN ASYMMETRIC CYCLE BY TIME REMAP, not by adding harmonics
        //     (Quake's cl_bobup). The step's progress is remapped before the
        //     sine so the head falls onto the planted foot faster than it rises
        //     off it. Doing it as a remap of a pure sine keeps the curve smooth
        //     everywhere; adding a second harmonic (the previous attempt) buys
        //     the same asymmetry but with a lumpier crest.
        //
        //  3. AMPLITUDE FROM SPEED, plus a small constant offset while moving
        //     (Quake's bob*0.3 + bob*0.7*sin). Walking really does lower the
        //     head slightly; the offset is what stops the bob reading as a
        //     thing bolted onto a stationary camera.
        //
        // And the thing that is OURS: the CADENCE comes from real footfalls
        // (BGSFootstepEvent), not from a guessed units-per-stride constant.
        // That constant is what produced four dips per step — stride length
        // varies per animation set, gait and armour weight, so no fixed number
        // can be right. The events set the FREQUENCY only; the phase free-runs.
        // Trying to phase-LOCK to them (the previous attempt) is what made it
        // jerky: steps never arrive exactly a period apart, so every correction
        // was felt. Frequency is perceptible, absolute phase is not.
        HeadBobState PollHeadBob(RE::PlayerCharacter* a_player, float a_dt)
        {
            const auto& settings = SettingsManager::GetSingleton();
            HeadBobState out;

            const bool wanted = settings.headBobIntensity   > 0.0001f ||
                                settings.headBobIntensityFp > 0.0001f;
            if (wanted) EnsureFootstepSink();

            static float sWeight  = 0.0f;
            static float sSpeedSm = 0.0f;
            static RE::NiPoint3 sPrevPos{};
            static bool  sPosInit = false;
            static std::uint32_t sLastCounter = 0;
            static float sSinceStep  = 0.0f;
            static float sStepPeriod = 0.5f;
            static float sFreeRunT   = 1.0e9f;
            static float sPhase      = 0.0f;   // strides; free-running
            static float sRate       = 0.0f;   // strides/sec, slew-bounded
            // Rendered outputs, low-passed. See the note at the bottom.
            static float sVertSm = 0.0f, sSideSm = 0.0f, sRollSm = 0.0f;

            float speedRaw = 0.0f;
            float dzRaw    = 0.0f;   // per-frame height change (slope read)
            float lenRaw   = 0.0f;
            float mvxRaw   = 0.0f;   // horizontal travel this frame (direction read)
            float mvyRaw   = 0.0f;
            if (a_player) {
                const RE::NiPoint3 pos = a_player->GetPosition();
                if (sPosInit && a_dt > 0.0001f) {
                    const float dx  = pos.x - sPrevPos.x;
                    const float dy  = pos.y - sPrevPos.y;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    constexpr float kTeleportStep = 500.0f;
                    if (len < kTeleportStep) {
                        speedRaw = len / a_dt;
                        dzRaw    = pos.z - sPrevPos.z;
                        lenRaw   = len;
                        mvxRaw   = dx;
                        mvyRaw   = dy;
                    }
                }
                sPrevPos = pos;
                sPosInit = true;
            }
            speedRaw = std::clamp(speedRaw, 0.0f, 2000.0f);
            // 0.10s (was 0.15) — snappier response when a stride starts or
            // stops (user request 2026-08-15).
            sSpeedSm += (speedRaw - sSpeedSm) * (1.0f - std::exp(-a_dt / 0.10f));
            const float speed = sSpeedSm;

            bool grounded = true;
            if (a_player) {
                if (auto* cc = a_player->GetCharController()) {
                    grounded = cc->context.currentState == RE::hkpCharacterStateType::kOnGround;
                }
                if (auto* as = a_player->AsActorState(); as && as->IsSwimming()) grounded = false;
            }
            // A hovering Vampire Lord has no footfalls — the levitate glide
            // was bobbing (user report 2026-08-15).
            if (StateResolver::GetSingleton().IsVampireLordLevitating()) grounded = false;

            // TERRAIN SLOPE (uphill emphasis, user request 2026-08-15).
            // rise/run from the same position deltas the speed uses, smoothed;
            // stairs' single-frame risers are excluded (they're the stair-
            // smoothing artefact, not ground) by capping the per-frame ratio.
            static float sSlopeSm = 0.0f;
            {
                float slopeRaw = 0.0f;
                if (grounded && lenRaw > 0.5f)
                    slopeRaw = std::clamp(dzRaw / lenRaw, -1.2f, 1.2f);
                sSlopeSm += (slopeRaw - sSlopeSm) * (1.0f - std::exp(-a_dt / 0.20f));
            }
            // Climbing reads as heavier steps: up to ~+50% on a steep grade.
            // Descending eases off slightly.
            const float slopeGain = 1.0f + std::clamp(sSlopeSm, 0.0f, 0.6f) * 0.85f
                                         + std::clamp(sSlopeSm, -0.6f, 0.0f) * 0.30f;

            // DIRECTION OF TRAVEL (user request 2026-08-17). A stride does not
            // have one shape: sidestepping is a wider, rockier gait than
            // running forward, and walking backwards is a shorter shuffle that
            // barely drops the head. The bob has always been built from the
            // forward stride alone, so strafing around an enemy and charging
            // one moved the camera identically.
            //
            // Measured from the SAME position deltas the speed and slope reads
            // use, resolved against the body's facing — engine geometry only,
            // no animation tags, so it works with any movement animation set
            // (see [[compatibility-first-design]]). Yaw convention matches the
            // rest of the codebase: forward = (sin z, cos z), so right is
            // (cos z, -sin z).
            //
            // Both fractions are one-poled and DECAY TO ZERO when not moving,
            // so tapping a strafe key morphs the gait over ~0.25s instead of
            // switching shape between frames, and stopping mid-sidestep leaves
            // nothing latched for the next stride.
            static float sStrafeSm = 0.0f;   // 0 = dead ahead, 1 = pure sidestep
            static float sBackSm   = 0.0f;   // 0 = forward,    1 = pure backpedal
            {
                float strafeRaw = 0.0f, backRaw = 0.0f;
                if (a_player && grounded && lenRaw > 0.5f) {
                    const float yaw  = a_player->data.angle.z;
                    const float fwdX = std::sin(yaw), fwdY = std::cos(yaw);
                    const float ux   = mvxRaw / lenRaw, uy = mvyRaw / lenRaw;
                    const float alongF = ux * fwdX + uy * fwdY;          // +1 ahead, -1 behind
                    const float alongR = ux * fwdY + uy * -fwdX;         // +1 to the right
                    strafeRaw = std::clamp(std::abs(alongR), 0.0f, 1.0f);
                    backRaw   = std::clamp(-alongF,          0.0f, 1.0f);
                }
                const float dirA = 1.0f - std::exp(-a_dt / 0.25f);
                sStrafeSm += (strafeRaw - sStrafeSm) * dirA;
                sBackSm   += (backRaw   - sBackSm)   * dirA;
            }

            constexpr float kMoveFloor = 40.0f;
            constexpr float kMoveFull  = 165.0f;
            float target = 0.0f;
            if (wanted && grounded && speed > kMoveFloor) {
                const float t = std::clamp((speed - kMoveFloor) / (kMoveFull - kMoveFloor), 0.0f, 1.0f);
                target = t * t * (3.0f - 2.0f * t);
            }
            // Faster attack (0.22 -> 0.12) so the bob lands with the first
            // strides instead of fading in over a quarter second.
            const float tau = (target > sWeight) ? 0.12f : 0.32f;
            sWeight += (target - sWeight) * (1.0f - std::exp(-a_dt / tau));
            if (sWeight < 0.0005f) sWeight = 0.0f;

            // Cadence from real footfalls. Frequency, plus a SMALL bounded
            // phase nudge per step: the phase used to free-run entirely, so
            // the rendered down-beat could sit anywhere relative to the real
            // footfall. Pulling it a fraction of the way toward the beat on
            // each step keeps the plant felt WHEN the foot lands without the
            // jerk a hard phase snap caused (user request 2026-08-15:
            // "more instantaneous in its detection of a foot hitting the
            // ground").
            // Gallop engagement, SMOOTHED (2026-09-05, "a tiny jerk when I
            // stop sprinting"): the quadruped flag flips the instant the
            // first upright footfall lands, and everything keyed on it - the
            // x2.5 depth and the phase-nudge skip - used to flip with it in
            // one frame. One-pole it over ~0.3 s both ways so the lope hands
            // back to the walk the way the weight already does.
            static float sGallopSm = 0.0f;
            {
                const bool  quadNow = StateResolver::GetSingleton().IsWerewolf() &&
                                      FootstepSink::sLastQuad.load(std::memory_order_relaxed);
                const float gTarget = quadNow ? 1.0f : 0.0f;
                sGallopSm += (gTarget - sGallopSm) * (1.0f - std::exp(-a_dt / 0.30f));
                if (sGallopSm < 0.001f) sGallopSm = 0.0f;
            }
            const std::uint32_t counter = FootstepSink::sCounter.load(std::memory_order_acquire);
            sSinceStep += a_dt;
            if (counter != sLastCounter) {
                sLastCounter = counter;
                if (sSinceStep > 0.05f && sSinceStep < 1.5f) {
                    const float measured = std::clamp(sSinceStep, 0.16f, 1.2f);
                    sStepPeriod += (measured - sStepPeriod) * 0.25f;
                }
                sSinceStep = 0.0f;
                sFreeRunT  = 0.0f;
                // Phase nudge: a footfall is the bottom of the vertical
                // cycle (half-stride progress 0). Wrap the current progress
                // error to [-0.5, 0.5) half-strides and correct 35% of it.
                // ...except in the werewolf gallop (2026-09-05, "a tiny
                // little jerk"): its plants are not half a stride apart, so
                // every nudge was a visible phase step. Frequency only there;
                // the lope free-runs at the measured bound rate.
                if (sGallopSm < 0.05f) {
                    const float stepUNow = (sPhase * 2.0f) - std::floor(sPhase * 2.0f);
                    float err = stepUNow;             // distance past the beat
                    if (err > 0.5f) err -= 1.0f;      // ...or short of the next
                    sPhase -= (err * 0.35f) * 0.5f;   // half-stride -> stride units
                    if (sPhase < 0.0f) sPhase += 4096.0f;
                }
            } else {
                sFreeRunT += a_dt;
            }

            // Beast-form cadence probe (2026-09-05, "make the head bobbing
            // work for werewolf sprinting"): twice a second while a beast
            // form moves, the numbers the bob is built from - speed, the
            // character controller's ground state, the gait weight, the
            // stride rate, the measured step period, and how many Front /
            // Back / Left-Right footfalls landed since the last line.
            {
                const auto& srP = StateResolver::GetSingleton();
                static float sProbeT = 0.0f;
                static std::uint32_t sPf = 0, sPb = 0, sPs = 0;
                static int sProbeLines = 0;
                sProbeT += a_dt;
                if ((srP.IsWerewolf() || srP.IsVampireLord()) && speed > kMoveFloor &&
                    sProbeT > 0.5f && sProbeLines < 80) {
                    sProbeT = 0.0f;
                    ++sProbeLines;
                    const auto f = FootstepSink::sFrontSteps.load(std::memory_order_relaxed);
                    const auto b = FootstepSink::sBackSteps.load(std::memory_order_relaxed);
                    const auto sd = FootstepSink::sSideSteps.load(std::memory_order_relaxed);
                    int ccState = -1;
                    if (a_player) if (auto* cc = a_player->GetCharController())
                        ccState = static_cast<int>(cc->context.currentState);
                    spdlog::info("[HeadBob] beast={} speed={:.0f} grounded={} cc={} weight={:.2f} rate={:.2f}/s period={:.2f}s "
                                 "steps/0.5s front={} back={} side={}",
                                 srP.IsWerewolf() ? "werewolf" : "vampire lord",
                                 speed, grounded, ccState, sWeight, sRate, sStepPeriod,
                                 f - sPf, b - sPb, sd - sPs);
                    sPf = f; sPb = b; sPs = sd;
                }
            }
            if (sWeight <= 0.0f) {
                sVertSm = sSideSm = sRollSm = 0.0f;
                return out;
            }

            const bool  freeRun = sFreeRunT > (sStepPeriod * 2.0f + 0.35f);
            const float rateTarget = freeRun
                ? std::clamp(speed / 210.0f, 0.0f, 2.75f)
                : (0.5f / std::max(0.10f, sStepPeriod));
            constexpr float kMaxRateSlew = 1.5f;   // strides/sec per second
            sRate += std::clamp(rateTarget - sRate, -kMaxRateSlew * a_dt, kMaxRateSlew * a_dt);

            sPhase += sRate * a_dt;
            if (sPhase > 4096.0f) sPhase -= 4096.0f;

            constexpr float kTwoPi = 6.28318531f;

            // FOOTFALL ASYMMETRY BY A SMOOTH PHASE WARP.
            //
            // Quake's cl_bobup remap is piecewise-linear in the step's progress,
            // which means the SLOPE of the rendered curve jumps at the junction
            // — a corner, and corners are exactly what "jerky" means. Quake got
            // away with it at a few units of view bob; a first-person camera
            // rotation does not.
            //
            // Warp the phase with a sinusoid instead: u' = u + k*sin(2*pi*u)/(2*pi)
            // has derivative 1 + k*cos(2*pi*u), which is smooth everywhere and
            // stays positive for k < 1. Same "falls onto the foot quicker than
            // it rises off it" asymmetry, no corner anywhere in the cycle.
            const float stepU = (sPhase * 2.0f) - std::floor(sPhase * 2.0f);
            constexpr float kWarp = 0.30f;
            const float warped = stepU + kWarp * std::sin(kTwoPi * stepU) / kTwoPi;
            const float bobV = -std::cos(kTwoPi * warped);

            // Lateral and roll at the stride rate against the vertical's double
            // rate — the figure of eight. Roll trails the sway slightly.
            const float phi  = kTwoPi * sPhase;
            const float bobH = std::sin(phi);
            const float bobR = std::sin(phi - 0.35f);

            // Beast forms stride heavier (user request 2026-08-15): Vampire
            // Lord slightly above human, werewolf slightly above that.
            // Folded into gait so both the 1p and 3p apply sites (which each
            // multiply by headBob.gait) pick it up identically.
            float formScale = 1.0f;
            float gaitCap   = 1.45f;
            {
                const auto& srForm = StateResolver::GetSingleton();
                // Werewolf 1.30 -> 1.60 (2026-09-05): a bounding gallop drops
                // the whole body per stride, and the user could not feel the
                // old value at all in a sprint.
                if (srForm.IsWerewolf())         formScale = 1.60f;
                else if (srForm.IsVampireLord()) formScale = 1.15f;
                // The 13:12 probe: the werewolf's upright run (speed ~450,
                // side footfalls every 0.4 s) and its gallop (speed ~600,
                // one front + one back per 0.58 s) both sat at the human gait
                // ceiling, so the gallop bobbed SLOWER at the SAME amplitude
                // and read as weaker ("walking in werewolf feels stronger
                // than sprinting"). Beast forms get a higher ceiling so the
                // extra speed counts, and a gallop - the footfalls are the
                // quadruped pair - is a heave, not a step: one more scale on
                // top while those plants are what is landing.
                if (srForm.IsWerewolf() || srForm.IsVampireLord()) gaitCap = 2.0f;
                // 1.5 -> 2.5 (same day): with the per-plant impact shots
                // gone ("deep and heavy bobbing but doesn't blow out the
                // camera with each step"), the depth has to come from the
                // lope itself.
                formScale *= 1.0f + 1.5f * sGallopSm;   // x2.5 at full gallop, eased
            }
            // Gait carries the slope emphasis (uphill = heavier steps).
            const float gait = std::clamp(0.60f + speed / 500.0f, 0.60f, gaitCap) *
                               slopeGain * formScale;
            // The sway/roll share of the cycle. 0.35 is the forward-stride
            // value; a sidestep opens it up to ~0.74 — the figure of eight
            // widens and the roll deepens, which is what a rocking sideways
            // gait actually is. The vertical term below already subtracts
            // 0.45*lean, so this trades drop for rock rather than adding
            // motion on top: strafing is a DIFFERENT stride, not a bigger one.
            // Backpedalling flattens the drop instead — a short shuffle keeps
            // the head far steadier than a stride does.
            //
            // Both are fixed constants, like slopeGain and formScale beside
            // them. This is the shape of a stride, not a preference; no dial.
            const float leanEff   = 0.35f * (1.0f + 1.10f * sStrafeSm);
            const float vertScale = 1.0f - 0.35f * sBackSm;

            // ONE MORE LOW-PASS, on the rendered shape itself.
            //
            // Everything upstream is smooth in theory, but the inputs driving it
            // are not perfectly steady — the measured cadence shifts a little
            // each step, the speed estimate breathes, and the weight ramps. A
            // short filter on the output absorbs all of that at once, and costs
            // only a few milliseconds of lag on a signal whose whole period is
            // half a second. This is the single biggest thing that separates
            // "walking" from "vibrating" in first person.
            // 45ms -> 75ms. Long enough to round off the remaining edge in
            // first person (where the camera IS the head and every degree
            // counts), short enough that the bob still lands with the step —
            // the whole cycle is around half a second.
            const float smA = 1.0f - std::exp(-a_dt / 0.075f);
            sVertSm += (bobV * (1.0f - 0.45f * leanEff) * vertScale - sVertSm) * smA;
            sSideSm += (bobH * leanEff - sSideSm) * smA;
            sRollSm += (bobR * leanEff - sRollSm) * smA;

            out.vert   = sVertSm;
            out.side   = sSideSm;
            out.roll   = sRollSm;
            out.yaw    = sSideSm;
            out.weight = sWeight;
            out.gait   = gait;
            return out;
        }

        // a_jumpAmp / a_fallAmp / a_repulse are the dials of the view we are
        // rendering, handed in because this poll runs once, ahead of the POV
        // split, and the three jump-arc entries carry a third- and a
        // first-person half now (2026-09-06). Everything else here — the
        // triggers, the airtime bookkeeping, the ground-clearance ray — is one
        // arc shared by both views; only its size changes.
        JumpArcState PollJumpArc(RE::PlayerCharacter* player, float dt,
                                 float a_jumpAmp, float a_fallAmp, float a_repulse)
        {
            JumpArcState out;

            static bool          sJumpPrevInAir     = false;
            static bool          sJumpLatched       = false;
            static float         sJumpBurstT        = 1.0e9f;
            static RE::NiPoint3  sJumpPrevPos{};
            static bool          sJumpPosInit       = false;
            static std::uint32_t sJumpLastUpCounter = 0;

            // Split dials (2026-08-15): the launch burst reads Jumping, the
            // airborne bridge + wind-rush read Falling. env leaves this
            // function PRE-SCALED by them, so consumers no longer multiply.
            const float jumpAmp = a_jumpAmp;
            const float fallAmp = a_fallAmp;

            // Velocity from the position delta (world u/s). BOTH components
            // matter: the vertical one drives the launch trigger, the full
            // SPEED drives the wind. Air rush is about how fast you're moving
            // THROUGH the air, not how fast you're dropping — whirlwind-
            // sprinting off a cliff carries you almost horizontally, and a
            // descent-only reading called that standing still. A frame delta
            // past kTeleportStep is a teleport / cell load, not motion.
            float vz = 0.0f, speed = 0.0f;
            if (player) {
                const RE::NiPoint3 pos = player->GetPosition();
                if (sJumpPosInit && dt > 0.0001f) {
                    const RE::NiPoint3 d   = pos - sJumpPrevPos;
                    const float        len = d.Length();
                    constexpr float kTeleportStep = 500.0f;   // units in ONE frame
                    if (len < kTeleportStep) {
                        vz    = d.z / dt;
                        speed = len / dt;
                    }
                }
                sJumpPrevPos = pos;
                sJumpPosInit = true;
            }
            vz    = std::clamp(vz, -3000.0f, 3000.0f);
            speed = std::clamp(speed, 0.0f, 6000.0f);

            // Physics character state — the launch trigger reads kJumping and
            // the airborne signal below corroborates IsInMidair() with it.
            bool ccJumpNow  = false;
            bool ccAirNow   = false;
            bool ccGrounded = false;
            if (player) {
                if (auto* cc = player->GetCharController()) {
                    const auto st = cc->context.currentState;
                    ccJumpNow  = st == RE::hkpCharacterStateType::kJumping;
                    ccAirNow   = ccJumpNow || st == RE::hkpCharacterStateType::kInAir;
                    ccGrounded = st == RE::hkpCharacterStateType::kOnGround;
                }
            }

            // Airborne signal. IsInMidair() alone is NOT dependable across a
            // long fall: clipping a ledge or scraping the cliff face on the
            // way down hands the character proxy a momentary support contact
            // and the flag drops for a frame or two while the player is still
            // plummeting. That reads as "the fall noise cuts out mid-fall" —
            // losing the resolve for even ONE frame reverts the ambient source
            // to the ground profile, which is a signature change, so the
            // crossfade re-arms from zero: ~0.2s of silence per graze, and a
            // scrapey descent keeps it near-silent for the rest of the drop.
            // Fix: OR the flag with the physics state, and require a CONFIRMED
            // touchdown (engine reports on-ground AND the descent has actually
            // stopped) before ending the airtime. Anything else is treated as
            // a graze and the airtime is held through a short grace window.
            const bool rawAir = (player && player->IsInMidair()) || ccAirNow;
            static float sJumpAirGrace = 0.0f;
            static float sJumpStillT   = 0.0f;
            constexpr float kAirGrace = 0.30f;
            bool inAirNow = rawAir;
            if (rawAir) {
                sJumpAirGrace = 0.0f;
                sJumpStillT   = 0.0f;
            } else if (sJumpPrevInAir) {
                sJumpAirGrace += dt;
                // Confirm over a couple of frames rather than by speed: on the
                // touchdown frame vz still carries the whole fall (the position
                // delta spans the impact), so a vz test reads "still falling"
                // and stalls the landing — [JUMPAIR] caught exactly that,
                // ccGrounded=true at vz=-1262 on the JumpLandEnd frame. The
                // engine reporting ground for more than a frame IS the landing;
                // the longer window only covers a total loss of both signals.
                //
                // SECOND CONFIRMATION PATH: SUSTAINED VERTICAL STILLNESS.
                // ccGrounded is the ORDINARY controller's kOnGround, and a
                // beast form does not reliably report it — the Vampire Lord is
                // a flying-capable race whose controller sits outside the
                // plain ground state, so every VL landing fell through to the
                // full 0.30s grace and the jump noise ran roughly a third of a
                // second past the touchdown ("jumping noise lasts too long in
                // vampire lord form"). Vertical stillness is the form-agnostic
                // ground truth: a genuinely landed character stops moving
                // vertically on the very next frame, while a mid-fall graze —
                // the case the grace window exists to protect — is still
                // plummeting, so |vz| stays large and this path can't fire.
                // (The touchdown frame itself always fails the stillness test,
                // per the note above — which is exactly right, it makes the
                // confirmation wait for the frame AFTER impact.)
                if (std::abs(vz) < 40.0f) sJumpStillT += dt;
                else                      sJumpStillT  = 0.0f;
                const bool touchedDown = ccGrounded
                    ? (sJumpAirGrace >= 0.05f)
                    : (sJumpStillT >= 0.06f || sJumpAirGrace >= kAirGrace);
                inAirNow = !touchedDown;
            }
            // TEMPORARY [JUMPAIR] — a HELD airtime that outlasts the landing
            // confirmation window, i.e. the engine claimed ground while the
            // player was still genuinely airborne. Ordinary landings confirm
            // in 0.05s and are not logged. Strip with [JUMPARC] below.
            {
                static bool sHoldPrev = false;
                static int  sHoldLogs = 0;
                const bool  holding   = inAirNow && !rawAir && sJumpAirGrace > 0.10f;
                if (holding && !sHoldPrev && sHoldLogs < 30) {
                    ++sHoldLogs;
                    spdlog::debug("[JUMPAIR] held airtime through a grounded read "
                                 "(midair={} ccAir={} ccGrounded={} vz={:.0f})",
                                 player && player->IsInMidair(), ccAirNow, ccGrounded, vz);
                }
                sHoldPrev = holding;
            }

            // Launch trigger — earliest of three signals, latched per airtime:
            //  1. physics character-controller entering its JUMPING state —
            //     flips on the jump impulse itself, ahead of the animation,
            //  2. the JumpUp graph event (the animation's launch marker),
            //  3. grounded→airborne edge with upward velocity (fallback).
            static bool sJumpPrevCcJump = false;
            static std::uint32_t sJumpLastParkourCounter = 0;
            // SkyParkour moves have a windup before the character actually
            // launches, so firing the burst the instant SkyParkour_Start
            // arrives lands the shake too early. Defer the parkour trigger by
            // a short delay so it hits nearer the leap. Negative = inactive.
            static float sJumpParkourDelay = -1.0f;
            constexpr float kParkourTriggerDelay = 0.20f;
            // A SkyParkour step-up in progress mutes the physics-based triggers
            // (character-controller jump state + grounded→airborne fallback) so
            // the mantle's brief upward motion can't fire the shake. The graph
            // triggers (JumpUp, parkour) are event-driven and unaffected — a
            // real jump mid-window still reads its own event.
            const bool stepSuppressed =
                CameraEffectClock::Now() < sParkourStepSuppressUntil;
            // Triggers latch regardless of the dials: the burst contributes
            // nothing at Jumping 0, but the latch also gates the landing thud
            // (its own entry now), so a hop with only Landing set still thuds.
            bool trigger = false;
            if (!sJumpLatched && ccJumpNow && !sJumpPrevCcJump &&
                !stepSuppressed) {
                trigger = true;
            }
            sJumpPrevCcJump = ccJumpNow;
            if (sJumpLastUpCounter != sJumpUpCounter) {
                sJumpLastUpCounter = sJumpUpCounter;
                if (!sJumpLatched) trigger = true;
            }
            // SkyParkour "big" traversal (Vault / Climb* / Grab / Fail) — the
            // sink bumps this counter; step-ups never do. Arm the deferred
            // timer instead of firing now so the shake lands nearer the leap.
            if (sJumpLastParkourCounter != sParkourJumpCounter) {
                sJumpLastParkourCounter = sParkourJumpCounter;
                if (!sJumpLatched)
                    sJumpParkourDelay = kParkourTriggerDelay;
            }
            // Deferred parkour launch: count the timer down and fire once it
            // elapses (unless a real jump already latched in the meantime).
            if (sJumpParkourDelay >= 0.0f) {
                sJumpParkourDelay -= dt;
                if (sJumpParkourDelay <= 0.0f) {
                    sJumpParkourDelay = -1.0f;
                    if (!sJumpLatched) trigger = true;
                }
            }
            if (!trigger && !sJumpLatched &&
                inAirNow && !sJumpPrevInAir && vz > 120.0f && !stepSuppressed) {
                trigger = true;
            }
            if (trigger) {
                sJumpLatched   = true;
                sJumpBurstT    = 0.0f;
                // Only snap the crossfade when a burst will actually mask the
                // cut — with Jumping at 0 the latch is thud bookkeeping only.
                out.snapXfade  = jumpAmp > 0.001f;
            }
            // A pending landing kick nobody consumed (the view whose branch
            // would render it early-outed — 1p noise disabled, say) must not
            // sit armed until the next POV switch fires it minutes later.
            static float sJumpLandKickAge = 0.0f;
            if (sJumpLandKickPending) {
                sJumpLandKickAge += dt;
                if (sJumpLandKickAge > 0.5f) sJumpLandKickPending = false;
            } else {
                sJumpLandKickAge = 0.0f;
            }
            // Impact bookkeeping for the landing thud: airtime length and the
            // hardest descent reached, tracked every airborne frame so the
            // landing edge below can size the kick from what actually happened.
            static float sJumpKickAirT    = 0.0f;
            static float sJumpKickPeakDsc = 0.0f;
            // Was any of this airtime spent under a deployed paraglider? A
            // glide's descent is deliberately slow, so the descent-scaled
            // thud below bottoms out at its floor and the touchdown reads as
            // nothing — but planting your feet after a flight deserves a
            // LITTLE shake (user request 2026-08-13). Latched across the
            // airtime because the glider is usually stowed an instant before
            // the feet touch.
            static bool sJumpWasParaglide = false;
            if (inAirNow) {
                sJumpKickAirT   += dt;
                sJumpKickPeakDsc = std::max(sJumpKickPeakDsc, -vz);
                if (StateResolver::GetSingleton().IsParagliding())
                    sJumpWasParaglide = true;
            }
            // Landing: cut any burst tail and re-arm for the next jump — the
            // source reverts this frame and crossfades into the ground noise.
            if (!inAirNow && sJumpPrevInAir) {
                // Landing thud. Gated on a real airtime (a latched jump, or
                // half a second of falling) so a curb step can't thud; fires
                // at the CONFIRMED touchdown, which is what makes a graze
                // unable to fire one (a graze never confirms). Scale from the
                // peak descent: a flat jump lands ~380-450 u/s (≈0.6 of the
                // kick), terminal plunges push past 1.
                if (a_repulse > 0.0001f &&
                    (sJumpLatched || sJumpKickAirT > 0.5f)) {
                    sJumpLandKickPending = true;
                    sJumpLandKickScale =
                        std::clamp(sJumpKickPeakDsc / 700.0f, 0.35f, 1.5f);
                    // Glide landings: a firm, felt tap regardless of the
                    // gentle descent — still through the user's Landing
                    // Thud sliders, never louder than a real fall.
                    if (sJumpWasParaglide)
                        sJumpLandKickScale = std::max(sJumpLandKickScale, 0.6f);
                    sJumpLandKickGlide = sJumpWasParaglide;
                }
                // [FPLAND] — "landing doesn't work for first person"
                // (user, 2026-09-06). A landing can end silent three ways and
                // the log could not tell them apart: the dial for THIS view is
                // 0, the airtime never qualified (a curb step), or it armed and
                // the consumer refused it. This line answers the first two at
                // the edge; the consumer answers the third. INFO, capped — two
                // sessions have already run with Verbose off.
                {
                    static int sLandProbe = 0;
                    if (sLandProbe < 30) {
                        ++sLandProbe;
                        spdlog::debug("[FPLAND] touchdown dial={:.2f} latched={} airT={:.2f} "
                                     "peakDsc={:.0f} armed={} ({}/30)",
                                     a_repulse, sJumpLatched ? 1 : 0, sJumpKickAirT,
                                     sJumpKickPeakDsc, sJumpLandKickPending ? 1 : 0,
                                     sLandProbe);
                    }
                }
                sJumpWasParaglide = false;
                sJumpKickAirT    = 0.0f;
                sJumpKickPeakDsc = 0.0f;
                sJumpLatched = false;
                sJumpBurstT  = 1.0e9f;
            }
            // JumpUp with no liftoff (staggered out of the launch): unlatch
            // once the burst has played out so the next real jump triggers.
            if (!inAirNow && sJumpBurstT >= 0.6f) sJumpLatched = false;
            sJumpPrevInAir = inAirNow;

            // Phase envelope.
            float burst = 0.0f;
            if (sJumpBurstT < 1.0e8f) {
                sJumpBurstT += dt;
                constexpr float kBurstDur = 0.30f;   // was 0.45 — read as lingering
                if (sJumpBurstT < kBurstDur) {
                    const float b = 1.0f - sJumpBurstT / kBurstDur;
                    burst = 2.6f * b * b;
                }
            }
            // GROUND CLEARANCE — is this "airborne" moment actually in free
            // air, or a fast skim a few inches off the floor?
            //
            // The wind ramp reads TOTAL airspeed (see below) precisely so a
            // near-horizontal cliff flight roars. Whirlwind Sprint is the case
            // that breaks: the dash is airborne, it clears the wind onset with
            // room to spare, and it happens at ANKLE HEIGHT along flat ground —
            // so a level dash down a corridor pulled a full plunge roar AND
            // latched sWindOverShout, which took the shout's own noise profile
            // away from it for the rest of the dash. Airspeed alone cannot tell
            // the two apart; height above the floor can. Same downward ray the
            // paraglider altitude fade uses, started just ABOVE the feet (the
            // player origin) so a skim at a few units still measures instead of
            // starting underground and reading as a miss.
            //
            // Seeded (not eased) on the first airborne frame so a dash that
            // begins at ground level is quiet from the START, then one-poled so
            // a ledge passing under the flight path can't step the envelope.
            static float sAirClearSm  = 600.0f;
            static bool  sAirClearHad = false;
            float clearGate = 1.0f;
            if (inAirNow) {
                constexpr float kStartUp = 10.0f;    // ray origin above the feet
                constexpr float kRange   = 560.0f;
                float ground = 600.0f;               // a miss is "high"
                if (player) {
                    if (auto* cell = player->GetParentCell()) {
                        if (auto* bhkW = cell->GetbhkWorld()) {
                            if (auto* hkW = bhkW->GetWorld1()) {
                                const float ws = RE::bhkWorld::GetWorldScale();
                                const auto  p  = player->GetPosition();
                                RE::hkpWorldRayCastInput  in;
                                RE::hkpWorldRayCastOutput out;
                                in.from.quad = _mm_setr_ps(p.x * ws, p.y * ws,
                                                           (p.z + kStartUp) * ws, 0.0f);
                                in.to.quad   = _mm_setr_ps(p.x * ws, p.y * ws,
                                                           (p.z + kStartUp - kRange) * ws, 0.0f);
                                in.filterInfo.filter = static_cast<std::uint32_t>(
                                    RE::COL_LAYER::kCameraSphere);
                                in.enableShapeCollectionFilter = false;
                                hkW->CastRay(in, out);
                                if (out.HasHit())
                                    ground = std::max(0.0f,
                                        out.hitFraction * kRange - kStartUp);
                            }
                        }
                    }
                }
                if (!sAirClearHad) {
                    sAirClearSm  = ground;
                    sAirClearHad = true;
                } else {
                    sAirClearSm += (ground - sAirClearSm) *
                                   (1.0f - std::exp(-dt / 0.20f));
                }
                // Shut below ~25u (a skim), full above ~120u. A running jump
                // apexes around 80 and lands mid-band, which is the honest
                // answer for it too — and its LAUNCH BURST is not gated, so
                // the felt part of a jump is untouched either way.
                float t = std::clamp((sAirClearSm - 25.0f) / 95.0f, 0.0f, 1.0f);
                clearGate = t * t * (3.0f - 2.0f * t);
            } else {
                sAirClearHad = false;
            }

            // Wind ramp — driven by AIRSPEED (total, any direction), because
            // that is what the rush actually is. Calibrated against real
            // speeds: a flat-ground jump lands at ~380-450 u/s, so the ONSET
            // sits at 450 — ordinary jumps produce essentially no wind — and
            // the ramp spans to ~2200 u/s so the rush BUILDS across a long
            // plunge or a fast horizontal flight instead of saturating on a
            // ledge drop. The steeper exponent keeps the mid-range modest: a
            // short hop is a whisper, real travel grows into the full roar.
            constexpr float kWindOnset = 450.0f;
            // Gated by ground clearance: the same speed reads as a roar in open
            // air and as nothing at all when it's a skim (see clearGate above).
            const float windF = inAirNow
                ? std::clamp((speed - kWindOnset) / 1750.0f, 0.0f, 1.0f) * clearGate
                : 0.0f;
            const float wind  = 3.2f * std::pow(windF, 1.6f);
            // Airspeed bridge. The launch burst is spent 0.30s in and the wind
            // ramp doesn't open until kWindOnset — a speed an ordinary jump
            // only reaches AT touchdown. Everything between those two was
            // carried by the bare 0.15 base, and THAT is the "cuts out
            // mid-fall": the burst dies around the apex and the rest of the
            // airtime is silent (the [JUMPAIR] diag proved the airborne signal
            // never dropped, so the gap was never a detection problem). Bridge
            // it with a floor that swells with airspeed and hands off to the
            // wind ramp exactly at its onset. Squared so a near-motionless
            // apex stays deliberately quiet, per the three-phase design — the
            // swell arrives with the travel, not before it.
            const float rushF = inAirNow
                ? std::clamp(speed / kWindOnset, 0.0f, 1.0f) : 0.0f;
            // The SWELL is clearance-gated, the 0.15 floor is not: an airborne
            // frame is still airborne a foot off the ground, it just isn't a
            // fall. That floor is what keeps a ground-level dash sounding like
            // the dash it is instead of falling silent.
            const float base  = inAirNow
                ? 0.15f + 0.85f * rushF * rushF * clearGate : 0.0f;
            // Pre-scaled by the split dials: launch burst is Jumping, the
            // whole airborne texture (bridge + wind) is Falling.
            const float env   = jumpAmp * burst + fallAmp * (base + wind);

            // The airborne rush outranks a shout. Whirlwind Sprint IS a shout,
            // and one that never delivers a stop tag — StateResolver holds the
            // Shout sub-state on a 3s safety timeout (see PollShoutLinger) —
            // so the whole dash AND the first seconds of the flight off a
            // cliff resolved to the shout profile and this layer never ran at
            // all. Once the airspeed has actually opened the wind ramp we are
            // rushing through the air and that has to win. Latched for the
            // rest of the airtime so a dip back under the onset can't flap the
            // source (a source flip re-arms the ambient crossfade from zero),
            // which is its own dropout. A plain hop never reaches the onset,
            // so a shout cast mid-jump keeps its own profile.
            // windF is clearance-gated, so a Whirlwind Sprint down a flat
            // corridor can no longer take the shout's profile away from it —
            // only a dash that actually leaves the ground behind does. The
            // threshold (not > 0) keeps a grazing clearance from latching the
            // override on a sliver of wind it will never build on.
            static bool sWindOverShout = false;
            if (!inAirNow)          sWindOverShout = false;
            else if (windF > 0.05f) sWindOverShout = true;

            out.inAir    = inAirNow;
            out.windOpen = inAirNow && sWindOverShout;
            out.env      = env;
            out.dbgSpeed = speed;
            out.dbgRushF = rushF;
            out.dbgWindF = windF;
            out.dbgClear = clearGate;
            // Busier as the fall builds — rushF carries the rate through the
            // bridge so the descent doesn't sit at the idle tempo waiting for
            // the wind ramp to open.
            out.speedMul = 1.25f + 0.40f * rushF * clearGate + 2.15f * windF;

            // TEMPORARY [JUMPARC] — one line per airtime, on landing. minEnv is
            // sampled only AFTER the burst has expired, so it reports exactly
            // what the flight sounded like: the old arc bottomed out at 0.15
            // there, the bridge should hold it well above that at any real
            // airspeed. shoutBlocked=true means a shout still owned the source
            // for part of the airtime and the wind layer never ran. Capped at
            // 20 lines. Strip together with [JUMPAIR] once confirmed.
            {
                static bool  sArcPrevAir = false;
                static float sArcT = 0.0f, sArcEnvMin = 1.0e9f;
                static float sArcSpeedMax = 0.0f, sArcVzMin = 0.0f;
                static float sArcClearMax = 0.0f, sArcGateMax = 0.0f;
                static int   sArcLogs = 0;
                if (inAirNow) {
                    sArcT += dt;
                    if (sArcT > 0.35f) sArcEnvMin = std::min(sArcEnvMin, env);
                    sArcSpeedMax = std::max(sArcSpeedMax, speed);
                    sArcVzMin    = std::min(sArcVzMin, vz);
                    sArcClearMax = std::max(sArcClearMax, sAirClearSm);
                    sArcGateMax  = std::max(sArcGateMax, clearGate);
                } else if (sArcPrevAir) {
                    if (sArcLogs < 20) {
                        ++sArcLogs;
                        // peakClear/peakGate: the clearance gate's verdict on
                        // this airtime. A Whirlwind Sprint down a corridor
                        // should read peakClear in the tens and peakGate ~0.
                        spdlog::debug("[JUMPARC] airtime={:.2f}s minEnvAfterBurst={:.2f} "
                                     "peakSpeed={:.0f} peakDescent={:.0f} "
                                     "peakClear={:.0f} peakGate={:.2f} shoutBlocked={}",
                                     sArcT,
                                     sArcEnvMin > 1.0e8f ? -1.0f : sArcEnvMin,
                                     sArcSpeedMax, -sArcVzMin,
                                     sArcClearMax, sArcGateMax, sJumpArcShoutBlocked);
                    }
                    sArcT = 0.0f; sArcEnvMin = 1.0e9f;
                    sArcSpeedMax = 0.0f; sArcVzMin = 0.0f;
                    sArcClearMax = 0.0f; sArcGateMax = 0.0f;
                    sJumpArcShoutBlocked = false;
                }
                sArcPrevAir = inAirNow;
            }

            return out;
        }
    }

    bool CameraNoiseController::IsSuppressed(RE::PlayerCamera* a_pc, bool a_bypassAnimated) const
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;

        if (ui->GameIsPaused()) return true;
        // Dialogue Menu used to suppress unconditionally. Noise IS
        // desired during dialogue (the cinematic shake gives the
        // dialogue camera life), and unpaused Barter sits on top of an
        // open Dialogue Menu — suppressing on dialogue open killed the
        // shake during the conversation/trade. Now noise rides through.

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->IsInKillMove()) return true;

        // First-person path: don't suppress at this layer. OnCameraUpdate
        // springs amp/speed toward the resolved profile and skips the
        // actual write when the springed value falls below ~0, so an
        // enabled→disabled transition can decay smoothly instead of
        // cutting off. None of the state-id gates below match
        // kFirstPerson, so 1p falls through cleanly.

        // Suppress during tween / transition / non-3p camera states.
        // cameraRoot->local is unstable across these (animated flythroughs,
        // inventory menu wheel, etc.) — additive Perlin offsets propagate
        // wrong and leave the camera shifted sideways / into the ground
        // after the transition completes.
        if (a_pc) {
            if (auto* state = a_pc->currentState.get()) {
                const auto id = state->id;
                // kTween / kPCTransition always suppress — these are
                // the menu-cam / camera-state-swap intermediates and
                // additive noise on their unstable cameraRoot causes
                // visible warping. When framing is enabled the
                // controller forces back to kThirdPerson so noise
                // applies cleanly; when framing is off the engine
                // leaves the camera in kTween where noise must NOT
                // ride.
                if (id == RE::CameraState::kTween ||
                    id == RE::CameraState::kPCTransition ||
                    id == RE::CameraState::kAutoVanity ||
                    id == RE::CameraState::kVATS ||
                    id == RE::CameraState::kBleedout ||
                    id == RE::CameraState::kFree) {
                    return true;
                }
                // kFurniture — suppress unconditionally (vanilla owns it).
                if (id == RE::CameraState::kFurniture) return true;
                // kAnimated — suppress unless caller has armed a transform
                // shake. Werewolf / VL morph animations ride on kAnimated,
                // and we want the build-up envelope to play DURING the
                // morph; without this bypass the envelope's start time
                // ends up stale by the time kAnimated releases.
                if (id == RE::CameraState::kAnimated && !a_bypassAnimated)
                    return true;
            }
        }

        return false;
    }

    void CameraNoiseController::SampleAmbientLayer(
        double a_clockTrans, double a_clockRot,
        float a_sway, float a_tilt, float a_amp,
        float a_driftJitter, float a_roughness,
        RE::NiPoint3& a_outTrans, RE::NiPoint3& a_outRotAxis, float& a_outRotTheta) const
    {
        // --- Two-band, rotation-weighted noise generator ---
        // Research (Eiserloh GDC, Cinemachine, Unreal): natural "camera life"
        // is a SLOW DRIFT (~0.2 Hz, larger) plus a FAST JITTER (~5 Hz, tiny)
        // layered together, and in 3D the ROTATION channel carries the feel
        // (translation reveals parallax / world-slide and is the weak channel).
        // We sample each Perlin axis at two time scales and blend, then weight
        // rotation heavily and translation lightly. Perlin1D already returns
        // ~[-1,1] (zero-mean), so there's no DC drift. This is a PURE sampler:
        // the caller owns the clocks (which is what lets two layers run at two
        // different Speeds simultaneously during a crossfade).
        const float posW = a_sway * a_amp;   // translation weight
        const float rotW = a_tilt * a_amp;   // rotation weight

        // Band scales (features/sec at Speed=1): slow drift vs fast jitter.
        constexpr double kDriftScale  = 0.22;
        constexpr double kJitterScale = 5.0;
        const double rough   = std::clamp(a_roughness, 0.1f, 0.8f);   // fractal persistence
        const float  mix     = std::clamp(a_driftJitter, 0.0f, 1.0f); // 0=drift, 1=jitter
        const float  driftW  = 1.0f - mix;
        const float  jitterW = mix;

        // One channel = drift octaves + jitter octaves, blended. The +100
        // offset decorrelates the jitter sample position from the drift one.
        auto band = [&](const dietdrcamera::Perlin1D& p, double t, double off) -> float {
            const double d = p.SampleFractal(t * kDriftScale + off, 2, rough);
            const double j = p.SampleFractal(t * kJitterScale + off + 100.0, 2, rough);
            return static_cast<float>(d) * driftW + static_cast<float>(j) * jitterW;
        };

        // Translation — deliberately light (the weak 3D channel).
        constexpr float kTransScale = 0.30f;
        const float tW = posW * kTransScale;
        a_outTrans = RE::NiPoint3{
            band(perlinTransX, a_clockTrans, 0.0)  * tW,
            band(perlinTransY, a_clockTrans, 11.3) * tW,
            band(perlinTransZ, a_clockTrans, 23.7) * tW
        };

        // Rotation — the primary channel. Single axis-angle pass; the axis
        // comes from the blended bands so it already carries drift+jitter.
        constexpr float kAxisScale = 6.2831853f;  // 2*pi
        a_outRotAxis = RE::NiPoint3{
            band(perlinRotX, a_clockRot, 0.0)  * kAxisScale,
            band(perlinRotY, a_clockRot, 7.1)  * kAxisScale,
            band(perlinRotZ, a_clockRot, 15.8) * kAxisScale
        };
        a_outRotTheta = 0.00020f * rotW;
    }

    void CameraNoiseController::ApplyPerlin(RE::TESCamera* a_camera, float a_dt, float a_ampMul, float a_speedMul,
                                            float a_transMul, float a_rotMul, float a_freqMul)
    {
        // Single-layer path — used when a cinematic/transform shake dominates
        // the frame (dragon/centurion/werewolf/vampire). Advances the shared
        // member clock and applies one texture with the current impact
        // character (curDriftJitter / curRoughness set by the caller).
        (void)a_freqMul;                 // wobble retired — rate is Speed only
        const float spd = a_speedMul;
        timeTrans += a_dt * spd;
        timeRot   += a_dt * spd;
        constexpr double kTimeWrap = 65536.0;
        if (timeTrans > kTimeWrap) timeTrans -= kTimeWrap;
        if (timeRot   > kTimeWrap) timeRot   -= kTimeWrap;

        RE::NiPoint3 transOffset, rotAxis;
        float        rotTheta = 0.0f;
        SampleAmbientLayer(timeTrans, timeRot, a_transMul, a_rotMul, a_ampMul,
                           curDriftJitter, curRoughness, transOffset, rotAxis, rotTheta);

        auto& local = a_camera->cameraRoot->local;
        // NOISE IS NOT THE TWEEN SNAP — REFUTED 2026-09-07, gate removed.
        //
        // The 2026-09-06 build withheld this translation while
        // ShowPlayerInMenus::IsActive(), on the theory that the engine
        // INTEGRATES the root/translation gap this apply opens (root->local
        // only, no lockstep) at 0.36 x the gap per frame. The 14:40 capture
        // ran with that gate live and the drift was unchanged: -0.14/frame,
        // 4.98u banked, the same jump at the next open. The gap model died
        // with it — on the second window the root sat 0.11u BELOW translation
        // and the drift kept the same sign and the same rate, so it is not
        // proportional to the gap at all. It is a constant-rate walk to a
        // fixed target, which stops dead on arrival.
        //
        // What the same capture DID name: the walk is exactly 4.98u along the
        // camera's own RIGHT axis (dr +4.84 / df +0.43, direction 5.08 deg off
        // body-right against a camera/body yaw split of 5.05 deg), at constant
        // camera height. That is a five-unit SHOULDER offset, i.e. the vanilla
        // fOverShoulderPosX / fOverShoulderCombatPosX pair delta. See the
        // [TWEENX] header in HookManager.cpp for the probe that names the
        // writer.
        local.translate.x += transOffset.x;
        local.translate.y += transOffset.y;
        local.translate.z += transOffset.z;
        if (std::abs(rotTheta) > 1e-6f) {
            local.rotate = local.rotate * MatrixFromAxisAngle(rotAxis, rotTheta);
        }
    }

    // FLOW FLOOR — the texture clock never stops. A state whose Speed sits at
    // ~0 (the stock magic-drawn cell, most idle cells) used to decelerate the
    // Perlin clock to a standstill between casts: amplitude stayed nonzero
    // but the sample froze, so the camera held a fixed tilt — a dead stop in
    // the middle of a cast chain ("I want things to never stop flowing").
    // 0.25 is a slow breath: visibly alive, well under the slowest tuned
    // state speeds (~0.6). Amp 0 still renders nothing, so true stillness is
    // unaffected — this floors motion only where there IS motion to show.
    static constexpr float kMinFlowRate = 0.25f;

    // Make an isotropic noise sample read as though it RADIATES from a point
    // in the world rather than happening to the camera.
    //
    // Amplitude is not the cue. A shake that only gets quieter with distance
    // is still the same shake in every direction, which is exactly why an
    // event reads as "the camera noticed something" instead of "something
    // happened over there". Two directional cues do nearly all the work:
    //
    //   * translation is pushed ONTO the line to the source, so the camera is
    //     shoved along that axis — the ground heaving from a specific place;
    //   * the rotation axis is pushed OFF that line, so the view rocks toward
    //     and away from the source instead of twisting about the sight line.
    //     (Rotating ABOUT the direction you're looking at something is the one
    //     motion that carries no information about where it is.)
    //
    // Both are pure REDISTRIBUTIONS of direction: each vector is renormalised
    // to the length it already had, so a tuned Intensity is untouched and this
    // can never make anything louder — only differently shaped.
    static void BiasTowardSource(RE::NiPoint3& a_trans, RE::NiPoint3& a_axis,
                                 const RE::NiPoint3& a_dir, float a_bias)
    {
        const float b = std::clamp(a_bias, 0.0f, 1.0f);
        auto dot = [](const RE::NiPoint3& u, const RE::NiPoint3& v) {
            return u.x * v.x + u.y * v.y + u.z * v.z;
        };
        auto len = [&](const RE::NiPoint3& u) { return std::sqrt(dot(u, u)); };

        const float m0t = len(a_trans);
        if (m0t > 1e-7f) {
            const float r = dot(a_trans, a_dir);
            const RE::NiPoint3 out{
                a_dir.x * r * (1.0f + b) + (a_trans.x - a_dir.x * r) * (1.0f - b),
                a_dir.y * r * (1.0f + b) + (a_trans.y - a_dir.y * r) * (1.0f - b),
                a_dir.z * r * (1.0f + b) + (a_trans.z - a_dir.z * r) * (1.0f - b) };
            const float m1 = len(out);
            if (m1 > 1e-7f) {
                const float k = m0t / m1;
                a_trans = RE::NiPoint3{ out.x * k, out.y * k, out.z * k };
            }
        }

        const float m0a = len(a_axis);
        if (m0a > 1e-7f) {
            const float p = dot(a_axis, a_dir);
            const RE::NiPoint3 out{ a_axis.x - a_dir.x * p * b,
                                    a_axis.y - a_dir.y * p * b,
                                    a_axis.z - a_dir.z * p * b };
            const float m1 = len(out);
            if (m1 > 1e-7f) {
                const float k = m0a / m1;
                a_axis = RE::NiPoint3{ out.x * k, out.y * k, out.z * k };
            }
        }
    }

    // Attack punctuation rate — how fast BOTH the 1p amp spring and the 1p flow
    // governor are allowed to be dragged while the Attack sub-state (plus a
    // short release window) is live. ~95% in 0.21s, which is what keeps an
    // attack texture inside its own animation at any Transition Speed. Shared
    // by the two floors so they can never drift apart; see the "ATTACK BEATS
    // ARE PUNCTUATION" block and the flow governor.
    static constexpr float kAttackOmega = 14.0f;

    // Equal-power crossfade weights from a linear phase [0..1]. smoothstep
    // gives zero slope at both ends (the fade can't kink the composite on its
    // first or last frame); sin/cos weights keep the summed POWER of two
    // uncorrelated noise textures constant through the fade. Linear weights
    // (w, 1-w) dip the perceived energy ~30% at the midpoint — every state
    // change pumped soft-then-back, which is a large part of what "rough
    // transitions" felt like.
    // How FAR APART two noise textures are, 0 (indistinguishable) to 1
    // (nothing in common). Drives how long the crossfade between them takes.
    //
    // A fixed fade length is the thing that makes wildly different states snap.
    // 0.35s is generous for a small change and far too short for a big one:
    // going from a near-still idle to a loud, fast, jittery attack in the same
    // 0.35s is a huge rate of change no matter how correct the weighting curve
    // is, because what you feel is d(motion)/dt, not the weights. A transition
    // has to take longer when it has further to travel.
    //
    // Energy dominates perception, rate is next, texture is a seasoning —
    // hence the weights. Everything is RELATIVE (change over magnitude) so a
    // 1->2 step counts as large and a 19->20 step counts as nothing, which is
    // how the eye reads it.
    static float TextureDistance(float a_ampA, float a_spdA, float a_tiltA, float a_swayA,
                                 float a_djA,  float a_rghA,
                                 float a_ampB, float a_spdB, float a_tiltB, float a_swayB,
                                 float a_djB,  float a_rghB)
    {
        auto rel = [](float x, float y) {
            const float m = (std::max)({ std::abs(x), std::abs(y), 1.0e-3f });
            return std::clamp(std::abs(x - y) / m, 0.0f, 1.0f);
        };
        const float dEnergy = rel(a_ampA * (a_tiltA + a_swayA),
                                  a_ampB * (a_tiltB + a_swayB));
        const float dRate   = rel(a_spdA, a_spdB);
        const float dTex    = std::clamp(
            (std::abs(a_djA - a_djB) + std::abs(a_rghA - a_rghB)) * 0.5f, 0.0f, 1.0f);
        return std::clamp(0.55f * dEnergy + 0.30f * dRate + 0.15f * dTex, 0.0f, 1.0f);
    }

    // Seconds a crossfade should take for that distance. The near end is the
    // long-standing default (small changes keep exactly the feel they had);
    // the far end is long enough that a state change with nothing in common
    // reads as a morph rather than a cut.
    static float AdaptiveXfadeDur(float a_dist)
    {
        constexpr float kMin = 0.35f;   // == kAmbientXfadeDur
        constexpr float kMax = 1.20f;
        const float t = std::clamp(a_dist, 0.0f, 1.0f);
        return kMin + (kMax - kMin) * (t * t * (3.0f - 2.0f * t));
    }

    // Sampling offset applied to the INCOMING layer when a crossfade arms.
    //
    // Equal-power weights are only correct for UNCORRELATED signals. Both
    // layers read the same Perlin field, and the incoming one inherits the
    // outgoing one's clock, so at the arm instant they are the SAME SIGNAL —
    // and sin+cos of a correlated pair peaks at ~1.41, so the composite
    // swelled ~40% through the middle of every fade and settled back. Pushing
    // the incoming layer to an unrelated part of the field makes the equal-power
    // assumption true. It is free: the incoming layer is at weight ZERO on the
    // frame this happens, so nothing can jump.
    static constexpr double kXfadeDecorrelate = 611.7;

    static void XfadeWeights(float a_phase, float& a_wIn, float& a_wOut)
    {
        NoiseFadeWeights(a_phase, a_wIn, a_wOut);
    }

    void CameraNoiseController::ApplyAmbientCrossfade(RE::TESCamera* a_camera, float a_dt, float a_masterW)
    {
        // Advance each layer's OWN clock at its OWN Speed, then weight-sum the
        // retained textures. Because each outgoing layer runs at its old
        // tempo the whole time it fades out, the composite never wallows.
        constexpr double kTimeWrap = 65536.0;
        float wCur, wPrev;
        XfadeWeights(ambientXfade, wCur, wPrev);
        wCur  *= a_masterW;
        wPrev *= a_masterW;

        // Slow Time Noise: every clock in this function advances on this
        // scaled dt, so ambient, the outgoing layer and all three beat layers
        // slow together and stay phase-coherent with each other. Amplitude and
        // the crossfade weight are untouched — only the tempo changes.
        a_dt *= SlowTimeNoiseFactor();

        curAmbient.clockTrans += a_dt * (std::max)(kMinFlowRate, curAmbient.speed);
        curAmbient.clockRot   += a_dt * (std::max)(kMinFlowRate, curAmbient.speed);
        if (curAmbient.clockTrans > kTimeWrap) curAmbient.clockTrans -= kTimeWrap;
        if (curAmbient.clockRot   > kTimeWrap) curAmbient.clockRot   -= kTimeWrap;
        ambientOutgoing.UpdateLayers([&](AmbientLayer& layer) {
            layer.clockTrans += a_dt * std::max(kMinFlowRate, layer.speed);
            layer.clockRot += a_dt * std::max(kMinFlowRate, layer.speed);
            if (layer.clockTrans > kTimeWrap) layer.clockTrans -= kTimeWrap;
            if (layer.clockRot > kTimeWrap) layer.clockRot -= kTimeWrap;
        });

        auto& local = a_camera->cameraRoot->local;
        // [MOTION] composite magnitudes, accumulated as SQUARES and rooted at
        // the meter — the felt composite of independent layers is the RMS.
        // Summing |θ| instead inflates through every equal-power fade (the
        // weights sum past 1 mid-fade), which made the meter flag phantom
        // steps at each fade start.
        float appliedThetaSq = 0.0f;
        float appliedTransSq = 0.0f;
        // Running total of what we actually put on the camera this frame, kept
        // so a suppressed state can walk it out instead of dropping it.
        sTailTrans  = RE::NiPoint3{};
        sTailRotVec = RE::NiPoint3{};
        auto applyLayer = [&](const AmbientLayer& L, float w) {
            if (w <= 0.001f) return;
            RE::NiPoint3 trans, axis;
            float        theta = 0.0f;
            SampleAmbientLayer(L.clockTrans, L.clockRot, L.sway, L.tilt, L.amp,
                               L.driftJitter, L.roughness, trans, axis, theta);
            if (L.srcBias > 0.0001f) BiasTowardSource(trans, axis, L.srcDir, L.srcBias);
            local.translate.x += trans.x * w;
            local.translate.y += trans.y * w;
            local.translate.z += trans.z * w;
            appliedTransSq += (trans.x * trans.x + trans.y * trans.y + trans.z * trans.z) * w * w;
            sTailTrans.x += trans.x * w;
            sTailTrans.y += trans.y * w;
            sTailTrans.z += trans.z * w;
            sApplied3pTrans.x += trans.x * w;
            sApplied3pTrans.y += trans.y * w;
            sApplied3pTrans.z += trans.z * w;
            sApplied3pAny = true;
            const float t = theta * w;   // small-angle: scaling theta ~ scaling the rotation
            appliedThetaSq += t * t;
            sTailRotVec.x += axis.x * t;   // small-angle rotations add as vectors
            sTailRotVec.y += axis.y * t;
            sTailRotVec.z += axis.z * t;
            if (std::abs(t) > 1e-6f) {
                const RE::NiMatrix3 m = MatrixFromAxisAngle(axis, t);
                local.rotate  = local.rotate * m;
                sApplied3pRot = sApplied3pRot * m;
            }
        };
        applyLayer(curAmbient,  wCur);
        ambientOutgoing.Visit([&](const AmbientLayer& layer, float weight) {
            applyLayer(layer, wPrev * weight);
        });
        // Additive one-shot beat on top of the crossfade, at full weight, on
        // its OWN clock (amp already enveloped by OnCameraUpdate). The clock
        // advances unconditionally — including while the beat is silent — so
        // consecutive beats never start from the same Perlin phase and a beat
        // can't inherit a frozen sample from a Speed-0 state.
        drawBeat.clockTrans += a_dt * std::max(0.1f, drawBeat.speed);
        drawBeat.clockRot   += a_dt * std::max(0.1f, drawBeat.speed);
        if (drawBeat.clockTrans > kTimeWrap) drawBeat.clockTrans -= kTimeWrap;
        if (drawBeat.clockRot   > kTimeWrap) drawBeat.clockRot   -= kTimeWrap;
        if (drawBeatValid) applyLayer(drawBeat, a_masterW);
        // Transformation shake — identical treatment, its own clock so the
        // Werewolf and Vampire Lord Speed sliders mean what they say.
        transBeat.clockTrans += a_dt * std::max(0.1f, transBeat.speed);
        transBeat.clockRot   += a_dt * std::max(0.1f, transBeat.speed);
        if (transBeat.clockTrans > kTimeWrap) transBeat.clockTrans -= kTimeWrap;
        if (transBeat.clockRot   > kTimeWrap) transBeat.clockRot   -= kTimeWrap;
        if (transBeatValid) applyLayer(transBeat, a_masterW);
        // Bats / Reanimation / Summoning — its own layer so it can ride
        // alongside a transformation instead of one silencing the other.
        eventBeat.clockTrans += a_dt * std::max(0.1f, eventBeat.speed);
        eventBeat.clockRot   += a_dt * std::max(0.1f, eventBeat.speed);
        if (eventBeat.clockTrans > kTimeWrap) eventBeat.clockTrans -= kTimeWrap;
        if (eventBeat.clockRot   > kTimeWrap) eventBeat.clockRot   -= kTimeWrap;
        if (eventBeatValid) applyLayer(eventBeat, a_masterW);
        dragonBeat.clockTrans += a_dt * std::max(0.1f, dragonBeat.speed);
        dragonBeat.clockRot   += a_dt * std::max(0.1f, dragonBeat.speed);
        if (dragonBeat.clockTrans > kTimeWrap) dragonBeat.clockTrans -= kTimeWrap;
        if (dragonBeat.clockRot   > kTimeWrap) dragonBeat.clockRot   -= kTimeWrap;
        if (dragonBeatValid) applyLayer(dragonBeat, a_masterW);
        // Continuous blends (proximity ambience / NPC casting). BOTH slots'
        // clocks advance every frame at their own speeds so neither ever
        // samples a frozen phase; applied weighted by the internal
        // mini-crossfade so a dominant-source change fades textures instead
        // of stepping.
        auto advanceBlend = [&](ContinuousBlend& b) {
            b.cur.clockTrans  += a_dt * std::max(0.1f, b.cur.speed);
            b.cur.clockRot    += a_dt * std::max(0.1f, b.cur.speed);
            b.prev.clockTrans += a_dt * std::max(0.1f, b.prev.speed);
            b.prev.clockRot   += a_dt * std::max(0.1f, b.prev.speed);
            if (b.cur.clockTrans  > kTimeWrap) b.cur.clockTrans  -= kTimeWrap;
            if (b.cur.clockRot    > kTimeWrap) b.cur.clockRot    -= kTimeWrap;
            if (b.prev.clockTrans > kTimeWrap) b.prev.clockTrans -= kTimeWrap;
            if (b.prev.clockRot   > kTimeWrap) b.prev.clockRot   -= kTimeWrap;
            if (b.valid) {
                float wc, wp;
                XfadeWeights(b.xfade, wc, wp);
                applyLayer(b.cur, wc * a_masterW);
                applyLayer(b.prev, wp * a_masterW);
            }
        };
        advanceBlend(npcBlend);
        advanceBlend(attackBlend);

        // ----- [SHOUT3P] THE NOISE ROTATION, LEAVING A SHOUT IN 3P -------
        //
        // The third-person twin of [SHOUTTAIL]. The 1p probe could not see the
        // reported event at all: it lives inside the in1p branch and the shout
        // was performed in THIRD person, so it logged a different session's
        // first-person shout instead ("a probe that cannot see its subject",
        // again). This one runs on the 3p composite.
        //
        // The 3p hand-off is a genuine two-layer crossfade — the shout's
        // texture becomes the OUTGOING layer and the parent rises underneath it
        // — so the thing to watch is the rendered rotation magnitude across it.
        // `theta` is the summed rotation the player actually sees. If it rises
        // above where it settles and comes back, that is the overshoot, and the
        // per-layer amps and tilts next to it say whether the equal-power mix
        // is summing two layers louder than either (curAmp/prevAmp both large
        // through the middle of the fade) or whether one layer's tilt is simply
        // bigger than the other's.
        {
            static bool  sPrevShout3p = false;
            static float sSince3p     = -1.0f;
            static int   sLogs3p      = 0;
            const bool shoutNow =
                StateResolver::GetSingleton().GetSubState() == CameraSubState::Shout;
            if (shoutNow) sSince3p = 0.0f;
            else if (sSince3p >= 0.0f) {
                sSince3p += a_dt;
                if (sSince3p > 2.5f) sSince3p = -1.0f;
            }
            sPrevShout3p = shoutNow;
            if (sSince3p >= 0.0f && sLogs3p < 500) {
                ++sLogs3p;
                spdlog::debug("[SHOUT3P] rel={} t={:.0f}ms theta={:.5f} trans={:.3f} xfade={:.2f} "
                             "cur(amp={:.3f} tilt={:.3f} sway={:.3f} spd={:.3f} dj={:.3f} rgh={:.3f}) "
                             "prev(amp={:.3f} tilt={:.3f} sway={:.3f} spd={:.3f} dj={:.3f} rgh={:.3f})",
                             shoutNow ? 0 : 1, sSince3p * 1000.0f,
                             std::sqrt(appliedThetaSq), std::sqrt(appliedTransSq),
                             ambientXfade,
                             curAmbient.amp, curAmbient.tilt, curAmbient.sway,
                             curAmbient.speed, curAmbient.driftJitter, curAmbient.roughness,
                             prevAmbient.amp, prevAmbient.tilt, prevAmbient.sway,
                             prevAmbient.speed, prevAmbient.driftJitter, prevAmbient.roughness);
            }
        }

        // [MOTION] jerk meter — the composite is the thing the player feels,
        // and "feels rough" reports need attribution, not vibes. Any single-
        // frame step of the summed rotation magnitude past ~0.17° is beyond
        // what a smooth envelope produces; log it with the per-layer amps so
        // the offending path is named in the log. Capped + cooldown so a
        // pathological state can't flood the file.
        {
            static float sPrevTheta  = 0.0f;
            static float sPrevTrans  = 0.0f;
            static float sEmaDTheta  = 0.0f;
            static float sEmaDTrans  = 0.0f;
            static int   sMotionLogs = 0;
            static std::chrono::steady_clock::time_point sMotionLast{};
            const float appliedTheta = std::sqrt(appliedThetaSq);
            const float appliedTrans = std::sqrt(appliedTransSq);
            const float dTheta = std::abs(appliedTheta - sPrevTheta);
            const float dTrans = std::abs(appliedTrans - sPrevTrans);
            sPrevTheta = appliedTheta;
            sPrevTrans = appliedTrans;
            // Relative trigger: a step is anything ≥5× the running per-frame
            // wander (EMA), with a small absolute floor so quiet scenes don't
            // flag their own noise floor. The first meter's flat 0.003 floor
            // was ~6× typical wander — real but sub-threshold pops never
            // logged, which is worse than an occasional false positive.
            sEmaDTheta += (dTheta - sEmaDTheta) * 0.04f;
            sEmaDTrans += (dTrans - sEmaDTrans) * 0.04f;
            const bool rotStep = dTheta > (std::max)(0.0012f, 5.0f * sEmaDTheta);
            // The translation floor must scale with how big the translation
            // currently IS. A flat 0.35 was ~nothing next to a high-Position-
            // Shake texture, so ordinary Perlin travel tripped it: the 19:06
            // log flagged dTrans=2.914 with the ambient fully settled
            // (xfade=1.00) and the attack layer at 0.90 energy — nothing on
            // screen could step 2.9 units, so that was the meter, not the
            // camera. A real jerk moves a good fraction of the current
            // magnitude in ONE frame; normal travel does not.
            const bool trnStep = dTrans > (std::max)({ 0.35f,
                                                       0.25f * appliedTrans,
                                                       5.0f * sEmaDTrans });
            if ((rotStep || trnStep) && sMotionLogs < 40) {
                const auto nowTp = CameraEffectClock::Now();
                if (nowTp - sMotionLast > std::chrono::milliseconds(250)) {
                    sMotionLast = nowTp;
                    ++sMotionLogs;
                    spdlog::debug(
                        "[MOTION] 3p step dTheta={:.5f} dTrans={:.3f} xfade={:.2f} curAmp={:.2f} "
                        "prevAmp={:.2f} draw={:.2f} trans={:.2f} event={:.2f} npc={:.2f} atk={:.2f}/{:.2f} axf={:.2f}",
                        dTheta, dTrans, ambientXfade, curAmbient.amp, prevAmbient.amp,
                        drawBeatValid ? drawBeat.amp : 0.0f,
                        transBeatValid ? transBeat.amp : 0.0f,
                        eventBeatValid ? eventBeat.amp : 0.0f,
                        npcBlend.valid ? npcBlend.cur.amp : 0.0f,
                        attackBlend.valid ? attackBlend.cur.amp  : 0.0f,
                        attackBlend.valid ? attackBlend.prev.amp : 0.0f,
                        attackBlend.xfade);
                }
            }
        }
    }

    // Timestamp of the last noise-slider movement (see the header note).
    // Main-thread only (menu render + camera update share it), so a plain
    // static is fine.
    static std::chrono::steady_clock::time_point sLiveNoiseEditTp{};
    static std::atomic<std::uint64_t> sLiveNoiseEditRevision{ 0 };

    void CameraNoiseController::NotifyLiveNoiseEdit()
    {
        sLiveNoiseEditTp = CameraEffectClock::Now();
        sLiveNoiseEditRevision.fetch_add(1, std::memory_order_relaxed);
    }

    bool CameraNoiseController::GetAppliedCameraOffset3p(RE::NiMatrix3& a_outRot,
                                                         RE::NiPoint3& a_outTrans)
    {
        if (!sApplied3pAny) return false;
        a_outRot   = sApplied3pRot;
        a_outTrans = sApplied3pTrans;
        return true;
    }

    // Fresh while a drag is plausibly still in progress. Half a second spans
    // the gaps between pad d-pad ticks and slow mouse drags; the cost of the
    // window is only that a REAL state change landing inside it snaps instead
    // of fading — the player is in a menu retuning noise at that moment, so
    // the trade is invisible in practice.
    static bool LiveNoiseEditActive()
    {
        return std::chrono::duration<float>(CameraEffectClock::Now() -
                                            sLiveNoiseEditTp).count() < 0.5f;
    }

    void CameraNoiseController::NotifyNpcArcheryShot(RE::ObjectRefHandle shooter, bool crossbow,
        RE::FormID weapon, RE::FormID projectile, const RE::NiPoint3& pos)
    {
        sNpcArcheryShots.Push({shooter, crossbow, weapon, projectile, pos, std::chrono::steady_clock::now()}, projectile);
    }

    void CameraNoiseController::NotifyNpcRaceChange(RE::Actor* actor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!actor || !player || actor == player || actor->IsDead() ||
            !NpcSourceEnabled(NpcNoise::Source::Transformations) ||
            actor->GetPosition().GetDistance(player->GetPosition()) >= kNpcNoiseRange) return;
        auto& memory = sNpcCombatMemory[actor->GetFormID()];
        const auto form = NpcFormOf(actor);
        if (memory.formKnown && form == memory.form) return;
        if (const auto change = NpcNoise::TransformationChange(memory.form, memory.formKnown, form))
            NotifyNpcTransformBeat(change->form == NpcNoise::Form::VampireLord, actor->GetPosition(), change->revert);
        memory.beat = {};
        memory.spellBeat = {};
        memory.swings = {};
        memory.archerySneak = false;
        memory.archeryDrawing = false;
        memory.archeryWeapon = 0;
        memory.archeryItem = {};
        memory.voiceActive = false;
        memory.form = form;
        memory.formKnown = true;
    }

    void CameraNoiseController::NotifyNpcTransformBeat(bool a_vampireLord, const RE::NiPoint3& a_pos, bool a_revert)
    {
        auto&       s      = SettingsManager::GetSingleton();
        if (!NpcSourceEnabled(NpcNoise::Source::Transformations)) return;
        const int slot = NpcNoise::TransformationSlot(
            a_vampireLord ? NpcNoise::Form::VampireLord : NpcNoise::Form::Werewolf, a_revert);
        const auto* location = s.ActiveLocationFxBeat(SettingsManager::kFxBeatLocKeys[slot]);
        const float baseInt = location ? location->intensity : a_vampireLord
            ? (a_revert ? s.vampireLordRevertIntensity : s.vampireLordTransformIntensity)
            : (a_revert ? s.werewolfRevertIntensity : s.werewolfTransformIntensity);
        if (baseInt <= 0.0001f) return;
        // No extra scale: the transform and event layers share the same ×7,
        // so a distance-0 NPC transform lands as loud as the player's own.
        sNpcTransformParams.intensity = baseInt;
        sNpcTransformParams.speed = location ? location->speed : a_vampireLord
            ? (a_revert ? s.vampireLordRevertSpeed : s.vampireLordTransformSpeed)
            : (a_revert ? s.werewolfRevertSpeed : s.werewolfTransformSpeed);
        sNpcTransformParams.chr = location ? location->chr : a_vampireLord
            ? (a_revert ? s.vampireLordRevertChar : s.vampireLordTransformChar)
            : (a_revert ? s.werewolfRevertChar : s.werewolfTransformChar);
        sNpcTransformParams.range     = kNpcNoiseRange;
        sNpcTransformParams.hold      = 1.2f;  // = kTransPeak, the player envelope's hold
        sNpcTransformBeat.rearmPending = true;
        sNpcTransformBeat.pos          = a_pos;
        sNpcTransformBeat.hasPos       = true;
        sNpcTransformBeat.npcSource    = NpcNoise::Source::Transformations;
        spdlog::debug("[NPCNOISE] transform beat armed (vl={}, revert={})", a_vampireLord, a_revert);
    }

    void CameraNoiseController::ApplyPerlin1p(float a_dt, float a_ampMul, float a_speedMul,
                                              float a_rotMul, float a_freqMul,
                                              float a_pitchKickRad,
                                              const RE::NiPoint3& a_kickAxis,
                                              bool  a_useDeferredApply)
    {
        // Diagnostic — log when ApplyPerlin1p enters with significant
        // amp. Bucket-coalesced so we don't spam during steady frames.
        {
            static int sLastAmpBucket = -1;
            static int sLastSpdBucket = -1;
            const int bucket = static_cast<int>(a_ampMul * 2);  // 0.5 amp buckets
            // Rate gets its own bucket. The old line logged a_freqMul, which is
            // the retired wobble parameter and is always 1.0 — so the clock
            // rate, the channel that turned out to be doing the damage, was the
            // one thing the diagnostic could not see.
            const int sbucket = static_cast<int>(a_speedMul * 5);  // 0.2 rate buckets
            if (bucket != sLastAmpBucket || sbucket != sLastSpdBucket) {
                sLastAmpBucket = bucket;
                sLastSpdBucket = sbucket;
                spdlog::debug("[FoFApply] enter amp={:.2f} rot={:.2f} rate={:.2f}",
                             a_ampMul, a_rotMul, a_speedMul);
            }
        }
        // 1p shake must be ROTATIONAL, not translational.
        // - cameraRoot->local writes affect only the skybox in 1p
        //   (proven by the "sky was shaking" feedback).
        // - cameraNI->world.translate writes get clobbered by the
        //   engine's head-bone-driven update every frame (proven by
        //   "shake only when menu is open" — paused state skips the
        //   engine's overwrite step).
        // - cameraNI->world.rotate is preserved across the engine's
        //   per-frame writes (same field the 1p dialogue face-lock
        //   manipulates successfully). That's the right target.
        auto* pc = RE::PlayerCamera::GetSingleton();
        if (!pc || !pc->cameraRoot) {
            static int sLogPcNull = 0;
            if ((sLogPcNull++ % 60) == 0)
                spdlog::warn("[FoFApply] EARLY RETURN: playerCamera or cameraRoot null");
            return;
        }
        auto* asNode = pc->cameraRoot->AsNode();
        if (!asNode || asNode->GetChildren().empty()) {
            static int sLogEmpty = 0;
            if ((sLogEmpty++ % 60) == 0)
                spdlog::warn("[FoFApply] EARLY RETURN: cameraRoot has no children");
            return;
        }
        // Iterate ALL children to find the NiCamera — during certain
        // camera states (cast wind-up, animated, etc.) the engine may
        // insert other nodes ahead of the NiCamera and GetChildren()[0]
        // would return the wrong type, silently dropping the noise.
        RE::NiCamera* niCam = nullptr;
        for (auto& child : asNode->GetChildren()) {
            if (auto* ni = child.get()) {
                if (auto* nc = skyrim_cast<RE::NiCamera*>(ni)) {
                    niCam = nc;
                    break;
                }
            }
        }
        if (!niCam) {
            static int sLogNoNi = 0;
            if ((sLogNoNi++ % 60) == 0)
                spdlog::warn("[FoFApply] EARLY RETURN: no NiCamera child found");
            return;
        }

        // Keep the established first-person waveform, gain and clock rate.
        // Each retained outgoing texture uses the exact same sampler.
        (void)a_freqMul;
        constexpr double kTimeWrap = 65536.0;
        const float sampleDt = a_dt * SlowTimeNoiseFactor();
        timeRot += sampleDt * std::max(kMinFlowRate, a_speedMul);
        if (timeRot > kTimeWrap) timeRot -= kTimeWrap;
        fp1pCurrent = {timeRot, a_ampMul, a_speedMul, a_rotMul, curDriftJitter, curRoughness};
        fp1pOutgoing.UpdateLayers([&](FirstPersonNoiseLayer& layer) {
            layer.clock += sampleDt * std::max(kMinFlowRate, layer.speed);
            if (layer.clock > kTimeWrap) layer.clock -= kTimeWrap;
        });

        RE::NiMatrix3 noiseRotation{};
        bool anyNoise = false;
        float appliedThetaSq1p = 0.0f;
        auto sampleLayer = [&](const FirstPersonNoiseLayer& layer, float weight) {
            if (weight <= 0.0f || layer.amp <= 0.0001f) return;
            constexpr float kFpRotShakeGain = 2.0f;
            constexpr float kAxisScale = 6.2831853f;
            const float baseR = layer.rot * kFpRotShakeGain * layer.amp;
            const double rough = std::clamp(layer.roughness, 0.1f, 0.8f);
            const float mix = std::clamp(layer.driftJitter, 0.0f, 1.0f);
            const float theta1 = 0.00006f * baseR * (1.0f - 0.5f * mix) * weight;
            const float theta2 = 0.00001f * baseR * (0.5f + mix) * weight;
            appliedThetaSq1p += theta1 * theta1 + theta2 * theta2;
            if (std::abs(theta1) > 1e-6f) {
                const RE::NiPoint3 axis{
                    static_cast<float>(perlinRotX.SampleFractal(layer.clock, 3, rough)) * kAxisScale,
                    static_cast<float>(perlinRotY.SampleFractal(layer.clock + 7.1, 3, rough)) * kAxisScale,
                    static_cast<float>(perlinRotZ.SampleFractal(layer.clock + 15.8, 3, rough)) * kAxisScale};
                noiseRotation = noiseRotation * MatrixFromAxisAngle(axis, theta1);
                anyNoise = true;
            }
            if (std::abs(theta2) > 1e-6f) {
                const RE::NiPoint3 axis{
                    static_cast<float>(perlinRotZ.SampleFractal(layer.clock * 2.2, 3, rough)) * kAxisScale,
                    static_cast<float>(perlinRotX.SampleFractal(layer.clock * 2.2 + 4.2, 3, rough)) * kAxisScale,
                    static_cast<float>(perlinRotY.SampleFractal(layer.clock * 2.2 + 9.6, 3, rough)) * kAxisScale};
                noiseRotation = noiseRotation * MatrixFromAxisAngle(axis, theta2);
                anyNoise = true;
            }
        };
        float curWeight, oldWeight;
        XfadeWeights(fp1pXfade, curWeight, oldWeight);
        sampleLayer(fp1pCurrent, curWeight);
        fp1pOutgoing.Visit([&](const FirstPersonNoiseLayer& layer, float weight) {
            sampleLayer(layer, oldWeight * weight);
        });
        if (std::abs(a_pitchKickRad) > 1e-6f) {
            noiseRotation = noiseRotation * MatrixFromAxisAngle(a_kickAxis, a_pitchKickRad);
            anyNoise = true;
        }
        // The renderer and dialogue face-lock share one composed delta. Reusing
        // it after each clean engine recompose cannot double-apply the history.
        m1pNoiseDelta = noiseRotation;
        m1pNoiseDeltaValid = anyNoise;

        // [MOTION] jerk meter, 1p — texture layers only (the kick is an
        // intentional impulse and would trip it by design). Same threshold
        // and attribution idea as the 3p meter.
        //
        // INFO, not debug, since 2026-09-06 ("jumping still jerks"): this is
        // the only instrument that measures the thing the word "jerk" means
        // — the one-frame change in the APPLIED rotation angle — and it has
        // been running as debug through several sessions with Verbose off,
        // which is the trap [FPFALL] just fell into. prevRot is printed
        // beside rot for attribution: the 1p jump fold raises rot to 2.7 and
        // drops it back in ONE frame each way, at the max() crossover, while
        // the AMP either side of that crossover is continuous and slewed. If
        // a flagged frame shows rot stepping, the character swap is the jerk
        // and the amp ramp is exonerated; if rot is flat and amp moved, it is
        // the ramp; if both are flat and xfade moved, it is the crossfade
        // re-arming (the tag flips 0->2 on the way in and 2->0 on the way
        // out, twice inside one 0.30 s burst).
        //
        // Debounce 250 -> 120 ms so BOTH edges of a burst can be caught; cap
        // raised to 60 to match.
        {
            static float sPrevTheta1p  = 0.0f;
            static float sEmaDTheta1p  = 0.0f;
            static float sPrevRot1p    = 0.0f;
            // TRUE last-frame amp. The `prevAmp` this line printed until now is
            // fp1pPrevAmp — the CROSSFADE's outgoing layer, which reads 0
            // whenever no fade is running. It looked like a frame-to-frame amp
            // comparison and is not one, and with fades now skipped on shape
            // changes it is 0.00 on every line. This is the real thing.
            static float sPrevAmpFrm1p = 0.0f;
            // HEAD BOB, which this meter has never been able to see (user
            // 2026-09-06: "i wonder if its due to head bobbing"). It measures
            // rotTheta1..4 — the TEXTURE layers — and the bob is a separate
            // write (pendingBob*, applied at :4114). A probe that cannot see
            // its subject cannot exonerate it, so the bob's own frame-to-frame
            // step goes on the line.
            static float sPrevBobMag1p = 0.0f;
            static int   sMotionLogs1p = 0;
            static std::chrono::steady_clock::time_point sMotionLast1p{};
            // RMS across the layers, same reason as the 3p meter: a sum of
            // absolutes inflates through equal-power fades and flagged every
            // fade start as a phantom step.
            const float applied1p = std::sqrt(appliedThetaSq1p);
            const float dTheta1p = std::abs(applied1p - sPrevTheta1p);
            sPrevTheta1p = applied1p;
            const float bobMag1p = pendingBobValid
                ? std::sqrt(pendingBobPitch * pendingBobPitch +
                            pendingBobRoll  * pendingBobRoll +
                            pendingBobYaw   * pendingBobYaw)
                : 0.0f;
            const float dBob1p = std::abs(bobMag1p - sPrevBobMag1p);
            // Relative trigger with a floor sized to 1p magnitudes — the 1p
            // texture runs ~10× smaller thetas than 3p, so the first meter's
            // flat 0.003 could never see a 1p cut at all.
            sEmaDTheta1p += (dTheta1p - sEmaDTheta1p) * 0.04f;
            if (dTheta1p > (std::max)(0.0004f, 5.0f * sEmaDTheta1p) && sMotionLogs1p < 60) {
                const auto nowTp = CameraEffectClock::Now();
                if (nowTp - sMotionLast1p > std::chrono::milliseconds(120)) {
                    sMotionLast1p = nowTp;
                    ++sMotionLogs1p;
                    spdlog::debug(
                        "[MOTION] 1p step dTheta={:.5f} xfade={:.2f} amp={:.2f} prevAmpFrm={:.2f} "
                        "dAmp={:.2f} rot={:.2f} prevRot={:.2f} bob={:.5f} dBob={:.5f} ({}/60)",
                        dTheta1p, fp1pXfade, a_ampMul, sPrevAmpFrm1p,
                        a_ampMul - sPrevAmpFrm1p, a_rotMul, sPrevRot1p,
                        bobMag1p, dBob1p, sMotionLogs1p);
                }
            }
            sPrevRot1p    = a_rotMul;
            sPrevAmpFrm1p = a_ampMul;
            sPrevBobMag1p = bobMag1p;
        }

        // Single apply path — ALWAYS pending + late hook (the mechanism the
        // animated-cast states have used all along, now used for every 1p
        // frame). Why no direct path anymore: the frame's normal recompose runs
        // BEFORE OnCameraUpdate (log-verified: NiCamera::UpdateWorldData fires
        // ~immediately before the next OnCameraUpdate every frame), so a direct
        // write usually survives — EXCEPT when something dirties the camera
        // later in the frame (Quick Tune FOV/zoom preview, FOV transition
        // springs, head-bone propagation), which fires an EXTRA recompose after
        // our write and wipes the noise → "shake freezes while changing FOV,
        // then jerks back". Which frames get an extra fire is unpredictable
        // from here, and mode-switching direct↔deferred produced boundary
        // jerks every way it was tried. So don't predict: store pending and
        // let the late hook multiply it onto the freshly recomposed matrix at
        // EVERY fire. Each fire recomposes a clean base first, so the re-apply
        // is idempotent — it can never double and never drop, no matter how
        // many recomposes fire or when. In 1p the camera is animation-driven
        // every frame (breath idle keeps it dirty), so the hook fires every
        // frame; the noise renders one recompose late, imperceptible for
        // ambient shake. On a hypothetical fully-static frame with no fire the
        // previous render (static base + previous noise) simply persists.
        (void)a_useDeferredApply;
        (void)niCam;
        pendingNoiseRotation = noiseRotation;
        pendingNoiseValid  = true;
    }

    void CameraNoiseController::ApplyPending1pNoise(RE::NiCamera* a_niCam)
    {
        if (!a_niCam) return;
        // Head Bobbing rides the same late-hook apply but its own validity:
        // it must still land on a frame where every noise slider is zero.
        if (pendingBobValid) {
            if (std::abs(pendingBobPitch) > 1e-6f) {
                a_niCam->world.rotate = a_niCam->world.rotate *
                    MatrixFromAxisAngle(RE::NiPoint3{ 1.0f, 0.0f, 0.0f }, pendingBobPitch);
            }
            if (std::abs(pendingBobRoll) > 1e-6f) {
                a_niCam->world.rotate = a_niCam->world.rotate *
                    MatrixFromAxisAngle(RE::NiPoint3{ 0.0f, 1.0f, 0.0f }, pendingBobRoll);
            }
            if (std::abs(pendingBobYaw) > 1e-6f) {
                a_niCam->world.rotate = a_niCam->world.rotate *
                    MatrixFromAxisAngle(RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, pendingBobYaw);
            }
        }
        if (!pendingNoiseValid) {
            if (pendingBobValid) {
                using fn_bob = void(*)(RE::NiCamera*);
                static REL::Relocation<fn_bob> updateW2SBob{ REL::RelocationID(69271, 70641) };
                updateW2SBob(a_niCam);
            }
            return;
        }

        a_niCam->world.rotate = a_niCam->world.rotate * pendingNoiseRotation;
        using fn_t = void(*)(RE::NiCamera*);
        static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
        updateW2S(a_niCam);
        // Do NOT clear pendingNoiseValid here. The engine fires
        // UpdateWorldData on the player NiCamera multiple times within
        // a single frame's scene-graph propagation, and each call's
        // _originalNiCameraUpdateWorldData wipes world.rotate (recomposes
        // from local). If we cleared pending after the first apply, all
        // subsequent fires this frame would no-op and the last engine
        // wipe would win → no visible noise. Keeping pending valid lets
        // every fire re-multiply the same axes/thetas (idempotent on
        // the clean matrix produced by _original). OnCameraUpdate
        // clears pending at end of the 1p path when we're not on the
        // deferred path (i.e. nothing fresh to apply next frame).
        pendingJustApplied = true;
    }

    bool CameraNoiseController::Get1pNoiseDelta(RE::NiMatrix3& a_out) const
    {
        if (!m1pNoiseDeltaValid) return false;
        a_out = m1pNoiseDelta;
        return true;
    }

    void CameraNoiseController::Reapply1pNoiseOnTop(RE::NiCamera* a_niCam)
    {
        // Skip when a deferred apply owns this frame's noise (cast wind-up) —
        // ApplyPending1pNoise already re-applied it post-recompose, and doubling
        // here would over-rotate. Also no-op when there's no noise this frame.
        if (!a_niCam || pendingNoiseValid || !m1pNoiseDeltaValid) return;
        a_niCam->world.rotate = a_niCam->world.rotate * m1pNoiseDelta;
        using fn_t = void(*)(RE::NiCamera*);
        static REL::Relocation<fn_t> updateW2S{ REL::RelocationID(69271, 70641) };
        updateW2S(a_niCam);
    }

    float CameraNoiseController::UpdateDragonShake(RE::PlayerCharacter* a_player, float a_dt, float& a_outSpeed,
                                                  bool a_fp)
    {
        const auto& settings = SettingsManager::GetSingleton();
        // Which half of every source this pass reads (2026-09-06). The
        // envelope below is shared — only one POV branch runs per frame, so
        // a view switch mid-shake decays cleanly through the same state.
        auto A = [&](float tp, float fp) { return a_fp ? fp : tp; };
        const bool anyEnabled =
            A(settings.dragonShakeBreathAmp, settings.dragonShakeBreathAmpFp)     > 0.0f || A(settings.dragonShakeProjectileAmp, settings.dragonShakeProjectileAmpFp) > 0.0f ||
            A(settings.dragonShakeBiteAmp, settings.dragonShakeBiteAmpFp)       > 0.0f || A(settings.dragonShakeTailAmp, settings.dragonShakeTailAmpFp)       > 0.0f ||
            A(settings.dragonShakeWingAmp, settings.dragonShakeWingAmpFp)       > 0.0f || A(settings.dragonShakeLandingAmp, settings.dragonShakeLandingAmpFp)    > 0.0f ||
            A(settings.dragonShakeTakeoffAmp, settings.dragonShakeTakeoffAmpFp)    > 0.0f || A(settings.centurionShakeWalkAmp, settings.centurionShakeWalkAmpFp)    > 0.0f ||
            A(settings.centurionShakeMeleeAmp, settings.centurionShakeMeleeAmpFp)   > 0.0f || A(settings.centurionShakeSteamAmp, settings.centurionShakeSteamAmpFp)   > 0.0f;

        float targetA = 0.0f, targetS = 0.0f;
        DragonShakeKind srcKind = DragonShakeKind::None;
        SettingsManager::CinematicShakeChar srcChar{};
        if (anyEnabled && a_player) {
            float searchRange = 0.0f;
            if (A(settings.dragonShakeBreathAmp, settings.dragonShakeBreathAmpFp)     > 0.0f) searchRange = std::max(searchRange, A(settings.dragonShakeBreathRange, settings.dragonShakeBreathRangeFp));
            if (A(settings.dragonShakeProjectileAmp, settings.dragonShakeProjectileAmpFp) > 0.0f) searchRange = std::max(searchRange, A(settings.dragonShakeProjectileRange, settings.dragonShakeProjectileRangeFp));
            if (A(settings.dragonShakeBiteAmp, settings.dragonShakeBiteAmpFp)       > 0.0f) searchRange = std::max(searchRange, A(settings.dragonShakeBiteRange, settings.dragonShakeBiteRangeFp));
            if (A(settings.dragonShakeTailAmp, settings.dragonShakeTailAmpFp)       > 0.0f) searchRange = std::max(searchRange, A(settings.dragonShakeTailRange, settings.dragonShakeTailRangeFp));
            if (A(settings.dragonShakeWingAmp, settings.dragonShakeWingAmpFp)       > 0.0f) searchRange = std::max(searchRange, A(settings.dragonShakeWingRange, settings.dragonShakeWingRangeFp));
            if (A(settings.dragonShakeLandingAmp, settings.dragonShakeLandingAmpFp)    > 0.0f) searchRange = std::max(searchRange, A(settings.dragonShakeLandingRange, settings.dragonShakeLandingRangeFp));
            if (A(settings.dragonShakeTakeoffAmp, settings.dragonShakeTakeoffAmpFp)    > 0.0f) searchRange = std::max(searchRange, A(settings.dragonShakeTakeoffRange, settings.dragonShakeTakeoffRangeFp));
            if (A(settings.centurionShakeWalkAmp, settings.centurionShakeWalkAmpFp)    > 0.0f) searchRange = std::max(searchRange, A(settings.centurionShakeWalkRange, settings.centurionShakeWalkRangeFp));
            if (A(settings.centurionShakeMeleeAmp, settings.centurionShakeMeleeAmpFp)   > 0.0f) searchRange = std::max(searchRange, A(settings.centurionShakeMeleeRange, settings.centurionShakeMeleeRangeFp));
            if (A(settings.centurionShakeSteamAmp, settings.centurionShakeSteamAmpFp)   > 0.0f) searchRange = std::max(searchRange, A(settings.centurionShakeSteamRange, settings.centurionShakeSteamRangeFp));
            searchRange = std::max(50.0f, searchRange);

            const auto src = FindNearestDragonShakeSource(a_player, searchRange, a_fp);
            if (src.distance >= 0.0f && src.intensityMul > 0.0f) {
                const float linF    = std::clamp(1.0f - (src.distance / src.range), 0.0f, 1.0f);
                const float falloff = 0.5f * (linF + linF * linF);
                constexpr float kDragonAmpDrama   = 2.5f;
                constexpr float kDragonSpeedDrama = 1.4f;
                targetA = falloff * src.intensityMul * kDragonAmpDrama;
                targetS = src.speed * falloff * src.intensityMul * kDragonSpeedDrama;
                srcKind = src.kind;
                srcChar = src.chr;
            }
        }

        // Asymmetric attack/decay (rising edge snaps; falling edge uses the
        // kind-specific tau). When disabled / no source, targetA is 0 so this
        // simply decays the envelope toward zero — a POV switch mid-shake
        // still settles cleanly.
        const int v = a_fp ? 1 : 0;
        if (targetA >= dragonShakeAmp[v]) {
            dragonShakeAmp[v]   = targetA;
            dragonShakeSpeed[v] = targetS;
            dragonShakeKind[v]  = static_cast<std::uint8_t>(srcKind);
            sDragonShakeCharOut[v] = srcChar;
        } else {
            // Per-source Fade Duration controls the decay tail (was per-kind).
            const float decayTau = std::max(0.05f, sDragonShakeCharOut[v].fadeDuration);
            const float alpha = 1.0f - std::exp(-a_dt / decayTau);
            dragonShakeAmp[v]   += (targetA - dragonShakeAmp[v])   * alpha;
            dragonShakeSpeed[v] += (targetS - dragonShakeSpeed[v]) * alpha;
            if (dragonShakeAmp[v] < 0.0001f) {
                dragonShakeAmp[v]  = 0.0f;
                dragonShakeKind[v] = static_cast<std::uint8_t>(DragonShakeKind::None);
            }
        }
        a_outSpeed = dragonShakeSpeed[v];
        return dragonShakeAmp[v];
    }

    void CameraNoiseController::OnCameraUpdate(RE::TESCamera* a_camera)
    {
        CameraEffectClock::Sync();
        const auto hitRotation = HitShakeController::Update(a_camera && a_camera->cameraRoot);
        const bool hitActive = std::abs(hitRotation.pitch) > 1e-7f || std::abs(hitRotation.yaw) > 1e-7f ||
                               std::abs(hitRotation.roll) > 1e-7f;
        EnsureBowShotSubscription();
        // Consume before suppression/POV/enable gates. A shot made in a menu,
        // first person, or with repulse disabled cannot replay on returning.
        const auto bowShot = PlayerBowShotSink::Get().Take();
        static std::uint32_t seenArrowCounter = 0;
        const bool arrowGraphRelease = seenArrowCounter != sArrowReleaseCounter;
        seenArrowCounter = sArrowReleaseCounter;
        if (!a_camera || !a_camera->cameraRoot) return;

        // Lazy graph-sink subscription. Idempotent — sGraphSubscribed
        // guards repeat calls. Needs to be deferred to runtime because
        // the player's animation graph manager doesn't exist at plugin
        // load time. Calling here means we subscribe the first time
        // the camera ticks with the player loaded.
        EnsureTransformGraphSubscription();

        auto& settings = SettingsManager::GetSingleton();
        if (settings.diagnosticSuspendOverrides) {
            sNpcArcheryShots.SetActive(false);
            hasLastTick = false;
            return;
        }
        // Attack Duration window — polled unconditionally, ahead of every
        // early-out below, so it always sees the swing's falling edge.
        const float atkElapsed = PollAttackNoiseElapsed();
        // Allow dragon shake to run even if base camera noise is disabled
        // — it's an opt-in cinematic effect, on whenever any source Intensity > 0.
        // EITHER half on is enough: this only guards an early-out, and the
        // view that actually sizes the shake is chosen inside
        // UpdateDragonShake (2026-09-06 per-view split).
        const bool anyDragonShakeEnabled =
            settings.dragonShakeBreathAmp > 0.0f || settings.dragonShakeBreathAmpFp > 0.0f ||
            settings.dragonShakeProjectileAmp > 0.0f || settings.dragonShakeProjectileAmpFp > 0.0f ||
            settings.dragonShakeBiteAmp > 0.0f || settings.dragonShakeBiteAmpFp > 0.0f ||
            settings.dragonShakeTailAmp > 0.0f || settings.dragonShakeTailAmpFp > 0.0f ||
            settings.dragonShakeWingAmp > 0.0f || settings.dragonShakeWingAmpFp > 0.0f ||
            settings.dragonShakeLandingAmp > 0.0f || settings.dragonShakeLandingAmpFp > 0.0f ||
            settings.dragonShakeTakeoffAmp > 0.0f || settings.dragonShakeTakeoffAmpFp > 0.0f ||
            settings.centurionShakeWalkAmp > 0.0f || settings.centurionShakeWalkAmpFp > 0.0f ||
            settings.centurionShakeMeleeAmp > 0.0f || settings.centurionShakeMeleeAmpFp > 0.0f ||
            settings.centurionShakeSteamAmp > 0.0f || settings.centurionShakeSteamAmpFp > 0.0f;
        // Effective transformation-beat intensity: the active place's copy
        // (Location Override on the beat row) replaces the global slider
        // wholesale, so every gate below asks through this. Index order is
        // kFxBeatLocKeys (WW T/R, VL T/R, Feeding, Bats).
        auto effBeatInt = [&](int i, float g) {
            auto* p = settings.ActiveLocationFxBeat(SettingsManager::kFxBeatLocKeys[i]);
            return p ? p->intensity : g;
        };
        const bool werewolfTransformActive    = effBeatInt(0, settings.werewolfTransformIntensity)    > 0.0001f ||
                                                effBeatInt(1, settings.werewolfRevertIntensity)       > 0.0001f;
        const bool vampireLordTransformActive = effBeatInt(2, settings.vampireLordTransformIntensity) > 0.0001f ||
                                                effBeatInt(3, settings.vampireLordRevertIntensity)    > 0.0001f;
        // EITHER half on is enough to keep the subscription and the frame
        // alive; which half actually shapes the beat is picked below, once the
        // view is known. A superset here only costs an early-out we skip.
        const bool weaponDrawShakeActive      = settings.weaponDrawNoiseIntensity      > 0.0001f ||
                                                settings.weaponDrawNoiseIntensityFp    > 0.0001f;
        // Bats / Reanimation / Summoning. Subscribing is cheap and idempotent,
        // but it's still gated on at least one of the three being turned on so
        // an untouched install never adds a sink it will never read.
        const bool tableBeatsActive = settings.AnyEventBeatActive() ||
                                      effBeatInt(4, 0.0f) > 0.0001f;
        const bool npcNoiseActive = NpcSourceEnabled(NpcNoise::Source::Magic) || NpcSourceEnabled(NpcNoise::Source::Shouts);
        if (!NpcSourceEnabled(NpcNoise::Source::Shouts))
            for (auto& [_, memory] : sNpcMemory) memory.shoutBeat = {};
        const bool npcCombatActive = NpcSourceEnabled(NpcNoise::Source::Melee) ||
                                     NpcSourceEnabled(NpcNoise::Source::Archery) ||
                                     NpcSourceEnabled(NpcNoise::Source::Transformations);
        if (!npcCombatActive) sNpcCombatMemory.clear();
        sNpcArcheryShots.SetActive(NpcSourceEnabled(NpcNoise::Source::Archery) && !CameraEffectClock::IsPaused());
        if (!NpcSourceEnabled(NpcNoise::Source::Transformations)) sNpcTransformBeat = {};
        const bool eventBeatsActive = effBeatInt(5, settings.vampireLordBatsIntensity) > 0.0001f ||
                                      settings.reanimateShakeIntensity   > 0.0001f ||
                                      settings.reanimateShakeIntensityFp > 0.0001f ||
                                      settings.summonShakeIntensity      > 0.0001f ||
                                      settings.summonShakeIntensityFp    > 0.0001f ||
                                      tableBeatsActive ||
                                      npcNoiseActive ||
                                      npcCombatActive ||
                                      werewolfTransformActive ||
                                      vampireLordTransformActive;
        if (eventBeatsActive) EnsureMagicBeatSubscription();
        // The table-driven one-shots' queue — armed the same lazy way, then
        // the pending events drained into the pool.
        EventBeatSources::SetActive(tableBeatsActive);
        if (tableBeatsActive) {
            EventBeatSources::BeatEvent evs[16];
            const std::size_t n = EventBeatSources::Drain(evs, 16);
            for (std::size_t i = 0; i < n; ++i) {
                const auto idx = static_cast<std::size_t>(evs[i].id);
                if (idx >= kEventBeatCount) continue;
                float drainInten = settings.eventBeats[idx].intensity;
                if (idx == static_cast<std::size_t>(BeatId::WerewolfFeed))
                    drainInten = effBeatInt(4, drainInten);
                if (drainInten <= 0.0001f) continue;
                auto& b        = sTableBeats[idx];
                b.rearmPending = true;   // resume-not-restart, like Arm()
                b.hasPos       = evs[i].hasPos;
                b.pos          = evs[i].pos;
                b.armScale     = evs[i].scale;
                b.falloffFloor = evs[i].playerOwned ? 0.35f : 0.0f;

            }
        }

        auto* playerCam = RE::PlayerCamera::GetSingleton();

        const bool in1p = playerCam && playerCam->IsInFirstPerson();
        // Sheathe / unsheathe: which half of the source this view reads. The
        // beat is one event with two tunings (2026-09-06) — the arming latch
        // and the envelope both follow the view, so turning the effect off in
        // one view behaves exactly as turning off the single slider used to.
        const float drawPovIntensity = in1p ? settings.weaponDrawNoiseIntensityFp
                                            : settings.weaponDrawNoiseIntensity;
        const float drawPovDuration  = in1p ? settings.weaponDrawNoiseDurationFp
                                            : settings.weaponDrawNoiseDuration;
        const bool  drawPovActive    = drawPovIntensity > 0.0001f;
        // Weapon draw / put-away edges must be sampled every frame or a fast
        // animation's transient state is missed entirely.
        PollWeaponDrawEdges(in1p, drawPovActive);
        // 1p noise is gated on the Camera Noise sub-toggle in the Global
        // tab. When off, fpProfile stays null and the spring decays
        // toward zero, restoring vanilla 1p with no shake.
        SettingsManager::FirstPersonProfile* fpProfile =
            (in1p && settings.firstPersonNoiseEnabled)
                ? settings.ResolveFirstPersonNoiseProfile()
                : nullptr;
        // Attack cells in FIRST PERSON are AMBIENT, not one-shots (user
        // ruling 2026-08-30 "go back to the old way where it was based on
        // the state being active" — the per-swing Duration model could
        // never fit the variation across modded power attacks). The cell's
        // noise applies for as long as the attack key resolves — the
        // animation bracket plus the linger — then hands back to the
        // parent through the normal ambient crossfade, like every other FP
        // state. 3p keeps its one-shot model; the 1p apply-site envelope
        // below simply never arms from here anymore.

        // While in 1p, always reach the spring loop so amp can decay
        // toward zero on enabled→disabled transitions. The spring's
        // own < ~0 check skips the actual rotation write when nothing
        // is happening, so the work is cheap when idle.
        if (!settings.noiseEnabled &&
            !in1p &&
            !anyDragonShakeEnabled &&
            !werewolfTransformActive &&
            !vampireLordTransformActive &&
            !weaponDrawShakeActive &&
            !eventBeatsActive && !hitActive) return;

        const bool transformShakeQueued = sWWGraphSignalLatch || sVLGraphSignalLatch ||
                                          sWWEnvelopeActive || sVLEnvelopeActive;
        // Pre-suppression diagnostic — capture camera-state-id and any
        // suppression decision so we can see if the 1p noise branch is
        // being gated out during FoF wind-up.
        {
            static int  sLastLoggedCamState = -2;
            static bool sLastLoggedSuppressed = false;
            int curId = -1;
            if (playerCam && playerCam->currentState) {
                curId = static_cast<int>(playerCam->currentState->id);
            }
            const bool suppNow = IsSuppressed(playerCam, transformShakeQueued);
            if (curId != sLastLoggedCamState || suppNow != sLastLoggedSuppressed) {
                spdlog::debug("[FoFDiag2] camState={} in1p={} suppressed={} fpProfile={} firstPersonNoiseEnabled={}",
                             curId, in1p, suppNow, fpProfile ? "set" : "NULL",
                             settings.firstPersonNoiseEnabled);
                sLastLoggedCamState  = curId;
                sLastLoggedSuppressed = suppNow;
            }
        }
        if (IsSuppressed(playerCam, transformShakeQueued)) {
            hasLastTick = false;
            sNpcArcheryShots.SetActive(false);
            // Keep observed phases across menu/POV handoffs so an interrupted
            // swing does not become a fresh hit on return. Cancel its effect.
            for (auto& [_, memory] : sNpcCombatMemory) {
                memory.beat = {};
                memory.spellBeat = {};
            }
            for (auto& [_, memory] : sNpcMemory) memory.shoutBeat = {};
            // Drop the 1p head bob outright: this branch returns before the
            // POV split, so without this the late hook would keep re-applying
            // the last frame's tilt for as long as the menu / cutscene holds
            // the camera — a frozen lean, not a bob.
            pendingBobValid = false;
            pendingBobPitch = 0.0f;
            pendingBobRoll  = 0.0f;
            pendingBobYaw   = 0.0f;
            // Entry-edge log. The previous gate tested sSuppressFade >= 1.0
            // AFTER decrementing it, so it could never be true and the log was
            // silent — which left it unknown whether this path even runs while
            // a menu holds the camera. Log the edge itself.
            {
                if (!sSuppressWasSupp) {
                    const float tl = std::sqrt(sTailTrans.x * sTailTrans.x +
                                               sTailTrans.y * sTailTrans.y +
                                               sTailTrans.z * sTailTrans.z);
                    spdlog::debug("[SUPPRESS] enter: |trans|={:.3f} fade={:.2f} root={}",
                                 tl, sSuppressFade,
                                 (a_camera && a_camera->cameraRoot) ? "ok" : "NULL");
                }
                sSuppressWasSupp = true;
            }
            // Walk the last applied offset out instead of dropping it. See the
            // note on sTailTrans: without this the camera steps back to
            // unshaken in a single frame the moment the menu cam takes over.
            // Frozen, never re-sampled, and it reaches exactly zero — so
            // nothing evolves on the menu's unstable cameraRoot and nothing is
            // left behind afterwards.
            if (sSuppressFade > 0.0f && a_camera && a_camera->cameraRoot) {
                const auto tNow = CameraEffectClock::Now();
                float sdt = 1.0f / 60.0f;
                if (sSuppressHasTick) {
                    sdt = std::chrono::duration<float>(tNow - sSuppressTick).count();
                    sdt = std::clamp(sdt, 0.0f, 0.1f);
                }
                sSuppressTick    = tNow;
                sSuppressHasTick = true;
                if (CameraEffectClock::IsPaused()) sdt = 0.0f;
                constexpr float kSuppressTailSec = 0.12f;
                sSuppressFade -= sdt / kSuppressTailSec;
                if (sSuppressFade < 0.0f) sSuppressFade = 0.0f;
                const float f = sSuppressFade * sSuppressFade * (3.0f - 2.0f * sSuppressFade);
                auto& sl = a_camera->cameraRoot->local;
                sl.translate.x += sTailTrans.x * f;
                sl.translate.y += sTailTrans.y * f;
                sl.translate.z += sTailTrans.z * f;
                const float rl = std::sqrt(sTailRotVec.x * sTailRotVec.x +
                                           sTailRotVec.y * sTailRotVec.y +
                                           sTailRotVec.z * sTailRotVec.z);
                if (rl > 1.0e-6f) {
                    const RE::NiPoint3 ax{ sTailRotVec.x / rl, sTailRotVec.y / rl,
                                           sTailRotVec.z / rl };
                    sl.rotate = sl.rotate * MatrixFromAxisAngle(ax, rl * f);
                }
                (void)rl;
            }
            return;
        }
        // Not suppressed — arm the tail for whenever suppression next begins.
        sSuppressFade    = 1.0f;
        sSuppressHasTick = false;
        sSuppressWasSupp = false;   // re-arms the entry-edge log

        // Wall time drives the noise texture; held effect time drives durations
        // and envelope/blend progress. They must not share a zeroed delta in QT.
        const auto wallNow = std::chrono::steady_clock::now();
        float realDelta = 1.0f / 60.0f;
        if (hasLastTick) {
            realDelta = std::chrono::duration<float>(wallNow - lastTick).count();
            realDelta = std::clamp(realDelta, 1.0f / 240.0f, 0.05f);
        }
        lastTick = wallNow;
        hasLastTick = true;
        const bool effectsPaused = CameraEffectClock::IsPaused();
        const bool mainNoisePreview = MenuUI::IsMainMenuOpen();
        const auto frame = EffectFrame::FromRealDelta(realDelta, effectsPaused,
            MenuUI::IsQuickTuneWindowOpen(), mainNoisePreview);
        const float dt = frame.envelopeDelta;
        const float noiseDt = frame.noiseDelta;
        static std::uint64_t sConsumedNoiseEditRevision = 0;
        const auto noiseEditRevision = sLiveNoiseEditRevision.load(std::memory_order_relaxed);
        // Main-menu compact, cinematic and specific-item editors do not all
        // emit the Quick Tune edit notification. While previewing, a changed
        // target is an edit too; retunePausedValue still leaves every unchanged
        // filter/velocity alone and does not advance the held action's time.
        const bool pausedNoiseEdit = effectsPaused &&
            (noiseEditRevision != sConsumedNoiseEditRevision || mainNoisePreview);
        sConsumedNoiseEditRevision = noiseEditRevision;

        // Jumping arc (Cinematic Effects) — polled here, ahead of the POV
        // split, because its launch trigger and airborne latch are edge
        // detectors that must see EVERY frame regardless of view. Both
        // branches below consume the result.
        const JumpArcState jumpArc =
            PollJumpArc(RE::PlayerCharacter::GetSingleton(), dt,
                        in1p ? settings.jumpNoiseAmpFp    : settings.jumpNoiseAmp,
                        in1p ? settings.fallNoiseAmpFp    : settings.fallNoiseAmp,
                        in1p ? settings.jumpRepulseFp     : settings.jumpRepulse);

        // Sheathe / unsheathe beat envelope — also POV-independent (its latch
        // is armed by the weapon-state edge poll above, which runs every frame
        // in both views).
        const float drawBeatEnv = PollWeaponDrawEnvelope(drawPovActive, drawPovDuration);

        // Bats / Reanimation / Summoning — resolved here for the same reason:
        // the envelopes are time-based off an event latch that can arrive in
        // either view, and both branches below consume the winner.
        ScanNpcCombat(RE::PlayerCharacter::GetSingleton());
        PollNpcArcheryShots(RE::PlayerCharacter::GetSingleton());
        const EventBeatResult eventBeatNow =
            PollEventBeats(RE::PlayerCharacter::GetSingleton(), in1p);
        if (eventBeatNow.amp > 0.001f && eventBeatNow.src &&
            (std::string_view(eventBeatNow.src) == "npc-archery" || std::string_view(eventBeatNow.src) == "npc-shout")) {
            static unsigned logged = 0;
            static std::chrono::steady_clock::time_point lastLog{};
            const auto now = std::chrono::steady_clock::now();
            if (logged < 24 && std::chrono::duration<float>(now - lastLog).count() >= 0.5f) {
                ++logged; lastLog = now;
                spdlog::debug("[NPCNOISE] contributing source={} view={} amp={:.3f}",
                    eventBeatNow.src, in1p ? "1p" : "3p", eventBeatNow.amp);
            }
        }


        // NPC casting — same treatment (the walk also runs the voiceState
        // shout fallback, which must see every frame).
        const NpcCastResult npcNow =
            ScanNpcCasters(RE::PlayerCharacter::GetSingleton(), dt);

        // Head Bobbing — POV-independent for the same reason as the jump arc:
        // its phase is an integrator and its rate comes from a position delta.
        const HeadBobState headBob =
            PollHeadBob(RE::PlayerCharacter::GetSingleton(), dt);

        // POV edge, read before the branch because the 1p path returns from the
        // middle of this function and never reaches a common tail.
        static bool sWasIn1pLastFrame = false;
        const bool  wasIn1p           = sWasIn1pLastFrame;
        sWasIn1pLastFrame             = in1p;

        // First-person branch — uses the per-state profile resolved
        // above so each weapon/movement state can carry its own noise
        // envelope. IsSuppressed already gated us in here.
        if (in1p) {
            const auto retunePausedValue = [pausedNoiseEdit](float a_target, float& a_previousTarget, float& a_current) {
                const bool edited = pausedNoiseEdit && a_target != a_previousTarget;
                if (edited) a_current = a_target;
                a_previousTarget = a_target;
                return edited;
            };
            static float sFpPreviousAmpTarget = 0.0f;
            static float sFpPreviousDirectTarget = 0.0f;
            static float sFpPreviousFlowTarget = 0.0f;
            static float sFpPreviousRotTarget = 0.0f;
            static float sFpPreviousDjTarget = 0.35f;
            static float sFpPreviousRghTarget = 0.45f;
            static float sFpAmpCurrent   = 0.0f;
            // Velocity of the amp spring. The amplitude is a CRITICALLY DAMPED
            // SPRING now, not an exp-lerp (see the omega block below), so it
            // carries state between frames the way the 1p FOV does. Cleared on
            // the 3p->1p edge: a stale velocity from whatever was happening the
            // last time we were in first person would kick the first frame back.
            static float sFpAmpVel       = 0.0f;
            if (!wasIn1p) sFpAmpVel = 0.0f;
            // TARGET sample rate. This is no longer the rendered rate — the
            // flow governor below owns that. Every writer here may step it
            // freely; nothing downstream sees the step.
            static float sFpSpeedCurrent = 0.0f;
            // RENDERED sample rate — the only value the Perlin clock ever
            // advances at. See the flow governor near the apply site. Seeded on
            // the 3p->1p edge because it is a static: without that, coming back
            // to first person minutes after leaving it mid-cast would resume at
            // the cast-era rate and spend most of a second winding down from a
            // tempo nothing in the scene is asking for.
            static float sFpFlowSm       = 0.0f;
            static bool  sFpFlowSeeded   = false;
            if (!wasIn1p) sFpFlowSeeded = false;
            // Countdown that keeps the attack rate floor engaged for a short
            // release window after the Attack sub-state drops (see the floor
            // block further down).
            static float sFpAttackFastTimer = 0.0f;
            // FoF tracking: live charge progress (0..1), last frame's
            // progress for fire-edge detection, and a decaying burst
            // envelope kicked when a spell fires.
            static float sFoFLastProgress = 0.0f;
            static float sFoFBurstElapsed = 999.0f;  // > duration = inactive
            static float sFoFBurstPeak    = 0.0f;
            // School + ritual flag captured at trigger time so the
            // release curve doesn't depend on what the resolver thinks
            // mid-burst (the resolver flips through transitional states
            // post-fire). Drives per-school recoil character and the
            // ritual size boost.
            static MagicSchool sFoFBurstSchool   = MagicSchool::None;
            static bool        sFoFBurstIsRitual = false;
            // Per-hand caster-state tracking. The chargeProgress falling-
            // edge isn't sufficient: with dual-cast (both hands held) the
            // bestTimer = max(L, R) stays high if only one hand releases.
            // Track each hand's last state directly; a transition from
            // kReady (held charged) to kNone/kCasting on EITHER hand
            // counts as a release event and triggers the burst.
            using CState = RE::MagicCaster::State;
            static CState sLastLeftCasterState  = CState::kNone;
            static CState sLastRightCasterState = CState::kNone;
            // Cached school cell during the charge windup. At the fire
            // frame the resolver instantly drops back to the bucket root
            // (cast state → None) and `fpProfile` no longer points at the
            // school cell — using that for the burst peak / pitch kick
            // would silently fall back to global/bucket-root values.
            // Caching the charge-time profile preserves the school's
            // amp + tilt + wobble across the edge.
            static std::optional<SettingsManager::FirstPersonProfile> sFoFChargeProfile;
            // Repulse multiplier (from the firing cell's per-FoF
            // slider) cached at charge time. Scales the release burst
            // peak and recoil pitch kick. Default 0 = no burst;
            // >0 enables; >1 stronger kick.
            static float sFoFChargeRepulse = 0.0f;
            // Staff vs spell flag cached at charge time. Staves have
            // a longer wind-down between the engine fire event and
            // the projectile visibly leaving the hand than spells,
            // so the burst pre-delay needs to be longer for them
            // (otherwise the kick lands during the cast pose).
            static bool sFoFChargeIsStaff = false;
            // Per-FoF-cell fade duration (seconds) cached at charge
            // time. Added on top of the 0.6s hold to form the total
            // post-fire tail. Default 0 = no fade beyond the hold.
            static float sFoFChargeFadeDuration = 0.0f;
            // Tier multiplier of the spell being charged. Captured
            // during the charge window so the burst at release uses
            // the firing spell's tier even though the caster's
            // currentSpell may already be cleared by then. 1.0 when
            // spell-level scaling is off.
            static float sFoFChargeTierMul = 1.0f;
            // Per-shot recoil pool. A follow-up cast arms the most-settled
            // slot instead of restarting a live one, so rapid fire rings
            // through rather than collapsing mid-swing. Every field is rolled
            // and fully scaled at arm time; the render loop never reads a live
            // profile. elapsed >= 1e8 marks a free slot.
            struct FpShot
            {
                float zeta      = 0.62f;
                float omega     = 16.0f;
                float wd        = 0.78f;
                float norm      = 1.0f;
                float tPeak     = 0.07f;
                float ax        = 1.0f;
                float ay        = 0.0f;
                float az        = 0.0f;
                float peakPitch = 0.0f;
                float burstPeak = 0.0f;
                float harmAmp   = 0.0f;
                float harmMul   = 2.0f;
                float elapsed   = 1.0e9f;   // settled / free
            };
            static FpShot sFpShots[3];

            // Ritual selects the profile, while native casting decides the
            // envelope. A Lightning Storm staff uses Ritual settings with
            // continuous beam timing, without a fire-and-forget charge/tail.
            const auto castType = StateResolver::GetSingleton().GetCastType();
            const bool isFoF =
                (castType == CastType::FireAndForget || castType == CastType::Ritual) &&
                !StateResolver::GetSingleton().IsConcentrationCast();
            const bool isRitual = castType == CastType::Ritual;

            // Read per-hand caster state + state-based charge progress.
            // Progress comes from caster.state, NOT castingTimer:
            //   - kCharging: timer/chargeTime (ramps 0..1)
            //   - kReady:    1.0 (locked at peak while held)
            //   - kCasting:  1.0 (still firing)
            //   - kNone:     0.0
            // The timer-only path collapsed to 0 the moment the engine
            // transitioned out of kCharging into kReady (timer resets
            // before the held-charge state), which is what made the
            // shake drop mid-cast and only "work" briefly during windup.
            using Src = RE::MagicSystem::CastingSource;
            auto* player = RE::PlayerCharacter::GetSingleton();
            CState curLeftState  = CState::kNone;
            CState curRightState = CState::kNone;
            float chargeProgress = 0.0f;
            // Per-hand FoF-spell flag, captured this frame so fire-edge
            // detection later can ignore non-FoF transitions (e.g. a
            // Concentration cast starting while we're in dual-cast).
            bool curLeftIsFoF  = false;
            bool curRightIsFoF = false;

            // Hoisted out of the `if (player)` block below so the
            // dual-cast overlay further down can reuse it. The engine
            // cycles a FoF caster through several charging sub-states
            // before reaching kReady (kUnk01, kUnk02, kUnk04,
            // kCharging). castingTimer COUNTS DOWN from chargeTime
            // to 0 during the windup; invert for progress. 0.4 floor
            // gives an instant-on hit at trigger press.
            auto handProgress = [](RE::MagicCaster* caster, CState cs) -> float {
                if (cs == CState::kNone)    return 0.0f;
                if (cs == CState::kCasting) return 1.0f;
                if (cs == CState::kReady)   return 1.0f;
                if (cs == CState::kUnk07 || cs == CState::kUnk08 || cs == CState::kUnk09)
                    return 0.0f;
                float raw = 0.0f;
                if (caster && caster->currentSpell) {
                    if (auto* spell = caster->currentSpell->As<RE::SpellItem>()) {
                        const float ct = (std::max)(0.1f, spell->GetChargeTime());
                        raw = 1.0f - std::clamp(caster->castingTimer / ct, 0.0f, 1.0f);
                    }
                }
                return (std::max)(raw, 0.4f);
            };

            if (player) {
                // Detect FoF for both spell casts AND staff fires. Staff
                // casters set currentSpell to an EnchantmentItem (not a
                // SpellItem); As<SpellItem>() returns null there. Calling
                // GetCastingType() on the MagicItem base covers both,
                // so curLeftIsFoF / curRightIsFoF now fire correctly for
                // staves and the fire-edge detection downstream triggers
                // the burst envelope and Repulse / Fade Duration scaling.
                auto isFoFSpell = [](RE::MagicCaster* caster) -> bool {
                    if (!caster || !caster->currentSpell) return false;
                    return caster->currentSpell->GetCastingType() ==
                           RE::MagicSystem::CastingType::kFireAndForget;
                };
                for (auto src : { Src::kLeftHand, Src::kRightHand }) {
                    auto* caster = player->GetMagicCaster(src);
                    if (!caster) continue;
                    const auto cs = caster->state.get();
                    const bool fof = isFoFSpell(caster);
                    if (src == Src::kLeftHand) { curLeftState  = cs; curLeftIsFoF  = fof; }
                    else                       { curRightState = cs; curRightIsFoF = fof; }
                    chargeProgress = (std::max)(chargeProgress, handProgress(caster, cs));
                }
                // DIAGNOSTIC — per-state-change log for the FoF investigation.
                // Logs the state pair, charge progress, isFoF, and the
                // active spell type/school. Fires only when something
                // material changes so we don't spam the log idle frames.
                {
                    static CState sLogLastL = (CState)255;
                    static CState sLogLastR = (CState)255;
                    static bool   sLogLastFoF = false;
                    static int    sLogLastBucket = -1;
                    auto* sr2  = &StateResolver::GetSingleton();
                    const int bucket = static_cast<int>(chargeProgress * 10);
                    const bool changed =
                        curLeftState  != sLogLastL ||
                        curRightState != sLogLastR ||
                        isFoF         != sLogLastFoF ||
                        bucket        != sLogLastBucket;
                    if (changed) {
                        // Identify spell-type and currentSpell status per hand
                        auto descHand = [&](Src s) -> std::string {
                            auto* c = player->GetMagicCaster(s);
                            if (!c) return "none";
                            std::string out;
                            out += "st=";
                            out += std::to_string(static_cast<int>(c->state.get()));
                            out += " timer=";
                            out += std::to_string(c->castingTimer);
                            if (!c->currentSpell) {
                                out += " spell=NULL";
                            } else if (auto* spell = c->currentSpell->As<RE::SpellItem>()) {
                                out += " type=";
                                out += std::to_string(static_cast<int>(spell->GetCastingType()));
                                out += " ct=";
                                out += std::to_string(spell->GetChargeTime());
                            } else {
                                out += " non-spell";
                            }
                            return out;
                        };
                        spdlog::debug("[FoFDiag] L({}) R({}) prog={:.2f} isFoF={} sr.ct={} sr.sch={} fpAmp={:.2f}",
                            descHand(Src::kLeftHand),
                            descHand(Src::kRightHand),
                            chargeProgress, isFoF,
                            static_cast<int>(sr2->GetCastType()),
                            static_cast<int>(sr2->GetSchool()),
                            fpProfile ? fpProfile->noise.amp : -1.0f);
                        sLogLastL = curLeftState;
                        sLogLastR = curRightState;
                        sLogLastFoF = isFoF;
                        sLogLastBucket = bucket;
                    }
                }
            }

            // (Spell-tier scaling removed 2026-08-01 — the 1p magic shake no
            // longer varies with the spell's Novice/Master tier. The cell's
            // own Intensity is the only amplitude authority again, so a
            // school cell reads the same whichever rank of spell fires it.)

            // Refresh the cache continuously during the pre-fire
            // window — every frame where a FoF cast is active AND the
            // post-fire tail hasn't started yet (sFoFBurstElapsed >=
            // kPostFireTailDuration, i.e., timer at its cap). Once
            // fire-edge sets sFoFBurstElapsed = -0.13, the cache locks
            // until the tail finishes.
            //
            // Why not cycle-leading-edge only: cycleStart requires
            // isFoF to be true on the exact frame the hand transitions
            // out of kNone, but cachedStaffCastType inside the
            // StateResolver can lag by one frame, so the leading-edge
            // check sometimes saw isFoF=false and skipped the update —
            // leaving stale cache from the previous customized cast.
            // Updating across the entire pre-fire window catches
            // every case regardless of which frame isFoF flips true.
            auto isCasterActiveLocal = [](CState s) {
                return s != CState::kNone &&
                       s != CState::kUnk07 && s != CState::kUnk08 &&
                       s != CState::kUnk09;
            };
            const bool anyActive =
                isCasterActiveLocal(curLeftState) ||
                isCasterActiveLocal(curRightState);
            const float kHoldEndForCache = 0.6f;
            const float kCacheTailDur = kHoldEndForCache + sFoFChargeFadeDuration;
            const bool preFireWindow = sFoFBurstElapsed >= kCacheTailDur;
            if (isFoF && anyActive && preFireWindow) {
                sFoFChargeProfile = SnapshotProfile(fpProfile);
                sFoFChargeRepulse = fpProfile ? fpProfile->repulse : 0.0f;
                sFoFChargeFadeDuration = fpProfile ? fpProfile->fofFadeDuration : 0.0f;
                // Detect staff fire: any active caster whose
                // currentSpell is NOT a SpellItem (it's an
                // EnchantmentItem). Cached so the fire-edge pre-delay
                // can use the right offset.
                bool isStaffFire = false;
                if (player) {
                    for (auto src : { Src::kLeftHand, Src::kRightHand }) {
                        auto* caster = player->GetMagicCaster(src);
                        if (!caster) continue;
                        if (caster->state.get() == CState::kNone) continue;
                        auto* magicItem = caster->currentSpell;
                        if (!magicItem) continue;
                        if (magicItem->As<RE::SpellItem>() == nullptr) {
                            isStaffFire = true;
                            break;
                        }
                    }
                }
                sFoFChargeIsStaff = isStaffFire;
                // Spell tier no longer scales the release burst — the cached
                // multiplier is pinned to 1.0 so every downstream user of it
                // (the repulse peak, the burst envelope) is a plain pass-through.
                sFoFChargeTierMul = 1.0f;
            }


            // Per-hand fire-edge detection. Gated per-hand on the hand
            // having an FoF spell — without this gate, a Concentration
            // cast STARTING (kCharging → kCasting on its hand) would
            // wrongly trigger an FoF burst during dual-cast scenarios.
            //
            // The FoF cycle per reference_fof_caster_states.md is:
            //   kNone → kUnk01 → kUnk02 → kReady → kUnk04 → kNone
            // FoF spells SKIP kCasting entirely, and the kReady→kNone
            // transition goes through kUnk04 — so both the legacy
            // "transition INTO kCasting" trigger and the "kReady→kNone
            // direct" trigger silently never fired for FoF. The release
            // edge below catches ANY transition out of kReady, which
            // covers every FoF release path (→kUnk04, →kNone,
            // →kCasting for the rare spell that uses it). The fire
            // edge stays for tap-fire / non-FoF-cycle scenarios.
            auto isCasting = [](CState s) { return s == CState::kCasting; };
            const bool leftFiredEdge   =
                curLeftIsFoF  && !isCasting(sLastLeftCasterState)  && isCasting(curLeftState);
            const bool rightFiredEdge  =
                curRightIsFoF && !isCasting(sLastRightCasterState) && isCasting(curRightState);
            const bool leftReleaseEdge  =
                curLeftIsFoF  &&
                sLastLeftCasterState  == CState::kReady && curLeftState  != CState::kReady;
            const bool rightReleaseEdge =
                curRightIsFoF &&
                sLastRightCasterState == CState::kReady && curRightState != CState::kReady;
            const bool anyFireEvent =
                leftFiredEdge || rightFiredEdge || leftReleaseEdge || rightReleaseEdge;
            if (anyFireEvent && sFoFChargeProfile) {
                // Pre-delay: the engine flips state to kUnk04 at the
                // animation kickoff, well before the projectile is
                // visually past the hand. Starting the timer negative
                // shifts the burst+recoil into the window where the
                // projectile is actually flying, so the camera kick
                // syncs with the visible release instead of preceding it.
                // Staves animate the release noticeably later than
                // spells — the projectile clears the staff tip about
                // ~150ms after the engine's fire event — so they need
                // a longer pre-delay to keep the kick aligned with
                // the visible launch.
                sFoFBurstElapsed = sFoFChargeIsStaff ? -0.40f : -0.13f;
                const float peakAmp = sFoFChargeProfile->noise.amp;
                // Release swell, scaled off the school's own amp. No floor: a
                // school with low amp gets a soft release, one with high amp
                // gets a punchy one. Rituals get a 1.7× boost on top — master-
                // tier spells should feel like a Big Deal when they go off, but
                // still carry the school's vibe.
                //
                // THE MULTIPLIER USED TO BE 7. On a cell amp of 5 that put the
                // ambient texture at 8× for about a third of a second and then
                // dropped it back, and amplitude-modulating a zero-mean noise
                // texture is not motion with a direction — it is the same wobble
                // made loud. So the loud part read as "shaking" and the return
                // to normal read as the camera STOPPING, which is the "noise
                // stops and snaps" that survived every fix aimed at the clock.
                // The [MOTION] meter had been saying so all along: it tripped on
                // every single cast with the rotation amplitude stepping ~45% of
                // full scale in one frame.
                //
                // Third person — the version that feels right — applies NO
                // texture modulation at all. Its repulse is purely the directed
                // impulse (rotation + push), and its ambient noise runs at the
                // same level right through a cast, which is exactly why nothing
                // there ever seems to stop. First person now splits the work the
                // same way: the spring kick below carries the punch (3.5° peak,
                // larger than this swell ever was) and the swell is seasoning
                // underneath it rather than the main event.
                const float ritualMul = isRitual ? 1.7f : 1.0f;
                constexpr float kBurstTextureScale = 1.5f;
                sFoFBurstPeak = peakAmp * kBurstTextureScale * ritualMul *
                                sFoFChargeTierMul * sFoFChargeRepulse;
                // Cache school + ritual at trigger time so the recoil
                // curve below uses consistent values across the entire
                // burst window even as the resolver transitions out.
                sFoFBurstSchool   = StateResolver::GetSingleton().GetSchool();
                sFoFBurstIsRitual = isRitual;
                // Roll this shot's recoil character (same bands + Feel
                // mapping the 3p RepShot arm uses): ζ band slides with the
                // cell's Repulse Feel — snappy rebound vs dead-blow; tempo
                // ±15% around the Feel tempo; size 0.80–1.25× with a 10%
                // chance of a heavy one; the kick axis tilts so shots land
                // up-left / up-right / near-straight; ~60% get a faster,
                // weaker harmonic on the settle.
                static std::uint32_t sFpKickRng = 0x9e3779b9u;
                auto kroll = [&]() {
                    sFpKickRng = sFpKickRng * 1664525u + 1013904223u;
                    return static_cast<float>(sFpKickRng >> 8) * (1.0f / 16777216.0f);
                };
                const float feel     = std::clamp(sFoFChargeProfile->repulseFeel, 0.0f, 1.0f);
                const float tempoMul = 0.70f + 0.60f * feel;
                const float kickMul  = 0.85f + 0.30f * feel;
                const float shudMul  = 0.60f + 0.80f * feel;
                const float zetaCtr  = 0.85f - 0.45f * feel;
                // Arm a free (or most-settled) slot — a follow-up cast must
                // NEVER restart a shot mid-swing.
                FpShot* shot = &sFpShots[0];
                for (auto& sh : sFpShots)
                    if (sh.elapsed > shot->elapsed) shot = &sh;
                shot->zeta  = std::clamp(zetaCtr - 0.175f + 0.35f * kroll(), 0.20f, 0.95f);
                shot->omega = 16.0f * tempoMul * (0.85f + 0.30f * kroll());
                shot->wd    = std::sqrt(1.0f - shot->zeta * shot->zeta);
                const float kickPhi = std::atan(shot->wd / shot->zeta);
                shot->norm  = 1.0f / (std::exp(-shot->zeta * kickPhi / shot->wd) * shot->wd);
                shot->tPeak = kickPhi / (shot->omega * shot->wd);
                const float ampJit   = kickMul * (0.80f + 0.45f * kroll()) *
                                       ((kroll() < 0.10f) ? 1.30f : 1.0f);
                const float rollFrac = 0.28f * (kroll() * 2.0f - 1.0f);
                const float yawFrac  = 0.38f * (kroll() * 2.0f - 1.0f);
                const float axLen    = std::sqrt(1.0f + rollFrac * rollFrac + yawFrac * yawFrac);
                shot->ax = 1.0f / axLen;
                shot->ay = rollFrac / axLen;
                shot->az = yawFrac / axLen;
                // Peaks fully scaled HERE (school amp vs the 5.0 reference,
                // ritual, tier, the cell's Repulse slider, this shot's size
                // roll) — the render loop never reads a live profile again.
                const float ampScaleArm = (std::clamp)(peakAmp / 5.0f, 0.0f, 2.0f);
                shot->peakPitch = 3.5f * 0.017453293f * ampScaleArm *
                                  (isRitual ? 1.5f : 1.0f) *
                                  sFoFChargeTierMul * sFoFChargeRepulse * ampJit;
                shot->burstPeak = sFoFBurstPeak;
                shot->harmAmp   = ((kroll() < 0.60f) ? (0.10f + 0.12f * kroll()) : 0.0f) * shudMul;
                shot->harmMul   = 1.8f + 1.2f * kroll();
                shot->elapsed   = sFoFChargeIsStaff ? -0.40f : -0.13f;
            }
            sLastLeftCasterState  = curLeftState;
            sLastRightCasterState = curRightState;
            sFoFLastProgress = chargeProgress;

            // Jump landing thud, first person — same slot pool as the cast
            // recoil, same Feel mapping, but the pitch is NEGATIVE: a landing
            // is the head dipping INTO the impact, not a muzzle kicking up.
            // No texture swell (burstPeak 0) — the thud is the whole event —
            // and no pre-delay, the touchdown confirmation already provided one.
            if (sJumpLandKickPending && settings.jumpRepulseFp > 0.0001f) {
                sJumpLandKickPending = false;
                // [FPLAND] consumer half — the thud reached the 1p shot pool.
                {
                    static int sLandFire = 0;
                    if (sLandFire < 30) {
                        ++sLandFire;
                        spdlog::debug("[FPLAND] 1p fire repulse={:.2f} feel={:.2f} scale={:.2f} ({}/30)",
                                     settings.jumpRepulseFp, settings.jumpRepulseFeelFp,
                                     sJumpLandKickScale, sLandFire);
                    }
                }
                // 1p glides are untouched by design; just keep the glide
                // flag from going stale into the next 3p landing.
                sJumpLandKickGlide   = false;
                static std::uint32_t sJumpKickRng = 0x2545f491u;
                auto jroll = [&]() {
                    sJumpKickRng = sJumpKickRng * 1664525u + 1013904223u;
                    return static_cast<float>(sJumpKickRng >> 8) * (1.0f / 16777216.0f);
                };
                const float feel     = std::clamp(settings.jumpRepulseFeelFp, 0.0f, 1.0f);
                const float tempoMul = 0.70f + 0.60f * feel;
                const float kickMul  = 0.85f + 0.30f * feel;
                const float shudMul  = 0.60f + 0.80f * feel;
                const float zetaCtr  = 0.85f - 0.45f * feel;
                FpShot* shot = &sFpShots[0];
                for (auto& sh : sFpShots)
                    if (sh.elapsed > shot->elapsed) shot = &sh;
                shot->zeta  = std::clamp(zetaCtr - 0.175f + 0.35f * jroll(), 0.20f, 0.95f);
                shot->omega = 16.0f * tempoMul * (0.85f + 0.30f * jroll());
                shot->wd    = std::sqrt(1.0f - shot->zeta * shot->zeta);
                const float jPhi = std::atan(shot->wd / shot->zeta);
                shot->norm  = 1.0f / (std::exp(-shot->zeta * jPhi / shot->wd) * shot->wd);
                shot->tPeak = jPhi / (shot->omega * shot->wd);
                const float ampJit = kickMul * (0.80f + 0.45f * jroll());
                // Mostly a straight-down nod; a whisper of roll so consecutive
                // landings aren't clones. No yaw — a landing has no bearing.
                const float rollFrac = 0.14f * (jroll() * 2.0f - 1.0f);
                const float axLen    = std::sqrt(1.0f + rollFrac * rollFrac);
                shot->ax = 1.0f / axLen;
                shot->ay = rollFrac / axLen;
                shot->az = 0.0f;
                // 5.0 deg, not the 2.6 this carried until 2026-09-06. The
                // log says the thud FIRES on every landing ([FPLAND] "1p fire"
                // beside every touchdown), so "I don't feel a landing repulse
                // at all" is magnitude, not plumbing: at the user's Repulse
                // 0.30 and a normal landing's 0.55 scale, 2.6 deg came to
                // 0.43 deg of pitch and NOTHING else.
                //
                // Nothing else is the point. The third-person thud spends the
                // event across four channels (armRepulse: pitch 3.0 deg, yaw
                // 1.2, roll 0.9, plus a 5.0-unit positional PUSH), and the 1p
                // shot pool has no push and no yaw ("a landing has no
                // bearing"). One axis has to carry what 3p spreads over four,
                // so the coefficient is the sum of 3p's three ROTATIONAL
                // channels (3.0 + 1.2 + 0.9). The 1p cast recoil above already
                // sits at 3.5 deg with a 0..2 scale, so this is not an outlier
                // in its own pool either.
                shot->peakPitch = -5.0f * 0.017453293f * settings.jumpRepulseFp *
                                  sJumpLandKickScale * ampJit;
                shot->burstPeak = 0.0f;
                shot->harmAmp   = ((jroll() < 0.60f) ? (0.10f + 0.12f * jroll()) : 0.0f) * shudMul;
                shot->harmMul   = 1.8f + 1.2f * jroll();
                shot->elapsed   = 0.0f;
            }

            // Base target: profile amp/speed. The resolver already picked
            // the winning cell, so no extra enabled check is needed.
            float targetAmp   = fpProfile ? fpProfile->noise.amp   : 0.0f;
            float targetSpeed = fpProfile ? fpProfile->noise.speed : 0.0f;

            // FoF charge ramp — scale amp with the live charge progress
            // so the shake builds with the spell instead of snapping
            // to full intensity instantly. chargeProgress rises to 1.0 at
            // kReady; the held charge stays at 1.0 so this is a no-op during
            // the post-windup hold. Provides the "Charging" stage of the
            // cast its own dynamic signature distinct from "Held".
            //
            // THE FLOOR IS NOT OPTIONAL — it is the whole "noise stops and
            // snaps back" bug. handProgress returns 0 for CState::kNone, which
            // is every frame a fire-and-forget spell is merely LOADED rather
            // than charging: the entire post-fire window, and all the time you
            // stand around with a spell out. Multiplying by that drove the
            // target to literal zero, so the camera died to a standstill after
            // every cast (measured: amp 5.5 -> 0.47 over ~300ms) and then came
            // back when the resolver finally handed off to the parent state.
            // A hole, not a step — the same shape the shout hand-off had, for
            // the same reason: a tail that walks to silence instead of to the
            // level the state actually sits at.
            //
            // It hid for so long because the amp spring's time constant is
            // 1.25s at this user's Transition Speed, so between rapid casts the
            // decay never reached bottom and the resting level LOOKED like a
            // deliberate ~0.4 of the cell. It wasn't; it was a decay caught
            // mid-flight. Making that level real is the fix, and it uses the
            // same 0.4 as handProgress's own floor so charge-start doesn't dip.
            if (isFoF && fpProfile) {
                constexpr float kFoFIdleFloor = 0.4f;
                targetAmp = fpProfile->noise.amp *
                            (std::max)(kFoFIdleFloor, chargeProgress);
            }

            // Held-charge tension hum — when the player holds a fully
            // charged FoF, layer a slow sinusoidal modulation on amp
            // so the camera reads as "sustained pressure" instead of
            // continuing the same wobble that played during charging.
            // ±18% amp at ~0.45 Hz (gentle breathing rhythm). Triggers
            // only when at least one hand is in kReady with an FoF
            // spell — kCasting / kUnk04 (post-fire) don't hum.
            const bool isHeldCharge =
                (curLeftIsFoF  && curLeftState  == CState::kReady) ||
                (curRightIsFoF && curRightState == CState::kReady);
            if (isHeldCharge && fpProfile) {
                static float sHoldPhase = 0.0f;
                sHoldPhase += noiseDt * 2.8f;  // ~0.45 Hz; texture motion, not cast age
                constexpr float kTwoPi = 6.2831853f;
                if (sHoldPhase > kTwoPi) sHoldPhase -= kTwoPi;
                const float breath = 1.0f + 0.18f * std::sin(sHoldPhase);
                targetAmp *= breath;
            }

            // (Per-frame spell-tier scaling on the steady-state amp removed
            // 2026-08-01. targetAmp is now exactly what the resolved cell's
            // Intensity slider says, at every spell rank.)

            // Route ALL active cast states through the deferred apply
            // path (kUnk01/kUnk02/kReady/kUnk04/kCharging/kCasting),
            // FoF and Concentration alike. The engine wipes
            // `world.rotate` between PlayerCamera::Update+0x1A6 and
            // NiCamera::UpdateWorldData during any animated cast pose,
            // not only the FoF wind-up — concentration's sustained
            // kCasting is also head-bone driven, so a direct write
            // here is clobbered before the renderer reads it and the
            // user sees zero shake while streaming a concentration
            // spell. Dropping the per-hand FoF gate routes both cast
            // types through the late hook, where the apply lands
            // after the engine's recompose.
            auto isCasterActive = [](CState s) {
                return s != CState::kNone &&
                       s != CState::kUnk07 && s != CState::kUnk08 &&
                       s != CState::kUnk09;
            };
            // Shouts also need the deferred-apply path: the shout wind-up
            // is an animated pose just like the FoF/concentration cases
            // above, so the engine recomposes world.rotate between our
            // direct write and the renderer, wiping the noise. Symptom
            // the user reported: "if I make it zoom in during shouting,
            // I don't see noise until the release." FOV transitions
            // trigger extra matrix recomposes that amplify the wipe;
            // without the FOV change the recompose is rarer and the
            // direct write occasionally survives — hence "same angle,
            // I see the full noise." Routing through the late hook
            // (HookedNiCameraUpdateWorldData) lands the noise after
            // every recompose, the same fix May 16 shipped for FoF.
            const auto& srEarly        = StateResolver::GetSingleton();
            // Shout Lag holds the sub-state for the CAMERA only — noise ends
            // on its own Fade Duration (see IsShoutLagHolding).
            const bool  isShoutingEarly = srEarly.GetSubState() == CameraSubState::Shout &&
                                          !srEarly.IsShoutLagHolding();
            const bool castAnimating =
                isCasterActive(curLeftState) || isCasterActive(curRightState) ||
                isShoutingEarly;

            // Concentration-overlay layering. When the resolver picked
            // an FoF cell (because FoF outranks Concentration in cast
            // priority) but a Concentration spell is ALSO streaming in
            // the other hand, the player wants both shake profiles
            // running simultaneously. Walk per-hand here, find any
            // concentration stream, resolve its school cell directly,
            // and additively layer its amp on top of whatever the
            // primary path produced. Skipped when concentration IS the
            // primary (fpProfile is already the concentration cell) to
            // avoid double-counting.
            auto resolveSpellSchoolLocal = [](const RE::SpellItem* spell) -> MagicSchool {
                if (!spell) return MagicSchool::None;
                auto* effect = spell->GetCostliestEffectItem();
                if (!effect || !effect->baseEffect) return MagicSchool::None;
                switch (effect->baseEffect->GetMagickSkill()) {
                case RE::ActorValue::kAlteration:  return MagicSchool::Alteration;
                case RE::ActorValue::kConjuration: return MagicSchool::Conjuration;
                case RE::ActorValue::kDestruction: return MagicSchool::Destruction;
                case RE::ActorValue::kIllusion:    return MagicSchool::Illusion;
                case RE::ActorValue::kRestoration: return MagicSchool::Restoration;
                default:                           return MagicSchool::None;
                }
            };
            auto schoolToKey = [](MagicSchool s) -> const char* {
                switch (s) {
                case MagicSchool::Alteration:  return "alteration";
                case MagicSchool::Conjuration: return "conjuration";
                case MagicSchool::Destruction: return "destruction";
                case MagicSchool::Illusion:    return "illusion";
                case MagicSchool::Restoration: return "restoration";
                default:                       return nullptr;
                }
            };
            // Dual-cast additive layering. Walk both hands; for any
            // hand with an active cast whose school+casttype DOESN'T
            // match the resolver's primary pick, look up its cell and
            // add its amp on top of the primary. Covers every dual
            // combination: FoF+Concentration, FoF+FoF (different
            // schools), Concentration+Concentration, Ritual+anything.
            // Same-cell hands (e.g., dual Firebolt) don't double-count
            // because their key matches the primary's. Per-hand
            // chargeProgress modulates FoF amps during their windup.
            if (player) {
                const auto& sr        = StateResolver::GetSingleton();
                const auto  primarySch = sr.GetSchool();
                const auto  primaryCT  = sr.GetCastType();
                auto* asState2 = player->AsActorState();
                const bool sneaking = asState2 && asState2->IsSneaking();
                auto castToKey = [](RE::MagicSystem::CastingType ct) -> const char* {
                    using CT = RE::MagicSystem::CastingType;
                    switch (ct) {
                    case CT::kConcentration: return "concentration";
                    case CT::kFireAndForget: return "fire_and_forget";
                    default:                 return nullptr;
                    }
                };
                for (auto src : { Src::kLeftHand, Src::kRightHand }) {
                    auto* caster = player->GetMagicCaster(src);
                    if (!caster) continue;
                    const auto cs = caster->state.get();
                    if (cs == CState::kNone) continue;
                    auto* spell = caster->currentSpell
                                      ? caster->currentSpell->As<RE::SpellItem>() : nullptr;
                    if (!spell) continue;
                    const auto handSchool = resolveSpellSchoolLocal(spell);
                    const auto handCT     = spell->GetCastingType();
                    // Skip this hand if it IS the primary — already
                    // counted in targetAmp via fpProfile.
                    // (Ritual spells are FoF in the engine but the
                    // resolver elevates them; treat the resolver's
                    // Ritual as covering this hand's FoF.)
                    const bool isPrimary =
                        handSchool == primarySch &&
                        ((handCT == RE::MagicSystem::CastingType::kFireAndForget &&
                          (primaryCT == CastType::FireAndForget || primaryCT == CastType::Ritual)) ||
                         (handCT == RE::MagicSystem::CastingType::kConcentration &&
                          primaryCT == CastType::Concentration));
                    if (isPrimary) continue;

                    const char* schKey = schoolToKey(handSchool);
                    const char* ctKey  = castToKey(handCT);
                    if (!schKey || !ctKey) continue;

                    std::string handKey = std::string("magic.") + schKey;
                    if (sneaking) handKey += ".sneak";
                    handKey += ".";
                    handKey += ctKey;
                    auto& map = settings.stateFirstPerson;
                    auto it = map.find(handKey);
                    SettingsManager::FirstPersonProfile* handProfile = nullptr;
                    if (it != map.end()) {   // [FP-MERGE] presence is the bind
                        handProfile = &it->second;
                    } else {
                        // Uncustomized fallback to Global so the
                        // overlay still applies.
                        handProfile = &settings.firstPersonGlobal;
                    }
                    // Per-hand FoF chargeProgress modulation so a
                    // mid-windup hand doesn't dump its full amp.
                    float handAmp = handProfile->noise.amp;
                    if (handCT == RE::MagicSystem::CastingType::kFireAndForget) {
                        handAmp *= handProgress(caster, cs);
                    }
                    targetAmp   += handAmp;
                    targetSpeed  = (std::max)(targetSpeed, handProfile->noise.speed);
                }
            }

            // Shouts envelope — progressive build through Skyrim's
            // three word-charging tiers. Each word charges in ~0.5s,
            // so the user's hold time IS the intensity dial: a quick
            // 1-word release sits at ~0.5×; a held 2-word at ~1.0×;
            // a fully-held 3-word at ~1.5×. Previously the curve
            // peaked at 1.5× in 0.12s and FLATLINED at 0.85× from
            // 0.6s onward — word 1 felt heavy (you hit the peak),
            // words 2 and 3 felt identical (plateau). New shape is
            // monotonically increasing through word boundaries,
            // smoothstep-eased within each tier so the boundary
            // crossings are continuous.
            // Shout post-fire ride-through state. Survives shout-state-end
            // so the fade tail can complete after the shout ends, mirroring
            // FoF's sFoFBurstElapsed pattern. Captured at fire-edge inside
            // the isShoutingNow branch below; consumed both inside (during
            // active shout fade) and AFTER (post-shout-end ride-through).
            static std::optional<SettingsManager::FirstPersonProfile> sShoutFadeProfile;
            static auto                                       sShoutFadeFireTp      = std::chrono::steady_clock::time_point{};
            static float                                      sShoutFadeEnvAtFire   = 0.0f;
            static float                                      sShoutFadeDurCaptured = 0.0f;
            // Release-punch transient (word-count scaled).
            static std::chrono::steady_clock::time_point      sFpShoutPunchTp{};
            static float                                      sFpShoutPunchAmp    = 0.0f;
            static int                                        sFpShoutWordApplied = 0;

            // 1p shout shake reads ~3x weaker than 3p at the same slider
            // value because the 1p rotation path uses a smaller angle scale
            // (0.00006 vs 0.00020 rad). At that strength the wobble survives
            // the render (verified: the 30deg deferred-apply tilt persists
            // through free-look) but is too subtle to perceive while the
            // player sweeps the camera during a shout — the look motion
            // visually masks it. Boost the shout amp to 3p parity so the
            // shake stays felt while looking. Same rationale + factor as the
            // dragon-shake 1p boost (kFpDragonBoost). Applied to targetAmp so
            // it flows through the sFpAmpCurrent spring (smooth wind-up and
            // fade, no pop at shout-end). Amplitude only — speed unchanged.
            constexpr float kFpShoutBoost = 3.0f;

            const auto& sr = StateResolver::GetSingleton();
            // Lag must not postpone the shout-end falling edge below — that
            // edge is what STARTS the fade tail, so inheriting the hold made
            // the 1p shout noise run past its Fade Duration.
            const bool isShoutingNow = sr.GetSubState() == CameraSubState::Shout &&
                                       !sr.IsShoutLagHolding();

            // Shout-end falling-edge clock for the fade tail. Runs every 1p
            // frame so it can't go stale. Replaces the old profile-identity
            // comparison, which missed the FIRST shout (its static seed was
            // the live non-null profile, so no edge was detected) and
            // mis-fired on any mid-shout sub-state flicker — both left the
            // fade clock pointing at the epoch, so elapsed read as huge, the
            // tail was marked complete on frame 1, and the shake cut off
            // instantly instead of fading.
            static bool sWasShouting   = false;
            static auto sShoutEndedAt  = std::chrono::steady_clock::time_point{};
            if (sWasShouting && !isShoutingNow) {
                sShoutEndedAt       = CameraEffectClock::Now();
                sFpShoutPunchAmp    = 0.0f;   // no transient into the fade tail
                sFpShoutWordApplied = 0;
            }
            sWasShouting = isShoutingNow;

            if (isShoutingNow && fpProfile) {
                const auto now2   = CameraEffectClock::Now();
                const auto start  = sr.GetShoutStartTime();
                const auto fire   = sr.GetShoutFireTime();
                const float windupElapsed =
                    std::chrono::duration<float>(now2 - start).count();

                // Pre-fire envelope tiers. Word boundaries match the
                // engine's charge timer; amps spread evenly so each
                // word feels distinctly heavier than the previous.
                // These now only shape the WIND-UP — once the engine
                // commits a shout variation, StateResolver hands us the
                // real word count and that takes over (see below).
                constexpr float kWord1End = 0.50f;
                constexpr float kWord2End = 1.00f;
                constexpr float kWord3End = 1.50f;
                constexpr float kAmp1     = 0.50f;
                constexpr float kAmp2     = 1.10f;
                constexpr float kAmp3     = 2.00f;
                // Release punch — same rationale as the 3p path: word
                // escalation alone is too gradual to land, so a full
                // 3-word release gets a real transient on top. Smoothstep
                // attack (the amp is an angle scale; steps read as jolts),
                // and it also has to survive the sFpAmpCurrent lerp below,
                // which is why the 1p values run a little hotter than 3p.
                constexpr float kPunch1      = 0.25f;
                constexpr float kPunch2      = 0.70f;
                constexpr float kPunch3      = 1.70f;
                constexpr float kPunchAttack = 0.08f;
                constexpr float kPunchDecay  = 0.60f;
                auto smooth3    = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
                auto wordAmp    = [&](int wc) { return wc >= 3 ? kAmp3   : (wc == 2 ? kAmp2   : kAmp1);   };
                auto wordPunch  = [&](int wc) { return wc >= 3 ? kPunch3 : (wc == 2 ? kPunch2 : kPunch1); };
                const int words = sr.GetShoutWordCount();

                float envelope = 0.0f;
                if (windupElapsed < kWord1End) {
                    const float t = windupElapsed / kWord1End;
                    const float s = t * t * (3.0f - 2.0f * t);
                    envelope = kAmp1 * s;
                } else if (windupElapsed < kWord2End) {
                    const float t = (windupElapsed - kWord1End) /
                                    (kWord2End - kWord1End);
                    const float s = t * t * (3.0f - 2.0f * t);
                    envelope = kAmp1 + s * (kAmp2 - kAmp1);
                } else if (windupElapsed < kWord3End) {
                    const float t = (windupElapsed - kWord2End) /
                                    (kWord3End - kWord2End);
                    const float s = t * t * (3.0f - 2.0f * t);
                    envelope = kAmp2 + s * (kAmp3 - kAmp2);
                } else {
                    envelope = kAmp3;
                }

                // Post-fire: two-phase fade matching FoF's release shape.
                // Phase 1 (0 → 0.30s): smooth settle from envAtFire down
                // to a "voice level" at ~50% of envAtFire — the camera
                // releases tension along with the voice but stays felt.
                // Phase 2 (0.30s → 1.60s): long smooth fade from voice
                // level to zero, matching the duration of the shout's
                // voice line so the noise dies with the sound instead
                // of holding a constant floor that read as a flatline.
                // Both phases use a captured pre-fire envelope value so
                // a 3-word release fades from a high peak and a 1-word
                // from a low one — release intensity tracks charge.
                const bool fireFired = fire != std::chrono::steady_clock::time_point{} &&
                                       fire >= start;
                if (fireFired) {
                    // Capture the peak envelope + profile + slider value
                    // ONCE per shout-fire-event. Voice_SpellFire fires at
                    // the START of voice playback ("FUS"). The voice line
                    // continues for ~1-1.5s after this firing "RO DAH" —
                    // we hold the envelope at the captured peak through
                    // the entire voice playback so the player still feels
                    // the shake on the audible 3rd word. The actual fade
                    // doesn't start until the shout state ends (after
                    // shoutStop + linger), handled in the ride-through
                    // block below.
                    if (fire != sShoutFadeFireTp) {
                        sShoutFadeFireTp      = fire;
                        sShoutFadeEnvAtFire   = envelope;
                        sShoutFadeProfile     = SnapshotProfile(fpProfile);
                        sFpShoutWordApplied   = 0;
                        sFpShoutPunchAmp      = 0.0f;
                        // Fade duration cascade: per-shout overrides Base
                        // when set (>0). User dials Base once for the
                        // "default linger for all shouts," and per-shout
                        // cells only need editing when that specific
                        // shout wants a different feel (e.g. a long
                        // dragon-roar shout vs. a quick Whirlwind Sprint).
                        float fadeDur = fpProfile->shoutFadeDuration;
                        if (fadeDur <= 0.0f) {
                            const std::string baseKey = sr.IsSneaking()
                                ? "shouts.base.sneak" : "shouts.base";
                            auto itBase = settings.stateFirstPerson.find(baseKey);
                            if (itBase == settings.stateFirstPerson.end() &&
                                sr.IsSneaking())
                            {
                                itBase = settings.stateFirstPerson.find("shouts.base");
                            }
                            if (itBase != settings.stateFirstPerson.end()) {
                                fadeDur = itBase->second.shoutFadeDuration;
                            }
                        }
                        sShoutFadeDurCaptured = fadeDur;
                    }
                    // The engine's real word count supersedes the elapsed-
                    // time guess the moment it exists — it can land on the
                    // fire frame or one frame later.
                    if (words > 0 && words != sFpShoutWordApplied) {
                        sFpShoutWordApplied = words;
                        sShoutFadeEnvAtFire = wordAmp(words);
                        sFpShoutPunchAmp    = wordPunch(words);
                        sFpShoutPunchTp     = now2;
                    }
                    // HOLD at peak during voice playback. The ride-through
                    // below does the actual fade after isShoutingNow ends.
                    envelope = sShoutFadeEnvAtFire;
                } else if (words > 0) {
                    // Variation committed before any release signal.
                    envelope = (std::max)(envelope, wordAmp(words));
                }

                // Release punch on top of the sustained level.
                if (sFpShoutPunchAmp > 0.0f) {
                    const float pt = std::chrono::duration<float>(now2 - sFpShoutPunchTp).count();
                    if (pt < kPunchAttack + kPunchDecay) {
                        const float shape = (pt < kPunchAttack)
                            ? smooth3(pt / kPunchAttack)
                            : 1.0f - smooth3((pt - kPunchAttack) / kPunchDecay);
                        envelope += sFpShoutPunchAmp * shape;
                    } else {
                        sFpShoutPunchAmp = 0.0f;
                    }
                }

                targetAmp   = fpProfile->noise.amp   * envelope * kFpShoutBoost;
                targetSpeed = fpProfile->noise.speed * envelope;

                // Telemetry: per-shout envelope/amp trace. Rate-limited to
                // every 50ms so we see the curve without spamming. Grep
                // "[SHOUT-NOISE]" in DietDrCamera.log.
                {
                    static auto sLastLog = std::chrono::steady_clock::time_point{};
                    if (std::chrono::duration<float>(now2 - sLastLog).count() > 0.05f) {
                        sLastLog = now2;
                        spdlog::debug(
                            "[SHOUT-NOISE] t={:.3f}s env={:.3f} prof.amp={:.3f} fadeDur=(prof={:.2f},cap={:.2f}) "
                            "-> targetAmp={:.3f} ampCur={:.3f} fired={} deferred={}",
                            windupElapsed, envelope,
                            fpProfile->noise.amp,
                            fpProfile->shoutFadeDuration, sShoutFadeDurCaptured,
                            targetAmp, sFpAmpCurrent,
                            (fire != std::chrono::steady_clock::time_point{} && fire >= start),
                            castAnimating);
                    }
                }
            }

            // Shout post-fire hand-off. The shout state stays true through
            // wind-up + Voice_SpellFire + voice playback + linger. When it
            // ends, the envelope has been HELD at envAtFire through all of
            // that, and we walk it back over the user-set shoutFadeDuration.
            //
            // It walks back to the UNDERLYING STATE'S level, not to zero.
            // Fading to zero and only then releasing the capture left a hole:
            // the shout died away properly, and then the state's own noise
            // arrived from silence a beat later, which reads as the noise
            // cutting out and switching back on. Interpolating to where the
            // state was always going to sit means there is nothing left to
            // transition when the capture is released — the values already
            // match. Third person solves the same problem with its two-layer
            // crossfade; first person has a single texture, so it does it by
            // interpolating the target.
            if (!isShoutingNow && sShoutFadeProfile) {
                const auto  now3    = CameraEffectClock::Now();
                const float elapsed = std::chrono::duration<float>(
                    now3 - sShoutEndedAt).count();
                const float fadeDur = (std::max)(0.45f, sShoutFadeDurCaptured);
                if (elapsed < fadeDur) {
                    const float t = elapsed / fadeDur;
                    const float s = t * t * (3.0f - 2.0f * t);
                    // Where the shout was, and where the state is headed.
                    const float shoutAmp   = sShoutFadeProfile->noise.amp *
                                             sShoutFadeEnvAtFire * kFpShoutBoost;
                    const float shoutSpeed = sShoutFadeProfile->noise.speed *
                                             sShoutFadeEnvAtFire;
                    const float baseAmp    = fpProfile ? fpProfile->noise.amp   : 0.0f;
                    const float baseSpeed  = fpProfile ? fpProfile->noise.speed : 0.0f;
                    targetAmp   = shoutAmp   + (baseAmp   - shoutAmp)   * s;
                    targetSpeed = shoutSpeed + (baseSpeed - shoutSpeed) * s;
                    // Character still rides the shout's profile for the
                    // duration; the swap back happens at the end, by which
                    // point both sides are at the same amplitude and a texture
                    // change at matched amplitude is not something you feel.
                    fpProfile   = &*sShoutFadeProfile;
                } else {
                    // Hand-off complete — release capture so next frame's
                    // resolution runs cleanly off the underlying state.
                    sShoutFadeProfile.reset();
                }
            }

            // Bow draw ramp — scale the shake target with the live bow
            // draw amount when the player is pulling a bow or crossbow.
            // Drawing is detected by meleeAttackState != kNone with a
            // bow/crossbow in the right hand. The draw amount comes
            // from the engine's per-draw timer stack (same source the
            // crosshair predictor uses). With the resolver routing
            // weapons.bow.draw / weapons.crossbow.draw cells, the user
            // can give the draw state a stronger shake profile and the
            // ramp here makes it build with bowstring tension — same
            // cinematic shape as the FoF charge ramp but driven by the
            // bow timer.
            if (fpProfile) {
                auto* drawPlayer = RE::PlayerCharacter::GetSingleton();
                auto* asState    = drawPlayer ? drawPlayer->AsActorState() : nullptr;
                const bool attacking = asState &&
                    asState->actorState1.meleeAttackState != RE::ATTACK_STATE_ENUM::kNone;
                if (attacking && drawPlayer) {
                    auto* rh    = drawPlayer->GetEquippedObject(false);
                    auto* rhWep = rh ? rh->As<RE::TESObjectWEAP>() : nullptr;
                    const bool ranged = rhWep && (rhWep->IsBow() || rhWep->IsCrossbow());
                    if (ranged) {
                        const float drawAmount = GetLiveBowDrawAmount(drawPlayer);
                        // 0.2 floor so first frame of draw already has
                        // a perceptible shake; 0..1 ramps the rest.
                        const float scale = 0.2f + 0.8f * drawAmount;
                        targetAmp   = fpProfile->noise.amp   * scale;
                        targetSpeed = fpProfile->noise.speed * scale;
                    }
                }
            }

            // Base-amp tail = 0.6s burst-window hold + per-cell fade
            // duration cached at charge time. Default tail 1.0s; user
            // can dial fade up to 4s per FoF/Ritual cell via the
            // Fade Duration slider on each cell.
            constexpr float kHoldEnd = 0.6f;
            const float kPostFireTailDuration = kHoldEnd + sFoFChargeFadeDuration;
            // Advance the timer through the longest active window
            // (pre-delay + tail). Burst envelope and base-tail decay
            // each clamp internally; we just need the counter to
            // keep moving through 1.0s so neither finishes early.
            if (sFoFBurstElapsed < kPostFireTailDuration) {
                sFoFBurstElapsed += dt;
            }
            // Recoil + release burst, rendered from the shot pool. Each slot
            // is the same underdamped spring the 3p RepShots use; slots SUM,
            // so rapid-fire casts ring through each other instead of the old
            // single-timer restart — which cut a mid-swing kick to zero and
            // left the burst dead through the next shot's pre-delay, the
            // "bad snap" the [MOTION] meter caught on every follow-up cast.
            //
            // The burst rides each shot's ENERGY envelope — smoothstep rise
            // to the kick's displacement peak, then the spring's own e^(-ζωt)
            // decay. NOT |displacement|: that crashes through zero at every
            // sin crossing, which made the shake throb mid-gesture. Peaks
            // were fully scaled at arm time, so nothing here reads a live
            // profile mid-swing.
            float burstAmp      = 0.0f;
            bool  fpShotsActive = false;
            float        pitchKickRad = 0.0f;
            RE::NiPoint3 pitchKickAxis{ 1.0f, 0.0f, 0.0f };
            {
                float kx = 0.0f, ky = 0.0f, kz = 0.0f;
                for (auto& sh : sFpShots) {
                    if (sh.elapsed >= 1.0e8f) continue;
                    sh.elapsed += dt;
                    if (sh.elapsed >= 6.0f / (sh.zeta * sh.omega)) {   // settled — free
                        sh.elapsed = 1.0e9f;
                        continue;
                    }
                    fpShotsActive = true;
                    if (sh.elapsed < 0.0f) continue;   // pre-delay
                    const float t = sh.elapsed;
                    float env = std::exp(-sh.zeta * sh.omega * t) *
                                std::sin(sh.omega * sh.wd * t) * sh.norm;
                    if (sh.harmAmp > 0.0f) {
                        env += sh.harmAmp *
                               std::exp(-sh.zeta * sh.omega * t * 1.15f) *
                               std::sin(sh.omega * sh.wd * sh.harmMul * t) * sh.norm;
                    }
                    const float mag = sh.peakPitch * env;
                    kx += mag * sh.ax;
                    ky += mag * sh.ay;
                    kz += mag * sh.az;
                    float bEnv;
                    if (t < sh.tPeak) {
                        const float a = t / sh.tPeak;
                        bEnv = a * a * (3.0f - 2.0f * a);
                    } else {
                        bEnv = std::exp(-sh.zeta * sh.omega * (t - sh.tPeak));
                    }
                    burstAmp += sh.burstPeak * bEnv;
                }
                // Composite axis-angle: a counter-swing shows up as the
                // component vector flipping, which axis-angle represents
                // exactly (negative components flip the axis).
                const auto hitAxes = HitShake::NiCameraRotationVector(hitRotation);
                kx += hitAxes[0];
                ky += hitAxes[1];
                kz += hitAxes[2];
                const float kmag = std::sqrt(kx * kx + ky * ky + kz * kz);
                if (kmag > 1e-7f) {
                    pitchKickRad  = kmag;
                    pitchKickAxis = RE::NiPoint3{ kx / kmag, ky / kmag, kz / kmag };
                }
            }
            // Clear the cache once the base-tail decay is done AND
            // we're no longer charging — only at that point can the
            // windup data be safely dropped without truncating the
            // smooth tail. Also never while any shot still rings (a short
            // user Fade Duration must not cut a kick off mid-swing).
            if (sFoFBurstElapsed >= kPostFireTailDuration &&
                !fpShotsActive && chargeProgress < 0.05f) {
                sFoFChargeProfile.reset();
            }

            // Per-state transition speed (else global). FoF skips the
            // spring on the charge target — chargeProgress is already
            // smooth — and on the burst, since the burst itself is a
            // shaped envelope. The spring still smooths transitions
            // between unrelated states (e.g. swap from Bow → Sheathed).
            //
            // ONE CURVE, ONE SLIDER, WITH THE FOV (user, 2026-09-03: "make the
            // first person camera noise for normal attack and power attack use
            // the same methodology as FOV").
            //
            // The 1p world FOV moves on a CLOSED-FORM CRITICALLY DAMPED SPRING
            // at omega 17 x the same per-state Transition Speed read here
            // (HookManager, omegaBaseFov — closed form because explicit Euler
            // is only stable to omega*dt~0.83 and the slider reaches omega 170).
            // The amplitude moved on a FIRST-ORDER exp-lerp at omega 8 x speed
            // instead, so a swing changed two things on two different curves at
            // roughly half each other's rate: the FOV settled in ~230ms while
            // the shake was still arriving. Two motions where the cell asked
            // for one — the same "two distinct changes" shape [FPFOV-TRACE]
            // adjudicated on the FOV channel itself.
            //
            // So the amplitude is now the FOV's step, on the FOV's constant,
            // driven by the FOV's slider. Base 17 (was 8): the exp-lerp's
            // time-to-90% is 2.3/omega and the critical spring's is ~3.9/omega,
            // so 17 lands a touch quicker than the old 8 rather than slower —
            // the same translation the FOV note documents for its own 10 -> 17.
            // The spring also starts from ZERO VELOCITY, which is what retires
            // the activation ramp below: an exp-lerp's fastest motion is frame
            // one, and on an angle channel that first frame IS the jolt.
            float perStateSpeed = settings.firstPersonTransitionSpeed;
            if (fpProfile && fpProfile->transitionSpeed > 0.0f) {
                perStateSpeed = fpProfile->transitionSpeed;
            }
            const float speedMul = (std::max)(0.05f, perStateSpeed);
            float       omega    = 17.0f * speedMul;
            // [FPNOISE] the FOV channel is instrumented and PROVEN a clean
            // single glide, yet the user still sees "two distinct changes"
            // at sprint edges — this is the other channel that moves there.
            // One line per resolved-cell change: source, amp/tilt targets.
            {
                static const void* sPrevNzPtr = nullptr;
                if (static_cast<const void*>(fpProfile) != sPrevNzPtr) {
                    sPrevNzPtr = fpProfile;
                    static int sNzLog = 0;
                    if (sNzLog < 200) {
                        ++sNzLog;
                        spdlog::debug("[FPNOISE] source={} ptr=0x{:x} amp={:.2f} tilt={:.2f} speed={:.2f}",
                                     settings.DescribeFpProfileSource(fpProfile),
                                     reinterpret_cast<std::uintptr_t>(fpProfile),
                                     fpProfile ? fpProfile->noise.amp : 0.0f,
                                     fpProfile ? fpProfile->noise.tilt : 0.0f,
                                     perStateSpeed);
                    }
                }
            }

            // ATTACK BEATS ARE PUNCTUATION, NOT AMBIENCE.
            //
            // The amplitude is eased toward the resolved cell at `omega`. The
            // Transition Speed slider goes down to 0.1, which is omega 0.8 — a
            // 1.25s time constant. A power attack is over in about a second, so
            // at low Transition Speed the shake for weapons.melee.power_attack
            // physically could not arrive before the swing ended: the user saw
            // the attack noise start as the animation finished ("power attack
            // noise doesn't apply until after the power attack ends"). Amplitude
            // alone masked it in testing because most attack cells share the
            // base state's amp and differ mainly in SPEED (e.g. weapons.melee
            // speed 0.6 → weapons.melee.attack speed 4.0) — a frozen-rate sample
            // reads as "no shake" no matter how large the amp is.
            //
            // The floor used to be applied to BOTH channels: omega here for
            // the amplitude, and the rate floor at the flow governor. THE AMP
            // FLOOR IS GONE (2026-09-03) — it is the thing that stopped the
            // amplitude obeying the same slider the FOV obeys, which is exactly
            // what this change is for. It also no longer buys anything at any
            // ordinary setting: it lifted omega to 14, and the spring's base is
            // 17 x speed, so it only ever bit below Transition Speed ~0.82 —
            // and below that the FOV is equally slow, which is the point. The
            // trade is stated plainly: at a very low Transition Speed the attack
            // shake now arrives as unhurriedly as the attack FOV does, the old
            // "power attack noise doesn't apply until after the power attack
            // ends" exposure, and the slider is the cure.
            //
            // The RATE floor stays. The clock is not an FOV-like channel: the
            // rendered angle is theta(t) = A * P(clock(t)), so its VELOCITY
            // carries d(clock)/dt, and the standing rate-floor rule (2026-08-02)
            // is that a slow slider must never be allowed to govern it. So
            // sFpAttackFastTimer still runs here and is still read by the flow
            // governor near the apply site — that is now its ONLY reader.
            {
                constexpr float kAttackReleaseWindow = 0.30f;
                const bool inAttack =
                    StateResolver::GetSingleton().GetSubState() == CameraSubState::Attack;
                if (inAttack) {
                    sFpAttackFastTimer = kAttackReleaseWindow;
                } else if (sFpAttackFastTimer > 0.0f) {
                    sFpAttackFastTimer = (std::max)(0.0f, sFpAttackFastTimer - dt);
                }
            }

            // Direct-track path: FoF cycle, post-fire burst tail, OR an
            // active shout. The shouts envelope is a shaped curve that
            // would be muddied if the spring smoothed across its build
            // and tone-down — track its target exactly.
            const bool isShoutDirect =
                StateResolver::GetSingleton().GetSubState() == CameraSubState::Shout &&
                !StateResolver::GetSingleton().IsShoutLagHolding();
            const bool inDirectPath =
                isFoF || isShoutDirect || sFoFBurstElapsed < kPostFireTailDuration ||
                fpShotsActive;   // a staff shot's long pre-delay can outlive the
                                 // tail window — the burst must not drop mid-ring
            // Direct-path amp slew governor state (used in both branches:
            // the else resets the entry latch so the next direct entry
            // re-seeds from the live amp).
            static float sFpDirectSm   = 0.0f;
            static bool  sFpDirectPrev = false;
            if (inDirectPath) {
                // Track directly — chargeProgress / burst envelopes are
                // shaped for the user's eye already. Spring smoothing
                // would dull the cinematic feel.
                //
                // Smooth base-amp release. Two problems if we just let
                // targetAmp control the steady shake post-fire:
                //   1. The hand goes kNone the frame after release, so
                //      chargeProgress drops to 0 and targetAmp snaps
                //      from cast-amp to 0 in one frame.
                //   2. The burst envelope has a 130ms pre-delay (so the
                //      camera kick syncs with the projectile leaving
                //      the hand), during which burstAmp is also 0.
                // Result: ~8 frames of dead silence between cast and
                // burst, then the burst pops in. High-amp schools mask
                // this with their large burst peak; low-amp schools
                // (illusion, restoration, alteration) don't have
                // enough magnitude to hide the gap and the user sees
                // it as an abrupt cutoff.
                //
                // Fix: hold the BASE amp at cast intensity through the
                // pre-delay, then decay it linearly to zero over a
                // tail window LONGER than the burst envelope itself
                // (1.0s vs 0.6s). The burst rides on top as the
                // cinematic kick; the base provides a continuous,
                // perceptibly-fading tail regardless of school amp.
                float baseTarget = targetAmp;
                float baseSpeed  = targetSpeed;
                if (sFoFChargeProfile && sFoFBurstElapsed < kPostFireTailDuration) {
                    const float castAmp = sFoFChargeProfile->noise.amp;
                    // Two-stage envelope so the shake doesn't drop off
                    // perceptibly while the burst is still playing:
                    //   stage 1: pre-delay + burst window (full amp hold)
                    //   stage 2: long smoothstep fade to zero
                    // Boundary lives at the burst-envelope end so the
                    // base only starts fading AFTER the cinematic kick
                    // has run its course — that way the user sees a
                    // sustained "post-cast" shake for the burst length,
                    // then a smooth perceptible fade for ~3 seconds.
                    float castTail;
                    if (sFoFBurstElapsed < kHoldEnd) {
                        castTail = castAmp;
                    } else {
                        const float t = (sFoFBurstElapsed - kHoldEnd) /
                                        (kPostFireTailDuration - kHoldEnd);
                        const float s = t * t * (3.0f - 2.0f * t);
                        castTail = castAmp * (1.0f - s);
                    }
                    baseTarget = (std::max)(targetAmp, castTail);
                }
                // THE CAST CELL'S SPEED IS NOT PINNED PAST THE RELEASE ANY
                // MORE. The old max() here held the LIVE layer's clock at the
                // cast cell's rate for the whole burst tail (1.6s at the
                // default Fade Duration) — which, with the texture hand-off
                // now arming AT the release, would have run the incoming
                // parent texture at the cast's tempo for a second and a half:
                // the exact "cast noise lasts far longer than the spell"
                // report. The cast tempo still rides out correctly, because
                // the OUTGOING crossfade layer samples at ITS OWN frozen
                // rate (fp1pPrevSpeed) for as long as it is audible — that is
                // the whole point of the two-layer design.
                //
                // The failure the pin was guarding — a parent cell at Speed 0
                // freezing the rotation mid-ring ("brief pause and snap"
                // after a second fireball) — is covered without it: the flow
                // governor floors the live clock at kMinFlowRate, and the
                // ringing kick itself (fpShots) is a rotation impulse that
                // never depended on the Perlin rate.
                // Slew-bound the direct amp. The burst envelope is C1 but
                // STEEP — ~20 amp inside 3-4 frames on a strong Repulse —
                // and castTail can step on a rapid-fire re-arm. Rendered
                // rotation scales linearly with amp, so an unbounded amp
                // slew IS a visible flick no matter how smooth the
                // envelope's own math is (the [MOTION] 1p log showed ~10
                // amp/frame at every release). Asymmetric one-pole: rise
                // ~95% inside 65ms keeps the punch; release ~210ms. Seeded
                // from the live amp on entry so the path switch can't dip.
                if (!sFpDirectPrev) sFpDirectSm = sFpAmpCurrent;
                sFpDirectPrev = true;
                {
                    // Rise was 45/s, which at 60 fps is 53% of the remaining
                    // gap in a single frame — not a bound on anything. 28/s
                    // reaches 95% in ~105ms, which is about the kick spring's
                    // own time to peak: there is no reason for the texture to
                    // swell faster than the impulse it is decorating.
                    const float want = baseTarget + burstAmp;
                    if (retunePausedValue(want, sFpPreviousDirectTarget, sFpDirectSm)) sFpAmpVel = 0.0f;
                    const float aa   = 1.0f - std::exp(-dt * (want > sFpDirectSm ? 28.0f : 14.0f));
                    sFpDirectSm += (want - sFpDirectSm) * aa;
                }
                // C1 HANDOFF. The direct path writes the amplitude flat, and
                // the spring below picks it up the frame the path ends. Handing
                // it a velocity of zero there would brake a still-moving burst
                // tail in one frame; handing it the slew's OWN velocity means
                // the curve simply continues. (The FOV does the same thing by
                // sharing sDialogueFovVel across its dialogue/gameplay branches.)
                if (dt > 1.0e-5f) sFpAmpVel = (sFpDirectSm - sFpAmpCurrent) / dt;
                sFpAmpCurrent   = sFpDirectSm;
                // Rate is a TARGET now; the flow governor smooths it. Setting
                // it flat here used to be a one-frame gear change: entering the
                // FoF cycle jumped the Perlin clock from the idle cell's Speed
                // straight to the cast cell's, and rendered angular VELOCITY is
                // proportional to clock rate, so the view visibly lurched into
                // motion at every charge-up.
                sFpSpeedCurrent = baseSpeed;
            } else {
                sFpDirectPrev = false;
                // THE AMPLITUDE IS THE FOV'S SPRING NOW. Closed-form critically
                // damped step, exact for any omega*dt — the same six lines
                // HookManager runs on the 1p world FOV, on the same omega, from
                // the same Transition Speed. A swing's FOV and its shake are one
                // motion because they are one curve.
                //
                // WHAT THIS RETIRES: the activation ramp. Amp near zero stepping
                // into a state that has shake used to switch to a faster
                // exp-lerp (kActivateOmega 25, ~95% in 0.12s) because 1p shake is
                // a rotation whose angle scales with amp, the Perlin sample is
                // nonzero at an arbitrary point on its curve, and an exp-lerp's
                // fastest motion is frame ONE — so engaging the shake at the
                // slider's own rate meant a one-frame ANGLE step at the start of
                // every normal attack. A critical spring starts from zero
                // velocity: there is no first frame to jolt, so the special case
                // has nothing left to prevent and the whole channel can run on
                // one curve. (This is the identical argument the FOV note makes
                // for its own zero-velocity start.)
                //
                // The rate is NOT eased here any more. It used to ride this
                // same spring, whose omega is the user's First Person
                // Transition Speed — 0.8 at the low end of the slider, a 1.25s
                // time constant, so leaving a cast took about four seconds to
                // coast back to the idle Speed. Coming the other way the direct
                // path set it flat. Instant up, four seconds down, is exactly
                // the "stops, then snaps back to moving" shape. The rate now
                // goes to the governor at a fixed tempo of its own — the same
                // rate-floor lesson the texture fields and the attack floor
                // already follow: a slow slider must never be allowed to
                // govern the clock.
                if (retunePausedValue(targetAmp, sFpPreviousAmpTarget, sFpAmpCurrent)) sFpAmpVel = 0.0f;
                const float eAmp   = std::exp(-omega * dt);
                const float dxAmp  = sFpAmpCurrent - targetAmp;
                const float tmpAmp = sFpAmpVel + omega * dxAmp;
                sFpAmpCurrent = targetAmp + (dxAmp + tmpAmp * dt) * eAmp;
                sFpAmpVel     = (sFpAmpVel - omega * tmpAmp * dt) * eAmp;
                // A critical spring cannot overshoot from rest, but it CAN when
                // it inherits velocity from the direct path above, and amp is a
                // magnitude: a negative amplitude is a sign flip on the rendered
                // angle, i.e. the one discontinuity this whole channel exists to
                // avoid. Clamp at the floor and kill the velocity there rather
                // than let it bounce.
                if (sFpAmpCurrent < 0.0f) {
                    sFpAmpCurrent = 0.0f;
                    if (sFpAmpVel < 0.0f) sFpAmpVel = 0.0f;
                }
                sFpSpeedCurrent = targetSpeed;
            }
            // Always reach ApplyPerlin1p when the recoil kick is active,
            // even if the shake amp has decayed. The kick is independent
            // of the Perlin shake — skipping it would drop the recoil
            // beat on profiles with very low amp.
            //
            // NO LONGER PINNED TO THE CACHED CAST CELL THROUGH THE TAIL.
            // The pin predates the 1p texture crossfade: it held the cast
            // cell's texture (and, via the crossfade
            // SIGNATURE) for the whole burst-tail window — 1.6s at the
            // default Fade Duration — which is why the release crossfade
            // armed a second and a half AFTER the spell left the hand
            // (18:56 log: cast end committed 57.600, [XFADE] 1p 58.775).
            // The crossfade does that job properly now: the resolution
            // hands the KEY back at the release (see ResolveFirstPersonKey),
            // the signature flips there, and the OUTGOING layer carries the
            // rendered cast texture down over the fade — including the
            // release burst's swell, which is snapshotted with it. The kick
            // itself (fpShots) never read this profile; its parameters were
            // cached at the charge latch.
            auto* applyProfile = fpProfile;

            // Cinematic dragon/centurion shakes also drive first person
            // (rotation-only — translation is skybox-only in 1p). Fold the
            // shake amplitude on top of the ambient 1p amp so a landing /
            // breath / bite kicks the 1p view even when ambient 1p noise is
            // off. Same envelope the 3p path uses (one call per frame).
            float dragonSpeed1p     = 0.0f;
            const float dragonAmp1pRaw = UpdateDragonShake(player, dt, dragonSpeed1p, /*a_fp=*/true);
            // The 1p rotation path uses a much smaller angle scale than 3p
            // (~0.00006 vs ~0.00020 rad), so the same shake amplitude reads
            // ~3x weaker in first person. Scale the dragon contribution up so
            // 1p cinematic shakes land as hard as 3p. (1p-only — 3p is a
            // separate code path and is untouched.)
            constexpr float kFpDragonBoost = 3.0f;
            const float dragonAmp1p = dragonAmp1pRaw * kFpDragonBoost;

            float amp1p   = sFpAmpCurrent;
            float speed1p = sFpSpeedCurrent;
            // [FPNOISE] per-layer decomposition. Two fixes for "centurion noise
            // in 1p with its 1p Intensity at 0" have now been shipped on
            // reasoning rather than a measurement, and the noise is still
            // there — so stop guessing and let the frame say which layer is
            // feeding it. Every fold below records what it contributed; the
            // line prints at the apply site.
            float dbgAmb = sFpAmpCurrent, dbgDrag = 0.0f, dbgJump = 0.0f,
                  dbgDraw = 0.0f, dbgAtk = 0.0f, dbgEvt = 0.0f, dbgNpc = 0.0f;
            float rot1p   = applyProfile ? applyProfile->noise.tilt        : 1.0f;
            float dj1p    = applyProfile ? applyProfile->noise.driftJitter : 0.35f;
            float rgh1p   = applyProfile ? applyProfile->noise.roughness   : 0.45f;
            // Texture-identity tag for the 1p crossfade signature (0 = state
            // profile, 1 = dragon source, 2 = jump source).
            int fpSrcTag = 0;
            if (dragonAmp1p > amp1p) {
                dbgDrag = dragonAmp1p;
                amp1p   = dragonAmp1p;
                speed1p = std::max(speed1p, dragonSpeed1p);
                // Per-source character (1p is rotation-dominant, so Position
                // Shake doesn't map here).
                rot1p   = std::max(rot1p, sDragonShakeCharOut[1].rotShake);
                dj1p    = sDragonShakeCharOut[1].driftJitter;
                rgh1p   = sDragonShakeCharOut[1].roughness;
                fpSrcTag = 1;
            }

            // ---- 1p <-> 3p AMP PARITY -------------------------------------
            // NOT a taste knob, and not a "first person feels stronger"
            // adjustment: the two apply paths render the same amp at
            // different angles. 3p rotation is 0.00020 rad per unit
            // (BuildPerlinSample) and 1p rotation is 0.00006 (ApplyPerlin1p)
            // — a factor of ~3.3. Without this a slider set to 1.00 in the
            // First Person half of an entry renders about a THIRD of what the
            // same 1.00 renders in the Third Person half, which is what "they
            // don't seem to work for first person" was (user, 2026-09-06).
            //
            // 3.0 rather than 3.33 because every other 1p fold in this branch
            // (dragon, shout, event, NPC) is calibrated on 3.0; matching them
            // matters more than the last 10%.
            //
            // The per-view DIALS are what express "1p wants less than 3p" now.
            // This only makes the two halves speak the same units, so a number
            // typed into either one means the same size of camera movement.
            constexpr float kFp1pAmpParity = 3.0f;

            // Jumping (Cinematic Effects) in FIRST PERSON. Third person swaps
            // the ambient source to the jump profile; 1p has no source-swap
            // machinery, so the arc is folded in the same way the dragon /
            // centurion shakes are — it WINS when it's louder than the state's
            // own shake, and brings the jump character with it. Same 3x boost
            // as the dragon path: the 1p rotation path uses a much smaller
            // angle scale, so without it the identical Intensity reads about a
            // third as strong here as it does in third person.
            //
            // The jump layer is rotation-led by design (tilt 2.7 / sway 1.2 in
            // 3p); 1p is rotation-only, so only the tilt term carries over.
            // env arrives PRE-SCALED by the Jumping / Falling dials.
            {
                // SLEW-BOUND (2026-08-15, "jumping noise causes a noise snap
                // in first person"): the launch burst steps env 0 -> ~2.6x
                // Jumping in ONE frame, and 1p rendered rotation scales
                // linearly with amp, so folding it in raw was a one-frame
                // ANGLE step — the same lesson as the direct-path amp slew
                // above (rise ~95% in ~105ms keeps the punch; release
                // slower). 3p never had this problem because its source-swap
                // rides the two-layer crossfade.
                // env arrives pre-scaled by the FIRST PERSON Jumping /
                // Falling dials (2026-09-06); kFp1pAmpParity then puts it in
                // the same units the Third Person half is written in (see the
                // block above — it is an angle-scale conversion, not a boost).
                static float sFpJumpSm = 0.0f;
                const float jumpWant = (jumpArc.env > 0.01f)
                    ? jumpArc.env * kFp1pAmpParity : 0.0f;
                const float ja = 1.0f - std::exp(-dt * (jumpWant > sFpJumpSm ? 28.0f : 14.0f));
                float jStep = (jumpWant - sFpJumpSm) * ja;
                // RATE CAP on the rise ("the jumping noise seems to jerk a
                // little bit sometimes", user 2026-09-06). A time constant
                // bounds the rise in TIME, not in SIZE: the step it delivers
                // on frame one is a fixed FRACTION of the target, so tripling
                // the target (parity) tripled the step. Measured at Jumping
                // 5.00 — [FPFALL] 16:10:19.308 caught the burst's FIRST frame
                // at ambient=5.00 rendered=11.33, a 6.3-unit amp move in one
                // 13.5 ms frame (~470 u/s). This caps it at 260 u/s, which
                // still lets the loudest possible burst (~39) reach full in
                // 0.15 s — inside its own 0.30 s window — and does not touch
                // ordinary settings, whose whole burst is under 8 units and
                // never approaches the cap. Rise only; the release is already
                // the slower of the two.
                // RELATIVE, not absolute (2026-09-06, third jerk capture).
                // 260 u/s sounded conservative and is not: against this user's
                // 5.00 ambient it still permits ~3.5 units in one 13.5 ms
                // frame, i.e. the rendered amplitude moving ~70% of full scale
                // between two frames. This file already names that pathology
                // exactly — the 1p cast snap was "the rotation amplitude
                // stepping ~45% of full scale in one frame" — and an ABSOLUTE
                // cap can never bound a fraction, because the fraction depends
                // on the ambient it is landing on.
                //
                // CAP THE RENDERED RISE, NOT THE FOLLOWER'S. The first pass at
                // this capped sFpJumpSm itself and killed the effect outright
                // ("there's no jump noise now"): the fold only reaches the
                // screen through `sFpJumpSm > amp1p`, so the follower must
                // first climb all the way from 0 PAST the ambient before one
                // unit of it is visible, and a rate tied to the ambient (25 u/s
                // at amp 5.00) spends the whole 0.30 s burst just getting
                // there — against a target that is collapsing the entire time.
                // It never crossed, so nothing rendered.
                //
                // Below the ambient the follower is INVISIBLE, so it climbs
                // free. Only the part that actually moves the rendered value is
                // limited, and only on the way up: at most 5x the rendered amp
                // per second (~8% per frame at 60 fps, a fifth of the
                // pathological step), with a small absolute floor so a silent
                // ambient can still get moving. From a 5.00 ambient that is
                // 5.4 -> 5.8 -> 6.3 ... reaching the top of a max-dial burst
                // inside its own 0.30 s window — just never in one frame.
                constexpr float kFpJumpRelRise = 5.0f;    // x rendered amp / second
                constexpr float kFpJumpAbsRise = 6.0f;    // amp units / second, floor
                float newJumpSm = sFpJumpSm + jStep;
                if (jStep > 0.0f && newJumpSm > amp1p) {
                    // What the jump layer is rendering right now: the ambient
                    // until the follower passes it, the follower after that.
                    const float renderedNow = (std::max)(sFpJumpSm, amp1p);
                    const float jMaxRise =
                        (std::max)(kFpJumpAbsRise, kFpJumpRelRise * renderedNow);
                    newJumpSm = std::min(newJumpSm, renderedNow + jMaxRise * dt);
                }
                sFpJumpSm = newJumpSm;
                if (sFpJumpSm < 0.001f) sFpJumpSm = 0.0f;
                if (sFpJumpSm > amp1p) {
                    dbgJump = sFpJumpSm;
                    amp1p   = sFpJumpSm;
                    speed1p = std::max(speed1p, jumpArc.speedMul);
                    rot1p   = std::max(rot1p, 2.7f);
                    dj1p    = 0.62f;   // jitter-leaning — rushing air, not idle sway
                    rgh1p   = 0.58f;
                    fpSrcTag = 2;
                }
                // [FPFALL] — "jumping and falling don't work for first
                // person". env is pre-scaled by the 1p dials, so the dials are
                // printed beside it: env==0 with a nonzero dial is an ARC
                // problem, env==0 with a zero dial is just an unset slider, and
                // a healthy env with rendered==ambient means the fold lost the
                // max() to the state's own noise. INFO and capped: this ran as
                // debug for a session with Verbose off and said nothing.
                if (jumpArc.inAir) {
                    static std::chrono::steady_clock::time_point sFpFallLog{};
                    static int sFpFallProbe = 0;
                    const auto fnow = CameraEffectClock::Now();
                    if (sFpFallProbe < 60 &&
                        std::chrono::duration<float>(fnow - sFpFallLog).count() > 0.5f) {
                        sFpFallLog = fnow;
                        ++sFpFallProbe;
                        spdlog::debug("[FPFALL] jumpDial={:.2f} fallDial={:.2f} env={:.2f} "
                                     "foldAmp={:.2f} ambient={:.2f} rendered={:.2f} | "
                                     "spd={:.0f} rushF={:.2f} windF={:.2f} clear={:.2f} ({}/60)",
                                     settings.jumpNoiseAmpFp, settings.fallNoiseAmpFp,
                                     jumpArc.env, sFpJumpSm, sFpAmpCurrent, amp1p,
                                     jumpArc.dbgSpeed, jumpArc.dbgRushF,
                                     jumpArc.dbgWindF, jumpArc.dbgClear,
                                     sFpFallProbe);
                    }
                }
            }

            // --- 1p crossfade SIGNATURE, captured HERE: after the texture-
            // identity swaps (state / dragon / jump), before the additive
            // beat folds below — beats decorate the texture, they are not a
            // texture change, and letting them into the signature would
            // re-arm a crossfade on every beat. Raw values only (rot/dj/rgh
            // straight off the source, not the eased renders), plus the
            // resolved-entry pointer, which is what flips on a state change.
            const int         fpSigTag = fpSrcTag;
            const float       fpSigRot = rot1p;
            const float       fpSigDj  = dj1p;
            const float       fpSigRgh = rgh1p;
            // Speed is part of the signature too (3p has always had it), but
            // as the RAW profile field — NOT the live speed1p, which carries
            // the dragon/jump/shout speed rides and would re-arm the fade
            // every frame of an envelope. Catches same-profile-object rate
            // changes (Quick Tune edits) that the pointer can't.
            const float       fpSigSpd = applyProfile ? applyProfile->noise.speed : 0.0f;

            // Sheathing / Unsheathing beat in FIRST PERSON. 3p adds it as a
            // separate additive layer on the crossfade; 1p has a single
            // texture, so the beat is folded onto the amp the same way the
            // dragon and jump layers are. Additive rather than max() here
            // because the beat is explicitly punctuation ON TOP of whatever
            // the state is already doing — that is the whole design of the
            // effect — and 1p's single-sample path has no other way to express
            // "swell the current texture".
            //
            // Its OWN Intensity / Speed / character, not the third-person
            // ones (user request 2026-09-06: "sheathe/unsheathe should just
            // have separate third person and first person sliders"), through
            // kFp1pAmpParity so the number typed into the First Person half
            // means the same size of movement the Third Person half's does.
            // What used to be wrong here was not the factor but the single
            // shared dial: one Intensity had to serve a view with a position
            // channel at arm's length AND a rotation-only view on the eye.
            const float drawBeatAmp = drawBeatEnv * settings.weaponDrawNoiseIntensityFp;
            if (drawBeatAmp > 0.0001f) {
                dbgDraw = drawBeatAmp * kFp1pAmpParity;
                amp1p += dbgDraw;
                rot1p  = std::max(rot1p, settings.weaponDrawNoiseCharFp.rotShake);
                speed1p = std::max(speed1p,
                                   std::max(0.1f, settings.weaponDrawNoiseSpeedFp));
                if (settings.weaponDrawNoiseCharFp.driftJitter > 0.0f)
                    dj1p  = settings.weaponDrawNoiseCharFp.driftJitter;
                if (settings.weaponDrawNoiseCharFp.roughness > 0.0f)
                    rgh1p = settings.weaponDrawNoiseCharFp.roughness;
            }

            // Attack beat in FIRST PERSON. The cell is already a 1p entry, so
            // unlike the dragon / jump / draw folds there is NO 3x view boost —
            // its Intensity means here exactly what it means in the menu.
            {
                // 1p is rotation-only, so energy is amp x tilt.
                float atkRise1p = 0.06f, atkFall1p = 0.35f;
                BeatRampFor(sAtkCell.amp * sAtkCell.tilt, amp1p * rot1p,
                            atkRise1p, atkFall1p);
                // Latched at ARM, same as 3p — the 1p ambient moves under a
                // running beat for the same reasons. See the long note there.
                if (sAtkBeat.rearmPending || !sAtkRampValid) {
                    sAtkRiseLatched = atkRise1p;
                    sAtkFallLatched = atkFall1p;
                    sAtkRampValid   = true;
                }
                atkRise1p = sAtkRiseLatched;
                atkFall1p = sAtkFallLatched;
                const bool atkLive1p = sAtkCellValid && sAtkCell.amp > 0.0001f;
                const float atkEnv1p = atkLive1p
                    ? EventBeatEnvelope(sAtkBeat, true,
                                        sAtkCell.attackDuration, atkFall1p, atkRise1p)
                    : 0.0f;   // see the note in `consider`: sAtkBeat is shared
                if (atkEnv1p > 0.0001f) {
                    dbgAtk = sAtkCell.amp * atkEnv1p;
                    amp1p  += dbgAtk;
                    rot1p   = std::max(rot1p, sAtkCell.tilt);
                    speed1p = std::max(speed1p, sAtkCell.speed);
                    if (sAtkCell.driftJitter > 0.0f) dj1p  = sAtkCell.driftJitter;
                    if (sAtkCell.roughness   > 0.0f) rgh1p = sAtkCell.roughness;
                }
            }

            // Bats / Reanimation / Summoning in FIRST PERSON. Folded in
            // exactly like the sheathe beat above — additive on the state's
            // own texture, with the same 3x view boost the other cinematic
            // sources use so an Intensity tuned in third person lands in the
            // same place here. 1p is rotation-only, so only the character's
            // Rotation Shake carries over.
            if (eventBeatNow.amp > 0.0001f && eventBeatNow.chr) {
                constexpr float kFpEventBoost = 3.0f;
                dbgEvt = eventBeatNow.amp * kFpEventBoost;
                amp1p  += dbgEvt;
                rot1p   = std::max(rot1p, eventBeatNow.chr->rotShake);
                speed1p = std::max(speed1p, eventBeatNow.speed);
                if (eventBeatNow.chr->driftJitter > 0.0f) dj1p  = eventBeatNow.chr->driftJitter;
                if (eventBeatNow.chr->roughness   > 0.0f) rgh1p = eventBeatNow.chr->roughness;
            }

            // NPC concentration casting in FIRST PERSON.
            if (npcNow.amp * settings.npcNoiseIntensityFp > 0.0001f) {
                constexpr float kFpNpcBoost = 3.0f;
                dbgNpc = npcNow.amp * settings.npcNoiseIntensityFp * kFpNpcBoost;
                amp1p  += dbgNpc;
                rot1p   = std::max(rot1p, npcNow.tilt);
                speed1p = std::max(speed1p, npcNow.speed);
                if (npcNow.dj  > 0.0f) dj1p  = npcNow.dj;
                if (npcNow.rgh > 0.0f) rgh1p = npcNow.rgh;
            }

            // --- FLOW GOVERNOR — the last unbounded channel in 1p ------------
            //
            // Everything above may STEP the sample rate: the direct path sets
            // it flat on the frame a cast begins, and every cinematic layer
            // (dragon, jump, draw beat, event beats, proximity, NPC) rides it
            // up with a bare max(). None of that was ever smoothed, and the
            // rate is not a cosmetic parameter — the rendered angle is
            // theta(t) = A · P(clock(t)), so its VELOCITY carries d(clock)/dt
            // as a factor. Stepping the rate steps the angular velocity, which
            // is felt as the view lurching into or out of motion even though
            // the position itself never jumps. That is what was left of "the
            // noise stops and then snaps back to moving" around a Repulse: at
            // rest the clock ran at the idle cell's Speed, a cast slammed it to
            // the cast cell's Speed in one frame, and the way back down was the
            // amp spring's omega — the First Person Transition Speed slider,
            // which at its low end is a 1.25s time constant, so the flow spent
            // four seconds dying back to a crawl before the next cast snapped
            // it awake again.
            //
            // So bound the slew here, at the render boundary, exactly the way
            // batch 8 bounded the direct-path amp: one governor, downstream of
            // every writer, on the value that is actually rendered. Fixed
            // tempo, never the user's transition slider (the standing
            // rate-floor lesson). Asymmetric — a beat may spin the texture up
            // quickly, but it always winds down more gently than it wound up,
            // which is what makes one event flow into the next instead of
            // ending. The floor is applied to the TARGET so the governor can
            // never be chasing a value the clock would clamp anyway.
            //
            // The attack floor lives here too. It used to reach the clock
            // through the amp spring's omega, which this governor replaced —
            // so without it an attack's texture would now take 0.86s to let go
            // instead of 0.21s and would outstay its own swing, which is the
            // exact failure kAttackReleaseWindow was added to prevent. Floor
            // both directions: an attack has to arrive AND leave inside the
            // animation.
            {
                constexpr float kFlowRiseRate = 9.0f;   // ~95% in 0.33s
                constexpr float kFlowFallRate = 3.5f;   // ~95% in 0.86s
                const bool  attackFloor = sFpAttackFastTimer > 0.0f;
                const float wantFlow    = (std::max)(kMinFlowRate, speed1p);
                retunePausedValue(wantFlow, sFpPreviousFlowTarget, sFpFlowSm);
                if (!sFpFlowSeeded) {
                    sFpFlowSm     = wantFlow;
                    sFpFlowSeeded = true;
                } else {
                    float flowRate = wantFlow > sFpFlowSm ? kFlowRiseRate : kFlowFallRate;
                    if (attackFloor) flowRate = (std::max)(flowRate, kAttackOmega);
                    sFpFlowSm += (wantFlow - sFpFlowSm) *
                                 (1.0f - std::exp(-dt * flowRate));
                }
                speed1p = sFpFlowSm;
            }

            // --- 1p texture CROSSFADE arm. A signature change snapshots the
            // outgoing texture EXACTLY as rendered (the eased statics below)
            // onto the prev layer, with its own frozen parameters and its own
            // clock — the 3p two-layer lesson applied to 1p. The new texture
            // fades in over ~0.2s while the old keeps playing underneath.
            static float sFpRotSm = 0.0f, sFpDjSm = 0.35f, sFpRghSm = 0.45f;
            // Per-transition fade rate, sized at the arm below.
            static float sFp1pXfadeRate = 1.0f / kAmbientXfadeDur;
            {
                static int         sSigTag = -1;
                static float sSigRot = 0.0f, sSigDj = 0.0f, sSigRgh = 0.0f;
                static float sSigSpd = 0.0f;
                // A DIFFERENT PROFILE OBJECT IS NOT A DIFFERENT TEXTURE.
                //
                // The pointer used to arm a fade on its own, and that is the
                // post-swing stutter (user, 2026-09-02: "a slight noise stutter
                // when I power attack and then go back", after the attack).
                // Measured across four exits, 21:04:57 onward — the cell hands
                // weapons.melee.power_attack back to weapons.melee and NOTHING
                // about the texture changes:
                //
                //   t=15ms  tgtAmp=5.000 tgtSpd=0.100 ampSm=5.000 amp1p=5.000
                //           flowSm=0.250 spd1p=0.250 rot=5.000 dj=0.350 rgh=0.450
                //           xfade=0.04
                //   t=356ms  ...every number identical...              xfade=1.00
                //
                // Every channel flat for the whole window; the only thing that
                // moved was the crossfade phase. And arming is not free: the
                // incoming layer is deliberately thrown 611.7 into the Perlin
                // field (kXfadeDecorrelate) so the equal-power mix holds its
                // RMS — which is right when the textures genuinely differ, and
                // when they do NOT it re-rolls the drift the player is watching
                // and fades between two unrelated wanderings of the same
                // texture. Constant loudness, discontinuous motion: a stutter
                // that no amplitude trace can see, which is why three of them
                // came back clean.
                //
                // The four signature fields plus amp ARE the 1p texture (wobble
                // is retired, and amp rides its own spring, not this fade), so
                // when all four match there is no destination to fade toward.
                // The tag stays: it marks a real source swap (state / dragon /
                // jump), where identical numbers can still mean a different
                // sampler.
                const bool sigChanged =
                    sSigTag >= 0 &&
                    (sSigTag != fpSigTag ||
                     std::abs(sSigRot - fpSigRot) > 0.01f ||
                     std::abs(sSigDj  - fpSigDj)  > 0.01f ||
                     std::abs(sSigRgh - fpSigRgh) > 0.01f ||
                     std::abs(sSigSpd - fpSigSpd) > 0.01f);
                // Same live-edit carve-out as the 3p signature: a slider drag
                // retunes the live texture in place, it does not arm a fade
                // (which per-tick would snapshot the outgoing layer over and
                // over — the same phase-jump artifact 3p had).
                // HOW FAR APART are the two textures? Measured here, ahead of
                // the outgoing snapshot, because it now decides whether a fade
                // arms AT ALL and not just how long it runs. Amp is 1 on both
                // sides deliberately — in 1p the amplitude is carried by its
                // own spring, so only shape and tempo belong in the measure.
                const float xfDist = TextureDistance(
                    1.0f, sSigSpd,  sSigRot,  0.0f, sSigDj,  sSigRgh,
                    1.0f, fpSigSpd, fpSigRot, 0.0f, fpSigDj, fpSigRgh);
                // A FADE WITH NOTHING TO MORPH IS PURE HARM. Arming one throws
                // the incoming layer to an unrelated part of the Perlin field
                // (kXfadeDecorrelate) so the equal-power weights are honest —
                // free when the two textures actually differ, because what you
                // hear is the morph. When they DON'T differ, the throw is the
                // only content the fade has, and on a rotation channel that is
                // a discontinuous change of DIRECTION at constant loudness.
                // The file already reached this conclusion once, from the
                // [SHOUTTAIL] capture on 2026-09-03 (see the cause= line
                // below); it named the artifact but left the fade arming.
                //
                // Measured again 2026-09-06 ("jumping still jerks"): [MOTION]
                // 1p flagged exactly ONE frame per jump, every one of them the
                // takeoff frame (five spikes, each ~13 ms after its [FPFALL]
                // launch line), and every one with amp AND rot identical
                // frame-to-frame (7.62/7.62, 5.00/5.00) and xfade=0.04 — a
                // fade one frame old. Nothing but the throw moved.
                //
                // 0.02 is where AdaptiveXfadeDur is still pinned to its 0.35 s
                // floor, i.e. where the sizing function itself already says
                // "there is nothing here to morph". Below it, adopt the new
                // signature in place: no snapshot, no phase throw, no fade.
                constexpr float kFp1pMinXfadeDist = 0.02f;
                // SHAPE-ONLY CHANGES NEVER NEED A FADE, at any distance.
                //
                // Ask what a crossfade is FOR. It carries a discontinuity the
                // per-field machinery cannot: a step in ENERGY (rot — amp has
                // its own spring) or in TEMPO (spd, and the flow governor only
                // smooths the rendered rate, not a source swap). Drift/Jitter
                // and Roughness are NOT such a case: sFpDjSm / sFpRghSm are
                // one-poled at texAlpha every frame and it is the SMOOTHED
                // values that get applied (`curDriftJitter = sFpDjSm`,
                // `curRoughness = sFpRghSm`). A dj/rgh change is already
                // continuous before the fade sees it, so the fade contributes
                // nothing but its decorrelation throw — the jerk.
                //
                // Measured 2026-09-06 ("i still feel a jerk", third capture).
                // The 0.02 floor above was right in kind and too small in
                // degree: every takeoff armed `cause=[tag dj rgh] dur=0.35s`
                // with `rot 5.00->5.00 spd 0.10->0.10 dj 0.350->0.620
                // rgh 0.450->0.580` — dTex 0.20, so dist = 0.15*0.20 = 0.030,
                // just over the floor. Energy and tempo IDENTICAL on both
                // sides; only the shape moved, and the shape was being eased
                // anyway. [MOTION] flagged one frame per jump, always that
                // arm. (It armed TWICE per jump too — tag 0->2 at takeoff and
                // 2->0 about 0.25 s later, interrupting its own 0.35 s fade.)
                const bool xfShapeOnly =
                    std::abs(sSigRot - fpSigRot) <= 0.01f &&
                    std::abs(sSigSpd - fpSigSpd) <= 0.01f;
                if (sigChanged && !LiveNoiseEditActive() &&
                    (xfShapeOnly || xfDist < kFp1pMinXfadeDist)) {
                    static int sXfSkip1p = 0;
                    if (sXfSkip1p < 40) {
                        ++sXfSkip1p;
                        spdlog::info("[XFADE] 1p SKIPPED why={} dist={:.4f} tag {}->{} "
                                     "spd {:.2f}->{:.2f} rot {:.2f}->{:.2f} "
                                     "dj {:.3f}->{:.3f} rgh {:.3f}->{:.3f} ({}/40)",
                                     xfShapeOnly ? "shape-only" : "no-distance",
                                     xfDist, sSigTag, fpSigTag, sSigSpd, fpSigSpd,
                                     sSigRot, fpSigRot, sSigDj, fpSigDj,
                                     sSigRgh, fpSigRgh, sXfSkip1p);
                    }
                } else if (sigChanged && !LiveNoiseEditActive()) {
                    // Capture the last sampled live layer plus the unfinished
                    // blend beneath it. No parameter averaging or clock reassignment:
                    // every audible outgoing texture continues on its own phase.
                    fp1pOutgoing.Capture(fp1pCurrent, fp1pXfade, sFp1pXfadeRate);
                    fp1pPrevAmp = 0.0f;
                    fp1pOutgoing.Visit([&](const FirstPersonNoiseLayer& layer, float weight) {
                        fp1pPrevAmp += weight * weight * layer.amp * layer.amp;
                    });
                    fp1pPrevAmp = std::sqrt(fp1pPrevAmp);
                    fp1pXfade = 0.0f;
                    // Size this fade to how far apart the two textures are —
                    // a fixed length is what makes very different states snap
                    // (see TextureDistance).
                    // Measured on the SIGNATURE values — the outgoing source
                    // against the incoming one. Not on the rendered numbers:
                    // the live parameters are still eased toward the destination,
                    // so comparing those would understate the intended change.
                    // Amp is deliberately 1 on both sides — in 1p the amplitude
                    // is carried by its own spring, not by this crossfade, so
                    // only the texture's shape and tempo belong in the measure.
                    sFp1pXfadeRate = 1.0f / AdaptiveXfadeDur(xfDist);
                    // WHICH FIELD ARMED THIS FADE. Promoted from debug to info
                    // and made self-adjudicating (2026-09-03): the [SHOUTTAIL]
                    // capture showed a fade arming on the post-shout hand-back
                    // with amp, tilt and speed all IDENTICAL on both sides
                    // (5.00 / 5.00 / 1.00 per [FPNOISE]) — so the fade's only
                    // audible content was the kXfadeDecorrelate phase throw,
                    // which on a rotation channel is a discontinuous change of
                    // direction at constant loudness. The old line printed all
                    // five fields but not WHICH one crossed its threshold, and
                    // dj/rgh are the two the other probes cannot see. `cause`
                    // names them, so the next capture does not need a third
                    // instrument.
                    static int sXfLogs1p = 0;
                    if (sXfLogs1p < 60) {
                        ++sXfLogs1p;
                        char cause[64];
                        std::snprintf(cause, sizeof(cause), "%s%s%s%s%s",
                            sSigTag != fpSigTag                      ? "tag " : "",
                            std::abs(sSigRot - fpSigRot) > 0.01f     ? "rot " : "",
                            std::abs(sSigDj  - fpSigDj)  > 0.01f     ? "dj "  : "",
                            std::abs(sSigRgh - fpSigRgh) > 0.01f     ? "rgh " : "",
                            std::abs(sSigSpd - fpSigSpd) > 0.01f     ? "spd " : "");
                        spdlog::info("[XFADE] 1p cause=[{}] dur={:.2f}s  tag {}->{} "
                                     "spd {:.2f}->{:.2f} rot {:.2f}->{:.2f} "
                                     "dj {:.3f}->{:.3f} rgh {:.3f}->{:.3f}  "
                                     "amp1p={:.3f} prevAmp={:.3f}",
                                     cause, 1.0f / sFp1pXfadeRate,
                                     sSigTag, fpSigTag, sSigSpd, fpSigSpd,
                                     sSigRot, fpSigRot, sSigDj, fpSigDj,
                                     sSigRgh, fpSigRgh, amp1p, fp1pPrevAmp);
                    }
                    // Decorrelate the INCOMING layer, which here is the live
                    // one on timeRot: the prev snapshot just took timeRot's
                    // value, so without this the two layers are the same
                    // signal and the equal-power mix swells through the middle.
                    // Safe to jump: cur is at weight 0 on this frame.
                    timeRot += kXfadeDecorrelate;
                }
                sSigTag = fpSigTag;
                sSigRot = fpSigRot;
                sSigDj  = fpSigDj;
                sSigRgh = fpSigRgh;
                sSigSpd = fpSigSpd;
                // Linear phase; the apply site maps it through equal-power
                // smoothstep weights (see XfadeWeights).
                fp1pOutgoing.Advance(dt);
                fp1pXfade += dt * sFp1pXfadeRate;
                if (fp1pXfade >= 1.0f) { fp1pXfade = 1.0f; fp1pOutgoing.Clear(); }
                fp1pPrevAmp = 0.0f;
                fp1pOutgoing.Visit([&](const FirstPersonNoiseLayer& layer, float weight) {
                    fp1pPrevAmp += weight * weight * layer.amp * layer.amp;
                });
                fp1pPrevAmp = std::sqrt(fp1pPrevAmp);
            }

            // Head Bobbing (first person) — published every frame, ahead of
            // the "is there any noise to apply" test below, because the two
            // are independent: bob must render with every noise slider at 0.
            // Peaks: 1.30 deg of pitch and 0.75 deg of roll at Intensity 1,
            // which is a walk you can feel without it becoming the view.
            {
                const float bobInt = settings.headBobIntensityFp;
                if (bobInt > 0.0001f && headBob.weight > 0.0f) {
                    constexpr float kDeg = 0.017453293f;
                    const float w = headBob.weight * bobInt * headBob.gait;
                    // Peaks at Intensity 1: 0.50 deg pitch, 0.30 roll, 0.18 yaw
                    // — less than half what the first pass used. In first
                    // person the camera IS the head, so a degree here reads far
                    // larger than a unit of translation does in third person,
                    // and the previous values were overwhelming at any setting.
                    pendingBobPitch = headBob.vert * 0.40f * kDeg * w;
                    pendingBobRoll  = headBob.roll * 0.24f * kDeg * w;
                    pendingBobYaw   = headBob.yaw  * 0.14f * kDeg * w;
                    pendingBobValid = true;
                } else {
                    pendingBobPitch = 0.0f;
                    pendingBobRoll  = 0.0f;
                    pendingBobYaw   = 0.0f;
                    pendingBobValid = false;
                }
            }

            // ----- [SHOUTTAIL] THE HAND-BACK OUT OF A SHOUT -----------------
            //
            // "Rotation seems to be overshooting when it returns back to
            // sheathed from my dragon aspect settings" (user, 2026-09-03).
            //
            // THE SUSPECT, stated so the log can refute it: the post-shout
            // hand-off walks the AMPLITUDE and the RATE back to the underlying
            // state over the shout's Fade Duration, but it deliberately keeps
            // the shout's TEXTURE for the whole tail and swaps it at the end,
            // on the argument that "both sides are at the same amplitude and a
            // texture change at matched amplitude is not something you feel".
            // The rendered angle is not amplitude — ApplyPerlin1p renders
            // baseR = amp * rotation. Matched amplitude with UNMATCHED Rotation
            // Shake is a matched number and a different angle: at the release
            // frame the view is still swinging at (sheathed amp x SHOUT
            // rotation) and eases to (sheathed amp x sheathed rotation) over
            // sFpRotSm's ~0.125s. Whenever the shout cell's Rotation is the
            // larger of the two, that ease comes DOWN from above the state's
            // own level — an overshoot, arriving exactly at the hand-back the
            // user is describing. The crossfade arms on that same frame
            // (rot/dj/rgh/spd all change at once), and its equal-power mix can
            // add a further swell on top.
            //
            // WHAT THE LOG DECIDES: `shoutRot` vs `stateRot` says whether the
            // two textures differ at all and in which direction; `rendered`
            // (amp1p * sFpRotSm, the actual angle scale) rising above
            // `settled` (amp1p * stateRot) after `rel=1` IS the overshoot,
            // measured. If rendered never exceeds settled, the artifact is not
            // this and the tail is exonerated.
            {
                static float sTailShoutRot = 0.0f;
                static float sTailShoutSpd = 0.0f;
                static float sTailShoutDj  = 0.0f;
                static float sTailShoutRgh = 0.0f;
                static float sTailSince    = -1.0f;
                if (sShoutFadeProfile) {
                    sTailShoutRot = sShoutFadeProfile->noise.tilt;
                    sTailShoutSpd = sShoutFadeProfile->noise.speed;
                    sTailShoutDj  = sShoutFadeProfile->noise.driftJitter;
                    sTailShoutRgh = sShoutFadeProfile->noise.roughness;
                    sTailSince    = 0.0f;
                } else if (sTailSince >= 0.0f) {
                    sTailSince += dt;
                    if (sTailSince > 1.2f) sTailSince = -1.0f;
                }
                if (sTailSince >= 0.0f) {
                    static int sTailLogs = 0;
                    if (sTailLogs < 400) {
                        ++sTailLogs;
                        const float stateRot = fpProfile ? fpProfile->noise.tilt : 0.0f;
                        spdlog::debug("[SHOUTTAIL] rel={} t={:.0f}ms tgtAmp={:.3f} amp1p={:.3f} "
                                     "shoutRot={:.3f} stateRot={:.3f} rotSm={:.3f} "
                                     "rendered={:.4f} settled={:.4f} xfade={:.2f} prevAmp={:.3f} "
                                     "sig(tag={} spd={:.2f} dj={:.3f} rgh={:.3f}) "
                                     "shoutSig(spd={:.2f} dj={:.3f} rgh={:.3f})",
                                     sShoutFadeProfile ? 0 : 1, sTailSince * 1000.0f,
                                     targetAmp, amp1p,
                                     sTailShoutRot, stateRot, sFpRotSm,
                                     amp1p * sFpRotSm, amp1p * stateRot,
                                     fp1pXfade, fp1pPrevAmp,
                                     fpSigTag, fpSigSpd, fpSigDj, fpSigRgh,
                                     sTailShoutSpd, sTailShoutDj, sTailShoutRgh);
                    }
                }
            }

            // ----- [FPEXIT] WHAT HAPPENS AFTER A SWING ----------------------
            //
            // "There's a slight noise stutter with my first person melee
            // settings when I power attack and then go back" — and the stutter
            // is AFTER the attack, not at its start (user, 2026-09-02).
            //
            // Three things end after a swing, on three different clocks, and
            // nothing lines them up:
            //   1. the resolved CELL hands back (power_attack -> melee), which
            //      arms the ~0.2s texture crossfade;
            //   2. the amp spring's omega floor releases kAttackReleaseWindow
            //      (0.30s) after the Attack SUB-STATE drops — omega 14 -> 8;
            //   3. the flow governor's rate floor releases on that same timer,
            //      taking the Perlin CLOCK's slew from 14 back to 3.5, and a
            //      clock-rate step is a velocity step ([FLOW], 2026-08-09).
            // The sub-state and the cell key do not drop on the same frame, so
            // (1) and (2)/(3) can land a few frames apart — two settles where
            // the player expects one.
            //
            // So trace the whole exit: armed on the FALLING edge of the Attack
            // sub-state, one line per frame for 0.9s, every channel that can
            // move side by side. A step in `amp1p` names the amp spring; a step
            // in `speed1p` names the governor; a jump in `xfade`/`rot` names the
            // texture swap; all three smooth means the artifact is not here.
            {
                static bool  sPrevInAttack = false;
                static float sExitT        = -1.0f;
                static int   sExitLogs     = 0;
                const bool inAttackNow =
                    StateResolver::GetSingleton().GetSubState() == CameraSubState::Attack;
                if (sPrevInAttack && !inAttackNow) sExitT = 0.0f;
                sPrevInAttack = inAttackNow;
                if (sExitT >= 0.0f) {
                    sExitT += dt;
                    if (sExitT > 0.9f) {
                        sExitT = -1.0f;
                    } else if (sExitLogs < 240) {
                        ++sExitLogs;
                        spdlog::debug("[FPEXIT] t={:.0f}ms atkTimer={:.3f} inAtk={} "
                                     "tgtAmp={:.3f} tgtSpd={:.3f} ampSm={:.3f} amp1p={:.3f} "
                                     "flowSm={:.3f} spd1p={:.3f} xfade={:.2f} prevAmp={:.3f} "
                                     "rot={:.3f} dj={:.3f} rgh={:.3f}",
                                     sExitT * 1000.0f, sFpAttackFastTimer, inAttackNow ? 1 : 0,
                                     targetAmp, targetSpeed, sFpAmpCurrent, amp1p,
                                     sFpFlowSm, speed1p, fp1pXfade, fp1pPrevAmp,
                                     sFpRotSm, sFpDjSm, sFpRghSm);
                    }
                }
            }

            // Keep applying while the OUTGOING layer is still audible even
            // if the new state is silent — a tuned state fading into a
            // silent one must ride the crossfade out, not cut.
            const bool prevAudible =
                (1.0f - fp1pXfade) * fp1pPrevAmp > 0.001f;
            retunePausedValue(rot1p, sFpPreviousRotTarget, sFpRotSm);
            retunePausedValue(dj1p, sFpPreviousDjTarget, sFpDjSm);
            retunePausedValue(rgh1p, sFpPreviousRghTarget, sFpRghSm);
            if (amp1p > 0.001f || std::abs(pitchKickRad) > 1e-6f || prevAudible) {
                // TEXTURE fields eased too. The amp and rate ride the 1p
                // spring, but rotation weight / drift / roughness used to
                // swap RAW on every state change — amplitude glided while the
                // texture stepped, which is what a "rough 1p transition"
                // feels like. Fixed tau ~0.125s (the 1p base omega), NOT the
                // user's transition-speed omega: the 2026-08-02 rate-floor
                // lesson — a slow slider must never freeze the texture.
                const float texAlpha = 1.0f - std::exp(-dt * 8.0f);
                sFpRotSm += (rot1p - sFpRotSm) * texAlpha;
                sFpDjSm  += (dj1p  - sFpDjSm)  * texAlpha;
                sFpRghSm += (rgh1p - sFpRghSm) * texAlpha;
                curDriftJitter = sFpDjSm;
                curRoughness   = sFpRghSm;
                // [FPNOISE] — which layer is actually feeding first person.
                // Capped and rate-limited; INFO because the last two sessions
                // ran with Verbose off. `amb` is the resolved FP state cell,
                // i.e. ordinary ambient 1p noise and not a cinematic source at
                // all; if that is the only non-zero column then nothing
                // creature-driven is firing and the shake is the state's own.
                {
                    static int  sFpNoiseLines = 0;
                    static auto sFpNoiseLast  = CameraEffectClock::Now();
                    const auto  nowFn = CameraEffectClock::Now();
                    if (sFpNoiseLines < 60 && amp1p > 0.001f &&
                        std::chrono::duration<float>(nowFn - sFpNoiseLast).count() > 0.25f) {
                        sFpNoiseLast = nowFn;
                        ++sFpNoiseLines;
                        spdlog::debug("[FPNOISE] amp={:.3f} | amb={:.3f} drag={:.3f} jump={:.3f} "
                                     "draw={:.3f} atk={:.3f} evt={:.3f} npc={:.3f} | "
                                     "rot={:.2f} dj={:.2f} rgh={:.2f} spd={:.2f} tag={} evtsrc={}",
                                     amp1p, dbgAmb, dbgDrag, dbgJump, dbgDraw, dbgAtk,
                                     dbgEvt, dbgNpc, sFpRotSm, curDriftJitter,
                                     curRoughness, speed1p, fpSrcTag,
                                     eventBeatNow.src ? eventBeatNow.src : "-");
                    }
                }
                ApplyPerlin1p(noiseDt, amp1p, speed1p,
                              sFpRotSm, /*wobble retired*/ 1.0f,
                              pitchKickRad, pitchKickAxis,
                              /*useDeferredApply*/ castAnimating);
            } else {
                // Nothing to apply this frame — clear any stale pending
                // so the late hook doesn't apply old data, and invalidate the
                // cached face-lock delta so a held view stops shaking too.
                pendingNoiseValid  = false;
                m1pNoiseDeltaValid = false;
                fp1pCurrent.amp = 0.0f;
            }
            pendingJustApplied = false;
            // pendingNoiseValid intentionally stays set across the frame: the
            // late hook is now the sole 1p applier (it re-multiplies the same
            // pending onto each clean recompose), so pending must survive until
            // next frame's OnCameraUpdate refreshes it with new axes/thetas.
            // The amp~0 branch above clears it when the shake stops.
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();

        // Resolve the active NoiseProfile from the current state. Two-step
        // priority: shouts overrides the underlying weapon-state noise,
        // so when the player is currently shouting AND the base state is
        // one of the 6 shoutable states, look up shouts.<state>.base
        // (with ".sneak" suffix if sneaking) directly. Otherwise fall
        // back to the regular pointer-keyed lookup against
        // CameraController's last-resolved CameraProfile.
        // The environment variant of globalNoise applies when the player is in
        // an interior cell (see SettingsManager::RuntimeEnv); an active
        // location override outranks it when it carries a global of its own.
        const int env = settings.RuntimeEnv();
        SettingsManager::NoiseProfile fallbackGlobal =
            settings.ActiveLocationGlobalNoise() ? *settings.ActiveLocationGlobalNoise()
                                                 : settings.GlobalNoiseFor(env);
        SettingsManager::NoiseProfile* active = &fallbackGlobal;

        // shoutActive = the Shout sub-state is live in a shoutable state (drives
        // the escalation envelope). shoutResolved = a shout-specific noise entry
        // actually won, which is what suppresses the ordinary state-noise
        // lookup below. Keeping them separate matters: when the player shouts in
        // a state whose shout noise was never configured (or was configured
        // standing and they're now sneaking, which is a different key), the two
        // used to collapse into one flag — the state lookup was skipped and the
        // shake dropped to globalNoise, so shouting SILENCED a carefully tuned
        // state instead of decorating it. Now the underlying state's noise
        // stays, with the shout envelope riding on top.
        bool shoutActive   = false;
        bool shoutResolved = false;
        {
            const auto& sr = StateResolver::GetSingleton();
            // Shout Lag is a FRAMING hold (see IsShoutLagHolding): once the
            // natural tail has elapsed the noise stops resolving the shout
            // cell and hands back, so the shout's own Fade Duration governs
            // the tail no matter how long the camera keeps the framing.
            if (sr.GetSubState() == CameraSubState::Shout && !sr.IsShoutLagHolding()) {
                if (auto idx = ShoutableIndexFor(sr.GetState()); idx) {
                    static constexpr const char* kShoutKeys[] = {
                        "sheathed", "melee", "bow", "crossbow", "magic", "staves",
                    };
                    const bool sneaking =
                        player && player->AsActorState() && player->AsActorState()->IsSneaking();
                    const std::string prefix = std::string("shouts.") + kShoutKeys[*idx] + ".";
                    const std::string suffix = sneaking ? ".sneak" : "";

                    // Point `active` at a stateNoise key (the active
                    // environment's variant preferred) when it exists AND is
                    // enabled. Mirrors ResolveStateNoise.
                    auto tryKey = [&](const std::string& key) -> bool {
                        if (auto* lp = settings.ActiveLocationStateNoise(key)) {
                            active = lp; return true;
                        }
                        if (env != SettingsManager::kEnvOutdoor) {
                            auto& envMap = settings.StateNoiseFor(env);
                            if (auto it = envMap.find(key);
                                it != envMap.end() && it->second.enabled) {
                                active = &it->second; return true;
                            }
                        }
                        if (auto it = settings.stateNoise.find(key);
                            it != settings.stateNoise.end() && it->second.enabled) {
                            active = &it->second; return true;
                        }
                        return false;
                    };

                    // Mod-added shout binding — its own per-slot noise wins
                    // over the per-shout / per-state-base keys (the binding's
                    // noise slots are always active once bound, mirroring the
                    // other binding categories).
                    bool got = false;
                    {
                        std::uint32_t fid = 0;
                        std::string   plg;
                        if (sr.GetActiveModShout(fid, plg)) {
                            const int slot = sneaking ? 1 : 0;
                            for (auto& b : settings.weaponBindings) {
                                if (b.fpOnly || b.category != SettingsManager::BindingCategory::Shout) continue;
                                if (b.formID != fid || b.pluginName != plg) continue;
                                // Location layer first — a place's per-binding
                                // noise outranks the binding's own slot.
                                if (auto* lp = settings.ActiveLocationStateNoise(
                                        SettingsManager::BindingNoiseLocationKey(b, slot))) {
                                    active = lp;
                                } else {
                                    active = &b.NoiseProfilesFor(env)[slot];
                                }
                                got = true;
                                break;
                            }
                        }
                    }

                    // Per-shout override (the specific shout being cast) takes
                    // priority over the per-state base, mirroring the camera
                    // profile path. Falls back to base when the per-shout cell
                    // is absent/disabled. Key form matches the UI exactly:
                    // shouts.<state>.<tomlKey>[.sneak].
                    if (!got) if (auto sid = sr.GetActiveShoutId()) {
                        const auto si = static_cast<std::size_t>(*sid);
                        if (si < kShoutCount) {
                            got = tryKey(prefix + kShouts[si].tomlKey + suffix);
                        }
                    }
                    if (!got) got = tryKey(prefix + "base" + suffix);
                    // Sneak-configured-standing (and vice versa) is a routine
                    // near-miss: the ".sneak" variant is a separate key with no
                    // implicit fallback. Try the non-sneak pair before giving up
                    // so a shout entry the player DID tune still applies.
                    if (!got && sneaking) {
                        if (auto sid = sr.GetActiveShoutId()) {
                            const auto si = static_cast<std::size_t>(*sid);
                            if (si < kShoutCount) got = tryKey(prefix + kShouts[si].tomlKey);
                        }
                        if (!got) got = tryKey(prefix + "base");
                    }

                    shoutActive   = true;
                    // Only skip the pointer path when a shout entry actually won;
                    // otherwise the underlying state's noise stays in charge.
                    shoutResolved = got;
                }
            }
        }

        // ==== Paragliding noise (Cinematic Effects; mod support) ====
        // While a glide is live the paraglide cell supplies the baseline —
        // over the shout copy, the airborne wind (a deployed glider is its own
        // state, not freefall), the attack-lag / cast-release bypasses and the
        // pointer path, all of which stand down below. A customized specific
        // animation replaces this baseline, matching framing. Its raw fields are
        // stable, so the crossfade arms exactly once per transition and the
        // ordinary two-layer machinery carries deploy (air wind -> glide) and
        // release (glide -> air or ground) like any state change.
        bool  paraResolved = false;
        bool  paraAnimationNoise = false;
        float paraAltMul   = 1.0f;
        // True on the frame the glide STARTS or ENDS — the two frames the
        // ambient source changes to/from the glide cell, and therefore the
        // two crossfades the user's Fade Duration governs. Consumed at the
        // arm site far below (same frame, same function).
        bool  paraXfadeEdge = false;
        // ...and true for the DEPLOY half only. The two hand-offs are not
        // the same event: landing is a settle that earns the user's full
        // Fade Duration, but deploying happens mid-fall with the fall/
        // landing wind still running, and that texture has to get out of
        // the way of the canopy quickly ("landing noise needs to fade out
        // sooner when activating paraglider", user 2026-08-16).
        bool  paraDeployEdge = false;
        {
            // Smoothed altitude factor persists across the landing frame so
            // the glide layer FADES OUT of the crossfade at its thinned
            // level instead of popping back to full for the hand-off.
            static float sParaAltSm  = 1.0f;
            static bool  sWasParaAlt = false;
            // Airspeed factor, same persistence reasoning as the altitude
            // one: it must survive the landing frame so the outgoing glide
            // layer fades from the level it was actually playing at.
            static float sParaSpdSm  = 1.0f;
            if (settings.paraglideEnabled &&
                StateResolver::GetSingleton().IsParagliding()) {
                // Location layer first, the same precedence every other noise
                // key uses: a place's paraglide noise outranks the global one
                // (2026-08-16, "add location overrides to paraglider"). The
                // key string must stay in step with the
                // RenderLocationNoiseButton call in the Paragliding pane.
                if (auto* lp = settings.ActiveLocationStateNoise("paraglide"))
                    active = lp;
                else
                    // Env variant, the way every other cell resolves
                    // (2026-09-05): indoor glides read the indoor twin.
                    active = settings.RuntimeEnv() == SettingsManager::kEnvIndoor
                                 ? &settings.paraglideNoiseIndoor
                                 : &settings.paraglideNoise;
                paraResolved  = true;
                shoutResolved = false;
                shoutActive   = false;
                // Match framing precedence: the cinematic glide is the
                // baseline, an explicitly tuned player animation replaces it.
                // Untuned animations keep the wind. A tuned zero amplitude
                // remains an intentional silence, not a request to fall back.
                if (auto* animationNoise = settings.ResolveAnimationNoise(
                        AnimationCameraController::GetSingleton().ActiveEntryUid())) {
                    active = animationNoise;
                    paraAnimationNoise = true;
                }

                // ALTITUDE FADE ("noise weaker when you get close to the
                // ground, fading naturally into sheathed"): ray straight
                // down, thin the glide texture as the ground closes, so the
                // landing crossfade picks up an already-quiet layer and the
                // ground state's noise takes over seamlessly. Rides the
                // POST-SIGNATURE amp channel (the jump-envelope pattern
                // below) — never the raw cell fields, which would re-arm
                // the crossfade every single frame (the live-noise-edit
                // lesson).
                //
                // RAY ORIGIN — fixed 2026-08-17. This used to start 70 units
                // BELOW the player origin ("under the capsule so it can't
                // self-hit"), but the origin is at the FEET (which is why the
                // projectile aim code adds 0.55 * GetHeight() to reach a
                // torso). So for the last ~70 units of the descent the ray
                // started UNDERGROUND, hit nothing, and reported the miss
                // value — "high" — which swelled the wind back to full texture
                // at exactly the moment this fade exists to quieten: the flare.
                // Start just ABOVE the feet instead, matching the ground-
                // clearance gate in PollJumpArc, so a low approach measures
                // instead of falling off the end of its own range.
                float ground = 600.0f;   // treat a miss as "high"
                if (player) {
                    if (auto* cell = player->GetParentCell()) {
                        if (auto* bhkW = cell->GetbhkWorld()) {
                            if (auto* hkW = bhkW->GetWorld1()) {
                                const float ws = RE::bhkWorld::GetWorldScale();
                                const auto  p  = player->GetPosition();
                                constexpr float kStartUp = 10.0f;
                                constexpr float kRange   = 600.0f;
                                RE::hkpWorldRayCastInput  in;
                                RE::hkpWorldRayCastOutput out;
                                in.from.quad = _mm_setr_ps(p.x * ws, p.y * ws,
                                                           (p.z + kStartUp) * ws, 0.0f);
                                in.to.quad   = _mm_setr_ps(p.x * ws, p.y * ws,
                                                           (p.z + kStartUp - kRange) * ws, 0.0f);
                                in.filterInfo.filter = static_cast<std::uint32_t>(
                                    RE::COL_LAYER::kCameraSphere);
                                in.enableShapeCollectionFilter = false;
                                hkW->CastRay(in, out);
                                if (out.HasHit())
                                    ground = std::max(0.0f,
                                        out.hitFraction * kRange - kStartUp);
                            }
                        }
                    }
                }
                // Full texture above ~350u, thinned to 15% skimming the
                // ground, smoothstep between; one-pole (~0.25s) so a ledge
                // edge crossing under the flight path can't step the amp.
                float t = std::clamp((ground - 80.0f) / 270.0f, 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                const float tgt = 0.15f + 0.85f * t;
                if (!sWasParaAlt) {
                    sParaAltSm = tgt;   // low deploys start quiet, no blip
                } else {
                    const float alpha = 1.0f - std::exp(-dt / 0.25f);
                    sParaAltSm += alpha * (tgt - sParaAltSm);
                }
                // TEMPORARY [PARAALT] — the origin move puts the ray start
                // INSIDE the player capsule, which the old placement was
                // avoiding on principle. kCameraSphere is the layer camera
                // collision uses precisely because it ignores the player, so
                // this should be fine — but "should be" is not "is", and a
                // self-hit reads as a permanently-zero clearance that would
                // silently pin the glide texture to its 15% floor. Two
                // signatures to look for: ground stuck near 0 while plainly
                // high in the air (self-hit), or ground pinned at 600 through
                // a whole descent (still missing). Half-second samples, hard
                // capped. Strip once a glide has been flown and read.
                {
                    static float sParaAltDiagT = 0.0f;
                    static int   sParaAltLogs  = 0;
                    sParaAltDiagT += dt;
                    if (sParaAltDiagT >= 0.5f && sParaAltLogs < 40) {
                        sParaAltDiagT = 0.0f;
                        ++sParaAltLogs;
                        spdlog::info("[PARAALT] ground={:.0f} altMul={:.2f} playerZ={:.0f}",
                                     ground, sParaAltSm,
                                     player ? player->GetPosition().z : 0.0f);
                    }
                }
                // AIRSPEED FADE ("lessen the noise when not moving while
                // paragliding"): the glide texture is WIND, so it should
                // answer to how fast you are actually travelling — hanging
                // almost stationary under the canopy should be near-silent,
                // a full-speed dive should be the full texture. Measured
                // from position deltas (3D: a vertical plummet is wind too)
                // and folded into the SAME post-signature amp channel as the
                // altitude fade, for the same reason — touching the raw cell
                // fields would re-arm the crossfade every frame.
                {
                    static RE::NiPoint3 sPrevPos{};
                    static bool         sPrevInit = false;
                    float spd = 0.0f;
                    if (player) {
                        const auto p = player->GetPosition();
                        if (sPrevInit && dt > 0.0001f) {
                            const float dx = p.x - sPrevPos.x;
                            const float dy = p.y - sPrevPos.y;
                            const float dz = p.z - sPrevPos.z;
                            const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
                            if (d < 800.0f) spd = d / dt;   // teleport guard
                        }
                        sPrevPos = p; sPrevInit = true;
                    }
                    // Silent-ish under ~120 u/s, full texture from ~520 u/s.
                    float ts = std::clamp((spd - 120.0f) / 400.0f, 0.0f, 1.0f);
                    ts = ts * ts * (3.0f - 2.0f * ts);
                    const float tgtSpd = 0.20f + 0.80f * ts;
                    if (!sWasParaAlt) {
                        sParaSpdSm = tgtSpd;   // deploy at the real level
                    } else {
                        // Slower than the altitude filter: gusts of speed
                        // should swell the wind, not flutter it.
                        const float aSpd = 1.0f - std::exp(-dt / 0.45f);
                        sParaSpdSm += aSpd * (tgtSpd - sParaSpdSm);
                    }
                }

                if (!sWasParaAlt) {
                    paraXfadeEdge  = true;   // deploy
                    paraDeployEdge = true;
                }
                sWasParaAlt = true;
            } else {
                if (sWasParaAlt) paraXfadeEdge = true;    // landing / closed

                // Ease back to neutral AFTER the glide so the outgoing glide
                // layer keeps its thinned level through the landing fade,
                // then the factor quietly stops mattering.
                sWasParaAlt = false;
                sParaAltSm += (1.0f - std::exp(-dt / 0.6f)) * (1.0f - sParaAltSm);
                sParaSpdSm += (1.0f - std::exp(-dt / 0.6f)) * (1.0f - sParaSpdSm);
            }
            // Both fades ride the one post-signature multiplier: low and
            // slow is quieter than either alone, which is the honest
            // answer — a stationary hover near the ground has almost no
            // wind in it.
            // Wind responds to height/speed; an animation's own texture should
            // retain the strength the user tuned. Keep the wind followers live
            // underneath so returning to the baseline uses the current flight.
            paraAltMul = paraAnimationNoise ? 1.0f : sParaAltSm * sParaSpdSm;
        }

        // ==== Swim speed fade — the paraglider's airspeed idea, in water ====
        //
        // "Should swimming noise depend on movement speed like paraglider"
        // (user, 2026-08-20): yes, and for the same reason. Treading water is
        // not the same event as swimming across a lake, but every `.swim` cell
        // played one flat texture for both, so floating still in a pool shook
        // exactly as hard as a full crossing.
        //
        // Deliberately NOT a per-cell setting. Swimming is not one noise cell —
        // it is a `.swim` sub-state on every weapon state (sheathed.swim,
        // weapons.melee.swim, …), so a per-cell knob would be ten knobs that
        // all want the same value. It rides the same POST-SIGNATURE amp channel
        // the paraglide fades use, which is what keeps it from re-arming the
        // ambient crossfade every frame (touching the raw cell fields would).
        //
        // Scaled for water, not air: the paraglide ramp starts at 120 u/s and
        // tops out at 520, and a swimming player never reaches either — vanilla
        // swim speed is around walking pace. Floor is 0.25 rather than the
        // glider's 0.20 because a body in water is never quite still.
        float swimSpdMul = 1.0f;
        {
            static float sSwimSpdSm = 1.0f;
            static bool  sWasSwim   = false;
            const bool swimming =
                StateResolver::GetSingleton().GetSubState() == CameraSubState::Swimming;
            if (swimming) {
                static RE::NiPoint3 sPrevSwimPos{};
                static bool         sPrevSwimInit = false;
                float spd = 0.0f;
                if (player) {
                    const auto p = player->GetPosition();
                    if (sPrevSwimInit && dt > 0.0001f) {
                        const float dx = p.x - sPrevSwimPos.x;
                        const float dy = p.y - sPrevSwimPos.y;
                        const float dz = p.z - sPrevSwimPos.z;
                        const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d < 800.0f) spd = d / dt;   // teleport guard
                    }
                    sPrevSwimPos = p; sPrevSwimInit = true;
                }
                float ts = std::clamp((spd - 20.0f) / 180.0f, 0.0f, 1.0f);
                ts = ts * ts * (3.0f - 2.0f * ts);
                const float tgt = 0.25f + 0.75f * ts;
                if (!sWasSwim) {
                    sSwimSpdSm = tgt;      // enter the water at the real level
                } else {
                    // Same reasoning as the glider's 0.45s: strokes should swell
                    // the texture, not flutter it.
                    sSwimSpdSm += (1.0f - std::exp(-dt / 0.45f)) * (tgt - sSwimSpdSm);
                }
                sWasSwim = true;
            } else {
                // Ease back to neutral AFTER leaving the water so the outgoing
                // swim layer keeps its thinned level through the exit
                // crossfade, exactly like the glide one.
                sWasSwim = false;
                sSwimSpdSm += (1.0f - std::exp(-dt / 0.6f)) * (1.0f - sSwimSpdSm);
            }
            swimSpdMul = sSwimSpdSm;
        }

        // ==== Jumping noise (Cinematic Effects) — third-person consumer ====
        // The arc itself is polled before the POV split (PollJumpArc, above);
        // this is only what third person does with it. While airborne the
        // ambient SOURCE swaps to a constant-character jump profile (raw
        // fields never change → the crossfade signature stays stable) and the
        // phase envelope rides targetAmp/targetSpeed AFTER the signature —
        // same pattern as the shout/roar envelopes, so per-frame modulation
        // can't re-arm blends. Landing crossfades back to the ground state's
        // noise via the existing two-layer machinery; the takeoff SNAPS the
        // crossfade (the burst masks the source cut) so there's no ramp-in lag.
        bool  jumpResolved  = false;
        float jumpAmpMul    = 1.0f;
        float jumpSpeedMul  = 1.0f;
        const bool snapJumpXfade = jumpArc.snapXfade;
        {
            // Diag bookkeeping: record that a shout held the source while the
            // player was in the air (read by [JUMPARC] on landing).
            if (shoutResolved && jumpArc.inAir) sJumpArcShoutBlocked = true;
            // env arrives PRE-SCALED by the Jumping / Falling dials.
            if (!paraResolved && (!shoutResolved || jumpArc.windOpen) &&
                jumpArc.env > 0.01f) {
                // Constant raw fields — the signature must never see a
                // per-frame change or the crossfade re-arms every frame.
                static SettingsManager::NoiseProfile sJumpProfile;
                sJumpProfile.enabled     = true;
                sJumpProfile.amp         = 1.0f;
                sJumpProfile.speed       = 1.0f;
                sJumpProfile.sway        = 1.2f;    // light positional buffeting
                sJumpProfile.tilt        = 2.7f;    // rotation-led, reads as body/head
                sJumpProfile.driftJitter = 0.62f;   // jitter-leaning — rushing air, not idle sway
                sJumpProfile.roughness   = 0.58f;
                active       = &sJumpProfile;
                jumpResolved = true;
                // The wind owns the source now — keep the shout envelope and
                // its post-end fade tail (which would otherwise re-point
                // `active` at the shout copy) off it for the rest of the
                // airtime. The shout resumes normally once we land.
                shoutResolved = false;
                shoutActive   = false;
                jumpAmpMul   = jumpArc.env;
                jumpSpeedMul = jumpArc.speedMul;
            }
        }

        // Attack Lag bypass: while the lag holds the CAMERA on the attack
        // profile (the feature is scoped to the Categories / Target Lock
        // framing trees only), the noise must follow the LIVE state — holding
        // the attack/cast shake for the whole lag window was the reported
        // problem. GetLastResolvedProfile() is the HELD framing profile, so
        // don't key off it here; a hold guarantees the live resolution is the
        // held family's PLAIN base (sub None, no cast, no power attack, not
        // sneaking), so the live noise source is exactly the state's base
        // noise entry. Resolve it by key with the same indoor-preferred /
        // enabled-gated lookup ResolveStateNoise uses; no entry (or disabled)
        // leaves the global fallback — the same result the pointer path would
        // give for an uncustomized base state. When the hold commits, the
        // pointer path resumes and picks the exact final source (per-weapon
        // noise etc.); for those setups the crossfade smooths any difference.
        // ...and the same treatment for the FIRE-AND-FORGET CAST LINGER.
        //
        // The linger is the other framing device the noise was silently
        // inheriting: from the trigger release, StateResolver keeps reporting
        // FireAndForget for 350ms (so the camera stays parked on the cast
        // framing while the projectile flies) and only then commits — at which
        // point the noise finally started its crossfade back. The result was
        // hold-then-morph: a third of a second of full cast texture AFTER the
        // action, then the fade — which is "doesn't fade out smoothly, doesn't
        // line up with the action" in one mechanism. Handing the noise back at
        // the RELEASE makes the ordinary adaptive crossfade BE the release
        // envelope: the cast texture rides down from the exact frame the spell
        // leaves the hand, over ~0.7s for typical cells, while the base fades
        // in underneath. Framing untouched — noise only, the same split the
        // Attack Lag bypass below already established.
        //
        // ONE stand-down, from a live signal rather than a timing guess:
        // IsLiveCasting — the linger can be masking a real cast (the next
        // charge already building, or a concentration stream in the other
        // hand). There the cast texture is still earned; no hand-off. A chain
        // therefore never flickers: charging the next spell makes this true
        // again before the fade can re-arm the other way.
        //
        // The SESSION is deliberately NOT consulted. It used to be a second
        // stand-down, and it made the release inconsistent in exactly the way
        // the user reported: a single cast (or both hands on one frame) is one
        // release with no session, so it faded on the spot — but casting one
        // hand and then the other within a second flips castSessionActive ON,
        // so the SECOND release got no hand-off at all and the cast texture
        // parked through the full 1000ms session linger before the ordinary
        // adaptive fade ran (~1.7s total against ~0.6s). The session is a
        // FRAMING device (it sizes the camera's parking window for rapid
        // fire) and it keeps doing that job; for the noise, every release
        // with empty hands is the same event and fades the same way —
        // live-casting alone decides whether the cast texture is still owed.
        const auto& srNoise = StateResolver::GetSingleton();
        const bool fofRelease =
            srNoise.IsCastLingering() && !srNoise.IsLiveCasting() &&
            (srNoise.GetState() == CameraState::Magic ||
             srNoise.GetState() == CameraState::Staves);

        bool lagResolved = false;
        if (!paraResolved && !shoutResolved && !jumpResolved &&
            (srNoise.IsAttackLagHolding() || fofRelease)) {
            const char* baseKey = nullptr;
            switch (srNoise.GetState()) {  // held == live family
            case CameraState::Melee:    baseKey = "weapons.melee";    break;
            case CameraState::Bow:      baseKey = "weapons.bow";      break;
            case CameraState::Crossbow: baseKey = "weapons.crossbow"; break;
            case CameraState::Magic:    baseKey = "weapons.magic";    break;
            case CameraState::Staves:   baseKey = "weapons.staves";   break;
            default: break;
            }
            if (baseKey) {
                // An Attack Lag hold guarantees the live resolution is the
                // PLAIN base (not sneaking); the cast linger does NOT — you
                // can sneak-cast — so on the release path the sneak variant
                // has to be tried first or a sneaking mage hands back to the
                // standing cell.
                std::string keyStr(baseKey);
                if (fofRelease && !srNoise.IsAttackLagHolding() &&
                    player && player->AsActorState() &&
                    player->AsActorState()->IsSneaking()) {
                    keyStr += ".sneak";
                }
                auto tryNoiseKey = [&](const std::string& k) -> bool {
                    if (auto* lp = settings.ActiveLocationStateNoise(k)) {
                        active = lp;
                        return true;
                    }
                    if (env != SettingsManager::kEnvOutdoor) {
                        auto& envMap = settings.StateNoiseFor(env);
                        if (auto it = envMap.find(k);
                            it != envMap.end() && it->second.enabled) {
                            active = &it->second;
                            return true;
                        }
                    }
                    if (auto it = settings.stateNoise.find(k);
                        it != settings.stateNoise.end() && it->second.enabled) {
                        active = &it->second;
                        return true;
                    }
                    return false;
                };
                if (!tryNoiseKey(keyStr) && keyStr != baseKey) {
                    tryNoiseKey(baseKey);   // no sneak cell configured
                }
                lagResolved = true;  // skip the held-pointer path either way
            }
        }

        bool isWerewolfRoar = false;
        if (!paraResolved && !shoutResolved && !jumpResolved && !lagResolved) {
            if (auto* selected = CameraController::GetLastResolvedProfile()) {
                // Match the outdoor pointer AND every environment variant of
                // it — CameraController routes through ResolveByEnv, which
                // hands back a different pointer (into the indoor or city
                // storage) whenever the player isn't plainly outdoors. Without
                // the variant check, isWerewolfRoar never fires there and the
                // entire envelope path is dead code inside cells or towns.
                // Match the Target Lock roar slot too — while locked the
                // picker selects tlTransformationsWerewolfRoar, and without
                // it the roar envelope (howl wind-up ramp, fast snap-off)
                // silently dropped to the generic lerp during target lock.
                auto* roarOut   = &settings.transformationsWerewolfRoar;
                auto* tlRoarOut = &settings.tlTransformationsWerewolfRoar;
                const auto matchesAnyEnv = [&](CameraProfile* outdoorPtr) {
                    if (selected == outdoorPtr) return true;
                    for (int e = SettingsManager::kEnvIndoor; e < SettingsManager::kEnvCount; ++e) {
                        auto* v = settings.VariantOf(outdoorPtr, e);
                        if (v && selected == v) return true;
                    }
                    return false;
                };
                if (matchesAnyEnv(roarOut) || matchesAnyEnv(tlRoarOut)) {
                    isWerewolfRoar = true;
                }
                // Werewolf Feeding is a BEAT (event beat 0, the Camera Noise
                // page's Feeding row). Its state-noise cell is orphaned -
                // nothing edits it any more - yet a preset that still carries
                // one (amp 5, enabled) was playing it for the whole feed
                // state, invisible to every menu (user, 2026-09-05). Play the
                // werewolf base cell underneath the beat instead.
                {
                    auto* feedOut   = &settings.transformationsWerewolfFeeding;
                    auto* tlFeedOut = &settings.tlTransformationsWerewolfFeeding;
                    if (matchesAnyEnv(feedOut) || matchesAnyEnv(tlFeedOut)) {
                        const bool drawn = StateResolver::GetSingleton().IsWeaponDrawn();
                        selected = settings.ResolveByEnv(
                            drawn ? &settings.transformationsWerewolf
                                  : &settings.transformationsWerewolfSheathed);
                    }
                }
                std::string resolvedKey;
                auto* perState =
                    SettingsManager::GetSingleton().ResolveStateNoise(selected, &resolvedKey);
                // [SHOUTNOISE] PROBE (2026-09-04, Verbose only). The shout
                // block above promises that an untuned shout "leaves the
                // underlying state's noise in charge" - but the pointer it
                // hands this path is the SHOUT profile, whose eligible key is
                // "shouts.base.<state>" / "shouts.override.<state>.<shout>",
                // and no Camera Noise cell is ever written under those keys
                // (the page writes "shouts.<state>.base"). Reading the code,
                // that lookup should miss and drop the shake to GLOBAL for the
                // whole shout. One line per shout names what actually
                // resolved, so the next play session either confirms the
                // fallthrough or clears it. No behaviour change here.
                {
                    // Info level with a hard cap: the 2026-09-04 19:25 session
                    // ran with Verbose off and the debug-level line never had
                    // a chance. Eight lines a session is the whole budget.
                    static std::string sShoutProbeSig;
                    static int         sShoutProbeLines = 0;
                    if (shoutActive) {
                        const std::string sig = resolvedKey + (perState ? "|cell" : "|global");
                        if (sig != sShoutProbeSig && sShoutProbeLines < 8) {
                            sShoutProbeSig = sig;
                            ++sShoutProbeLines;
                            spdlog::debug("[SHOUTNOISE] shouting with no shout cell: pointer path "
                                         "key='{}' -> {} ({}/8)",
                                         resolvedKey, perState ? "state cell" : "GLOBAL noise",
                                         sShoutProbeLines);
                        }
                    } else {
                        sShoutProbeSig.clear();   // one line per shout
                    }
                }
                if (perState) {
                    active = perState;
                    // Attack Duration — hand back to the parent state once the
                    // swing's noise window is up.
                    if (IsAttackNoiseKey(resolvedKey)) {
                        // ON-ACTION, not a state. Snapshot this cell as the
                        // beat's character, fire one beat per swing, and let
                        // the AMBIENT state carry on as the parent — so the
                        // attack never blankets anything, it punctuates it.
                        sAtkCell      = *active;
                        sAtkCellValid = true;
                        const bool keyChanged = sAtkLastKey != resolvedKey;
                        const bool freshSwing = (atkElapsed >= 0.0f && !sAtkArmedForSwing) ||
                                                sAtkSwingPending;
                        const auto atkNow     = CameraEffectClock::Now();
                        const float sinceArm  = sAtkArmTpValid
                            ? std::chrono::duration<float>(atkNow - sAtkArmTp).count()
                            : 1.0e9f;
                        // See kAtkSettleSec: an early re-classification renames
                        // this swing, it does not start another one.
                        const bool correction = keyChanged && !freshSwing &&
                                                sinceArm < kAtkSettleSec;
                        if (freshSwing || (keyChanged && !correction)) {
                            sAtkBeat.rearmPending = true;   // resume-not-restart
                            sAtkArmedForSwing     = true;
                            sAtkSwingPending      = false;  // consumed
                            sAtkLastKey           = resolvedKey;
                            sAtkArmTp             = atkNow;
                            sAtkArmTpValid        = true;
                        } else if (correction) {
                            sAtkLastKey      = resolvedKey;  // adopt in place
                            sAtkKeyCorrected = true;         // no refade
                        }
                        // Hand the AMBIENT back to the state we were actually
                        // in. Copy rather than alias: `active` is a mutable
                        // pointer into live settings.
                        // The state we were ACTUALLY in is the one that was
                        // playing a frame ago — not the key-stripped parent.
                        // Those differ for every modified attack: the parent of
                        // sprint_power_attack is weapons.melee, the STANDING
                        // entry, so a sprint attack used to yank the ambient off
                        // the sprint texture on the way in and back on the way
                        // out (the 18:44 log: amp 3.0->5.0 tilt 3.0->0.5 at the
                        // swing, and the mirror of it afterwards). Riding the
                        // real previous ambient means a swing punctuates what
                        // was already playing and changes nothing underneath.
                        // Standing attacks are unaffected — for those the last
                        // non-attack state IS the parent.
                        // TRIED AND REVERTED: releasing this hold the moment the
                        // player stops sprinting (to move the sprint->standing
                        // change off the swing's end, where it collides with the
                        // next attack) was clearly worse. Changing the ambient
                        // DURING a swing also changes the swing — the beat floors
                        // its own tilt/sway against the ambient, so the ambient
                        // moving mid-beat morphs the action's character while it
                        // plays. A pending transition after the swing is the
                        // lesser evil. Hold it for the whole attack.
                        if (sLastNonAtkValid) {
                            fallbackGlobal = sLastNonAtkNoise;
                        } else if (const auto* parent =
                                       ResolveNoiseByKey(AttackNoiseParentKey(resolvedKey))) {
                            fallbackGlobal = *parent;   // no prior state yet
                        }
                        active = &fallbackGlobal;
                    } else {
                        sLastNonAtkNoise = *active;
                        sLastNonAtkKey   = resolvedKey;
                        sLastNonAtkValid = true;
                    }
                } else if (StateResolver::GetSingleton().GetSubState() !=
                           CameraSubState::Attack) {
                    // No per-state entry: this state rides the global noise, so
                    // there is no "previous ambient" to hand an attack back to.
                    // Forget the stale one rather than resurrecting a state the
                    // player has since left. Skipped while attacking so an
                    // untuned swing inside a combo can't wipe the ambient the
                    // rest of the combo still hands back to.
                    sLastNonAtkValid = false;
                }
            }
        }
        // Shout noise envelope (3p) — ported from the 1p path so the 3 words
        // of a shout escalate instead of blanketing at one flat strength.
        // Progressive build through Skyrim's 3 word-charge tiers (~0.5s each):
        // word 1 ~0.7x, word 2 ~1.0x, word 3 ~1.5x of the profile amp,
        // smoothstep-eased across boundaries, so the hold time IS the intensity
        // dial. On the voice-fire edge we capture the peak reached (a 1-word
        // release holds low, a 3-word high) and hold through the voice line;
        // when the shout state ends the peak is handed to the ambient
        // crossfade, which brings the parent state's noise up underneath it as
        // it comes down (see the shout-end edge). Guards on
        // active != &fallbackGlobal so only a real (enabled) shout entry drives
        // the envelope. Non-shout frames leave shoutEnvelope at 1.0.
        static std::chrono::steady_clock::time_point sShoutFireTp{};
        static float                                 sShoutFadeDur     = 0.0f;
        static float                                 sShoutEnvAtFire   = 0.0f;
        static bool                                  sWasShoutingNoise = false;
        // Release punch: transient added on top of the sustained level a
        // shout's word count earns.
        static std::chrono::steady_clock::time_point sShoutPunchTp{};
        static float                                 sShoutPunchAmp    = 0.0f;
        static int                                   sShoutWordApplied = 0;
        // Sustain retarget blend (see kSustainRate below). The duration is
        // per-retarget, not a constant, because how far the guess was wrong is
        // exactly what should decide how long the correction takes.
        static std::chrono::steady_clock::time_point sShoutSustainTp{};
        static float                                 sShoutSustainFrom = 0.0f;
        static float                                 sShoutSustainDur  = 0.15f;
        float shoutEnvelope = 1.0f;   // amp/speed multiplier; 1.0 = no shout
        // Set on the frame a shout ends, and consumed by the ambient crossfade
        // further down. See the long note at the shout-end edge for why the
        // hand-off back to the parent state is the crossfade's job rather than
        // a fade of its own.
        bool  shoutHandoffArmed = false;
        float shoutHandoffDur   = 0.0f;
        // Werewolf roar hand-off — same idea as the shout's, see the roar
        // envelope below for why it needs its own.
        bool  roarHandoffArmed  = false;
        float roarHandoffDur    = 0.0f;
        {
            const auto& sr = StateResolver::GetSingleton();
            // Keyed off shoutActive, not shoutResolved: the escalation envelope
            // rides on whatever noise is playing, including the underlying
            // state's when no shout-specific entry exists.
            const bool  isShoutingNow = shoutActive && active != &fallbackGlobal;

            // Word-tier boundaries + amps. Word 1 rises FAST (kRiseTime) to a
            // visible floor and holds for the rest of the word, instead of the
            // 1p shape that ramps from 0 across the whole first word (which left
            // the quick first word barely felt). Each later word then steps up
            // smoothstep-eased so the escalation reads clearly. kAmp1 raised
            // from 0.5 so word 1 is plainly visible even at high intensity.
            constexpr float kRiseTime = 0.12f;   // fast initial rise to the word-1 floor
            constexpr float kWord1End = 0.50f, kWord2End = 1.00f, kWord3End = 1.50f;
            // kAmp1/2/3 are the SUSTAINED multipliers for a 1/2/3-word shout,
            // as a multiple of the profile amp the user tuned (1.50 -> 2.20
            // -> 2.40 over successive passes). The gap between them is now
            // wider than it was, because these are selected by the real word
            // count rather than by how far an elapsed-time ramp happened to
            // get — there is no longer any reason to keep the tiers close
            // together to hide a misclassification. kClimax is the same
            // ceiling for the no-fire path below; keep the two in step.
            constexpr float kAmp1 = 0.70f, kAmp2 = 1.20f, kAmp3 = 2.40f;
            constexpr float kClimax     = 2.40f;   // no-fire path peak (== kAmp3)
            constexpr float kClimaxRise = 0.35f;   // seconds from cast start to the peak
            // Release PUNCH. The sustained level alone made a full 3-word
            // shout read as "louder", never as a hit — the escalation is
            // gradual by construction, so nothing about it lands. This is a
            // short transient stacked on top of the sustain at the release
            // moment, scaled hard by word count so three words are
            // categorically different from one. The attack is smoothstepped
            // rather than instant on purpose: amp scales the shake ANGLE, so
            // a one-frame amp step is a one-frame angle step — the exact
            // failure mode behind the old 1p attack snap and the weapon-draw
            // double-jolt.
            constexpr float kPunch1 = 0.18f, kPunch2 = 0.50f, kPunch3 = 1.30f;
            constexpr float kPunchAttack = 0.08f;   // smoothstep in
            constexpr float kPunchDecay  = 0.55f;   // smoothstep back to sustain
            auto smooth = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
            auto wordAmp   = [&](int wc) { return wc >= 3 ? kAmp3   : (wc == 2 ? kAmp2   : kAmp1);   };
            auto wordPunch = [&](int wc) { return wc >= 3 ? kPunch3 : (wc == 2 ? kPunch2 : kPunch1); };

            if (isShoutingNow) {
                // Fade Duration is now the hand-off duration (see the
                // shout-end edge below). Read from whatever source is
                // actually playing, so it works whether the shout has its own
                // entry or is only decorating the state's noise.
                sShoutFadeDur  = active->shoutFadeDuration;

                const auto  now2  = CameraEffectClock::Now();
                const auto  start = sr.GetShoutStartTime();
                const auto  fire  = sr.GetShoutFireTime();
                const float wind  = std::chrono::duration<float>(now2 - start).count();

                float env;
                if      (wind < kRiseTime) env = kAmp1 * smooth(wind / kRiseTime);   // quick rise to word-1 floor
                else if (wind < kWord1End) env = kAmp1;                              // hold word 1 (visible)
                else if (wind < kWord2End) env = kAmp1 + smooth((wind - kWord1End) / (kWord2End - kWord1End)) * (kAmp2 - kAmp1);
                else if (wind < kWord3End) env = kAmp2 + smooth((wind - kWord2End) / (kWord3End - kWord2End)) * (kAmp3 - kAmp2);
                else                       env = kAmp3;

                // Hold a stable strength through the voice line. Two cases:
                //   * Voice_SpellFire arrives (vanilla graph): capture the env
                //     reached at that edge (floored at word 1) and hold it, so a
                //     held 3-word shout locks in its higher peak.
                //   * NO Voice_SpellFire: the player's MELEE / behaviour-framework
                //     graph emits none (log shows BeginCastVoice + SBF_ShoutStart/
                //     Stop but no Voice_SpellFire), so `fired` never trips and the
                //     wind-based curve — which can't tell a tap from a held shout —
                //     ramped 0 -> 1.5 across the WHOLE cast. That is exactly the
                //     "melee shout noise nearly stops, then picks up right before
                //     it ends" report. Hold steady at the word-1 floor instead of
                //     escalating on elapsed time alone.
                //   * Neither of those is a guess any more: StateResolver now
                //     reads the engine's own shout variation, so `words` is
                //     the REAL 1/2/3 and drives both the sustain and the
                //     punch. The elapsed-time curve above only covers the
                //     wind-up, before the engine has committed a variation.
                const int  words = sr.GetShoutWordCount();
                const bool fired = fire != std::chrono::steady_clock::time_point{} && fire >= start;
                // Retargeting the sustain is BLENDED, never assigned. The
                // wind-up curve and the true word count can disagree: hold a
                // shout you only know two words of and the timer will have
                // climbed to the 3-word tier by the time the engine grants
                // two. Snapping the sustain down at that moment would be a
                // one-frame amp step, i.e. a one-frame angle step — a visible
                // jolt at the exact instant the shout fires.
                // The blend runs at a fixed RATE, not over a fixed time.
                //
                // The wind-up curve guesses the word count from elapsed time;
                // the engine only commits the real variation at release, and on
                // this rig that lands ~950ms into the cast (log: "word count = 1
                // (via poll)" at +11ms, then "= 3 (via kVoiceFire)" at +967ms).
                // At +967ms the curve is mid-way up the word-2 ramp, around
                // 1.19, and the truth is kAmp3 = 2.40 — so a fixed 0.15s blend
                // DOUBLED the amplitude in about nine frames. amp scales the
                // shake angle, so doubling the amp in 0.15s is doubling the
                // angle in 0.15s: a lurch in the middle of the shout, right
                // where Unrelenting Force is supposed to be settling into its
                // sustain. A one-word shout, whose guess was nearly right, got
                // the same 0.15s for a correction of almost nothing.
                //
                // Capping the rate makes the size of the correction set its own
                // duration: a near-miss lands almost instantly, a 2x miss takes
                // ~0.6s and rides underneath the punch that fires on the same
                // frame, so the two read as one swell instead of a step plus a
                // hit. The floor keeps small corrections from being abrupt.
                constexpr float kSustainRate = 2.0f;    // amp units per second
                constexpr float kSustainMin  = 0.12f;   // seconds
                auto retargetDur = [&](float from, float to) {
                    return (std::max)(kSustainMin, std::abs(to - from) / kSustainRate);
                };
                auto liveSustain = [&]() {
                    const float bt = std::chrono::duration<float>(now2 - sShoutSustainTp).count();
                    return sShoutSustainFrom +
                           (sShoutEnvAtFire - sShoutSustainFrom) * smooth(bt / sShoutSustainDur);
                };
                if (fired) {
                    if (fire != sShoutFireTp) {
                        sShoutFireTp      = fire;
                        sShoutWordApplied = 0;
                        sShoutPunchAmp    = 0.0f;
                        sShoutSustainFrom = env;
                        sShoutSustainTp   = now2;
                        sShoutEnvAtFire   = (std::max)(env, kAmp1);
                        sShoutSustainDur  = retargetDur(sShoutSustainFrom, sShoutEnvAtFire);
                    }
                    // The variation can land on the fire frame or a frame
                    // later, so apply it whenever it shows up rather than
                    // only at the edge.
                    if (words > 0 && words != sShoutWordApplied) {
                        sShoutSustainFrom = liveSustain();   // blend from where we are
                        sShoutSustainTp   = now2;
                        sShoutWordApplied = words;
                        sShoutEnvAtFire   = wordAmp(words);
                        sShoutSustainDur  = retargetDur(sShoutSustainFrom, sShoutEnvAtFire);
                        sShoutPunchAmp    = wordPunch(words);
                        sShoutPunchTp     = now2;
                    }
                    env = liveSustain();
                } else if (words > 0) {
                    // Word count known before any release signal (setups
                    // where the engine commits the variation early). Follow
                    // the real count instead of the elapsed-time guess, but
                    // never dip below the curve already reached.
                    env             = (std::max)(env, wordAmp(words));
                    sShoutEnvAtFire = env;
                } else {
                    // No fire signal → no word count to escalate on, so shape
                    // the climax on elapsed time instead: rise from the FULL
                    // configured strength (1.0x) to kClimax and hold there for
                    // the rest of the cast. Starting AT 1.0 rather than below
                    // it is what fixed the old "melee shout noise nearly stops,
                    // then picks up right before it ends" report — the noise
                    // never dips under what the user tuned, it only builds past
                    // it. This branch is the LIVE one on graphs that emit
                    // BeginCastVoice + shoutStop but no Voice_SpellFire (the
                    // player's behaviour-framework graph does exactly that), so
                    // its flat 1.0 meant those setups got no climax at all —
                    // the word tiers above never ran.
                    env             = 1.0f + (kClimax - 1.0f) *
                                             smooth(wind / kClimaxRise);
                    sShoutEnvAtFire = env;   // keep the fade reference in sync
                }

                // Stack the release punch on the sustain. Rises over
                // kPunchAttack, falls back to the sustained level over
                // kPunchDecay, then disarms — the sustain itself is
                // untouched, so the tail behaviour below is unchanged.
                if (sShoutPunchAmp > 0.0f) {
                    const float pt = std::chrono::duration<float>(now2 - sShoutPunchTp).count();
                    if (pt < kPunchAttack + kPunchDecay) {
                        const float shape = (pt < kPunchAttack)
                            ? smooth(pt / kPunchAttack)
                            : 1.0f - smooth((pt - kPunchAttack) / kPunchDecay);
                        env += sShoutPunchAmp * shape;
                    } else {
                        sShoutPunchAmp = 0.0f;
                    }
                }
                shoutEnvelope = env;
            }

            // ==== Hand-off back to the parent state ====
            //
            // This used to be a fade of its own: hold the shout's profile and
            // ride its amplitude down to ZERO over Fade Duration, then let go.
            // The flaw is in that last word — at the moment it let go, the
            // shout was at zero and the parent state's noise had to arrive
            // from silence, through a 0.2s crossfade. So a shout with a fade
            // set produced a hole: the shout faded away properly, then the
            // ordinary noise popped back in a beat later. Unrelenting Force
            // shows it worst because it earns the biggest peak, so it has the
            // furthest to fall and leaves the widest hole.
            //
            // The crossfade already does exactly the right thing and does it
            // between TWO live textures: the outgoing one decays at its own
            // tempo while the incoming one rises at its own. So the shout's
            // texture is handed to it as the outgoing layer and Fade Duration
            // becomes the crossfade's duration. The shout never reaches zero
            // alone — the parent is already coming up underneath it — and
            // there is no second transition afterwards, because the hand-off
            // IS the transition.
            //
            // Fade Duration keeps its meaning (how long the shout takes to
            // disappear) and its word-count scaling: a full 3-word shout
            // lingers the whole set duration, a 1-word one clears quickly.
            if (sWasShoutingNoise && !isShoutingNow) {
                sShoutPunchAmp    = 0.0f;   // never carry a transient into the tail
                sShoutWordApplied = 0;
                if (!jumpResolved) {
                    shoutHandoffArmed = true;
                    // Scaled by how big a peak this shout actually reached, so
                    // the size of the drop sets its own duration: three words
                    // come down from 2.4x and take the whole time, one word
                    // barely climbed and clears quickly. Clamped because the
                    // peak can equal the ceiling and an unclamped ratio would
                    // stretch past the duration the user set.
                    const float peakRatio = std::clamp(sShoutEnvAtFire / kAmp3, 0.0f, 1.0f);
                    // Floor, not just the user's slider: a full-strength shout
                    // dropping 2.4x in the ordinary 0.2s reads as a cut whether
                    // or not anyone set a Fade Duration, and the default IS
                    // zero. The slider raises this, never lowers it.
                    constexpr float kMinHandoff = 0.45f;
                    shoutHandoffDur = (std::max)(sShoutFadeDur, kMinHandoff) * peakRatio;
                }
            }
            sWasShoutingNoise = isShoutingNow;
        }

        float targetAmp   = active->amp   * shoutEnvelope;
        float targetSpeed = active->speed * shoutEnvelope;

        // Werewolf roar ramp envelope. Track time since the roar
        // engaged and shape the target with a quadratic ease-in so
        // the camera shake stays subtle during the visible wind-up
        // and only reaches full intensity around the roar peak.
        // Without this curve the exponential lerp hits the target
        // amp inside ~150ms which reads as "the roar shake started
        // before the wolf actually opens its mouth".
        static std::chrono::steady_clock::time_point sRoarStarted{};
        static bool                                   sRoarActiveLast = false;
        // How loud the roar's envelope actually got, and how far through its
        // ramp it was. Latched every frame the roar renders so the hand-off
        // below can start from exactly what was last on screen.
        static float sRoarLastScale = 0.0f;
        // Absence debounce. isWerewolfRoar is derived from the RESOLVED PROFILE
        // POINTER, and a revert walks through frames where that pointer is
        // briefly something else (the race switch, the momentary non-third-
        // person camera state the transform passes through). A single such
        // frame used to look like "the roar ended and restarted": sRoarStarted
        // reset, elapsed went back to 0, and the quadratic ease-in multiplied
        // the amp by ZERO — a one-frame hole with no crossfade to hide it,
        // because the raw cell values that make up the crossfade signature had
        // not changed. That is the "odd cutout" mid-roar.
        static std::chrono::steady_clock::time_point sRoarOffTp{};
        constexpr float kRoarOffDebounce = 0.20f;   // seconds of absence before it counts
        const auto roarNow = CameraEffectClock::Now();
        if (isWerewolfRoar) {
            const bool genuinelyNew = !sRoarActiveLast &&
                (sRoarOffTp.time_since_epoch().count() == 0 ||
                 std::chrono::duration<float>(roarNow - sRoarOffTp).count() > kRoarOffDebounce);
            if (genuinelyNew) sRoarStarted = roarNow;
            constexpr float kRampSec      = 0.55f;  // ~timing of howl wind-up→peak
            constexpr float kEarlyWindow  = 1.00f;  // first second held at lower intensity
            constexpr float kEarlyScale   = 0.30f;  // amplitude scaler during the window
            const float elapsed = std::chrono::duration<float>(
                CameraEffectClock::Now() - sRoarStarted).count();
            const float prog    = std::clamp(elapsed / kRampSec, 0.0f, 1.0f);
            const float baseCurve  = prog * prog;  // quadratic ease-in
            // Decouple amp and speed during the breath-in window.
            // If we scale speed too, the Perlin oscillation rate slows to
            // the point where the shake reads as "static" — visible amp
            // doesn't feel like shake without motion. Keep speed on the
            // base quadratic ramp so oscillation reads as motion; only
            // dampen amplitude.
            float ampScale   = (elapsed < kEarlyWindow)
                               ? baseCurve * kEarlyScale
                               : baseCurve;
            const float spdScale   = baseCurve;
            targetAmp   *= ampScale;
            targetSpeed *= spdScale;
            sRoarLastScale = ampScale;
        } else if (sRoarActiveLast) {
            // The roar just stopped rendering — most often because the player
            // reverted mid-howl.
            sRoarOffTp = roarNow;
            // HAND THE ROAR TO THE CROSSFADE, exactly as the shout does.
            //
            // Relying on the signature change alone is not enough here. The
            // signature is built from the RAW cell values, so it only fires if
            // the state you revert INTO happens to have a different character —
            // and it says nothing about the roar's envelope, which is where all
            // the loudness lives. Arming explicitly means the outgoing layer is
            // the roar as last rendered (curAmbient still holds its enveloped
            // amp) and it rides down into whatever state you are now in, over a
            // window scaled by how loud it had actually got. A roar cut short at
            // full cry takes the longest to clear; one that had barely started
            // clears immediately.
            if (sRoarLastScale > 0.01f) {
                roarHandoffArmed = true;
                // Sized to be a hand-off, not a tail. The first pass at 0.25 -
                // 0.85s read as the roar noise outstaying the roar by about two
                // tenths; this is the same curve, ended sooner.
                constexpr float kRoarHandoffMax = 0.55f;
                roarHandoffDur = (std::max)(0.20f, kRoarHandoffMax * std::clamp(sRoarLastScale, 0.0f, 1.0f));
            }
            sRoarLastScale = 0.0f;
        }
        sRoarActiveLast = isWerewolfRoar;

        // Jumping-noise phase envelope — rides AFTER the raw fields feed the
        // signature (below), same as the shout/roar envelopes, so per-frame
        // burst/wind modulation can't masquerade as a source change.
        targetAmp   *= jumpAmpMul;
        targetSpeed *= jumpSpeedMul;
        // Paraglide altitude fade — same post-signature channel; see the
        // paraResolved block for the mapping.
        targetAmp   *= paraAltMul;
        // Swim speed fade rides the same channel, for the same reason.
        targetAmp   *= swimSpdMul;

        // --- Ambient state-noise crossfade (transition rework) ---
        bool ambientArmedThisFrame = false;
        // Detect a change in the resolved noise SOURCE — its intrinsic Speed /
        // character, ignoring the per-frame shout amplitude ride — and arm a
        // crossfade. The outgoing texture then fades out at ITS tempo while the
        // incoming one fades in at ITS tempo, so states with very different
        // Speed values transition without the single-clock "wallow". Two states
        // that share a character produce no signature change (nothing to hide),
        // so they simply track continuously as before.
        {
            // AMP is part of the signature as well as the character. It is the
            // source's RAW amp, not the enveloped one, so a shout's per-frame
            // ride still can't masquerade as a source change — but a switch
            // between two entries that happen to share a character and differ
            // only in how loud they are now crossfades instead of stepping.
            // That case was silently common: most states ship with the default
            // character and are distinguished purely by Intensity.
            const bool sigChanged = ambientHasSig &&
                (std::abs(active->speed       - ambientSigSpeed) > 1e-4f ||
                 std::abs(active->sway        - ambientSigSway ) > 1e-4f ||
                 std::abs(active->tilt        - ambientSigTilt ) > 1e-4f ||
                 std::abs(active->driftJitter - ambientSigDrift) > 1e-4f ||
                 std::abs(active->roughness   - ambientSigRough) > 1e-4f ||
                 std::abs(active->amp         - ambientSigAmp  ) > 1e-4f);
            // A signature change caused by the user DRAGGING a noise slider is
            // not a texture change to hide — it is the live texture being
            // retuned, and the honest response is to track it directly (cur
            // snaps to the new values below either way). Arming here instead
            // re-armed the fade on every tick of the drag, and the mid-flight
            // collapse gave the dominant layer a fresh decorrelated clock each
            // time: one Perlin phase jump per tick, felt as the noise briefly
            // racing before settling whenever Speed was adjusted live.
            const bool liveEdit = LiveNoiseEditActive();
            if (!ambientHasSig) {
                // Match the original startup fade from a silent layer, including
                // its weight in the ambient shape used by additive beat floors.
                ambientOutgoing.Capture(prevAmbient, 1.0f, ambientXfadeRate);
            }
            if ((sigChanged && !liveEdit) || shoutHandoffArmed || roarHandoffArmed) {
                ambientOutgoing.Capture(curAmbient, ambientXfade, ambientXfadeRate);
                // A representative profile is useful for the existing adaptive
                // duration and logs, but is never used to synthesize motion.
                prevAmbient = {};
                float totalWeight = 0.0f;
                prevAmbient.driftJitter = prevAmbient.roughness = 0.0f;
                ambientOutgoing.Visit([&](const AmbientLayer& layer, float weight) {
                    prevAmbient.amp += weight * weight * layer.amp * layer.amp;
                    prevAmbient.speed += weight * layer.speed;
                    prevAmbient.tilt += weight * layer.tilt;
                    prevAmbient.sway += weight * layer.sway;
                    prevAmbient.driftJitter += weight * layer.driftJitter;
                    prevAmbient.roughness += weight * layer.roughness;
                    totalWeight += weight;
                });
                prevAmbient.amp = std::sqrt(prevAmbient.amp);
                if (totalWeight > 0.0f) {
                    prevAmbient.speed /= totalWeight;
                    prevAmbient.tilt /= totalWeight;
                    prevAmbient.sway /= totalWeight;
                    prevAmbient.driftJitter /= totalWeight;
                    prevAmbient.roughness /= totalWeight;
                }
                ambientXfade = 0.0f;         // fade the incoming layer in from scratch
                ambientArmedThisFrame = true;
                // Push the incoming layer to an unrelated part of the noise
                // field so the two layers are independent — see
                // kXfadeDecorrelate. Free here: cur is at weight 0 this frame.
                curAmbient.clockTrans += kXfadeDecorrelate;
                curAmbient.clockRot   += kXfadeDecorrelate * 0.73;
                // The phase is linear now, so rate = 1/duration exactly: a
                // hand-off asked to take `d` seconds takes `d` seconds; the
                // ordinary state change takes kAmbientXfadeDur.
                ambientXfadeRate = (shoutHandoffArmed && shoutHandoffDur > 0.01f)
                    ? 1.0f / (std::max)(0.15f, shoutHandoffDur)
                    : (roarHandoffArmed && roarHandoffDur > 0.01f)
                        ? 1.0f / (std::max)(0.15f, roarHandoffDur)
                        : kAmbientXfadeRateDefault;
            }
            ambientSigSpeed = active->speed;
            ambientSigSway  = active->sway;
            ambientSigTilt  = active->tilt;
            ambientSigDrift = active->driftJitter;
            ambientSigRough = active->roughness;
            ambientSigAmp   = active->amp;
            ambientHasSig   = true;
        }

        // Incoming layer = the current state's live parameters, SNAPPED (the
        // crossfade weight does the blending — no per-parameter easing, which
        // is what used to drag Speed through the wallow). amp/speed carry the
        // shout/roar envelope and the master enable gate.
        curAmbient.sway        = active->sway;
        curAmbient.tilt        = active->tilt;
        curAmbient.driftJitter = active->driftJitter;
        curAmbient.roughness   = active->roughness;
        curAmbient.amp         = settings.noiseEnabled ? targetAmp : 0.0f;
        curAmbient.speed       = targetSpeed;

        // Now that BOTH textures are known, size the fade to the distance
        // between them. A shout hand-off keeps its own explicitly-asked-for
        // duration.
        if (ambientArmedThisFrame &&
            !(shoutHandoffArmed && shoutHandoffDur > 0.01f) &&
            !(roarHandoffArmed  && roarHandoffDur  > 0.01f)) {
            const float d = TextureDistance(
                prevAmbient.amp, prevAmbient.speed, prevAmbient.tilt,
                prevAmbient.sway, prevAmbient.driftJitter, prevAmbient.roughness,
                curAmbient.amp,  curAmbient.speed,  curAmbient.tilt,
                curAmbient.sway, curAmbient.driftJitter, curAmbient.roughness);
            float dur = AdaptiveXfadeDur(d);
            // A CAST RELEASE IS AN ACTION, NOT A STATE MORPH. The adaptive
            // duration exists for wandering between states, where a big
            // difference earns a long morph; an action's hand-off must track
            // the action. For the user's typical cells the adaptive length
            // came out at ~0.7s, which read as the cast noise outstaying the
            // spell — the same lesson the attack beat already taught ("a
            // state morph can take a second; a swing must be heard now").
            // Fixed and semi-quick: long enough that the cast texture
            // audibly rides out instead of cutting, short enough that the
            // hand-off belongs to the release that caused it.
            //
            // 0.45 -> 0.60: reported as "fades almost instantly" at 0.45, and
            // the report is honest even though 0.45s of fade ran — in this
            // preset the PARENT cell is louder than the cast cell (energy 16
            // vs 10), so the incoming texture masks the outgoing jitter well
            // before its weight reaches zero. The equal-loudness 1p pairing
            // makes the same-length fade read as a settle, which is why 1p
            // was approved at ~0.5s while 3p read as a cut.
            // 0.60 -> 0.75: user-tuned, +0.15 by request after testing 0.60.
            constexpr float kFofReleaseFadeSec = 0.75f;
            if (fofRelease) dur = kFofReleaseFadeSec;
            // PARAGLIDE hand-off: deploying and landing take the user's own
            // Fade Duration instead of the adaptive length. A glide cell is
            // usually far from the ground state, so the adaptive curve
            // saturated at its 1.20s ceiling and still read as abrupt
            // ("not smooth even with max fade duration") — the ceiling was
            // the limit, not the mapping. This slider IS the ceiling now.
            if (paraXfadeEdge && settings.paraglideLandFade > 0.01f) {
                dur = std::clamp(settings.paraglideLandFade, 0.15f, 6.0f);
                // DEPLOY is the fast half: the outgoing texture is the fall
                // wind (or the landing rush), and it has to clear out as the
                // canopy catches — riding the full landing-length fade left
                // it audible well into the glide. Scaled off the same slider
                // so the user's preference still shapes it, capped short.
                if (paraDeployEdge) dur = std::clamp(dur * 0.35f, 0.12f, 0.50f);
                spdlog::debug("[PARAX] paraglide noise hand-off: dur={:.2f}s dist={:.2f} "
                             "amp {:.1f}->{:.1f} spd {:.2f}->{:.2f} tilt {:.1f}->{:.1f}",
                             dur, d, prevAmbient.amp, curAmbient.amp,
                             prevAmbient.speed, curAmbient.speed,
                             prevAmbient.tilt, curAmbient.tilt);
            }
            ambientXfadeRate = 1.0f / dur;
            static int sXfLogs = 0;
            if (sXfLogs < 25) {
                ++sXfLogs;
                spdlog::debug("[XFADE] 3p dist={:.2f} dur={:.2f}s{}  amp {:.1f}->{:.1f} "
                             "spd {:.2f}->{:.2f} tilt {:.1f}->{:.1f} dj {:.2f}->{:.2f}",
                             d, dur, fofRelease ? " (release)" : "",
                             prevAmbient.amp, curAmbient.amp,
                             prevAmbient.speed, curAmbient.speed,
                             prevAmbient.tilt, curAmbient.tilt,
                             prevAmbient.driftJitter, curAmbient.driftJitter);
            }
        }

        // Advance the linear crossfade phase. At 1 the steady state is a clean
        // single-layer apply identical to the pre-rework behavior; the weight
        // shaping (smoothstep + equal-power) lives at the apply sites.
        ambientOutgoing.Advance(dt);
        ambientXfade += dt * ambientXfadeRate;
        if (ambientXfade >= 1.0f) { ambientXfade = 1.0f; ambientOutgoing.Clear(); }
        // (No rate morph here either — see the note on Pv.speed in the attack
        // blend. The outgoing layer keeps the Speed it was snapshotted with.)
        // Jump takeoff: much faster fade than a state change (0.10s vs 0.35s)
        // so the launch burst lands on time — but no longer a hard snap to 1.
        // The snap was the last deliberate zero-length texture swap left in
        // 3p, and with equal-power weights a 0.10s fade reaches half power in
        // ~50ms, which is inside the burst's own attack. Landing keeps the
        // normal crossfade, which is what flows the air noise back into the
        // ground state's.
        if (snapJumpXfade && ambientXfade < 1.0f) {
            ambientXfadeRate = std::max(ambientXfadeRate, 1.0f / 0.10f);
        }

        // Cinematic Effects → Dragons. Five per-source triggers (breath,
        // fireball, bite, landing, takeoff) layered on top of base camera
        // noise. Per-source enable + amp + speed + range gates fire
        // inside FindNearestDragonShakeSource and the result carries the
        // winning source's speed and range for the falloff calc.
        // effAmp is the crossfaded composite amplitude, so the dragon/transform
        // dominance check below compares against what's actually on screen.
        // Perceived composite amplitude under equal-power mixing is the RMS of
        // the weighted layers (exact at either end, no phantom mid-fade dip).
        float effAmp;
        // ...and the ambient SHAPE as actually rendered. curAmbient.tilt/sway
        // are SNAPPED to the incoming state (see the "no per-parameter easing"
        // note above), which is right for the crossfaded pair — cur sits at
        // weight 0 on the frame it snaps, so the fade absorbs the change. It is
        // WRONG for every layer that reads them while applying at FULL weight:
        // the beats floor their tilt/sway against the ambient, so a state
        // change stepped the beat's shape in one frame with nothing blending
        // it. That is the residual snap after a sprint attack — the 18:51 log
        // has the ambient going tilt 3.0 -> 0.5 on the frame the attack ended
        // while the attack beat was still live at 4.37 energy, and [MOTION]
        // caught it as dTrans=0.487 with rotation clean (dTheta=0.0014).
        float ambTiltEff, ambSwayEff;
        {
            float wA, wB;
            XfadeWeights(ambientXfade, wA, wB);
            float ampSq = wA * wA * curAmbient.amp * curAmbient.amp;
            float totalWeight = wA;
            ambTiltEff = curAmbient.tilt * wA;
            ambSwayEff = curAmbient.sway * wA;
            ambientOutgoing.Visit([&](const AmbientLayer& layer, float weight) {
                const float w = weight * wB;
                ampSq += w * w * layer.amp * layer.amp;
                ambTiltEff += layer.tilt * w;
                ambSwayEff += layer.sway * w;
                totalWeight += w;
            });
            effAmp = std::sqrt(ampSq);
            const float denominator = std::max(totalWeight, 1.0e-3f);
            ambTiltEff /= denominator;
            ambSwayEff /= denominator;
        }

        float transMul = ambSwayEff;   // cinematic path max()-floors these
        float rotMul   = ambTiltEff;
        float freqMul  = 1.0f;              // wobble retired (ignored by ApplyPerlin)
        float effSpeed = settings.noiseEnabled ? curAmbient.speed : 0.0f;

        // Cinematic impact band-character. When a cinematic shake (dragon/
        // centurion/transformation) dominates this frame, the noise is drawn
        // with an IMPACT character — its own drift/jitter mix per event kind —
        // instead of inheriting the ambient profile's idle sway. Mirrors what
        // the 1p path already does, and decouples impact feel from the user's
        // ambient-noise slider. cinematicShake stays false when no cinematic
        // dominates → ambient crossfade path unchanged.
        bool  cinematicShake = false;
        float cinematicDj     = 0.6f;
        float cinematicRoughness = -1.0f;  // >=0 overrides curRoughness for the cinematic layer
        float dragonAmpNow    = 0.0f;      // live (decaying) cinematic amp, for the path fade-out
        {
            // Cinematic dragon/centurion shake envelope (scan + asymmetric
            // attack/decay), shared with the first-person path.
            float dragonSpeed = 0.0f;
            const float dAmp = UpdateDragonShake(player, dt, dragonSpeed, /*a_fp=*/false);
            dragonAmpNow = std::max(0.0f, dAmp);
            dragonBeatValid = false;
            if (dAmp > 0.0001f) {
                // Additive layer on its own clock — NOT a takeover. The
                // impact character is the source's own (Cinematic Effects
                // sliders); it no longer floors the ambient's transMul /
                // rotMul / speed, so a dragon overhead cannot rewrite the
                // texture your state or shout is playing.
                const double kTrans = dragonBeat.clockTrans;
                const double kRot   = dragonBeat.clockRot;
                dragonBeat            = curAmbient;
                dragonBeat.clockTrans = kTrans;
                dragonBeat.clockRot   = kRot;
                dragonBeat.amp        = dAmp;
                dragonBeat.speed      = std::max(0.1f, dragonSpeed);
                dragonBeat.tilt       = std::max(ambTiltEff, sDragonShakeCharOut[0].rotShake);
                dragonBeat.sway       = std::max(ambSwayEff, sDragonShakeCharOut[0].posShake);
                dragonBeat.driftJitter = sDragonShakeCharOut[0].driftJitter;
                if (sDragonShakeCharOut[0].roughness > 0.0f)
                    dragonBeat.roughness = sDragonShakeCharOut[0].roughness;
                dragonBeatValid = true;
                effAmp += dAmp;
                cinematicShake = false;
                // Character is now per-source (Cinematic Effects sliders):
                // Drift/Jitter, Position Shake (transMul), Rotation Shake
                // (rotMul), Roughness. freqMul (oscillation rate texture) stays
                // kind-derived — it isn't user-exposed.
                cinematicDj        = sDragonShakeCharOut[0].driftJitter;
                cinematicRoughness = sDragonShakeCharOut[0].roughness;
                transMul = std::max(transMul, sDragonShakeCharOut[0].posShake);
                rotMul   = std::max(rotMul,   sDragonShakeCharOut[0].rotShake);
                freqMul  = std::max(freqMul,  1.0f);
                if (static_cast<DragonShakeKind>(dragonShakeKind[0]) == DragonShakeKind::Heavy) {
                    freqMul = std::min(freqMul, 0.85f);  // slower, thuddier
                } else if (static_cast<DragonShakeKind>(dragonShakeKind[0]) == DragonShakeKind::Sharp) {
                    freqMul = std::max(freqMul, 1.6f);   // snappier
                }
            }
            if (dragonSpeed > effSpeed) effSpeed = dragonSpeed;
        }

        // Transformation shake — a low BUILD-UP ramp from the cast (archetype
        // apply / graph start tag) that RESUMES into the race-edge climax:
        // smooth rise → hold → smooth fade. The revert (OUT) edge carries its
        // own tuning entry.
        constexpr float kTransPeak = 1.2f;
        const auto nowTp = CameraEffectClock::Now();
        auto smoothstep = [](float t) {
            t = std::clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };

        // Drain any latched graph signals so they don't accumulate.
        sWWGraphSignalLatch = false;
        sVLGraphSignalLatch = false;


        // ==== Werewolf / Vampire Lord transformation shake ====
        // NOT routed through applyEnvelope, for the same reason the weapon-draw
        // beat below isn't: that path sets cinematicShake, which swaps the
        // entire frame from the two-layer ambient crossfade to the single-layer
        // ApplyPerlin. Different clock, different drift/jitter, different
        // roughness — so the shake announces itself with a texture jump when
        // it engages and a second one when it releases, and no amount of
        // shaping the amplitude in between can hide either. That reads as two
        // jolts bracketing the morph instead of one swell riding through it.
        // Rebuilt here as an additive layer on the live texture: only the SIZE
        // changes, and the envelope takes that from 0 and back to 0.
        //
        // Both edges fire. Reverting out of Vampire Lord or Werewolf is exactly
        // as violent on screen as going in, and it used to get nothing at all —
        // a hard state-noise swap with no beat covering it. Same Intensity, so
        // zeroing the source still silences both.
        constexpr float kTransAttack = 0.10f;   // rise to full, smoothstepped
        auto transEnv = [&](bool& sActive,
                            const std::chrono::steady_clock::time_point& sStart,
                            float fadeDur) -> float {
            if (!sActive) return 0.0f;
            const float fd = std::max(0.05f, fadeDur);
            const float elapsed = std::chrono::duration<float>(nowTp - sStart).count();
            if (elapsed >= kTransPeak + fd) { sActive = false; return 0.0f; }
            if (elapsed < kTransAttack)     return smoothstep(elapsed / kTransAttack);
            if (elapsed < kTransPeak)       return 1.0f;
            return 1.0f - smoothstep((elapsed - kTransPeak) / fd);
        };

        float transAmp = 0.0f, transSpeed = 1.0f;
        const SettingsManager::CinematicShakeChar* transChar = nullptr;

        // Transformation BUILD-UP: a low ramp from the moment the change is
        // CAST — the kWerewolf / kVampireLord archetype apply (engine signal,
        // latched by MagicBeatSink) or the graph start tag, whichever came
        // last — until the race edge fires the climax. Capped well under the
        // climax so the edge still lands as the peak; the climax RESUMES from
        // the ramp's amplitude (back-dated attack) so there is no step; if
        // the edge never comes the ramp quietly decays.
        constexpr float kBuildupCap     = 0.35f;  // fraction of climax amplitude
        constexpr float kBuildupRise    = 4.0f;   // seconds to reach the cap
        constexpr float kBuildupTimeout = 15.0f;
        auto buildupEnv = [&](std::chrono::steady_clock::time_point a_arch,
                              std::chrono::steady_clock::time_point a_graph) -> float {
            auto latest = a_arch > a_graph ? a_arch : a_graph;
            if (latest.time_since_epoch().count() == 0) return 0.0f;
            const float t = std::chrono::duration<float>(nowTp - latest).count();
            if (t < 0.0f || t > kBuildupTimeout) return 0.0f;
            float e = smoothstep(t / kBuildupRise) * kBuildupCap;
            if (t > 8.0f)
                e *= std::clamp(1.0f - (t - 8.0f) / (kBuildupTimeout - 8.0f), 0.0f, 1.0f);
            return e;
        };

        if (werewolfTransformActive) {
            static std::chrono::steady_clock::time_point sStart{};
            static bool sLastRace = false;
            static bool sWWIsRevert = false;
            const bool  isRace  = StateResolver::GetSingleton().IsWerewolf();
            const float wwBuild = buildupEnv(sWWArchetypeTp, sWWGraphStartTime);
            if (isRace != sLastRace && !sWWEnvelopeActive) {
                sWWEnvelopeActive = true;
                sWWIsRevert       = !isRace;
                sStart            = nowTp;
                // Climax resumes from the build-up's current amplitude.
                if (!sWWIsRevert && wwBuild > 0.0f) {
                    sStart = nowTp - std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(std::chrono::duration<float>(
                            InvSmoothstep(std::min(wwBuild, 1.0f)) * kTransAttack));
                }
                sWWArchetypeTp   = {};
                sWWGraphStartTime = {};
                spdlog::info("[TransformShake] werewolf {} fired", isRace ? "climax" : "revert");
            }
            sLastRace = isRace;
            // Location layer: the active place's beat copy replaces the
            // global tuning wholesale (kFxBeatLocKeys 0 = transform,
            // 1 = revert). The chr pointer targets stable storage either way.
            auto* wLoc = settings.ActiveLocationFxBeat(
                SettingsManager::kFxBeatLocKeys[sWWIsRevert ? 1 : 0]);
            auto* wTLoc = settings.ActiveLocationFxBeat(SettingsManager::kFxBeatLocKeys[0]);
            const auto& wc  = wLoc ? wLoc->chr
                            : sWWIsRevert ? settings.werewolfRevertChar
                                          : settings.werewolfTransformChar;
            const float wIn = wLoc ? wLoc->intensity
                            : sWWIsRevert ? settings.werewolfRevertIntensity
                                          : settings.werewolfTransformIntensity;
            const float wSp = wLoc ? wLoc->speed
                            : sWWIsRevert ? settings.werewolfRevertSpeed
                                          : settings.werewolfTransformSpeed;
            const float env = transEnv(sWWEnvelopeActive, sStart, wc.fadeDuration);
            if (env > 0.0f && wIn > 0.0001f) {
                transAmp   = env * wIn;
                transSpeed = wSp;
                transChar  = &wc;
            } else if (!sWWEnvelopeActive && !isRace && wwBuild > 0.0f &&
                       (wTLoc ? wTLoc->intensity : settings.werewolfTransformIntensity) > 0.0001f) {
                transAmp   = wwBuild * (wTLoc ? wTLoc->intensity : settings.werewolfTransformIntensity);
                transSpeed = wTLoc ? wTLoc->speed : settings.werewolfTransformSpeed;
                transChar  = wTLoc ? &wTLoc->chr : &settings.werewolfTransformChar;
            }
        }
        if (vampireLordTransformActive) {
            static std::chrono::steady_clock::time_point sStart{};
            static bool sLastRace = false;
            static bool sVLIsRevert = false;
            const bool  isRace  = StateResolver::GetSingleton().IsVampireLord();
            const float vlBuild = buildupEnv(sVLArchetypeTp, sVLGraphStartTime);
            if (isRace != sLastRace && !sVLEnvelopeActive) {
                sVLEnvelopeActive = true;
                sVLIsRevert       = !isRace;
                sStart            = nowTp;
                if (!sVLIsRevert && vlBuild > 0.0f) {
                    sStart = nowTp - std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(std::chrono::duration<float>(
                            InvSmoothstep(std::min(vlBuild, 1.0f)) * kTransAttack));
                }
                sVLArchetypeTp    = {};
                sVLGraphStartTime = {};
                spdlog::info("[TransformShake] vampire lord {} fired", isRace ? "climax" : "revert");
            }
            sLastRace = isRace;
            // Location layer, VL flavor (kFxBeatLocKeys 2 = transform,
            // 3 = revert) — same wholesale replacement as the werewolf block.
            auto* vLoc = settings.ActiveLocationFxBeat(
                SettingsManager::kFxBeatLocKeys[sVLIsRevert ? 3 : 2]);
            auto* vTLoc = settings.ActiveLocationFxBeat(SettingsManager::kFxBeatLocKeys[2]);
            const auto& vc  = vLoc ? vLoc->chr
                            : sVLIsRevert ? settings.vampireLordRevertChar
                                          : settings.vampireLordTransformChar;
            const float vIn = vLoc ? vLoc->intensity
                            : sVLIsRevert ? settings.vampireLordRevertIntensity
                                          : settings.vampireLordTransformIntensity;
            const float vSp = vLoc ? vLoc->speed
                            : sVLIsRevert ? settings.vampireLordRevertSpeed
                                          : settings.vampireLordTransformSpeed;
            const float env = transEnv(sVLEnvelopeActive, sStart, vc.fadeDuration);
            // A werewolf envelope and a Vampire Lord envelope can't be live at
            // once in practice, but if they ever overlap the louder one owns
            // the layer rather than the two fighting over its character.
            float amp = 0.0f, spd = 1.0f;
            const SettingsManager::CinematicShakeChar* chr = nullptr;
            if (env > 0.0f && vIn > 0.0001f) {
                amp = env * vIn;
                spd = vSp;
                chr = &vc;
            } else if (!sVLEnvelopeActive && !isRace && vlBuild > 0.0f &&
                       (vTLoc ? vTLoc->intensity : settings.vampireLordTransformIntensity) > 0.0001f) {
                amp = vlBuild * (vTLoc ? vTLoc->intensity : settings.vampireLordTransformIntensity);
                spd = vTLoc ? vTLoc->speed : settings.vampireLordTransformSpeed;
                chr = vTLoc ? &vTLoc->chr : &settings.vampireLordTransformChar;
            }
            if (amp > transAmp) {
                transAmp   = amp;
                transSpeed = spd;
                transChar  = chr;
            }
        }

        // ==== Sheathing / Unsheathing beat (Cinematic Effects) ====
        // Deliberately NOT routed through applyEnvelope. That path sets
        // cinematicShake, which swaps the ENTIRE frame from the two-layer
        // ambient crossfade to the single-layer ApplyPerlin — a different
        // clock, a different drift/jitter mix and a different roughness. Over
        // a 2.7s transformation the swap is masked by the event itself; over a
        // short draw tick the swap IS what you feel: two texture jolts a
        // fraction of a second apart, at the start and the end. That is why
        // cutting the amplitude ~7x didn't help — amplitude was never the thing
        // being felt. The beat is a third ADDITIVE layer instead, so the
        // frame's texture never changes; only its size does, and only for as
        // long as the envelope lasts.
        //
        // The layer runs its OWN clock (advanced every frame in
        // ApplyAmbientCrossfade, at the source's own Speed) rather than
        // borrowing curAmbient's. It used to borrow, which was safe but had
        // one bad consequence: a state whose noise Speed is 0 — the default,
        // and what most states actually ship as — has a clock that never
        // advances, so the beat swelled and faded over a FROZEN Perlin sample.
        // That is a slow lean, not a shake, and it is the other half of "barely
        // felt". An independent clock is still step-free because the layer is
        // additive and its amplitude envelope starts and ends at exactly 0.
        //
        // Units: amp is in the same scale as a state's own Intensity slider
        // (0-5), so 1.00 adds one notch of the live state's noise at its peak.
        drawBeatValid = false;
        {
            const float beatAmp = drawBeatEnv * settings.weaponDrawNoiseIntensity;
            if (beatAmp > 0.0001f) {
                const auto& dc = settings.weaponDrawNoiseChar;
                // Keep the layer's persistent clocks; everything else is
                // rebuilt from the live texture + this source's own sliders.
                // Because the beat is sampled separately and its amp is
                // enveloped, its character can differ from the ambient's
                // without ever producing a step.
                const double keepTrans = drawBeat.clockTrans;
                const double keepRot   = drawBeat.clockRot;
                drawBeat             = curAmbient;
                drawBeat.clockTrans  = keepTrans;
                drawBeat.clockRot    = keepRot;
                drawBeat.amp         = beatAmp;
                drawBeat.speed       = std::max(0.1f, settings.weaponDrawNoiseSpeed);
                drawBeat.tilt        = std::max(ambTiltEff, dc.rotShake);
                drawBeat.sway        = std::max(ambSwayEff, dc.posShake);
                if (dc.driftJitter > 0.0f) drawBeat.driftJitter = dc.driftJitter;
                if (dc.roughness   > 0.0f) drawBeat.roughness   = dc.roughness;
                drawBeatValid = true;
                // Keeps the "nothing is contributing, skip the frame"
                // early-out honest, and carries the beat when a dragon /
                // transform shake has taken the single-layer path.
                effAmp += beatAmp;
            }
        }

        // ==== Transformation beat layer (same construction as the draw beat) ====
        transBeatValid = false;
        if (transChar && transAmp > 0.0001f) {
            const double keepTrans = transBeat.clockTrans;
            const double keepRot   = transBeat.clockRot;
            transBeat            = curAmbient;
            transBeat.clockTrans = keepTrans;
            transBeat.clockRot   = keepRot;
            // x7 to match what applyEnvelope used to feed effAmp, so an
            // Intensity tuned against the old path lands in the same place on
            // the new one.
            transBeat.amp        = transAmp * 7.0f;
            transBeat.speed      = std::max(0.1f, transSpeed);
            // SHAPE IS THE BEAT'S OWN when it HAS one — see the note on the
            // event layer below. Falls back to the ambient's, which is what
            // every all-zero default character depends on to render at all.
            transBeat.tilt       = transChar->rotShake > 0.0001f ? transChar->rotShake : ambTiltEff;
            transBeat.sway       = transChar->posShake > 0.0001f ? transChar->posShake : ambSwayEff;
            if (transChar->driftJitter > 0.0f) transBeat.driftJitter = transChar->driftJitter;
            if (transChar->roughness   > 0.0f) transBeat.roughness   = transChar->roughness;
            transBeatValid = true;
            effAmp += transBeat.amp;
        }

        // ==== Bats / Reanimation / Summoning beat layer ====
        // Same construction as the two layers above, and the same x7 so an
        // Intensity here means the same thing it means on every other
        // cinematic source. The proximity falloff for reanimation and
        // summoning is already folded into eventBeatNow.amp.
        eventBeatValid = false;
        if (eventBeatNow.chr && eventBeatNow.amp > 0.0001f) {
            const double keepTrans = eventBeat.clockTrans;
            const double keepRot   = eventBeat.clockRot;
            eventBeat            = curAmbient;
            eventBeat.clockTrans = keepTrans;
            eventBeat.clockRot   = keepRot;
            eventBeat.amp        = eventBeatNow.amp * 7.0f;
            eventBeat.speed      = std::max(0.1f, eventBeatNow.speed);
            // SHAPE: THE BEAT'S OWN WHEN IT HAS ONE, THE AMBIENT'S WHEN IT
            // DOESN'T. Never a max() of the two.
            //
            // These two used to read max(ambTiltEff, chr->rotShake) — the same
            // "never smaller than the texture you are swelling" rule the
            // attack beat uses. That rule is only dimensionally honest when
            // the beat's amp is in STATE units, which the attack beat's is
            // (it IS a state cell). This layer's amp is already intensity x7,
            // so flooring the shape on the ambient MULTIPLIES the two: a
            // Casting entry tuned to Rotation Shake 3.0 rendered a 1.00
            // Summoning beat at 7 x 3.0 instead of 7 x its own 1.10, and the
            // louder the entry the louder the beat — reported as "cast a
            // spell that has noise and it gets increased absurdly while a
            // summoning noise is happening".
            //
            // Dropping the max() outright was too far the other way. Half the
            // shipped characters here are ALL ZERO (kCharSustained, kCharSharp,
            // kCharHeavy, kCharWerewolf, kCharVampLord — see SettingsManager),
            // and a user who zeroed a character was relying on the same thing:
            // the ambient was the only shape those beats ever had, so using the
            // beat's own value unconditionally rendered exactly nothing, which
            // is the "summoning noise now cuts out immediately" regression.
            //
            // So: a beat that HAS a shape uses it, and means the same thing
            // whatever state you are standing in; a beat with no shape of its
            // own borrows the live texture, which is the documented "swells
            // what is already playing" behaviour. Either way the two are never
            // multiplied together. (drawBeat above keeps a plain floor on
            // purpose — its amp is in state Intensity units, so there the
            // max() is like-for-like.)
            eventBeat.tilt = eventBeatNow.chr->rotShake > 0.0001f
                           ? eventBeatNow.chr->rotShake : ambTiltEff;
            eventBeat.sway = eventBeatNow.chr->posShake > 0.0001f
                           ? eventBeatNow.chr->posShake : ambSwayEff;
            if (eventBeatNow.chr->driftJitter > 0.0f) eventBeat.driftJitter = eventBeatNow.chr->driftJitter;
            if (eventBeatNow.chr->roughness   > 0.0f) eventBeat.roughness   = eventBeatNow.chr->roughness;

            // Distance shapes CHARACTER, not just level. High frequencies lose
            // energy over distance, so a far event should arrive as a low
            // rumble and a near one as a sharp crack. Scaling amplitude alone
            // makes every source sound identical and merely quieter, which is
            // half of why a beat reads as a global effect rather than a thing
            // that happened somewhere.
            const float farness = 1.0f - std::clamp(eventBeatNow.nearness, 0.0f, 1.0f);
            eventBeat.speed       *= 1.0f - 0.35f * farness;
            eventBeat.driftJitter *= 1.0f - 0.60f * farness;   // slides toward drift
            eventBeat.roughness   *= 1.0f - 0.30f * farness;

            // And point it at the thing that caused it.
            eventBeat.srcBias = 0.0f;
            if (eventBeatNow.hasPos && eventBeatNow.dirBias > 0.0001f) {
                const auto& cp = a_camera->cameraRoot->world.translate;
                RE::NiPoint3 d{ eventBeatNow.pos.x - cp.x,
                                eventBeatNow.pos.y - cp.y,
                                eventBeatNow.pos.z - cp.z };
                const float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                // Below a stride or so the direction is noise — an event that
                // close has no meaningful bearing and biasing on a jittery one
                // would swim.
                if (dl > 60.0f) {
                    eventBeat.srcDir  = RE::NiPoint3{ d.x / dl, d.y / dl, d.z / dl };
                    eventBeat.srcBias = std::clamp(eventBeatNow.dirBias, 0.0f, 1.0f);
                }
            }
            eventBeatValid = true;
            effAmp += eventBeat.amp;
        }

        // ==== Attack beat (normal / power attack, on action) ====
        // Duration is the HOLD: 0 gives a pure impulse — a fast rise and a
        // decay, which is what a hit should feel like — and dialling it up
        // sustains the shake that long before it lets go. No x7: the cell's
        // Intensity is already in state units, because it IS a state cell.
        const bool atkWasPlaying = attackBlend.valid;
        attackBlend.valid = false;
        {
            // The crossfade here smooths a TEXTURE SWAP only — amplitude is the
            // envelope's job, and letting both ramp was the double-fade the
            // trace caught. But a FIXED 0.15s was an overcorrection: the trace
            // then showed normal -> power climbing 10.8 -> 28 energy inside
            // ~0.19s, stepping 3.6 energy in a single frame. Distance-scaled
            // like everything else, but in a SHORT band — an action has to stay
            // responsive, so this never approaches the state range (0.35-1.20s).
            constexpr float kAtkBlendMin = 0.12f;
            constexpr float kAtkBlendMax = 0.55f;
            // A FRESH beat (nothing to fade from) performs no swap, so the
            // swap-triggered trace never saw one — which is exactly the case
            // being reported: sprint power attack, then a separate power attack
            // whose beat starts from silence. Trace those too.
            const bool atkFreshArm = sAtkBeat.rearmPending && !attackBlend.valid;
            float atkRise = 0.06f, atkFall = 0.35f;
            BeatRampFor(sAtkCell.amp * (std::max)(ambTiltEff, sAtkCell.tilt) +
                        sAtkCell.amp * (std::max)(ambSwayEff, sAtkCell.sway),
                        effAmp * (ambTiltEff + ambSwayEff),
                        atkRise, atkFall);
            // LATCH the envelope's timing at ARM and hold it for the beat's
            // life. BeatRampFor sizes the ramps against the AMBIENT, and the
            // ambient moves — so these were being recomputed every single frame
            // underneath a running envelope. The 19:42 trace shows one power
            // attack's constants sliding rise 0.24 -> 0.50 and fall 0.51 -> 0.80
            // while it played, because the sprint->standing crossfade was
            // dragging ambE from 17.0 down to 5.0 beneath it.
            //
            // EventBeatEnvelope reads both: kAttack gates the rise and
            // (kAttack + hold + decay) gates the finish, with
            // env = 1 - smooth((t - kAttack - hold)/decay). Growing decay
            // mid-decay redefines the curve the beat is already on, and growing
            // kAttack past the elapsed time drops a beat that had reached full
            // back INTO its attack ramp. Either one is a step, and both land
            // exactly where this was reported: a new attack starting while the
            // previous swing's ambient transition is still running.
            //
            // The beat's shape belongs to the action, like its cell — decided
            // when the swing starts, not renegotiated with the world mid-swing.
            //
            // TRIED AND REVERTED: latching the beat's SHAPE FLOOR
            // (max(ambient, cell) on tilt/sway) at arm as well. It is the same
            // "belongs to the action" argument and it is wrong here, because
            // the floor is a MAXIMUM. A power attack armed while the ambient
            // was the sprint texture (tilt 3.0) then held tilt 3.0 for its
            // whole life INCLUDING its long decay, instead of relaxing to 0.5
            // with the ambient — so its tail stayed loud and ran over the next
            // swing. Reported as "power attack leaking into normal attacks".
            // A floor that tracks the ambient must keep tracking it; only
            // things the beat OWNS (cell, envelope timing) may be latched.
            if (sAtkBeat.rearmPending || !sAtkRampValid) {
                sAtkRiseLatched = atkRise;
                sAtkFallLatched = atkFall;
                sAtkRampValid   = true;
            }
            atkRise = sAtkRiseLatched;
            atkFall = sAtkFallLatched;
            const bool  atkLive = sAtkCellValid && sAtkCell.amp > 0.0001f;
            const float atkEnv  = atkLive
                ? EventBeatEnvelope(sAtkBeat, true,
                                    sAtkCell.attackDuration, atkFall, atkRise)
                : 0.0f;   // shared with the 1p path — see the note in `consider`
            if (atkEnv > 0.0001f) {
                // A change of attack cell mid-gesture (power -> normal in a
                // combo) snapshots the outgoing texture and refades, exactly
                // like a state change. Keyed on the cell's own identity.
                const int atkKey = static_cast<int>(
                    std::hash<std::string>{}(sAtkLastKey) & 0x7fffffff);
                const bool atkCorrected = sAtkKeyCorrected;
                sAtkKeyCorrected = false;   // one frame only
                bool atkSwapThisFrame = false;
                if (attackBlend.domKey != atkKey && atkCorrected) {
                    // Same swing, corrected name (see kAtkSettleSec). Adopt the
                    // cell on the LIVE layer and leave the fade alone: starting
                    // one here would cross-fade the swing against itself.
                    attackBlend.domKey = atkKey;
                } else if (attackBlend.domKey != atkKey) {
                    // Mid-flight re-arm, same defect the ambient crossfade had
                    // and the same cure: a combo changes cell faster than these
                    // (adaptive, so long) fades complete, and dropping the
                    // outgoing cell steps the composite. Collapse the pair of
                    // CELLS at their current weights — amp as RMS, shape as the
                    // weighted mean — and make that the new outgoing cell.
                    if (attackBlend.xfade < 1.0f && sAtkPrevCellValid) {
                        float wc0, wp0;
                        XfadeWeights(attackBlend.xfade, wc0, wp0);
                        const float tw0 = (std::max)(wc0 + wp0, 1.0e-3f);
                        auto mean0 = [&](float c, float p) { return (c * wc0 + p * wp0) / tw0; };
                        SettingsManager::NoiseProfile m = sAtkCurCell;
                        m.amp = std::sqrt(wc0 * wc0 * sAtkCurCell.amp  * sAtkCurCell.amp +
                                          wp0 * wp0 * sAtkPrevCell.amp * sAtkPrevCell.amp);
                        m.speed       = mean0(sAtkCurCell.speed,       sAtkPrevCell.speed);
                        m.tilt        = mean0(sAtkCurCell.tilt,        sAtkPrevCell.tilt);
                        m.sway        = mean0(sAtkCurCell.sway,        sAtkPrevCell.sway);
                        m.driftJitter = mean0(sAtkCurCell.driftJitter, sAtkPrevCell.driftJitter);
                        m.roughness   = mean0(sAtkCurCell.roughness,   sAtkPrevCell.roughness);
                        sAtkPrevCell  = m;
                    } else {
                        sAtkPrevCell = sAtkCurCell;
                    }
                    attackBlend.prev  = attackBlend.cur;
                    sAtkPrevCellValid = true;
                    // Ceiling for the outgoing cell, in AMPLITUDE. Capping the
                    // envelope FRACTION was defeated by a chain of swaps: the
                    // fraction was re-captured at each swap, and by the second
                    // one env had climbed back toward 1.0, so the cap was
                    // ~no cap — while the collapsed outgoing cell still carried
                    // the sprint power cell's amplitude through the RMS merge.
                    // Hence sprint_power -> power -> attack still leaking. An
                    // amplitude ceiling composes across a chain because each
                    // swap records what was ACTUALLY audible at that instant.
                    sAtkPrevAmpCap    = sAtkPrevCell.amp * atkEnv;
                    attackBlend.domKey = atkKey;
                    // ONLY cross-fade when there is something to fade FROM.
                    //
                    // The 18:16 trace caught this on the very first swing:
                    // env hit 1.000 at 0.28s but xf crawled to 1.00 over 1.19s
                    // with prevE=0.0 the whole way, so the composite reached
                    // full a FULL SECOND after the envelope did. The beat was
                    // fading in from silence twice over — once via its own
                    // envelope and again via a crossfade that had no outgoing
                    // layer at all. On a fresh beat there is nothing to cross
                    // to, so start at 1 and let the envelope do its job.
                    // ...and only when that outgoing texture is AUDIBLE.
                    //
                    // atkWasPlaying only asked "did the layer render at all",
                    // which is true even at env=0.005 — the last sliver of a
                    // spent beat. A new swing arriving there got a full texture
                    // crossfade (up to 0.55s) while its OWN envelope rose to
                    // full in 0.24s. So the entire punch of the new swing was
                    // rendered with the PREVIOUS cell's texture, and only
                    // afterwards morphed into its own: a power attack landing
                    // with the sprint-power shake, then turning into a power
                    // attack. That is both "the transition is all over the
                    // place" and "power attack leaking into normal attacks",
                    // and it is why the amplitude curve traced clean the whole
                    // time — the level was right, the texture was not.
                    //
                    // env IS the composite's level (both layers are scaled by
                    // it), so below this threshold a texture swap is masked by
                    // the envelope itself. Take it instantly and let the new
                    // swing punch with its own character.
                    constexpr float kAtkXfadeAudible = 0.20f;
                    attackBlend.xfade =
                        (atkWasPlaying && atkEnv > kAtkXfadeAudible) ? 0.0f : 1.0f;
                    // And when there IS an outgoing cell, swap FAST. The same
                    // trace showed a power->normal switch rendering
                    // comp=28.0 (all old power cell) while the new normal cell
                    // sat at 10.8 and silent — using the adaptive STATE
                    // duration meant the swing you just made took up to 1.2s to
                    // be heard. An action's texture swap is punctuation, not a
                    // state morph.
                    {
                        const float dSwap = TextureDistance(
                            sAtkPrevCell.amp, sAtkPrevCell.speed, sAtkPrevCell.tilt,
                            sAtkPrevCell.sway, sAtkPrevCell.driftJitter, sAtkPrevCell.roughness,
                            sAtkCell.amp, sAtkCell.speed, sAtkCell.tilt,
                            sAtkCell.sway, sAtkCell.driftJitter, sAtkCell.roughness);
                        const float t = std::clamp(dSwap, 0.0f, 1.0f);
                        sAtkBlendRate = 1.0f / (kAtkBlendMin +
                            (kAtkBlendMax - kAtkBlendMin) * (t * t * (3.0f - 2.0f * t)));
                    }
                    attackBlend.cur.clockTrans += kXfadeDecorrelate;
                    attackBlend.cur.clockRot   += kXfadeDecorrelate * 0.73;
                    atkSwapThisFrame = true;
                }
                attackBlend.xfade += dt * sAtkBlendRate;
                if (attackBlend.xfade > 1.0f) attackBlend.xfade = 1.0f;
                auto& L       = attackBlend.cur;
                const double kT = L.clockTrans, kR = L.clockRot;
                L             = curAmbient;
                L.clockTrans  = kT;
                L.clockRot    = kR;
                L.amp         = sAtkCell.amp * atkEnv;
                L.speed       = std::max(0.1f, sAtkCell.speed);
                L.tilt        = std::max(ambTiltEff, sAtkCell.tilt);
                L.sway        = std::max(ambSwayEff, sAtkCell.sway);
                L.driftJitter = sAtkCell.driftJitter;
                L.roughness   = sAtkCell.roughness;
                sAtkCurCell = sAtkCell;
                // FRAME TRACE while a beat is live. Bounded, and only while
                // something is actually playing. This exists because reasoning
                // about this layer from the code has repeatedly produced the
                // wrong answer — one recording of a bad swing should end the
                // guessing.
                {
                    // Trace a WINDOW AROUND EACH SWAP, not the first N frames.
                    // The first-N budget was spent on the opening swings and had
                    // run dry long before the combo swap actually being
                    // reported, which left the 18:57 step with one [MOTION] line
                    // and no frames either side of it.
                    static int sTraceLeft    = 0;
                    static int sTraceWindows = 0;
                    // Window on a CELL CHANGE only. Tracing every fresh arm
                    // burned the whole budget on repeated identical swings —
                    // the 20:16 log spent all 12 windows before the reported
                    // transition ever happened. A swap fires whenever the key
                    // changes, including when the fade is skipped as inaudible,
                    // so this still catches the case that matters.
                    (void)atkFreshArm;
                    if (atkSwapThisFrame && sTraceWindows < 20) {
                        ++sTraceWindows;
                        sTraceLeft = 45;    // ~1.3s at 35fps
                    }
                    if (sTraceLeft > 0) {
                        --sTraceLeft;
                        float wcT, wpT;
                        XfadeWeights(attackBlend.xfade, wcT, wpT);
                        const float curE  = L.amp * (L.tilt + L.sway);
                        const float prevE = attackBlend.prev.amp *
                                            (attackBlend.prev.tilt + attackBlend.prev.sway);
                        spdlog::debug("[ATKTRACE] key={} env={:.3f} xf={:.2f} "
                                     "curE={:.1f} prevE={:.1f} comp={:.1f} ambE={:.1f} "
                                     "spd {:.2f}/{:.2f} rise={:.2f} fall={:.2f} "
                                     "shape {:.2f}/{:.2f} ambNow {:.2f}/{:.2f}",
                                     sAtkLastKey, atkEnv, attackBlend.xfade,
                                     curE, prevE,
                                     std::sqrt(wcT * wcT * curE * curE + wpT * wpT * prevE * prevE),
                                     effAmp * (ambTiltEff + ambSwayEff),
                                     L.speed, attackBlend.prev.speed, atkRise, atkFall,
                                     L.tilt, L.sway, ambTiltEff, ambSwayEff);
                    }
                }
                // Rebuild the OUTGOING layer against the live envelope too, so
                // the pair rises and falls together and only their TEXTURES
                // cross-fade. Its own clock and parameters are untouched.
                if (sAtkPrevCellValid && attackBlend.xfade < 1.0f) {
                    auto& Pv        = attackBlend.prev;
                    const double pT = Pv.clockTrans, pR = Pv.clockRot;
                    Pv              = curAmbient;
                    Pv.clockTrans   = pT;
                    Pv.clockRot     = pR;
                    // The outgoing cell may FALL with the shared envelope, but
                    // it must never RISE with it.
                    //
                    // Both layers were scaled by the live env so the pair
                    // "rises and falls together and only their textures cross-
                    // fade". That was safe only while a re-arm restarted near
                    // zero. Now that the resume works correctly, a new swing
                    // lifts env from wherever the old beat was back up to 1.0 —
                    // and the OUTGOING cell rode that lift at its own, much
                    // larger amplitude. A normal attack (10.8 energy) arming
                    // over a sprint power tail (50 energy) therefore re-fired
                    // the sprint power cell at full strength and then faded it
                    // out: "sprinting power attack noise leaks into normal
                    // attack". Capping at the level it held when the swap
                    // happened keeps the swap frame continuous (min == that
                    // value there) while making the leak impossible.
                    Pv.amp          = (std::min)(sAtkPrevCell.amp * atkEnv,
                                                 sAtkPrevAmpCap);
                    // A layer's clock rate is FIXED for its lifetime. Easing the
                    // outgoing rate toward the incoming one was tried here to
                    // morph between very different Speeds (4.00 -> 0.30 on this
                    // rig) and made things clearly worse: winding a still-loud
                    // layer's clock DOWN decelerates its motion to near-stop,
                    // which reads as a stall-then-resume. That is the same
                    // failure as the 1p repulse dead-stop — there the clock rate
                    // stepped, here it ramped, and both are visible for the same
                    // reason. Rate belongs to the texture; only weight fades.
                    Pv.speed        = std::max(0.1f, sAtkPrevCell.speed);
                    Pv.tilt         = std::max(ambTiltEff, sAtkPrevCell.tilt);
                    Pv.sway         = std::max(ambSwayEff, sAtkPrevCell.sway);
                    Pv.driftJitter  = sAtkPrevCell.driftJitter;
                    Pv.roughness    = sAtkPrevCell.roughness;
                } else {
                    attackBlend.prev.amp = 0.0f;
                }
                attackBlend.valid = true;
                float wc, wp;
                XfadeWeights(attackBlend.xfade, wc, wp);
                effAmp += std::sqrt(wc * wc * L.amp * L.amp +
                                    wp * wp * attackBlend.prev.amp * attackBlend.prev.amp);
            }
        }

        // ==== NPC concentration casting (continuous blend) ====
        // Carries the resolved player entry's texture verbatim; a change of
        // dominant SCHOOL refades exactly like the prox blend.
        npcBlend.valid = false;
        // Third-person half of the NPC layer. ScanNpcCasters returns an
        // unscaled amplitude now (2026-09-07 per-view split), so the Intensity
        // this view owns is applied here.
        const float npcAmp3p = npcNow.amp * settings.npcNoiseIntensity;
        if (npcAmp3p > 0.0001f) {
            if (npcBlend.domKey != npcNow.domKey) {
                npcBlend.prev   = npcBlend.cur;
                npcBlend.xfade  = 0.0f;
                npcBlend.domKey = npcNow.domKey;
            }
            npcBlend.xfade += dt * kAmbientXfadeRateDefault;
            if (npcBlend.xfade > 1.0f) npcBlend.xfade = 1.0f;
            auto& L   = npcBlend.cur;
            L.amp     = npcAmp3p;
            L.speed   = std::max(0.1f, npcNow.speed);
            L.tilt    = npcNow.tilt;
            L.sway    = npcNow.sway;
            if (npcNow.dj  > 0.0f) L.driftJitter = npcNow.dj;
            if (npcNow.rgh > 0.0f) L.roughness   = npcNow.rgh;
            npcBlend.valid = true;
            {
                float wc, wp;
                XfadeWeights(npcBlend.xfade, wc, wp);
                effAmp += std::sqrt(wc * wc * L.amp * L.amp +
                                    wp * wp * npcBlend.prev.amp * npcBlend.prev.amp);
            }
        }

        // ==== Projectile Repulse (3p, Cinematic Effects) ====
        // 3p sibling of the 1p FoF release recoil: a cannon-recoil pitch
        // impulse when a projectile leaves the player. Same shared release
        // shape as 1p (3.5° peak × slider, quartic decay over 0.28s) so the
        // two POVs feel like one effect. Triggers:
        //  - Magic:   per-hand caster release edge (any transition out of
        //             kReady — the same edge set the 1p burst uses, since FoF
        //             skips kCasting) for hands holding a fire-and-forget
        //             spell, plus the kCasting fire-edge for tap-fire spells.
        //             The edge only records a PENDING shot — the actual kick
        //             is armed by the graph's spell-fire event (the visual
        //             release frame of whatever cast animation is playing),
        //             with per-context fallback timers (spell/staff/ritual/
        //             Vampire Lord) when no event arrives.
        //  - Archery: a confirmed TESPlayerBowShotEvent for bows; graph
        //             release for crossbows. Bow recovery annotations and
        //             draw cancellation cannot trigger another shot.
        // 3p-only by construction (this is the 3p path; the 1p branch
        // returned long ago).
        float repPitchRad = 0.0f, repYawRad = 0.0f, repRollRad = 0.0f;
        float repPushUnits = 0.0f, repPushSideUnits = 0.0f, repPushVertUnits = 0.0f;
        {
            // Per-shot envelope state. The kick is the impulse response of an
            // UNDERDAMPED spring (ζ≈0.62): k(t) = e^(-ζωt)·sin(ω√(1-ζ²)·t),
            // normalized to peak 1. That gives a smooth ~70ms RISE (no
            // single-frame snap — the "weird jerk" complaint was the old
            // instant-peak quartic), a settle over ~0.4s, and one small
            // (~8%) counter-swing as the mass rebounds — the shape of a real
            // recoil absorbed by a body. Per-shot randomness (amplitude,
            // tempo, yaw/roll seasoning) keeps consecutive shots from feeling
            // stamped from the same mold.
            // The per-shot parameter SET is what carries the variation — not
            // just size but CHARACTER: damping (snappy-with-rebound vs heavy
            // dead-blow), tempo, kick direction, push lean, shove-vs-flick
            // balance, and a secondary shudder harmonic that some shots have
            // and some don't. Two consecutive shots should never decompose
            // identically.
            //
            // OVERLAPPING SLOTS: rapid-fire used to RESET the single envelope
            // on re-arm — the output snapped from mid-peak to zero (the
            // pre-delay) in one frame = the "instant jerk when releasing one
            // spell right after another". Each shot now takes its own slot
            // and the outputs SUM: the previous kick keeps settling while the
            // new one rises on top, which is also what real consecutive
            // impulses do to a body.
            struct RepShot {
                float elapsed   = 1.0e9f;  // time since trigger; negative = pre-delay
                float omega     = 16.0f;   // spring rate (tempo)
                float zeta      = 0.62f;   // damping — LOW = visible rebound, HIGH = dead-blow
                float wd        = 0.7846f; // √(1-ζ²)
                float norm      = 2.60f;   // 1 / envelope peak for this ζ
                float peakPitch = 0.0f;    // rad
                float peakYaw   = 0.0f;    // rad, signed
                float peakRoll  = 0.0f;    // rad, signed
                float peakPush  = 0.0f;    // units
                float pushSide  = 0.0f;    // lateral fraction, signed
                float pushVert  = 0.0f;    // vertical fraction, signed
                float harmAmp   = 0.0f;    // secondary shudder (0 = clean)
                float harmMul   = 2.3f;    // secondary frequency ratio
            };
            static RepShot sRepShots[3];
            // Per-hand trigger debounce: one physical cast can produce BOTH a
            // release edge (out of kReady) and a fire edge (into kCasting) a
            // frame or two apart — without this, the second edge re-armed the
            // kick right at its rise (another source of the rapid-fire jerk).
            static std::chrono::steady_clock::time_point sRepHandArm[2]{};
            // Pending magic shot — the event-anchored release model. The
            // caster edge only says "a shot is coming" and records strength;
            // the actual arm waits for the graph's spell-fire event (the
            // visual release frame of WHATEVER cast animation is playing —
            // human, Vampire Lord, modded sets — which is what makes the
            // timing compatible with arbitrary animations instead of a
            // per-context delay guess). If no event arrives, a per-context
            // fallback timer arms the kick at the old tuned delay.
            static bool          sRepPending          = false;
            static float         sRepPendingCountdown = 0.0f;
            static float         sRepPendingSlider    = 0.0f;
            static float         sRepPendingFeel      = 0.5f;
            static float         sRepPendingStrength  = 1.0f;
            static std::uint32_t sRepSeenSpellFire    = 0;
            constexpr float kDeg2Rad = 0.017453293f;

            // Per-entry repulse (magic school cells / shout entries / archery
            // drawing). The master gate asks "is any entry configured"; the
            // per-shot slider is resolved from the entry that actually fired.
            const bool wantAny = settings.AnyRepulseConfigured();

            if (wantAny) {
                // Cheap per-shot randomness (LCG → [0,1)); deterministic
                // quality doesn't matter here, only variation.
                static std::uint32_t sRepRngState = 0x51ed270bu;
                auto frand = [&]() {
                    sRepRngState = sRepRngState * 1664525u + 1013904223u;
                    return static_cast<float>(sRepRngState >> 8) * (1.0f / 16777216.0f);
                };
                // Arm a shot into a free (or most-settled) slot. a_strength
                // folds in everything shot-specific (tier / draw / ritual /
                // dual-cast), the slider scales it, and the per-shot rolls
                // set SIZE and CHARACTER:
                //  - amplitude 0.65–1.40×, tempo ±20%, and a 10% chance of a
                //    "heavy one" (+30%) so the occasional shot lands notably
                //    harder than its neighbors.
                //  - damping ζ ∈ [0.45, 0.80]: low = snappy kick with a
                //    visible rebound past center, high = heavy dead-blow that
                //    just sinks back. Changes the whole gesture of the shot.
                //  - shove-vs-flick balance: a per-shot bias trades rotation
                //    against push (±25% each way, opposed) — some shots are
                //    mostly a body shove, others mostly a view flick.
                //  - kick-axis tilt: yaw up to ~±40% of the pitch, so shots
                //    land up-left / up-right / near-straight; roll seasons
                //    the same way.
                //  - push lean: the dolly leans sideways (±22%) and
                //    vertically (±12%) per shot.
                //  - secondary shudder: ~60% of shots get a faster, weaker
                //    harmonic on the settle; the rest settle clean.
                //  - timing: up to 30ms of extra per-shot release latency so
                //    consecutive shots aren't metronomic.
                // Heavier shots also recoil a touch SLOWER (lower ω) — big
                // impacts read as mass, not twitch.
                auto armRepulse = [&](float a_slider, float a_strength, float a_preDelay,
                                      float a_feel = 0.5f) {
                    RepShot* slot = &sRepShots[0];
                    for (auto& sh : sRepShots) {
                        if (sh.elapsed > slot->elapsed) slot = &sh;
                    }
                    // Per-entry FEEL — one axis. 0 = heavy dead-blow (slower,
                    // damped, push-forward), 0.5 = the shipped shape exactly,
                    // 1 = sharp snap (faster, springy overshoot,
                    // rotation-forward). Every factor is 1.0 at feel 0.5.
                    const float feel     = std::clamp(a_feel, 0.0f, 1.0f);
                    const float tempoMul = 0.70f + 0.60f * feel;
                    const float kickMul  = 0.85f + 0.30f * feel;
                    const float pushMul  = 1.15f - 0.30f * feel;
                    const float shudMul  = 0.60f + 0.80f * feel;
                    const float heavyRoll = (frand() < 0.10f) ? 1.30f : 1.0f;
                    const float ampJit  = (0.65f + 0.75f * frand()) * heavyRoll;
                    const float durJit  = 0.80f + 0.40f * frand();
                    const float rotBias = 0.75f + 0.50f * frand();   // rotation weight
                    const float pushBias= 2.0f - rotBias;            // opposed push weight
                    const float s       = a_slider * a_strength * ampJit;
                    const float heavy   = std::clamp(a_strength, 0.0f, 1.6f);
                    slot->elapsed   = a_preDelay - 0.03f * frand();
                    slot->omega     = 16.0f * tempoMul * durJit / (0.85f + 0.25f * heavy);
                    // Per-shot damping + its normalization (peak of
                    // e^(-ζωt)·sin(ω·wd·t) is e^(-ζφ/wd)·wd at φ=atan(wd/ζ)).
                    // Feel slides the ζ band: 0 → heavy dead-blows
                    // (ζ≈0.68..0.95), 0.5 → the shipped 0.45..0.80,
                    // 1 → springy 0.23..0.58 with a visible overshoot.
                    const float zetaCenter = 0.85f - 0.45f * feel;
                    slot->zeta      = std::clamp(zetaCenter - 0.175f + 0.35f * frand(), 0.20f, 0.95f);
                    slot->wd        = std::sqrt(1.0f - slot->zeta * slot->zeta);
                    const float phi = std::atan(slot->wd / slot->zeta);
                    slot->norm      = 1.0f / (std::exp(-slot->zeta * phi / slot->wd) * slot->wd);
                    slot->peakPitch = 3.0f  * kDeg2Rad * s * rotBias * kickMul;
                    slot->peakYaw   = 1.20f * kDeg2Rad * s * rotBias * kickMul * (frand() * 2.0f - 1.0f);
                    slot->peakRoll  = 0.90f * kDeg2Rad * s * rotBias * kickMul * (frand() * 2.0f - 1.0f);
                    slot->peakPush  = 5.0f * s * pushBias * pushMul * (0.85f + 0.35f * frand());
                    slot->pushSide  = 0.22f * (frand() * 2.0f - 1.0f);
                    slot->pushVert  = 0.12f * (frand() * 2.0f - 1.0f);
                    const float harmRoll = frand();
                    slot->harmAmp   = ((harmRoll < 0.6f) ? 0.10f + 0.12f * frand() : 0.0f) * shudMul;
                    slot->harmMul   = 1.8f + 1.2f * frand();
                };

                // --- jump landing thud ---
                // Armed by PollJumpArc at the confirmed touchdown; the impact
                // scale (how hard the fall was) rides the strength argument the
                // same way a shot's tier does. No pre-delay — the confirmation
                // itself already sits a few frames past the visual impact.
                if (sJumpLandKickPending) {
                    sJumpLandKickPending = false;
                    sJumpLandKickGlide   = false;
                    // The thud's shape belongs to the Jumping entry's own
                    // Repulse / Thud-Snap sliders. Fade Duration means ONE
                    // thing — how long the glide NOISE takes to hand over —
                    // and stopped scaling this envelope 2026-08-13.
                    armRepulse(settings.jumpRepulse, sJumpLandKickScale, 0.0f,
                               settings.jumpRepulseFeel);
                }

                // --- spell / staff release edges ---
                using CState = RE::MagicCaster::State;
                using Src    = RE::MagicSystem::CastingSource;
                static CState sRepLastLeft  = CState::kNone;
                static CState sRepLastRight = CState::kNone;
                CState curL = CState::kNone, curR = CState::kNone;
                bool fofL = false, fofR = false, staffL = false, staffR = false;
                // Concentration spells have no discrete release, so none of the
                // edges below ever fired for them and the Repulse slider on a
                // concentration cell was inert — visible, tunable, and doing
                // nothing (user report 2026-08-19). A beam still has an
                // IGNITION, and that is the one moment a kick belongs: the
                // kCasting entry edge, once per cast.
                bool concL = false, concR = false;
                RE::MagicItem* spellL = nullptr;
                RE::MagicItem* spellR = nullptr;
                if (player) {
                    auto readHand = [&](Src a_src, bool a_isLeft, CState& a_out,
                                        bool& a_fof, bool& a_conc, bool& a_staff,
                                        RE::MagicItem*& a_spell) {
                        if (auto* c = player->GetMagicCaster(a_src)) {
                            a_out = c->state.get();
                            if (auto* sp = c->currentSpell) {
                                a_spell = sp;
                                const auto ct = sp->GetCastingType();
                                a_fof  = ct == RE::MagicSystem::CastingType::kFireAndForget;
                                a_conc = ct == RE::MagicSystem::CastingType::kConcentration;
                            }
                        }
                        if (auto* obj = player->GetEquippedObject(a_isLeft)) {
                            if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                                a_staff = w->IsStaff();
                            }
                        }
                    };
                    readHand(Src::kLeftHand,  true,  curL, fofL, concL, staffL, spellL);
                    readHand(Src::kRightHand, false, curR, fofR, concR, staffR, spellR);
                }
                const bool relL  = fofL && sRepLastLeft  == CState::kReady && curL != CState::kReady;
                const bool relR  = fofR && sRepLastRight == CState::kReady && curR != CState::kReady;
                const bool fireL = fofL && sRepLastLeft  != CState::kCasting && curL == CState::kCasting;
                const bool fireR = fofR && sRepLastRight != CState::kCasting && curR == CState::kCasting;
                // Ignition edge only — the state stays kCasting for the whole
                // beam, so this cannot repeat while the button is held.
                const bool concFireL = concL && sRepLastLeft  != CState::kCasting && curL == CState::kCasting;
                const bool concFireR = concR && sRepLastRight != CState::kCasting && curR == CState::kCasting;
                sRepLastLeft  = curL;
                sRepLastRight = curR;
                // Debounce: one physical cast can hit both the release edge
                // and the fire edge frames apart (kReady→kUnk04→kCasting) —
                // collapse them to one kick per hand per 120ms.
                const auto repNow = CameraEffectClock::Now();
                auto handFresh = [&](int h) {
                    return (repNow - sRepHandArm[h]) >= std::chrono::milliseconds(120);
                };
                const bool shotL = (relL || fireL || concFireL) && handFresh(0);
                const bool shotR = (relR || fireR || concFireR) && handFresh(1);
                if (shotL || shotR) {
                    if (shotL) sRepHandArm[0] = repNow;
                    if (shotR) sRepHandArm[1] = repNow;
                    // Spell POWER: how hard should this spell kick? The old
                    // skill-tier-only curve under-reported anything that
                    // doesn't play the vanilla progression game — leveled and
                    // race spells (Vampire Lord Drain) report minimum-skill 0
                    // and kicked like novice cantrips. Take the MAX of three
                    // independent power reads so a spell that's big by ANY
                    // measure kicks big, while firebolt-class spells land
                    // exactly where the tier curve already put them (their
                    // magnitude/cost reads sit below their tier value):
                    //  - skill tier of the costliest effect (novice 0.5x …
                    //    master 1.7x — the 1p burst's curve),
                    //  - costliest effect MAGNITUDE (0.40 + mag/95, cap 1.7:
                    //    ~25dmg apprentice ≈ 0.66, 60+ ≈ 1.0+, 100+ ≈ 1.45+),
                    //  - base magicka COST (0.50 + cost/300, cap 1.8).
                    auto spellPowerMul = [&](RE::MagicItem* item) -> float {
                        if (!item) return 1.0f;
                        float mul = 0.5f;
                        if (auto* eff = item->GetCostliestEffectItem();
                            eff && eff->baseEffect) {
                            const int sk = eff->baseEffect->GetMinimumSkillLevel();
                            mul = (sk >= 100) ? 1.7f
                                : (sk >=  75) ? 1.3f
                                : (sk >=  50) ? 1.0f
                                : (sk >=  25) ? 0.75f
                                              : 0.5f;
                            const float mag = eff->effectItem.magnitude;
                            mul = (std::max)(mul,
                                std::clamp(0.40f + mag / 95.0f, 0.40f, 1.7f));
                        }
                        const float cost = item->CalculateMagickaCost(player);
                        mul = (std::max)(mul,
                            std::clamp(0.50f + cost / 300.0f, 0.50f, 1.8f));
                        return mul;
                    };
                    const bool dual      = (shotL && shotR) ||
                                           StateResolver::GetSingleton().GetCastingHand() ==
                                               CastingHand::Both;
                    const bool viaStaff  = shotL ? staffL : staffR;
                    RE::MagicItem* firing = shotL ? spellL : spellR;
                    const bool isRitual = ResolveMagicCastType(firing,
                        viaStaff ? MagicCastSource::Staff : MagicCastSource::Spell) == CastType::Ritual;
                    const bool isVL      = StateResolver::GetSingleton().IsVampireLord();
                    // Which cell's Repulse slider this shot reads. Taken from
                    // the hand that actually fired, not the resolver, so a
                    // concentration beam in one hand and a bolt in the other
                    // each read their own row.
                    const bool isConc    = shotL ? concL : concR;
                    float power = 1.0f;
                    if (shotL && shotR)
                        power = std::max(spellPowerMul(spellL), spellPowerMul(spellR));
                    else
                        power = spellPowerMul(shotL ? spellL : spellR);
                    // Vampire Lord floor: VL powers defeat every metric above
                    // (skill 0, cost 0), yet a monster hurling life-drain
                    // bolts should land mid-adept at minimum. Floor, not
                    // multiplier, so genuinely big VL casts still exceed it.
                    if (isVL) power = std::max(power, 1.25f);
                    const float strength = power *
                                           (isRitual ? 1.5f : 1.0f) *
                                           (dual ? 1.65f : 1.0f);
                    // Record the pending shot; the spell-fire graph event
                    // arms it at the true visual release. The countdown is
                    // the FALLBACK for animation sets that never emit one —
                    // per-context guesses matching the old tuned pre-delays,
                    // with a dedicated (longer) Vampire Lord timing for its
                    // slow levitating throw.
                    // Per-entry slider: the key is WHAT fired — school from
                    // the firing hand's spell, staves vs magic from the
                    // weapon, ritual/fof from the firing item, sneak from the
                    // live stance. An enabled hand-specific entry wins first
                    // (the noise resolver's hand-redirect rule).
                    float repSlider = 0.0f;
                    float repFeel   = 0.5f;
                    if (isVL) {
                        // Vampire Lord casts read their OWN cell. The VL tree
                        // is a family of its own, not a magic-school variant —
                        // and VL powers report junk schools anyway.
                        if (const auto* entry = RepulseEntryForKey(
                                isConc ? "transformations.vampire_lord.concentration"
                                       : "transformations.vampire_lord.fire_and_forget")) {
                            repSlider = entry->repulse;
                            repFeel   = entry->repulseFeel;
                        }
                    } else {
                        int schoolIdx = -1;
                        if (firing) {
                            if (auto* eff = firing->GetCostliestEffectItem();
                                eff && eff->baseEffect)
                                schoolIdx = NpcSchoolIndex(eff->baseEffect->GetMagickSkill());
                        }
                        if (schoolIdx >= 0) {
                            const bool  sneak = StateResolver::GetSingleton().IsSneaking();
                            const char* cast  = isRitual                ? "ritual"
                                              : isConc                 ? "concentration"
                                                                        : "fire_and_forget";
                            std::string key = std::string(viaStaff ? "staves." : "magic.") +
                                              kSchoolNames[schoolIdx] +
                                              (sneak ? ".sneak." : ".") + cast;
                            const auto* entry = RepulseEntryForKey(key);
                            if (!viaStaff) {
                                const char* hand = dual ? "both" : (shotL ? "left" : "right");
                                if (const auto* he = RepulseEntryForKey(
                                        key + ".hand." + hand, /*enabledOnly=*/true))
                                    entry = he;
                            }
                            if (entry) {
                                repSlider = entry->repulse;
                                repFeel   = entry->repulseFeel;
                            }
                        }
                    }
                    if (repSlider > 0.001f) {
                        sRepPending          = true;
                        // A beam's kick belongs AT ignition — there is no
                        // wind-up to wait out and no projectile to leave the
                        // hand, so the pre-delay is only long enough for the
                        // spell-fire event to beat the fallback if it comes.
                        sRepPendingCountdown = isConc   ? 0.06f
                                             : isVL     ? 0.55f
                                             : viaStaff ? 0.40f
                                             : isRitual ? 0.45f
                                                        : 0.18f;
                        sRepPendingSlider    = repSlider;
                        sRepPendingFeel      = repFeel;
                        sRepPendingStrength  = strength;
                        // Only spell-fire events AFTER this edge may arm it.
                        sRepSeenSpellFire    = sSpellFireCounter;
                    }
                }

                // Pending-shot resolution: the graph's spell-fire event is
                // the release anchor; the countdown is the no-event fallback.
                if (sRepPending) {
                    if (sSpellFireCounter != sRepSeenSpellFire) {
                        sRepSeenSpellFire = sSpellFireCounter;
                        sRepPending       = false;
                        // Tiny random latency keeps event-armed shots from
                        // being metronomic; never early (event = release).
                        armRepulse(sRepPendingSlider, sRepPendingStrength,
                                   -0.025f * frand(), sRepPendingFeel);
                        spdlog::debug("[Repulse] magic kick armed via SpellFire event");
                    } else {
                        sRepPendingCountdown -= dt;
                        if (sRepPendingCountdown <= 0.0f) {
                            sRepPending = false;
                            armRepulse(sRepPendingSlider, sRepPendingStrength, 0.0f,
                                       sRepPendingFeel);
                            spdlog::debug("[Repulse] magic kick armed via fallback timer");
                        }
                    }
                }

                // --- confirmed bow shot / crossbow graph release ---
                const auto stNow = StateResolver::GetSingleton().GetState();
                auto* equipped = player ? player->GetEquippedObject(false) : nullptr;
                auto* rangedWeapon = equipped ? equipped->As<RE::TESObjectWEAP>() : nullptr;
                const bool crossbow = rangedWeapon && rangedWeapon->IsCrossbow();
                const bool confirmedBow = bowShot &&
                    CameraEffectClock::Now() - bowShot->fired < std::chrono::milliseconds(500);
                const bool released = crossbow ? arrowGraphRelease : confirmedBow;
                if (released && (stNow == CameraState::Bow || stNow == CameraState::Crossbow ||
                                 stNow == CameraState::Horseback)) {
                    const auto& resolver = StateResolver::GetSingleton();
                    const bool sneak = crossbow ? resolver.IsSneaking() : bowShot->sneaking;
                    const bool zoomed = crossbow ? resolver.IsBowZoomed() : bowShot->zoomed;
                    const std::string mode = zoomed ? ".zoom" : ".draw";
                    const SettingsManager::NoiseProfile* aEntry = nullptr;
                    std::string repulseKey;
                    auto pick = [&](const char* base) {
                        if (aEntry) return;
                        const auto key = std::string(base) + mode;
                        if (const auto* entry = RepulseEntryForKey(key)) {
                            aEntry = entry;
                            repulseKey = key;
                        }
                    };
                    // Zoomed and Drawing are separate effects. Only missing
                    // entries inherit within the same mode; zero is an explicit
                    // off value and must not select a different effect.
                    if (stNow == CameraState::Horseback) {
                        pick("mounts.horseback.archery");
                    } else {
                        if (crossbow) pick(sneak ? "weapons.crossbow.sneak" : "weapons.crossbow");
                        pick(sneak ? "weapons.bow.sneak" : "weapons.bow");
                        if (sneak) pick("weapons.bow");
                    }
                    // The shot event supplies draw power without sampling the
                    // stale draw timer again during the return to unsheathed.
                    const float strength = crossbow ? 1.15f : 0.45f + 0.55f * bowShot->power;
                    if (aEntry && aEntry->repulse > 0.001f) {
                        armRepulse(aEntry->repulse, strength, 0.0f, aEntry->repulseFeel);
                        spdlog::info("[Repulse] {} kick armed (strength={:.2f}, source={}, key={})",
                            crossbow ? "crossbow" : "bow", strength, crossbow ? "graph" : "confirmed shot", repulseKey);
                    }
                }

                // --- shout release (kVoiceFire) ---
                // Anchored on the engine's voice-fire edge (the same signal
                // the word count uses) — NOT the graph's voice-tagged
                // spellfire, which stays excluded. The slider comes from the
                // player's per-shout entry for the current state, falling
                // through to the state's base shout entry; strength scales
                // with the word tier like the shout noise envelope.
                static std::chrono::steady_clock::time_point sRepLastShoutFire{};
                const auto shoutFireTp = StateResolver::GetSingleton().GetShoutFireTime();
                if (shoutFireTp.time_since_epoch().count() != 0 &&
                    shoutFireTp != sRepLastShoutFire) {
                    // ALWAYS consume the stamp — skipping it while in beast
                    // form would leave a stale roar timestamp that kicks the
                    // moment the player reverts.
                    sRepLastShoutFire = shoutFireTp;
                    // Beast-form roars ride the voice pipeline too (kVoiceFire
                    // stamps), but they are NOT shout entries — their repulse
                    // lives on the werewolf ROAR entry instead of
                    // shouts.<state>.base ("roar had repulse with no slider").
                    const bool isWW = StateResolver::GetSingleton().IsWerewolf();
                    const bool isVL = StateResolver::GetSingleton().IsVampireLord();
                    if (isWW || isVL) {
                        // Vampire Lord has no roar entry — nothing to read.
                        if (isWW) {
                            if (const auto* re =
                                    RepulseEntryForKey("transformations.werewolf.roar");
                                re && re->repulse > 0.001f) {
                                armRepulse(re->repulse, 1.0f, 0.0f, re->repulseFeel);
                                spdlog::debug("[Repulse] werewolf roar kick armed");
                            }
                        }
                    } else {
                        const char* bucket = NpcShoutStateBucket();
                        const bool  sneak  = StateResolver::GetSingleton().IsSneaking();
                        const SettingsManager::NoiseProfile* sEntry = nullptr;
                        std::string key;
                        if (auto id = StateResolver::GetSingleton().GetActiveShoutId()) {
                            key = std::string("shouts.") + bucket + "." +
                                  kShouts[static_cast<std::size_t>(*id)].tomlKey +
                                  (sneak ? ".sneak" : "");
                            sEntry = RepulseEntryForKey(key, /*enabledOnly=*/true);
                        }
                        if (!sEntry) {
                            key = std::string("shouts.") + bucket + ".base" +
                                  (sneak ? ".sneak" : "");
                            sEntry = RepulseEntryForKey(key);
                        }
                        if (sEntry && sEntry->repulse > 0.001f) {
                            const int words = std::clamp(
                                StateResolver::GetSingleton().GetShoutWordCount(), 1, 3);
                            const float strength = words == 3 ? 1.5f
                                                 : words == 2 ? 1.1f
                                                              : 0.8f;
                            armRepulse(sEntry->repulse, strength, 0.0f, sEntry->repulseFeel);
                            spdlog::debug("[Repulse] shout kick armed key={} words={}", key, words);
                        }
                    }
                }
            }

            // Advance every active slot and SUM the outputs — overlapping
            // shots stack instead of resetting each other (the rapid-fire
            // jerk fix). Runs even if the sliders were zeroed mid-flight (an
            // armed kick just finishes its settle).
            for (auto& sh : sRepShots) {
                if (sh.elapsed >= 1.0e8f) continue;
                sh.elapsed += dt;
                const float t = sh.elapsed;
                // Six time constants ≈ fully settled — free the slot.
                if (t >= 6.0f / (sh.zeta * sh.omega)) {
                    sh.elapsed = 1.0e9f;
                    continue;
                }
                if (t < 0.0f) continue;  // still in this shot's pre-delay
                // Primary impulse (this shot's damping/tempo/norm) plus the
                // optional secondary shudder — a faster, weaker harmonic that
                // decays ~1.6× quicker. Both start at exactly 0 (sin(0)=0),
                // so the rise stays snap-free.
                const float k1 = std::exp(-sh.zeta * sh.omega * t) *
                                 std::sin(sh.omega * sh.wd * t) * sh.norm;
                float kRot = k1;
                if (sh.harmAmp > 0.0f) {
                    const float k2 =
                        std::exp(-sh.zeta * sh.omega * 1.6f * t) *
                        std::sin(sh.omega * sh.wd * sh.harmMul * t) * sh.norm;
                    kRot += sh.harmAmp * k2;
                }
                repPitchRad += sh.peakPitch * kRot;
                repYawRad   += sh.peakYaw   * kRot;
                repRollRad  += sh.peakRoll  * kRot;
                // Push rides the clean primary only — positional shudder
                // reads as collision jitter, not recoil.
                const float push = sh.peakPush * k1;
                repPushUnits     += push;
                repPushSideUnits += push * sh.pushSide;
                repPushVertUnits += push * sh.pushVert;
            }
        }

        // Everything is a LAYER now — there is no cinematic path to blend
        // against, so the ambient crossfade always renders at full weight and
        // the dragon rides on top of it like every other cinematic source.
        repPitchRad += hitRotation.pitch;
        repYawRad += hitRotation.yaw;
        repRollRad += hitRotation.roll;
        const bool repulseActive =
            std::abs(repPitchRad) > 1e-7f || std::abs(repYawRad) > 1e-7f ||
            std::abs(repRollRad) > 1e-7f || std::abs(repPushUnits) > 1e-5f;
        // Head Bobbing (third person). Peaks at Intensity 1: 1.9 units of rise
        // and fall, 1.1 units of sway, 0.55 deg of roll — a gait you read in
        // the frame rather than a shake. Counts toward the "is anything
        // contributing this frame" gate below, or the bob would be invisible
        // for every player who runs it with the noise sliders at zero.
        const float bobW3p = SettingsManager::GetSingleton().headBobIntensity *
                             headBob.weight * headBob.gait;
        const bool  bobActive = bobW3p > 0.0001f;
        // Applied-offset ledger reset: from here on, every mutation of
        // cameraRoot->local (layers, bob, Repulse) records itself so aim
        // consumers can recover the clean basis. Reset BEFORE the early-out
        // so an idle frame reports "nothing applied".
        ResetApplied3p();
        if (effAmp < 0.0001f && !repulseActive && !bobActive) {
            return;
        }
        ApplyAmbientCrossfade(a_camera, noiseDt);

        // Composed after the noise texture and before Repulse, on the same
        // cameraRoot->local channel every other 3p layer writes to. World up
        // for the rise/fall (a stride lifts you against gravity, not against
        // the view) and the camera's own right column for the sway, matching
        // the column convention the Repulse push uses just below.
        if (bobActive && a_camera->cameraRoot) {
            auto& local = a_camera->cameraRoot->local;
            const float vertU = headBob.vert * 1.5f * bobW3p;
            const float sideU = headBob.side * 0.9f * bobW3p;
            local.translate.z    += vertU;
            sTailTrans.z         += vertU;
            sApplied3pTrans.z    += vertU;
            sApplied3pAny         = true;
            if (std::abs(sideU) > 1e-5f) {
                const auto& wr = a_camera->cameraRoot->world.rotate;
                const RE::NiPoint3 rgt{ wr.entry[0][2], wr.entry[1][2], wr.entry[2][2] };
                local.translate.x += rgt.x * sideU;
                local.translate.y += rgt.y * sideU;
                local.translate.z += rgt.z * sideU;
                sTailTrans.x      += rgt.x * sideU;
                sTailTrans.y      += rgt.y * sideU;
                sTailTrans.z      += rgt.z * sideU;
                sApplied3pTrans.x += rgt.x * sideU;
                sApplied3pTrans.y += rgt.y * sideU;
                sApplied3pTrans.z += rgt.z * sideU;
            }
            const float rollRad = headBob.roll * 0.40f * 0.017453293f * bobW3p;
            if (std::abs(rollRad) > 1e-7f) {
                const RE::NiMatrix3 mRoll =
                    MatrixFromAxisAngle(RE::NiPoint3{ 0.0f, 1.0f, 0.0f }, rollRad);
                local.rotate  = local.rotate * mRoll;
                sApplied3pRot = sApplied3pRot * mRoll;
                // Feed the suppression tail too, so opening a menu mid-stride
                // walks the bob out over the same 0.12s as the noise instead
                // of dropping it in one frame.
                sTailRotVec.y += rollRad;
            }
        }

        // Projectile Repulse — composed AFTER the noise texture so it stacks
        // on top of whatever the ambient/cinematic layers did this frame.
        // Rotation: pitch up-kick (same local-right convention as the 1p
        // recoil) seasoned with the shot's random yaw/roll. Translation: a
        // backward dolly along the view ray — the camera physically recoils
        // away from the shot, which is what makes it read as a REPULSE
        // instead of a view twitch. The world-space backward direction comes
        // from cameraRoot->world.rotate's forward column (same extraction the
        // dialogue code uses); local.translate is the ambient noise's own
        // translation channel, so it propagates identically.
        if (repulseActive && a_camera->cameraRoot) {
            auto& local = a_camera->cameraRoot->local;
            if (std::abs(repPitchRad) > 1e-7f) {
                const RE::NiMatrix3 m =
                    MatrixFromAxisAngle(RE::NiPoint3{ 1.0f, 0.0f, 0.0f }, repPitchRad);
                local.rotate  = local.rotate * m;
                sApplied3pRot = sApplied3pRot * m;
                sApplied3pAny = true;
            }
            if (std::abs(repYawRad) > 1e-7f) {
                const RE::NiMatrix3 m =
                    MatrixFromAxisAngle(RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, repYawRad);
                local.rotate  = local.rotate * m;
                sApplied3pRot = sApplied3pRot * m;
                sApplied3pAny = true;
            }
            if (std::abs(repRollRad) > 1e-7f) {
                const RE::NiMatrix3 m =
                    MatrixFromAxisAngle(RE::NiPoint3{ 0.0f, 1.0f, 0.0f }, repRollRad);
                local.rotate  = local.rotate * m;
                sApplied3pRot = sApplied3pRot * m;
                sApplied3pAny = true;
            }
            if (std::abs(repPushUnits) > 1e-5f) {
                // Column convention (same as the dialogue toYP/fromYP
                // extraction): 0 = forward, 1 = up, 2 = right. The push is
                // mostly backward with this shot's random sideways/vertical
                // lean folded in.
                const auto& wr = a_camera->cameraRoot->world.rotate;
                const RE::NiPoint3 fwd{ wr.entry[0][0], wr.entry[1][0], wr.entry[2][0] };
                const RE::NiPoint3 up { wr.entry[0][1], wr.entry[1][1], wr.entry[2][1] };
                const RE::NiPoint3 rgt{ wr.entry[0][2], wr.entry[1][2], wr.entry[2][2] };
                const RE::NiPoint3 push{
                    -fwd.x * repPushUnits + rgt.x * repPushSideUnits + up.x * repPushVertUnits,
                    -fwd.y * repPushUnits + rgt.y * repPushSideUnits + up.y * repPushVertUnits,
                    -fwd.z * repPushUnits + rgt.z * repPushSideUnits + up.z * repPushVertUnits,
                };
                local.translate.x += push.x;
                local.translate.y += push.y;
                local.translate.z += push.z;
                sApplied3pTrans.x += push.x;
                sApplied3pTrans.y += push.y;
                sApplied3pTrans.z += push.z;
                sApplied3pAny      = true;
            }
        }

        // CRITICAL: propagate to scene graph + rebuild view matrix so the
        // renderer actually sees our mutation. Without this the offsets are
        // invisible on screen.
        RE::NiUpdateData updateData;
        a_camera->cameraRoot->UpdateDownwardPass(updateData, 0);
        if (auto* niCam = FindNiCamera(a_camera)) {
            UpdateWorldToScreenMatrix(niCam);
        }

    }


}
