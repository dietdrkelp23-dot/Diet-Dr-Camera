#pragma once

#include "Settings/Defaults.h"
#include "Settings/SettingsManager.h"
#include "UI/SliderApply.h"

namespace DietDrCamera::SliderApply
{
    template <class T> struct Profile
    {
        T* current;
        std::function<T&()> resolve;
        Environment environment;
        explicit Profile(T& value, Environment env = Environment::Shared) :
            current(&value), resolve([&value]() -> T& { return value; }), environment(env) {}
        Profile(T* value, std::function<T&()> get, Environment env = Environment::Shared) :
            current(value), resolve(std::move(get)), environment(env) {}
    };

    template <class T>
    inline void Member(Collector& out, const Profile<T>& p, float T::* member,
                       std::string_view field, std::string_view tab, bool overrides,
                       float low, float high, std::function<void()> changed = {})
    {
        out.Add(field, tab, overrides, p.current ? &(p.current->*member) : nullptr,
                [get = p.resolve, member] { return &(get().*member); }, low, high, std::move(changed), p.environment);
    }

    inline void Camera(Collector& out, CameraProfile& p, std::string_view tab, bool overrides,
                       bool targetLock, bool shout, const Defaults::SliderRange& zoom,
                       bool dragon = false, bool transitionsOnly = false,
                       Environment environment = Environment::Shared)
    {
        Profile profile(p, environment);
        const auto add = [&](const char* name, float CameraProfile::* member, float low, float high) {
            Member(out, profile, member, name, tab, overrides, low, high);
        };
        const auto range = [&](const char* name, float CameraProfile::* member, const auto& bounds) {
            add(name, member, bounds.min, bounds.max);
        };
        if (!transitionsOnly) {
            range("camera.sideOffset", &CameraProfile::sideOffset, dragon ? Defaults::DragonSideOffset : Defaults::SideOffset);
            range("camera.height", &CameraProfile::height, dragon ? Defaults::DragonHeight : Defaults::Height);
            range("camera.zoom", &CameraProfile::zoom, zoom);
            range("camera.fov", &CameraProfile::fov, dragon ? Defaults::DragonFOV : Defaults::FOV);
            range("camera.rotation", &CameraProfile::rotation, Defaults::Rotation);
            range("camera.pitchOffset", &CameraProfile::pitchOffset, dragon ? Defaults::DragonPitchOffset : Defaults::PitchOffset);
            if (shout) add("camera.shoutLag", &CameraProfile::shoutLag, 0.0f, 10.0f);
        }
        add("camera.transitionRotation", &CameraProfile::transitionRotation, 0.05f, 2.0f);
        add("camera.transitionPosition", &CameraProfile::transitionPosition, 0.05f, 2.0f);
        add("camera.transitionZoom", &CameraProfile::transitionZoom, 0.05f, 2.0f);
        add("camera.transitionFOV", &CameraProfile::transitionFOV, 0.05f, 2.0f);
        add("camera.transitionPitch", &CameraProfile::transitionPitch, 0.05f, 2.0f);
        add("camera.transitionWeight", &CameraProfile::transitionWeight, 0.0f, 1.0f);
        add("camera.transitionLooseness", &CameraProfile::transitionLooseness, 0.0f, 1.0f);
        if (targetLock) {
            add("camera.transitionAimBias", &CameraProfile::transitionAimBias, 0.0f, 1.5f);
            add("camera.transitionHeightBias", &CameraProfile::transitionHeightBias, -1.5f, 1.5f);
            add("camera.transitionZoomBias", &CameraProfile::transitionZoomBias, -1.5f, 1.5f);
            add("camera.transitionFOVBias", &CameraProfile::transitionFOVBias, -1.5f, 1.5f);
            add("camera.transitionPitchBias", &CameraProfile::transitionPitchBias, -1.5f, 1.5f);
        }
    }

    struct NoiseFeatures { bool shout = false, launch = false, attack = false, hit = false; };

    inline void Noise(Collector& out, const Profile<SettingsManager::NoiseProfile>& p,
                      std::string_view tab, bool overrides, NoiseFeatures features = {}, bool firstPerson = false)
    {
        using N = SettingsManager::NoiseProfile;
        const auto add = [&](const char* name, float N::* member, float low, float high) {
            Member(out, p, member, name, tab, overrides, low, high);
        };
        add("noise.intensity", &N::amp, 0.0f, 5.0f);
        add("noise.speed", &N::speed, 0.0f, 4.0f);
        add("noise.rotation", &N::tilt, 0.0f, 5.0f);
        if (!firstPerson) add("noise.position", &N::sway, 0.0f, 5.0f);
        add("noise.driftJitter", &N::driftJitter, 0.0f, 1.0f);
        add("noise.roughness", &N::roughness, 0.0f, 1.0f);
        if (!firstPerson) {
            if (features.shout) add("noise.fade", &N::shoutFadeDuration, 0.0f, 10.0f);
            if (features.attack) add("noise.duration", &N::attackDuration, 0.0f, 5.0f);
            if (features.launch) {
                add("noise.repulse", &N::repulse, 0.0f, 3.0f);
                add("noise.repulseFeel", &N::repulseFeel, 0.0f, 1.0f);
            }
        }
        if (features.hit) {
            Profile<HitShake::Tuning> hit(p.current ? &p.current->hitShake : nullptr,
                [get = p.resolve]() -> HitShake::Tuning& { return get().hitShake; }, p.environment);
            const auto authored = [get = p.resolve] { get().hitShake.authored = true; };
            Member(out, hit, &HitShake::Tuning::strength, "hit.strength", tab, overrides, 0.0f, 3.0f, authored);
            Member(out, hit, &HitShake::Tuning::speed, "hit.speed", tab, overrides, 0.0f, 1.0f, authored);
            Member(out, hit, &HitShake::Tuning::bounce, "hit.bounce", tab, overrides, 0.0f, 1.0f, authored);
            Member(out, hit, &HitShake::Tuning::texture, "hit.texture", tab, overrides, 0.0f, 1.0f, authored);
        }
    }

    inline void FirstPerson(Collector& out, const Profile<SettingsManager::FirstPersonProfile>& p,
                            std::string_view tab, bool overrides, NoiseFeatures features = {})
    {
        using F = SettingsManager::FirstPersonProfile;
        Member(out, p, &F::worldFov, "fp.worldFov", tab, overrides, 40.0f, 140.0f);
        Member(out, p, &F::handsFov, "fp.handsFov", tab, overrides, 40.0f, 140.0f);
        Member(out, p, &F::transitionSpeed, "fp.transitionSpeed", tab, overrides, 0.05f, 10.0f);
        if (features.shout) Member(out, p, &F::shoutFadeDuration, "fp.fade", tab, overrides, 0.0f, 10.0f);
        if (features.launch) {
            Member(out, p, &F::repulse, "fp.repulse", tab, overrides, 0.0f, 3.0f);
            Member(out, p, &F::repulseFeel, "fp.repulseFeel", tab, overrides, 0.0f, 1.0f);
            Member(out, p, &F::fofFadeDuration, "fp.fade", tab, overrides, 0.0f, 10.0f);
        }
        Profile<SettingsManager::NoiseProfile> noise(p.current ? &p.current->noise : nullptr,
            [get = p.resolve]() -> SettingsManager::NoiseProfile& { return get().noise; }, p.environment);
        Noise(out, noise, tab, overrides, features, true);
    }

    inline void Character(Collector& out, SettingsManager::CinematicShakeChar& p,
                          std::string_view tab, bool overrides)
    {
        Profile profile(p);
        using C = SettingsManager::CinematicShakeChar;
        Member(out, profile, &C::rotShake, "noise.rotation", tab, overrides, 0.0f, 5.0f);
        Member(out, profile, &C::posShake, "noise.position", tab, overrides, 0.0f, 5.0f);
        Member(out, profile, &C::driftJitter, "noise.driftJitter", tab, overrides, 0.0f, 1.0f);
        Member(out, profile, &C::roughness, "noise.roughness", tab, overrides, 0.0f, 1.0f);
        Member(out, profile, &C::fadeDuration, "noise.fade", tab, overrides, 0.0f, 10.0f);
    }
    inline void Beat(Collector& out, SettingsManager::BeatTuning& p, std::string_view tab,
                     bool overrides, bool positional)
    {
        out.Add("noise.intensity", tab, overrides, p.intensity, 0.0f, 5.0f);
        out.Add("noise.speed", tab, overrides, p.speed, 0.1f, 5.0f);
        Character(out, p.chr, tab, overrides);
        if (positional) {
            out.Add("noise.range", tab, overrides, p.range, 200.0f, 8000.0f);
            out.Add("noise.direction", tab, overrides, p.direction, 0.0f, 1.0f);
        }
    }
}
