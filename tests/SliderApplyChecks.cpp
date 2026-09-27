#include "PCH.h"
#include "UI/SliderApplyProfiles.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera;
using namespace DietDrCamera::SliderApply;

static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

int main()
{
    try {
        for (int scope = 0; scope < 4; ++scope) {
            std::array<CameraProfile, 4> p{};
            Collector out("camera.fov");
            Camera(out, p[0], "Sheathed", false, false, false, Defaults::Zoom);
            Camera(out, p[1], "Sheathed", true, false, false, Defaults::Zoom);
            Camera(out, p[2], "Melee", false, false, false, Defaults::Zoom);
            Camera(out, p[3], "Melee", true, false, false, Defaults::Zoom);
            // Shared targets (enemy slots and non-env profiles) appear more than once.
            Camera(out, p[0], "Sheathed", false, false, false, Defaults::Zoom);
            const auto count = out.Apply({"camera.fov", "Sheathed", 105}, static_cast<Scope>(scope));
            constexpr std::size_t counts[] = {1, 2, 2, 4};
            Require(count == counts[scope], "scope did not select the expected entries");
            for (int i = 0; i < 4; ++i) {
                const bool selected = (scope >= 2 || i < 2) && (scope % 2 == 1 || i % 2 == 0);
                CameraProfile expected;
                if (selected) expected.fov = 105;
                Require(p[i] == expected, "Apply changed another slider or override activation");
            }
        }
        // Environment-only scopes span tabs, preserve the other environment and
        // shared values, and include only the requested override tier.
        for (const auto selectedEnvironment : {Environment::Outdoor, Environment::Indoor}) {
            for (const bool includeOverrides : {false, true}) {
                std::array<CameraProfile, 8> profiles{};
                CameraProfile shared;
                Collector out("camera.fov");
                for (int i = 0; i < 8; ++i) {
                    const auto environment = i < 4 ? Environment::Outdoor : Environment::Indoor;
                    Camera(out, profiles[i], i % 4 < 2 ? "Sheathed" : "Magic", i % 2 != 0,
                        false, false, Defaults::Zoom, false, false, environment);
                }
                Camera(out, shared, "Sheathed", true, false, false, Defaults::Zoom);
                const Request request{"camera.fov", "Sheathed", 107, selectedEnvironment};
                const auto scope = includeOverrides ? Scope::CurrentEnvironmentWithOverrides : Scope::CurrentEnvironment;
                Require(out.Apply(request, scope) == (includeOverrides ? 4u : 2u), "environment scope count is wrong");
                for (int i = 0; i < 8; ++i) {
                    const auto environment = i < 4 ? Environment::Outdoor : Environment::Indoor;
                    CameraProfile expected;
                    if (environment == selectedEnvironment && (includeOverrides || i % 2 == 0)) expected.fov = 107;
                    Require(profiles[i] == expected, "environment Apply changed the other environment or an unrelated field");
                }
                Require(shared == CameraProfile{}, "environment Apply changed a value shared with the other environment");
                Require(out.Apply({"camera.fov", "Sheathed", 110}, scope) == 0, "missing environment accepted");
                Require(out.Apply(request, Scope::AllTabsWithOverrides) == 9,
                    "all-tabs no longer includes both environments and shared values");
                Require(shared.fov == 107, "all-tabs omitted shared overrides");
            }
        }

        // Filtering must precede lazy resolution, including nested hit-shake fields.
        for (const auto selectedEnvironment : {Environment::Outdoor, Environment::Indoor}) {
            std::array<std::optional<SettingsManager::NoiseProfile>, 2> lazyNoise;
            Collector out("hit.strength");
            for (int i = 0; i < 2; ++i) {
                Profile<SettingsManager::NoiseProfile> profile(nullptr, [&, i]() -> auto& {
                    if (!lazyNoise[i]) lazyNoise[i].emplace();
                    return *lazyNoise[i];
                }, i == 0 ? Environment::Outdoor : Environment::Indoor);
                Noise(out, profile, "Melee", false, {.hit = true});
            }
            Require(out.Apply({"hit.strength", "Magic", 2, selectedEnvironment}, Scope::CurrentEnvironment) == 1,
                "environment scope did not reach a nested field in another tab");
            const int selected = selectedEnvironment == Environment::Outdoor ? 0 : 1;
            SettingsManager::NoiseProfile expected;
            expected.hitShake.strength = 2;
            expected.hitShake.authored = true;
            Require(lazyNoise[selected] && *lazyNoise[selected] == expected && !lazyNoise[1 - selected],
                "environment filtering created or edited an excluded state");
        }

        CameraProfile camera;
        camera.fov = 113;
        Collector identify(&camera.fov);
        Camera(identify, camera, "Magic", false, false, false, Defaults::Zoom, false, false, Environment::Indoor);
        Require(identify.Identified().has_value(), "source not identified");
        const auto captured = *identify.Identified();
        camera.fov = 80;
        Require(captured.value == 113 && captured.tab == "Magic" && captured.environment == Environment::Indoor, "popup did not snapshot the source value");

        // Absent states are seeded by the caller only after a matching scope is chosen.
        int created = 0;
        std::optional<SettingsManager::FirstPersonProfile> lazy;
        SettingsManager::FirstPersonProfile global;
        global.handsFov = 98;
        global.noise.roughness = 0.8f;
        Profile<SettingsManager::FirstPersonProfile> missing(nullptr, [&]() -> auto& {
            if (!lazy) { lazy = global; ++created; }
            return *lazy;
        });
        Collector lookup(&camera.fov);
        FirstPerson(lookup, missing, "Archery", false);
        Require(created == 0, "opening/canceling Apply created a state");
        Collector states("fp.worldFov");
        FirstPerson(states, missing, "Archery", false);
        Require(states.Apply({"fp.worldFov", "Magic", 100}, Scope::Tab) == 0 && created == 0,
            "an excluded tab materialized a state");
        Require(states.Apply({"fp.worldFov", "Magic", 100}, Scope::AllTabs) == 1 && created == 1,
            "all-tabs omitted an unopened state");
        Require(lazy->worldFov == 100 && lazy->handsFov == 98 && lazy->noise.roughness == 0.8f,
            "new state did not preserve the fallback's other values");

        CameraProfile dragon, person, shout;
        Collector zoom("camera.zoom");
        Camera(zoom, dragon, "Mounts", false, false, false, Defaults::DragonZoom, true);
        Camera(zoom, person, "Sheathed", false, false, false, Defaults::Zoom);
        Require(zoom.Apply({"camera.zoom", "Mounts", -100}, Scope::AllTabs) == 2,
            "zoom scope failed");
        Require(dragon.zoom == -100 && person.zoom == 0, "target slider ranges were not respected");
        Collector lag("camera.shoutLag");
        Camera(lag, person, "Sheathed", false, false, false, Defaults::Zoom);
        Camera(lag, shout, "Sheathed", false, false, true, Defaults::Zoom);
        Require(lag.Apply({"camera.shoutLag", "Sheathed", 2}, Scope::Tab) == 1 && person.shoutLag == 0 && shout.shoutLag == 2,
            "shout-only value reached a normal camera entry");

        SettingsManager::NoiseProfile attack, ambient;
        attack.enabled = true;
        attack.repulse = 1.5f;
        auto expectedAttack = attack;
        expectedAttack.hitShake.strength = 2;
        expectedAttack.hitShake.authored = true;
        Collector hits("hit.strength");
        Noise(hits, Profile(attack), "Melee", false, {.attack = true, .hit = true});
        Noise(hits, Profile(ambient), "Sheathed", false);
        Require(hits.Apply({"hit.strength", "Melee", 2}, Scope::AllTabs) == 1,
            "hit shake included a non-hit entry");
        Require(attack == expectedAttack && ambient == SettingsManager::NoiseProfile{},
            "hit shake changed unrelated fields");
        Collector speed("noise.speed");
        Noise(speed, Profile(attack), "Melee", false, {.hit = true});
        (void)speed.Apply({"noise.speed", "Melee", 3}, Scope::Tab);
        Require(attack.speed == 3 && attack.hitShake.speed == 0.5f,
            "ambient Speed collided with Hit Shake Speed");

        SettingsManager::FirstPersonProfile first;
        first.handsFov = 93;
        const auto originalNoise = first.noise;
        Collector fov("camera.fov");
        FirstPerson(fov, Profile(first), "Sheathed", false);
        Require(fov.Apply({"camera.fov", "Sheathed", 110}, Scope::AllTabs) == 0 && first.handsFov == 93 && first.noise == originalNoise,
            "third-person fields crossed into first-person fields");
        const auto unchanged = person;
        Collector invalid("camera.fov");
        Camera(invalid, person, "Sheathed", false, false, false, Defaults::Zoom);
        Require(invalid.Apply({"camera.fov", "Sheathed", std::numeric_limits<float>::quiet_NaN()}, Scope::Tab) == 0 && person == unchanged,
            "nonfinite Apply corrupted the profile");
        Require(invalid.Apply({"camera.zoom", "Sheathed", 10}, Scope::Tab) == 0, "field identity mismatch accepted");
        std::cout << "Slider Apply scope, single-field, lazy-state, range, semantic and compatibility checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
