#pragma once

#include "Camera/MagicCast.h"
#include "Shouts/ShoutRegistry.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace DietDrCamera
{
    enum class CameraState
    {
        Sheathed,
        Melee,
        Blocking,
        Bow,
        Crossbow,
        Magic,
        Staves,
        Werewolf,
        // Vampire Lord sub-states. The blanket VampireLord state was split
        // into five so each gets its own profile slot. Sub-state resolution
        // happens in ResolveVampireLord (priority just below werewolf, just
        // above mounts).
        VampireLordSheathed,
        VampireLordSheathedLevitating,
        VampireLordMelee,
        VampireLordMagic,
        VampireLordConcentration,
        VampireLordFireAndForget,
        Horseback,
        DragonRiding
    };

    // Shouts can only fire in these states. Blocking interrupts the shout,
    // werewolf uses howls, vampire-lord uses its own powers, horseback and
    // dragon-riding both disallow shouting in vanilla, and the engine blocks
    // shouts while swimming or falling (sub-state conditions, not top-level).
    // UI for per-state shout profiles only exposes these six.
    inline constexpr std::array<CameraState, 6> kShoutableStates = {
        CameraState::Sheathed,
        CameraState::Melee,
        CameraState::Bow,
        CameraState::Crossbow,
        CameraState::Magic,
        CameraState::Staves,
    };

    [[nodiscard]] inline std::optional<std::size_t> ShoutableIndexFor(CameraState s)
    {
        for (std::size_t i = 0; i < kShoutableStates.size(); ++i) {
            if (kShoutableStates[i] == s) return i;
        }
        return std::nullopt;
    }

    enum class CameraSubState
    {
        None,
        Sneak,
        Sprint,
        Swimming,
        Attack,
        Shout
    };

    // What the dragon you are RIDING is doing right now.
    //
    // A separate axis from CameraSubState because it is read off a DIFFERENT
    // ACTOR — your mount, not you — and none of the ordinary sub-states
    // (sneak, sprint, swim) can occur while dragon riding anyway. The
    // vocabulary deliberately mirrors the Cinematic Effects dragon sources so
    // the two read the same way in the menu.
    //
    // Bite, Tail Attack and Wing Attack collapse into one Attack: telling
    // them apart rests on a substring match against attackData->event that
    // has never been verified (the [DRAGON] probe in CameraNoiseController
    // has never printed a line). Splitting them later is a pure append.
    //
    // ORDER IS PRIORITY, low to high — ResolveDragonAction takes the highest
    // that fires, so an action always beats a posture (a hovering dragon that
    // starts breathing resolves Breath, not Hovering).
    // It is a MATRIX, not a list: what the dragon is DOING crossed with where
    // it is doing it. A breath on the ground, a breath from a hover and a
    // breath on a strafing pass want three different camera angles — the
    // dragon's body is in a completely different relationship to the player
    // and to the target in each (user ruling 2026-08-23). The posture-only
    // rows are the idle cases of the same matrix.
    //
    // Takeoff and Landing are postures rather than actions, and they collapse
    // into Flying for the action rows: both are airborne motion, and a dragon
    // that breathes during either is doing a flying attack.
    enum class DragonAction
    {
        Cruising,          // flying, idle — the base profile and the fallback
        Perched,           // grounded or perched on a structure, idle
        Hovering,          // holding station, idle
        Takeoff,
        Landing,
        AttackGrounded,    // bite / tail / wing, or a fire-and-forget lob
        AttackHovering,
        AttackFlying,
        BreathGrounded,    // concentration breath — the one that holds
        BreathHovering,
        BreathFlying
    };

    inline constexpr std::size_t kDragonActionCount = 11;

    // What a RIDER has out. Horseback splits into Melee / Archery profiles
    // (and Archery again into zoomed), and this is the one question that
    // decides which — kept as an enum rather than three loose equipment
    // booleans so the picker cannot accidentally route a mounted MAGE onto
    // the melee profile (spell / staff resolve to None and keep the base).
    enum class RiderWeapon : std::uint8_t
    {
        None,      // sheathed, or holding something with no mounted profile
        Melee,     // includes unarmed
        Archery    // bow or crossbow
    };

    enum class MagicSchool
    {
        None,
        Alteration,
        Conjuration,
        Destruction,
        Illusion,
        Restoration
    };

    // Which hand(s) produced the active spell cast. Values 0..2 double as
    // the storage index into the per-hand override arrays (kMagicHandCount);
    // None means "no active cast" and disables per-hand routing entirely.
    // Both covers engine dual-casting (perk) AND firing the same
    // school+type from both hands at once — the profile picker treats them
    // identically ("Both Hands / Dual Cast" tab).
    enum class CastingHand : std::int8_t
    {
        None  = -1,
        Left  = 0,
        Both  = 1,
        Right = 2
    };

    // Resolved kind of the active block. Populated by ResolveBlocking when
    // CameraState::Blocking wins the priority stack (or while the player is
    // actively casting a ward). Routed by CameraController and HookManager to
    // pick the right weaponsBlocking* / tlWeaponsBlocking* slot.
    //   OneHanded — one-hand melee weapon, no shield
    //   TwoHanded — two-hand melee weapon
    //   Shield    — shield equipped (wins over OneHanded even if both)
    //   Ward      — actively casting a ward spell (Restoration Concentration
    //               that modifies the WardPower actor value); not blocking
    //               in the IsBlocking sense, but routes to Blocking state
    //               per the priority spec.
    enum class BlockKind
    {
        None,
        OneHanded,
        TwoHanded,
        Shield,
        Ward
    };

    class StateResolver :
        public RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent>,
        public RE::BSTEventSink<RE::TESEquipEvent>,
        public RE::BSTEventSink<SKSE::ActionEvent>,
        public RE::BSTEventSink<RE::BSAnimationGraphEvent>
    {
    public:
        [[nodiscard]] static StateResolver& GetSingleton();

        // NOTE on Attack Lag: the routing getters below (state, sub-state,
        // school, cast type, casting hand, power-attack flag/direction) report
        // the HELD attack snapshot while an Attack Lag hold is active — a
        // user-configured delay on the attack→base transition (Cinematic
        // Effects). The hold is only ever active in third person, so 1p
        // consumers always see live values. See UpdateAttackLag.
        // MENU STATE HOLD (generalized from the old QT shout-only hold,
        // user ruling 2026-08-16: "it should always maintain whatever is
        // being done when the menu or quick tune menu is open"): while the
        // mod control panel or Quick Tune is open, every reported output
        // below freezes at its open-moment value, so tuning an entry keeps
        // that entry's framing live for as long as the edit takes. The
        // underlying state machine keeps running truthfully (timers,
        // watchdogs, event latches untouched — same design as the
        // attack-lag hold); only the GETTERS consult the snapshot. Both
        // menus block game input, so nothing new can start mid-hold —
        // snapshot-at-open IS "whatever was being done". See
        // RequestMenuStateHold / UpdateMenuStateHold.
        [[nodiscard]] CameraState    GetState() const
        {
            if (menuHoldActive) return menuHold.state;
            return attackLagHolding ? attackLagHeld.state : state;
        }
        [[nodiscard]] CameraState GetLiveState() const { return state; }
        [[nodiscard]] CameraSubState GetSubState() const {
            if (menuHoldActive) return menuHold.sub;
            return attackLagHolding ? attackLagHeld.sub : subState;
        }
        [[nodiscard]] MagicSchool    GetSchool() const
        {
            if (menuHoldActive) return menuHold.school;
            return attackLagHolding ? attackLagHeld.school : school;
        }
        [[nodiscard]] CastType       GetCastType() const
        {
            if (menuHoldActive) return menuHold.cast;
            return attackLagHolding ? attackLagHeld.cast : castType;
        }
        // Ritual is a profile category. A master staff can still be a native
        // concentration beam, which must keep continuous effects and no shot lag.
        [[nodiscard]] bool IsConcentrationCast() const
        {
            if (menuHoldActive) return menuHold.concentration;
            return attackLagHolding ? attackLagHeld.concentration : IsLiveConcentrationCast();
        }
        // Hand(s) behind the active spell cast. Tracks the winning
        // (type, school) classification, freezes through the cast linger
        // alongside cachedCastType/cachedCastSchool, and reverts to None
        // when no cast is live. Used by the per-hand magic overrides.
        [[nodiscard]] CastingHand    GetCastingHand() const
        {
            if (menuHoldActive) return menuHold.hand;
            return attackLagHolding ? attackLagHeld.hand : cachedCastHand;
        }
        [[nodiscard]] BlockKind      GetBlockKind() const
        {
            return menuHoldActive ? menuHold.block : blockKind;
        }
        // Eagle Eye (or any mod's equivalent) is holding the bow zoom.
        //
        // Read straight off PlayerCamera::bowZoomedIn, which the engine's own
        // BowZoom anim-event handler sets — NOT from the perk. Mods that hand
        // the zoom out at level 1, or move it behind a different perk, fire
        // the same event and so drive this identically
        // ([[compatibility-first-design]]). Only meaningful while a bow or
        // crossbow is drawn; false otherwise.
        [[nodiscard]] bool           IsBowZoomed() const
        {
            if (menuHoldActive) return menuHold.bowZoomed;
            return attackLagHolding ? attackLagHeld.bowZoom : isBowZoomed;
        }
        static constexpr std::size_t kGraphRing = 24;
        void RecordGraphEventForFreeze(std::string_view a_tag);
        [[nodiscard]] std::size_t GetRecentGraphEvents(
            std::array<std::string, kGraphRing>& a_out, std::size_t& a_dropped) const;

        // Drives the Horseback Melee / Archery split. Sheathed reports None,
        // so a rider with the weapon away keeps the plain Horseback framing.
        [[nodiscard]] RiderWeapon    GetRiderWeapon() const
        {
            if (menuHoldActive) return menuHold.rider;
            if (!isWeaponDrawn) return RiderWeapon::None;
            if (isBowEquipped || isCrossbowEquipped) return RiderWeapon::Archery;
            if (isMeleeEquipped)                     return RiderWeapon::Melee;
            return RiderWeapon::None;   // staff / spell — no mounted slot
        }
        // Which side a mounted melee swing is going to. None whenever the
        // rider is not mid-swing, so the plain Horseback Melee framing covers
        // everything between attacks.
        //
        // THE SIGNAL IS THE ATTACK HAND, not anything camera-relative.
        // Mounted, the trigger you press picks the side the weapon swings —
        // left trigger, left side — even with a two-hander. Read from
        // BGSAttackData::IsLeftAttack() on the engine's live attack data.
        //
        // TWO AIM-BASED GUESSES WERE MEASURED AND KILLED FIRST, so don't
        // reach for them again: `player.angle.z - horse.angle.z` reads -0.0
        // on every swing (mounted, the rider's body angle is locked to the
        // horse), and `freeRotation.x` reads under ONE DEGREE on every swing
        // (the camera is locked to the horse too), so a sign test on it calls
        // everything Left. Nothing about where you are LOOKING distinguishes
        // the two attacks.
        //
        // LATCHED on the swing's first frame ([[attack-variant-modifiers-must-be-latched]]):
        // attackData is rebuilt through the swing, and a side that flips
        // mid-animation would swap framing halfway through the hit.
        enum class MountAttackSide : std::uint8_t { None, Left, Right };
        [[nodiscard]] MountAttackSide GetMountAttackSide() const
        {
            if (menuHoldActive) return menuHold.mountSide;
            return mountAttackSide;
        }

        [[nodiscard]] bool           IsWerewolf() const    { return isWerewolf; }
        [[nodiscard]] bool           IsVampireLord() const { return isVampireLord; }

        // The ridden dragon's current action. Meaningless unless GetState()
        // is DragonRiding — Cruising is the resting value. Freezes under the
        // menu hold like every other getter, so tuning a Breath framing while
        // the panel is open keeps the breath framing live.
        [[nodiscard]] DragonAction   GetDragonAction() const
        {
            return menuHoldActive ? menuHold.dragon : dragonAction;
        }

        // Skyrim's Paraglider (mod support). Latched from the paraglide
        // ANIMATIONS' own annotations in the BSAnimationGraphEvent sink
        // (ParaglideEquip / Parachute / Para_EquipOut to start — airborne
        // only — and ParaglideUnequip / Para_UnequipOut / ParaDummy to end).
        // NOT the plugin's NotifyAnimationGraph("StartPara"/"EndPara"): those
        // are inputs INTO the graph and were log-proven never to reach the
        // sink. A per-frame grounded/swimming net in PollParaglide mirrors
        // the mod's own force-end (it clears its state on kOnGround), so an
        // exit that emits nothing can't strand the camera. With the
        // paraglider absent the tags never exist and this is never true.
        [[nodiscard]] bool IsParagliding() const
        {
            return menuHoldActive ? menuHold.paragliding : isParagliding;
        }

        // Shout Lag hand-off (see activeShoutLagSec in the private section):
        // CameraController publishes the resolved shout entry's Lag every 3p
        // frame while the Shout sub-state owns the camera.
        void SetActiveShoutLag(float a_sec) { activeShoutLagSec = a_sec; }

        // True while the shout linger is running on LAG ALONE — the natural
        // tail has already elapsed and only the per-entry Lag is still
        // holding the Shout sub-state. Same contract as
        // IsAttackLagHolding: consumers that must follow the LIVE state
        // (camera NOISE — Lag is scoped to the framing trees only) bypass
        // the held routing while this is true, so a shout's noise still
        // ends on its own Fade Duration no matter how long the camera holds.
        [[nodiscard]] bool IsShoutLagHolding() const { return shoutLagHolding; }

        // Werewolf feeding (2026-08-15). Latched from the vanilla feed magic
        // effect — archetype kWerewolfFeed, a record property, so any mod's
        // feed drives it too (compatibility-first; the effect lands on the
        // CORPSE with the player as caster). The feed idle has no clean end
        // signal, so PollWerewolfFeed releases by stillness-net: real
        // movement after a minimum hold, leaving beast form, or a hard
        // timeout. Drives the Werewolf Feeding camera entries (Third Person
        // + Target Lock).
        [[nodiscard]] bool IsWerewolfFeeding() const
        {
            return menuHoldActive ? menuHold.wwFeeding : isWerewolfFeeding;
        }
        void NotifyWerewolfFeed(RE::FormID a_effect = 0);
        // Every graph tag while a feed is latched (from the noise controller's
        // graph sink): logged, and an idle-end tag releases the feed.
        void NotifyWerewolfFeedGraphTag(const char* a_tag);

        // True while an Attack Lag hold is masking the attack→base transition.
        // Exposed so consumers that must follow the LIVE state (camera noise —
        // the lag is scoped to the Categories/Target Lock framing trees only)
        // can bypass the held routing. While true, the live resolution is
        // guaranteed to be GetState()'s PLAIN base (held and live share the
        // top-level state; sub None, no cast, no power attack, not sneaking).
        [[nodiscard]] bool IsAttackLagHolding() const { return attackLagHolding; }

        // The fire-and-forget cast linger, exposed for the same reason as
        // IsAttackLagHolding: it is a FRAMING device (it keeps the camera
        // parked on the cast profile while the projectile flies), and the
        // noise inherits it only because the resolved state keeps reporting
        // FireAndForget for the window. IsCastLingering is true from the
        // RELEASE (the trigger edge) to the linger's commit; session means the
        // player is provably rapid-firing (a new cast began during a linger).
        // IsLiveCasting is the LIVE caster state this frame, before the linger
        // freezes anything — false the moment the spell leaves the hand, true
        // again the moment the next charge begins (or a concentration stream
        // is running in the other hand).
        [[nodiscard]] bool IsCastLingering() const    { return castLingering; }
        [[nodiscard]] bool IsCastSessionActive() const { return castSessionActive; }
        [[nodiscard]] bool IsLiveCasting() const       { return castLiveCasting; }

        // True when the active hand cast should lock the camera (suppress
        // free-rotation). Ritual spells are always locked; Lightning Storm
        // is locked only during the windup (kCharging) and becomes free
        // once the charge completes. Used by the camera hook to preserve
        // vanilla lockdown behavior without discarding the rotation offset.
        [[nodiscard]] bool IsCastLocked() const { return isCastLocked; }

        // Live sneak flag. Exposed so the camera routing can pick combo
        // profiles (e.g. Bow Sneak Drawing) when the resolved sub-state
        // would otherwise mask the sneak signal — Attack outranks Sneak in
        // ResolveSubState, so a sneaking archer's draw resolves as Attack
        // and the routing layer needs the raw flag to disambiguate.
        [[nodiscard]] bool IsSneaking() const
        {
            return menuHoldActive ? menuHold.sneaking : isSneaking;
        }

        // Live sprint flag. Like IsSneaking, exposed so the routing layer can
        // pick sprint-attack combo profiles when Attack masks Sprint in the
        // sub-state priority stack.
        [[nodiscard]] bool IsSprinting() const
        {
            return menuHoldActive ? menuHold.sprinting : isSprinting;
        }

        // Sprint/sneak AS OF THE SWING THAT IS PLAYING — use these, not the
        // live flags above, to pick an attack-variant profile.
        //
        // A swing's KIND is decided when it starts and does not change
        // halfway through. The live sprint flag does: vanilla clears it
        // within ~200ms of a sprint power attack while the animation is
        // still running, so a routing layer reading IsSprinting() fell from
        // sprint_power_attack to power_attack MID-SWING and the noise/camera
        // saw a spurious profile swap in the middle of one player action.
        // Latched at every new-swing site alongside powerAttackDir, which is
        // captured for exactly the same reason. Outside an attack these fall
        // through to the live flags, so non-attack callers are unaffected.
        [[nodiscard]] bool IsAttackSprint() const {
            if (menuHoldActive) return menuHold.attackSprint;
            return isAttacking ? attackSprintLatched : isSprinting;
        }
        [[nodiscard]] bool IsAttackSneak() const {
            if (menuHoldActive) return menuHold.attackSneak;
            return isAttacking ? attackSneakLatched : isSneaking;
        }

        // True while the menu state hold is engaged (any DDC menu open).
        // The FP fast-attack gate defers to it — the hold's whole job is
        // to keep the state the user opened the menu in on screen, and a
        // live engine gate would strip the attack key out from under an
        // open editor within 150ms.
        [[nodiscard]] bool IsMenuHoldActive() const { return menuHoldActive; }

        // The graph's per-swing KIND announcement, raw. The start tag names
        // the swing ("MCO_PowerAttackInitiate" vs a plain attack start) at
        // WINDUP START — long before the engine latches kPowerAttack at
        // release — and the counter lets a caller tell a fresh
        // announcement from a stale one. The FP fast gate uses this to
        // resolve the windup ambiguity in both directions (a power windup
        // and a light windup are otherwise identical: kDraw, flag down).
        // Graphs that never tag (the blind path) never advance the
        // counter, and callers fall back to flag-and-state proof.
        [[nodiscard]] bool GetAttackSwingPowerHint(std::uint32_t& a_counter) const {
            a_counter = attackAnimPowerCounter.load(std::memory_order_relaxed);
            return attackAnimPowerHint.load(std::memory_order_relaxed);
        }

        // Contact callbacks need the live swing, before camera/menu holds or
        // linger can preserve a previous attack. Closed brackets cannot lend
        // their last power hint to a later hit.
        [[nodiscard]] bool HasLivePowerAttackHint() const {
            return attackAnimBracketOpen.load(std::memory_order_relaxed) &&
                   attackAnimPowerHint.load(std::memory_order_relaxed);
        }

        // Live weapon-drawn flag. Exposed so the routing layer can split the
        // Werewolf base into Sheathed (claws in) vs Unsheathed (claws out).
        [[nodiscard]] bool IsWeaponDrawn() const
        {
            return menuHoldActive ? menuHold.weaponDrawn : isWeaponDrawn;
        }

        // True when the player Vampire Lord is levitating (hovering), from the
        // vanilla DLC1VampireLevitateStateGlobal. Set during ResolveVampireLord.
        // Used to split VL Sprinting into Ground vs Levitating.
        [[nodiscard]] bool IsVampireLordLevitating() const
        {
            return menuHoldActive ? menuHold.vlLevitating : isVampireLordLevitating;
        }

        // True when the current attack carries the engine's kPowerAttack flag
        // (BGSAttackData on the player's high process — universal across
        // vanilla/MCO/BFCO). Held through the attack linger, cleared on end.
        // The routing layer uses this to pick power-attack profile variants;
        // vanilla sprint attacks set it (sprint power attack), MCO/BFCO normal
        // sprint attacks don't (sprint attack).
        [[nodiscard]] bool IsPowerAttacking() const
        {
            if (menuHoldActive) return menuHold.powerAttacking;
            return attackLagHolding ? attackLagHeld.pa : isPowerAttack;
        }

        // Directional classification of the current power attack. Captured at
        // the rising edge of kPowerAttack and held across the swing + linger.
        // Hybrid signal: BGSAttackData::event substring (vanilla embeds the
        // direction in the event name) with ActorState1 movement-flag
        // fallback (MCO/BFCO/SkySA emit flattened events; their movesets pick
        // direction via OAR conditions on the same input flags). See
        // [[power-attack-direction-classification]] for details.
        enum class PowerAttackDirection : std::uint8_t {
            InPlace = 0,
            Forward = 1,
            Back    = 2,
            Left    = 3,
            Right   = 4,
        };
        static constexpr std::size_t kPowerAttackDirectionCount = 5;
        [[nodiscard]] PowerAttackDirection GetPowerAttackDirection() const
        {
            if (menuHoldActive) return menuHold.paDir;
            return attackLagHolding ? attackLagHeld.paDir : powerAttackDir;
        }


        // Orthogonal to sub-state: true whenever TDM reports a target lock.
        // The profile picker uses this to route to the target-lock profile
        // tree while the sub-state still resolves normally (sprint/sneak/
        // attack/swim/shout all compose with lock).
        [[nodiscard]] bool IsTargetLocked() const
        {
            return menuHoldActive ? menuHold.locked : isTargetLocked;
        }

        // Exposed for CameraNoiseController's per-state gating.
        //   IsChargingSpell:    player is holding a fire-and-forget / ritual
        //                       cast mid-charge (button held down, spell
        //                       building up). Maps to CastType::FireAndForget
        //                       or CastType::Ritual while hand caster is
        //                       active.
        //   IsCastingStream:    player is firing a concentration spell
        //                       (continuous stream — destruction, healing).
        //                       Maps to CastType::Concentration while hand
        //                       caster is active.
        [[nodiscard]] bool IsChargingSpell() const
        {
            if (menuHoldActive) return menuHold.chargingSpell;
            return isHandCasterActive &&
                   (cachedCastType == CastType::FireAndForget || cachedCastType == CastType::Ritual);
        }
        [[nodiscard]] bool IsCastingStream() const
        {
            if (menuHoldActive) return menuHold.castingStream;
            return isHandCasterActive && cachedCastType == CastType::Concentration;
        }

        // ShoutId of the currently-active shout, resolved from the
        // player's equipped TESShout at BeginCastVoice. Nullopt when no
        // shout is active or when the shout isn't one of the known 27
        // (e.g. mod-added shouts). Cleared on shoutstop / linger expiry.
        [[nodiscard]] std::optional<ShoutId> GetActiveShoutId() const {
            if (menuHoldActive && menuHold.shoutId) return menuHold.shoutId;
            return activeShoutId;
        }

        // Identity of the active MOD-ADDED shout — a TESShout kVoiceCast
        // delivered that isn't one of the known 27 (ShoutRegistry miss).
        // Local form id + source plugin filename, the same scheme
        // WeaponBinding uses, so the camera/noise/1p resolvers can match
        // it against a Shout binding. Same lifecycle as activeShoutId
        // (set at kVoiceCast, cleared on linger expiry / lock-off reset),
        // and honours the Quick Tune shout hold. Returns false when no
        // mod-added shout is active.
        [[nodiscard]] bool GetActiveModShout(std::uint32_t& a_outFormID,
                                             std::string&   a_outPlugin) const {
            if (menuHoldActive) {
                if (menuHold.modShoutFormID == 0) return false;
                a_outFormID = menuHold.modShoutFormID;
                a_outPlugin = menuHold.modShoutPlugin;
                return true;
            }
            if (activeModShoutFormID == 0 || activeModShoutPlugin.empty()) return false;
            a_outFormID = activeModShoutFormID;
            a_outPlugin = activeModShoutPlugin;
            return true;
        }

        // MenuUI calls this every frame EITHER the mod control panel or the
        // Quick Tune overlay is open. First request snapshots the full
        // reported state (see the getter block comment); requests keep the
        // snapshot live; when they stop (menus closed) the short TTL lapses
        // and normal resolution resumes with a stateChanged notification.
        void RequestMenuStateHold();

        // Steady-clock timestamp of the most recent BeginCastVoice
        // animation event — i.e. the start of the active shout windup.
        // Used by the 1p shouts noise envelope to compute elapsed time
        // since the wind-up began (which is consistent across 1/2/3
        // word holds — each new shout press resets it). Default-
        // constructed when no shout has fired this session.
        [[nodiscard]] std::chrono::steady_clock::time_point GetShoutStartTime() const {
            return shoutStartTime;
        }
        // Steady-clock timestamp of the most recent Voice_SpellFire_Event
        // — i.e. the actual frame the voice line fires (separate from
        // BeginCastVoice which is the wind-up start). Equal to the
        // default-constructed time_point if no voice has fired yet
        // during the current shout.
        [[nodiscard]] std::chrono::steady_clock::time_point GetShoutFireTime() const {
            return shoutFireTime;
        }
        // Words actually spent on the active shout: 0 while the count is
        // still unknown (wind-up in progress), otherwise 1, 2 or 3. Comes
        // from the engine's own shout variation, so it is correct on every
        // graph — including the ones that emit no Voice_SpellFire_Event and
        // therefore left the noise envelopes guessing from hold time.
        [[nodiscard]] int GetShoutWordCount() const { return shoutWordCount; }

        void Register();
        void Update(RE::PlayerCharacter* a_player);

        RE::BSEventNotifyControl ProcessEvent(const RE::TESSwitchRaceCompleteEvent* a_event,
                                              RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event,
                                              RE::BSTEventSource<RE::TESEquipEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event,
                                              RE::BSTEventSource<SKSE::ActionEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                              RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override;

    private:
        StateResolver() = default;

        void EnsureAnimGraphSubscription(RE::PlayerCharacter* a_player);

        // Per-frame polls. Each compares the live game value against its cached
        // member, updates the cache on change, logs the transition, and sets
        // stateChanged = true so the next Update() re-runs resolution.
        void PollRace(RE::PlayerCharacter* a_player);
        void PollEquipment(RE::PlayerCharacter* a_player);
        void PollDragonRiding();
        // Reads the RIDDEN dragon (the player's mount), not the nearest one.
        // Only runs while isDragonRiding; resets to Cruising when it ends.
        void PollDragonAction(RE::PlayerCharacter* a_player);
        void PollHorseback(RE::PlayerCharacter* a_player);
        void PollWeaponDrawn(RE::PlayerCharacter* a_player);
        void PollHandCaster(RE::PlayerCharacter* a_player);
        void PollSneak(RE::PlayerCharacter* a_player);
        void PollParaglide(RE::PlayerCharacter* a_player);
        void PollSprint(RE::PlayerCharacter* a_player);
        void PollSwimming(RE::PlayerCharacter* a_player);
        void PollAttack(RE::PlayerCharacter* a_player);
        void PollBlocking(RE::PlayerCharacter* a_player);

        // Priority-stack resolution. Each Resolve* returns true if it claimed
        // the state (caller stops walking the stack). ResolveSheathed is the
        // terminal fallback and never returns false.
        bool ResolveTransformation();
        bool ResolveVampireLord();
        bool ResolveMount();
        bool ResolveBlocking();
        bool ResolveWeapon();
        void ResolveSheathed();

        // Sub-state precedence is the same regardless of which top-level
        // resolver wins (Shout > Sneak > None).
        [[nodiscard]] CameraSubState ResolveSubState() const;

        CameraState    state    = CameraState::Sheathed;
        CameraSubState subState = CameraSubState::None;
        MagicSchool    school   = MagicSchool::None;
        CastType       castType = CastType::None;
        BlockKind      blockKind = BlockKind::None;

        bool isWerewolf      = false;
        bool isVampireLord   = false;
        bool isVampireLordLevitating = false; // VL hovering (DLC1VampireLevitateStateGlobal)
        bool isHorseback     = false;
        bool isDragonRiding  = false;
        // Ridden-dragon action + the dwell that stops it flickering. A camera
        // profile that switches for 200ms and switches back is nausea, so a
        // resolved action HOLDS for kDragonActionDwell before anything of
        // lower priority may replace it. Higher priority preempts instantly —
        // a breath starting mid-hover must not wait.
        DragonAction dragonAction        = DragonAction::Cruising;
        DragonAction dragonActionPending = DragonAction::Cruising;
        float        dragonActionHeldFor = 0.0f;
        bool isWeaponDrawn     = false;
        bool isBowZoomed       = false;   // PlayerCamera::bowZoomedIn, polled
        MountAttackSide mountAttackSide = MountAttackSide::None;
        static constexpr std::size_t kGraphTagLen = 48;
        struct FreezeGraphEvent
        {
            std::array<char, kGraphTagLen> tag{};
            std::chrono::steady_clock::time_point time{};
        };
        std::array<FreezeGraphEvent, kGraphRing> graphRing{};
        std::size_t graphRingN = 0;
        mutable std::mutex graphRingMutex;
        std::atomic<std::size_t> graphRingDropped{0};
        bool isMeleeEquipped   = true; // unarmed counts as melee — default at load
        bool isBowEquipped      = false;
        bool isCrossbowEquipped = false;
        bool isStaffEquipped   = false;
        bool isSpellEquipped   = false;
        // Block-routing equipment flags. Updated by PollEquipment alongside
        // the other isXEquipped flags so ResolveBlocking can pick a kind.
        bool isShieldEquipped     = false;
        bool isOneHandedEquipped  = false; // 1H melee in either hand
        bool isTwoHandedEquipped  = false; // 2H melee in main hand
        // Live ward-casting flag — true while any hand caster is actively
        // casting a Restoration spell whose effects modify WardPower.
        // Computed in PollHandCaster.
        bool isCastingWard        = false;
        bool isHandCasterActive = false; // any of player's magic casters in non-idle state (covers staff fires AND spell casts)
        bool isSneaking         = false;
        bool isSprinting        = false;
        // Sprint-jump carry: primed from the live sprint flag while grounded;
        // while airborne it holds isSprinting true so a sprint-takeoff jump
        // (Better Jumping) keeps the Sprint sub-state through the leap instead
        // of bouncing base→sprint across takeoff/landing. See PollSprint.
        bool sprintAirCarry     = false;
        bool isSwimming         = false;
        // Skyrim's Paraglider glide state — see IsParagliding().
        bool isParagliding      = false;
        // Werewolf feeding — see IsWerewolfFeeding(). Latched by
        // NotifyWerewolfFeed (the kWerewolfFeed magic-effect apply, relayed
        // from CameraNoiseController's sink), released by the stillness net
        // in PollWerewolfFeed.
        bool isWerewolfFeeding = false;
        std::chrono::steady_clock::time_point feedStartTp{};
        // The kWerewolfFeed effect that latched the feed (2026-09-05): while
        // it is still on the player the feed is live; the frame it is gone
        // the feed is over, standing still or not.
        RE::FormID feedEffectId = 0;
        int        feedNotifies = 0;
        // 2026-09-05 12:33 log: the effect is applied once and is off the
        // player within 0.5 s, so it cannot be the end signal. The feed is an
        // idle animation; the animation is the signal. bAnimationDriven's
        // falling edge and the idle-end tags both release, and a status line
        // during the feed records what the graph looked like.
        bool       feedAnimDrivenSeen = false;
        std::chrono::steady_clock::time_point feedLastStatusTp{};
        void PollWerewolfFeed(RE::PlayerCharacter* a_player);
        bool isAttacking        = false;
        bool isPowerAttack      = false; // BGSAttackData kPowerAttack, held through attack linger
        PowerAttackDirection powerAttackDir = PowerAttackDirection::InPlace; // captured at PA rising edge / combo flip
        bool attackSprintLatched = false; // sprint/sneak as of the swing that is
        bool attackSneakLatched  = false; // playing; see IsAttackSprint above
        bool isBlocking         = false; // IsBlocking graph var OR active bash (BGSAttackData::kBashAttack)
        bool isShouting         = false;
        bool isTargetLocked     = false; // polled from TDM API each frame
        // Last animation-graph manager we attached our event sink to. The
        // player's graph manager is swapped wholesale on transformation AND
        // when a behaviour framework (Nemesis/Pandora) reloads the player
        // graph; a sink on the old manager then dangles and shout tags stop
        // arriving. Re-subscribe whenever this pointer changes (see
        // EnsureAnimGraphSubscription) instead of latching once.
        void* lastAnimGraphMgr = nullptr;

        // Set by polls and event handlers when any cached input changes;
        // gates the resolution pass in Update(). Initial true so the very
        // first Update() runs resolution against the seeded state.
        bool stateChanged = true;

        // Cached school of the player's currently-equipped spell (right hand
        // primary, left hand fallback). Populated on equip event; consumed by
        // Update() when entering a Magic state.
        MagicSchool spellSchool = MagicSchool::None;

        // Live active cast type, derived per-frame by PollHandCaster from
        // the active hand caster's currentSpell. Reflects the priority-
        // resolved type across both hands (FAF > Concentration); reverts
        // to None when no hand caster is active.
        CastType cachedCastType = CastType::None;

        // Live active cast school, paired with cachedCastType. Captures the
        // school of whichever cast wins priority resolution — so dual-
        // wielding Destruction + Restoration with the Destruction casting
        // gives Destruction here, not the primary-hand fallback that
        // spellSchool stores. Used by ResolveWeapon's spell branches when
        // isHandCasterActive is true.
        MagicSchool cachedCastSchool = MagicSchool::None;

        // Hand(s) that produced the winning (type, school) pair above.
        // Updated and frozen in lockstep with cachedCastType/School (same
        // linger rules) so the three always describe one coherent cast.
        CastingHand cachedCastHand = CastingHand::None;
        // True from the moment any frame of the current cast reports a dual
        // cast until that cast ends. Both engine dual-cast signals are edges
        // that clear mid-cast, so this is what keeps the per-hand attribution
        // on Both for the whole stream. See PollHandCaster's dual latch.
        bool        castDualLatched = false;

        // Staff fire detection — separate from spell cast fields so a
        // staff+spell dual-wield can track both independently.
        CastType    cachedStaffCastType   = CastType::None;
        bool        cachedStaffConcentration = false;
        MagicSchool cachedStaffCastSchool = MagicSchool::None;

        [[nodiscard]] bool IsLiveConcentrationCast() const
        {
            return castType == CastType::Concentration ||
                   (state == CameraState::Staves && cachedStaffConcentration);
        }

        // Camera lockdown flag — set per-frame by PollHandCaster when the
        // active spell cast should prevent free-look (vanilla ritual
        // windup, Lightning Storm windup). Consumed by HookManager.
        bool isCastLocked = false;

        // Spell-cast linger: when a hand cast ends, hold the cached cast
        // values for a short window before reverting. Smooths the camera
        // transition off a cast so releasing the trigger doesn't snap
        // back instantly. Staff fires are excluded — button-driven, not
        // charge-and-release.
        //
        // castSessionActive toggles on when a new cast starts *during*
        // a linger (proof the player is rapid-firing). While active, the
        // linger uses the longer window so inter-cast gaps stay on the
        // cast profile instead of flickering back to base. Resets on
        // linger commit (session definitely over).
        std::chrono::steady_clock::time_point castEndTime{};
        bool castLingering      = false;
        bool castSessionActive  = false;
        // The LIVE caster state this frame, published before the linger
        // freezes the cached cast fields. See IsLiveCasting().
        bool castLiveCasting    = false;

        // ===== Attack Lag (Cinematic Effects) =====
        // User-configured hold on the attack→base transition (per category:
        // melee / magic / archery; seconds; 0 = off). When the effective
        // output transitions from an attack condition to the SAME state's
        // plain base (sub None, no cast, no power attack), the routing
        // getters keep reporting the frozen attack snapshot until the timer
        // expires. Any OTHER fresh resolution (sprint, sneak, block, sheathe,
        // state change, new attack, shout) cancels the hold and commits
        // immediately — only the return-to-base is delayed. Third person
        // only: the hold neither starts nor survives in 1p, so 1p noise/FOV
        // routing always sees live values. Implemented as a getter-level
        // overlay so the resolver's internal caches and edge detection are
        // never touched. Evaluated every frame by UpdateAttackLag (end of
        // Update, after the resolution gate).
        struct AttackLagSnap {
            CameraState          state  = CameraState::Sheathed;
            CameraSubState       sub    = CameraSubState::None;
            MagicSchool          school = MagicSchool::None;
            CastType             cast   = CastType::None;
            CastingHand          hand   = CastingHand::None;
            bool                 pa     = false;
            PowerAttackDirection paDir  = PowerAttackDirection::InPlace;
            // The engine drops bowZoomedIn the instant the arrow leaves, so
            // without this the zoom framing snapped away on release no matter
            // what Projectile Lag was set to — the hold kept the STATE but the
            // picker read a live-false zoom flag and fell to the draw profile.
            // A lag exists precisely so the shot framing outlives the shot.
            bool                 bowZoom = false;
            bool                 concentration = false;
        };
        void UpdateAttackLag();
        bool attackLagHolding = false;
        std::chrono::steady_clock::time_point attackLagUntil{};
        AttackLagSnap attackLagHeld{};
        // Previous frame's EFFECTIVE (post-hold) outputs — the attack→base
        // edge is detected against what consumers were actually seeing.
        AttackLagSnap attackLagPrevEff{};

        // Per-hand release tracking. The combined cast-type view
        // collapses both hands into one classification, which hides the
        // dual-cast case: when one hand releases F&F while the other is
        // still F&F, cachedCastType stays put and the standard "F&F →
        // non-F&F" trigger never fires. We instead record release
        // edges per hand and prime castSessionActive when the upcoming
        // linger starts, so casting Firebolt right-then-left counts as
        // a session just like firing twice from one hand.
        bool lastLeftHandFF  = false;
        bool lastRightHandFF = false;
        // Sticky-within-cycle "saw firing state" per hand. Set true any
        // frame the caster's state is kReady or kCasting (i.e., the
        // spell actually committed to firing) while the F&F flag is
        // up; cleared on the falling edge. Lets us tell a real cast
        // (kCharging → kCasting → kNone) from a tap-cancel
        // (kCharging → kNone, never reaches firing). Only real casts
        // start a linger.
        bool leftSawFiring  = false;
        bool rightSawFiring = false;
        std::chrono::steady_clock::time_point lastFFReleaseTime{};
        bool castSessionPending = false;

        // Attack exit linger — same pattern as cast linger, tuned for
        // combo swings. Holds isAttacking true for a window after
        // IsAttacking drops, so the camera stays on the Attack sub-
        // state profile through the recovery gap between swings. On a
        // new swing during linger, attackSessionActive flips on and
        // subsequent linger windows use the longer duration to cover
        // sustained combo tempo.
        std::chrono::steady_clock::time_point attackEndTime{};
        bool attackLingering     = false;
        bool attackSessionActive = false;
        // The werewolf howl's own animation emits MeleeStart, so an attack
        // session that begins while the werewolf is mid-shout belongs to the
        // HOWL, not to a swing. Remembered so the shout-end commit can end it
        // together with the roar — its recovery outlives the shout linger, and
        // the leftover sub=Attack window eased the camera toward the werewolf
        // Attack profile and back between every roar and the base state.
        bool attackBeganDuringHowl = false;
        // Set when that force-end fires while the graph's IsAttacking is still
        // true; suppresses the attack rising edge until it drops, so the same
        // graph session can't restart the attack we just ended.
        bool howlAttackSwallow     = false;

        // ----- Animation-bracketed attack window -----
        // "IsAttacking" is a BEHAVIOUR GRAPH variable, and every modern
        // moveset framework (MCO / BFCO / SkySA) raises it at the ATTACK
        // WINDOW — the hit-frame region — not across the animation. So the
        // graph var starts late (the whole wind-up plays with no Attack
        // sub-state) and drops early (the recovery plays with none either),
        // and the fixed 150/500 ms linger then tacked an arbitrary tail on
        // the far end. Net effect: the attack camera was offset from the
        // attack it was framing.
        //
        // Graphs also emit an explicit START/STOP bracket around the whole
        // animation. The naming varies by framework, so ProcessEvent
        // substring-matches the vanilla vocabulary that every Nemesis /
        // Pandora patch annotates rather than replaces — see the
        // compatibility contract in the sink for the exact rules.
        //
        // This bracket is OPTIONAL, not assumed. It has to prove itself
        // (attackAnimBracketProven: one complete open→close cycle) before it
        // is allowed to influence anything, and it is permanently abandoned
        // on a graph where the watchdog has to force a bracket shut
        // (attackAnimDistrust). Absent or withdrawn, PollAttack behaves
        // exactly as it did before: IsAttacking plus the 150/500 ms linger.
        // A user on vanilla animations, an unpatched graph, or a framework
        // we have never heard of therefore loses nothing.
        //
        // Written from the animation thread (ProcessEvent), read from the
        // main thread (PollAttack) — hence atomics. The start COUNTER
        // exists so a swing whose whole bracket opens and closes between
        // two polls still registers.
        std::atomic<bool>          attackAnimBracketOpen{ false };
        std::atomic<bool>          attackAnimSawStart{ false };
        std::atomic<bool>          attackAnimBracketProven{ false };
        // Which KIND of swing the most recent start tag announced, and a
        // counter so PollAttack can tell a fresh announcement from a stale
        // one. The tag names the swing ("SBF_PowerAttackStart" vs
        // "SBF_NormalAttackStart"), which is information attackData does not
        // reliably carry inside a combo — see PollAttack.
        std::atomic<bool>          attackAnimPowerHint{ false };
        std::atomic<std::uint32_t> attackAnimPowerCounter{ 0 };
        std::uint32_t              lastAttackAnimPowerCounter = 0;
        // Set once a start tag has decided the power flag for the current
        // swing. While it holds, attackData is not allowed to overrule that
        // decision — otherwise the two fight every frame. Cleared when the
        // attack session ends.
        bool                       powerFlagFromGraph = false;
        std::atomic<bool>          attackAnimDistrust{ false };
        std::atomic<std::uint32_t> attackAnimStartCounter{ 0 };
        std::uint32_t              attackAnimStartSeen = 0;
        // How long the bracket has been open with the engine insisting no
        // attack is happening, for the stuck-bracket watchdog.
        std::chrono::steady_clock::time_point attackAnimOpenedAt{};
        bool                                  attackAnimOpenObserved = false;

        // ----- Projectile-launch counters (Attack Lag gating) -----
        // Attack Lag is scoped to attacks that actually LAUNCH something, so
        // it needs a real release signal rather than "the attack state
        // ended". Both are bumped from the animation-graph sink.
        //   magic  — SKSE kSpellFire, plus MLh_/MRh_SpellFire_Event from the
        //            graph (any tag containing "spellfire"; "voice" excluded,
        //            that's the shout fire, not a hand cast).
        //            BOTH sources are filtered through
        //            MagicItemLaunchesProjectile: a "spell fired" signal is
        //            not a "projectile launched" signal — Oakflesh, Healing,
        //            Bound Sword and every other self/touch cast used to bump
        //            these and arm a lag with nothing in flight (user report
        //            2026-08-16). The SKSE path checks the fired spell
        //            directly (sourceForm); the graph path checks the caster
        //            for the hand the tag names.
        //   arrow  — arrowRelease AND arrowDetach. This graph only emits
        //            arrowDetach, so matching arrowRelease alone finds
        //            nothing. No filter needed: an arrow IS the projectile.
        std::atomic<std::uint32_t> projectileMagicCounter{ 0 };
        std::atomic<std::uint32_t> projectileArrowCounter{ 0 };
        std::uint32_t              projectileMagicSeen = 0;
        std::uint32_t              projectileArrowSeen = 0;
        // Latched when a projectile launches during the current attack;
        // consumed and cleared when the lag hold arms (or the attack ends
        // without one, in which case no hold arms at all).
        bool attackLagMagicFired   = false;
        bool attackLagArcheryFired = false;

        // Shout exit linger — on shoutstop, hold isShouting true for a
        // beat so the Shout sub-state profile gets a cinematic tail
        // instead of snapping back the instant the animation completes.
        // Shouts don't chain (multi-second cooldown) so no session
        // adaptation needed.
        std::chrono::steady_clock::time_point shoutEndTime{};
        bool shoutLingering = false;
        // Per-entry shout Lag (CameraProfile::shoutLag, seconds): published
        // each 3p frame by CameraController from the RESOLVED shout profile
        // (base / per-shout override / mod-shout binding / TL variant / env
        // variant — whichever owns the frame). PollShoutLinger adds it to
        // the exit linger so the switch back to sheathed/unsheathed waits
        // this long. Reset at every shout start so a stale value from a
        // previous shout can't leak into one cast without a 3p resolve
        // (e.g. entirely in first person).
        float activeShoutLagSec = 0.0f;
        // See IsShoutLagHolding — the lag-only slice of the exit linger.
        bool  shoutLagHolding   = false;
        // Time of the most recent BeginCastVoice anim event. Reset on
        // each new shout press so the 1p envelope restarts cleanly.
        std::chrono::steady_clock::time_point shoutStartTime{};
        // Paused time inside the current shout, and the last poll tick that
        // measured it. The 3s missing-stop safety window in PollShoutLinger
        // is wall-clock and the poll runs while the game is paused, so a
        // menu opened mid-shout used to force-end the shout on the clock
        // (2026-09-04 log). The window is measured net of this span.
        std::chrono::steady_clock::duration   shoutPausedAccum{};
        std::chrono::steady_clock::time_point shoutPauseTick{};
        // Time of the most recent Voice_SpellFire_Event anim event
        // (the actual voice-firing moment, distinct from the wind-up).
        // NOTE: many behaviour-framework graphs — including this load
        // order's — never emit Voice_SpellFire_Event at all. PollShoutWords
        // synthesises the same edge from the engine's own voice state so
        // the noise envelopes get a release moment either way.
        std::chrono::steady_clock::time_point shoutFireTime{};

        // How many words of the shout were actually used: 0 = not yet
        // known (still winding up, or no shout active), else 1/2/3. Read
        // straight off HighProcessData::currentShoutVariation, which the
        // engine sets when the shout commits — this is a REAL word count,
        // not the elapsed-hold-time guess the noise envelopes used to make.
        int shoutWordCount = 0;
        // Per-frame safety net for the word count (the primary capture is on
        // the SKSE kVoiceFire action event, which also synthesises
        // shoutFireTime on graphs with no Voice_SpellFire_Event).
        void PollShoutWords(RE::PlayerCharacter* a_player);
        void CaptureShoutWordCount(const char* a_via);

        // Werewolf roar deferred-start. The howl's kVoiceCast fires
        // before the visible roar animation peaks; setting isShouting
        // immediately makes the camera transition to the Roar profile
        // ahead of the visual cue. We schedule isShouting for a small
        // delay after kVoiceCast so the camera reaches Roar in sync
        // with the animation, applied ONLY for werewolf howls — every
        // other shout still uses the BeginCastVoice anim-tag path.
        std::chrono::steady_clock::time_point werewolfRoarStartTime{};
        bool werewolfRoarScheduled = false;

        // ShoutId resolved at BeginCastVoice time, kept until shoutstop
        // commits the exit (or the shout linger expires, whichever is
        // later). Used by the camera routing to pick per-shout profiles.
        std::optional<ShoutId> activeShoutId = std::nullopt;

        // Mod-added shout identity (registry miss at kVoiceCast). Local
        // form id + plugin filename; 0/empty when the active shout is a
        // known one or no shout is active. Cleared wherever activeShoutId
        // is cleared.
        std::uint32_t activeModShoutFormID = 0;
        std::string   activeModShoutPlugin;

        // Per-frame expiry check for shout linger. Runs each frame because
        // shout end isn't detected from a polled graph var — it's an anim
        // graph event — so there's no natural poll to piggyback on.
        void PollShoutLinger();

        // Menu state hold (see the getter block comment + RequestMenuStateHold).
        // menuHoldTtl counts down each Update; MenuUI re-arms it every frame
        // either the mod control panel or Quick Tune is open. The first armed
        // frame snapshots the full REPORTED state (so attack-lag holds and
        // lingers fold in) and every getter reports the snapshot until the
        // TTL lapses.
        struct MenuHoldSnapshot {
            CameraState    state{};
            CameraSubState sub{};
            MagicSchool    school{};
            CastType       cast{};
            bool           concentration = false;
            CastingHand    hand{};
            BlockKind      block{};
            DragonAction   dragon = DragonAction::Cruising;
            PowerAttackDirection paDir{};
            bool           sneaking       = false;
            bool           sprinting      = false;
            bool           weaponDrawn    = false;
            bool           locked         = false;
            bool           attackSprint   = false;
            bool           attackSneak    = false;
            bool           powerAttacking = false;
            bool           wwFeeding      = false;
            bool           vlLevitating   = false;
            bool           paragliding    = false;
            bool           chargingSpell  = false;
            bool           castingStream  = false;
            bool           bowZoomed      = false;
            RiderWeapon    rider          = RiderWeapon::None;
            MountAttackSide mountSide     = MountAttackSide::None;
            std::optional<ShoutId> shoutId = std::nullopt;
            std::uint32_t  modShoutFormID = 0;
            std::string    modShoutPlugin;
        };
        MenuHoldSnapshot menuHold{};
        bool             menuHoldActive = false;
        int              menuHoldTtl    = 0;
        void UpdateMenuStateHold();

        // Polls TDM's target-lock state via the integration singleton.
        void PollTargetLock();
    };
}
