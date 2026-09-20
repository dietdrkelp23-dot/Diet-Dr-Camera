#pragma once

#include "Settings/ItemBindingIdentity.h"
#include "Settings/CameraProfile.h"
#include "Settings/Defaults.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace DietDrCamera::CinematicViews
{
    inline constexpr float kDefaultLockOnTightness = .5f;
    struct Point {
        float x = 0, y = 0, z = 0;
        bool operator==(const Point&) const = default;
        Point operator+(Point b) const { return {x+b.x,y+b.y,z+b.z}; }
        Point operator-(Point b) const { return {x-b.x,y-b.y,z-b.z}; }
        Point operator*(float scale) const { return {x*scale,y*scale,z*scale}; }
        float Length() const { return std::hypot(x,y,z); }
        bool Finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    };
    struct View {
        std::string id, name;
        ItemBindings::FormIdentity target, space;
        bool exterior = true, enabled = true;
        Point point{}, targetOffset{};
        // Absent in older presets, whose radius follows the resolved subject.
        std::optional<Point> triggerPoint;
        float radius = 500, idleDelay = 5;
        float lockOnTightness = kDefaultLockOnTightness;
        CameraProfile profile = CameraProfile::Default3p();
        bool operator==(const View&) const = default;
    };
    inline float Bounded(float value, float low, float high, float fallback)
    {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    }
    inline View Sanitize(View view)
    {
        if (view.name.size() > 96) {
            std::size_t end = 96;
            while (end > 0 && (static_cast<unsigned char>(view.name[end]) & 0xC0) == 0x80) --end;
            view.name.resize(end);
        }
        view.radius = Bounded(view.radius,50,12000,500);
        view.idleDelay = Bounded(view.idleDelay,.5f,600,5);
        view.lockOnTightness = Bounded(view.lockOnTightness,0,1,kDefaultLockOnTightness);
        auto& p = view.profile;
        const auto defaults = CameraProfile::Default3p();
        const auto channel = [](float& value, Defaults::SliderRange range, float fallback) {
            value = Bounded(value,range.min,range.max,fallback);
        };
        channel(p.sideOffset,Defaults::SideOffset,defaults.sideOffset);
        channel(p.height,Defaults::Height,defaults.height);
        channel(p.zoom,Defaults::Zoom,defaults.zoom);
        channel(p.fov,Defaults::FOV,defaults.fov);
        channel(p.rotation,Defaults::Rotation,defaults.rotation);
        channel(p.pitchOffset,Defaults::PitchOffset,defaults.pitchOffset);
        for (auto* speed : {&p.transitionRotation,&p.transitionPitch,&p.transitionPosition,&p.transitionZoom,&p.transitionFOV})
            *speed = Bounded(*speed,.05f,1,.5f);
        p.transitionLooseness = Bounded(p.transitionLooseness,0,1,0);
        p.transitionWeight = Bounded(p.transitionWeight,0,1,0);
        p.SyncTransitionOverride();
        return view;
    }
    inline bool Valid(const View& view)
    {
        return !view.id.empty() && view.id.size() <= 64 && view.id.find('\0') == std::string::npos && !view.name.empty() &&
            view.space.Valid() && view.point.Finite() && view.targetOffset.Finite() &&
            view.point.Length() < 1.0e8f && view.targetOffset.Length() < 1.0e6f &&
            (!view.triggerPoint || (view.triggerPoint->Finite() && view.triggerPoint->Length() < 1.0e8f));
    }
    inline float TriggerDistance(const View& view, Point player, Point resolvedSubject)
    {
        return (view.triggerPoint.value_or(resolvedSubject)-player).Length();
    }
    inline bool MatchesSpace(const View& view, const ItemBindings::FormIdentity& space, bool exterior)
    {
        return view.space.Valid() && space.Valid() && view.exterior == exterior &&
            ItemBindings::FormKey(view.space) == ItemBindings::FormKey(space);
    }
    inline constexpr float kEncounterDuration = 10;
    inline bool Eligible(const View& view, bool seen, float distance, bool visible,
        double idleSeconds)
    {
        return view.enabled && visible && std::isfinite(distance) && distance >= 0 && distance <= view.radius &&
            (!seen || idleSeconds+.0001 >= view.idleDelay);
    }
    class IdleTimer
    {
    public:
        void Reset() { age = 0; }
        void Step(bool activity, float delta)
        {
            if (activity || !std::isfinite(delta) || delta < 0 || delta > .5f) { Reset(); return; }
            age = (std::min)(600.0,age+delta);
        }
        double Seconds() const { return age; }
    private:
        double age = 0;
    };
    // Input has already passed the engine's mapping/dead zone. Do not discard
    // small look events that actually move the camera.
    inline bool StickMoved(float x, float y)
    {
        return std::isfinite(x) && std::isfinite(y) && (x != 0 || y != 0);
    }
    // Feed ordinary Rotation/Pitch targets into CameraController's springs
    // BEFORE native pose/collision/render composition. Never edit the authored
    // entry. Native angles exclude DDC offsets, preventing focus feedback.
    inline CameraProfile FocusProfile(CameraProfile profile, Point toSubject, float nativeYaw, float nativePitch,
        float lockOnTightness = kDefaultLockOnTightness)
    {
        if (!toSubject.Finite() || toSubject.Length() < .01f ||
            !std::isfinite(nativeYaw) || !std::isfinite(nativePitch)) return profile;
        constexpr float radians = .0174532925199433f;
        const float yaw = std::remainder(std::atan2(toSubject.x,toSubject.y)-nativeYaw,6.28318530718f);
        const float pitch = std::atan2(toSubject.z,std::hypot(toSubject.x,toSubject.y))-nativePitch;
        const float angle = std::hypot(yaw,pitch);
        const float tightness = Bounded(lockOnTightness,0,1,kDefaultLockOnTightness);
        // Tightness shrinks the framing zone, rather than scaling yaw travel.
        // Even the loosest lock keeps following a subject behind the player.
        // The midpoint retains the existing zone; maximum tracks it exactly.
        const float slack = std::clamp(profile.fov*.1f,2.0f,10.0f)*2*(1-tightness)*radians;
        const float gain = angle > slack ? 1-slack/angle : 0;
        // Full yaw travel maintains focus when walking around/past the subject.
        // The native profile springs provide softness, not a 90-degree limit.
        profile.rotation = std::remainder(profile.rotation+yaw*gain/radians,360.0f);
        // Engine body pitch is positive down; DDC's pitch offset is positive up.
        profile.pitchOffset += pitch*gain*100;
        profile.pitchOffset = std::clamp(profile.pitchOffset,(-1.55f-nativePitch)*100,(1.55f-nativePitch)*100);
        return profile;
    }
    class ResumeGate
    {
    public:
        void Reset() { age = 0; }
        bool Step(bool menuOpen, float delta)
        {
            if (menuOpen) { Reset(); return false; }
            if (std::isfinite(delta) && delta > 0) age = (std::min)(.2f,age+(std::min)(delta,.05f));
            return age >= .2f;
        }
    private:
        float age = 0;
    };
    // Ownership only: the normal third-person springs own entry and return.
    // First radius encounters last ten seconds; later idle visits hold until
    // manual look/action. Walking never drops either kind of subject lock.
    class Playback
    {
    public:
        void Start(const View& view, bool firstEncounter) { *this = {}; tuning = Sanitize(view); active = true; timed = firstEncounter; }
        void Step(float delta, bool looking, bool action, bool tuningOpen = false)
        {
            if (!active || tuningOpen) return;
            if (looking || action) { active = false; return; }
            if (!std::isfinite(delta) || delta <= 0) return;
            age += (std::min)(delta,.25f);
            if (timed && age+.0001f >= kEncounterDuration) active = false;
        }
        View tuning{};
        float age = 0;
        bool active = false, timed = false;
    };
}
