#include "Camera/BleedoutFreeLook.h"
#include "Camera/NativeGameplayLook.h"
#include "Camera/GameplayCameraInput.h"
#include "Camera/BleedoutCameraPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using DietDrCamera::BleedoutFreeLook;
constexpr double pi = 3.14159265358979323846;

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

struct Matrix { float m[3][3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; };

static Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix out;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        out.m[i][j] = 0;
        for (int k = 0; k < 3; ++k) out.m[i][j] += a.m[i][k] * b.m[k][j];
    }
    return out;
}

// Independent axis-angle composition supplies capture fixtures, including the
// yaw +/-90-degree singularities in the former heading/attitude/bank code.
static Matrix Rotation(int axis, double radians)
{
    Matrix out;
    const int a = (axis + 1) % 3, b = (axis + 2) % 3;
    out.m[a][a] = out.m[b][b] = float(std::cos(radians));
    out.m[a][b] = -float(std::sin(radians));
    out.m[b][a] = float(std::sin(radians));
    return out;
}

static Matrix View(double yaw, double pitch, double roll = 0)
{
    return Multiply(Multiply(Rotation(2, yaw), Rotation(0, pitch)), Rotation(1, roll));
}

static bool Same(const Matrix& a, const Matrix& b)
{
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        if (std::abs(a.m[i][j] - b.m[i][j]) > 2e-6f) return false;
    return true;
}

static void CheckBasis(const Matrix& m)
{
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double dot = 0;
        for (int k = 0; k < 3; ++k) dot += double(m.m[k][i]) * m.m[k][j];
        Check(std::abs(dot - (i == j ? 1.0 : 0.0)) < 2e-6, "Camera basis lost orthonormality");
    }
    const auto& a = m.m;
    const double determinant = a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1]) -
        a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0]) + a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    Check(std::abs(determinant - 1.0) < 2e-6, "Camera basis became mirrored");
}

static void CameraSuppressionChecks()
{
    using namespace DietDrCamera::BleedoutCameraPolicy;
    for (bool death : {false, true}) for (bool disableDeath : {false, true})
        for (bool disableRagdoll : {false, true}) {
            const bool disabled = Disabled(death, disableDeath, disableRagdoll, false);
            Check(disabled == (death ? disableDeath : disableRagdoll), "Death and ragdoll toggles crossed contexts");
            Check(!Disabled(death, disableDeath, disableRagdoll, true), "Diagnostic suspend retained camera suppression");
            Check(Resolve(true, true, true, disabled, true, false) ==
                (disabled ? Action::KeepCurrent : Action::PassThrough), "Normal POV was not retained");
            Check(Resolve(true, true, true, disabled, false, false) ==
                (disabled ? Action::ReturnToGameplay : Action::PassThrough), "Active bleedout did not return to gameplay");
        }
    for (int frame = 0; frame < 300; ++frame) {
        Check(Resolve(true, true, true, true, true, false) == Action::KeepCurrent,
            "Repeated engine requests restart the gameplay camera");
        Check(Resolve(true, true, true, true, false, true) == Action::KeepCurrent,
            "Open Tween menu loses camera ownership");
    }
    Check(Resolve(true, true, true, true, false, false) == Action::ReturnToGameplay,
        "Closed Tween menu returns to the disabled camera");
    Check(Resolve(false, true, true, true, false, false) == Action::PassThrough &&
        Resolve(true, false, true, true, false, false) == Action::PassThrough &&
        Resolve(true, true, false, true, false, false) == Action::PassThrough,
        "A non-player, missing-player, or unrelated state transition was intercepted");
    Session session;
    Check(!session.ResumeNative(true, false, false), "Default death changed without a blocked request");
    session.Blocked();
    Check(!session.ResumeNative(false, true, true) && session.active, "Ragdoll suppression ended before recovery");
    Check(session.ResumeNative(true, true, false), "Death during a suppressed ragdoll ignored the death toggle");
    Check(session.ResumeNative(true, false, false), "Menu delay lost the pending native camera return");
    session.Reset();
    Check(!session.ResumeNative(true, false, false), "Reload carried suppression into another session");
    session.Blocked();
    Check(!session.ResumeNative(false, false, true) && !session.active, "Getting up retained a ragdoll session");
}

// Execute the production death classifier with the same startup state as the
// two 2026-09-27 crashes: a non-null player, but no loaded base actor. Any
// virtual actor/health query while uninitialized is an immediate test failure.
static void StartupActorChecks()
{
    enum class Value { Health };
    struct Player {
        bool initialized = false;
        bool dead = false;
        bool hasValues = true;
        float health = 100.0f;
        mutable int deathQueries = 0, ownerQueries = 0, healthQueries = 0;
        const Player* GetActorBase() const { return initialized ? this : nullptr; }
        bool IsDead() const
        {
            Check(initialized, "Startup queried the uninitialized player's death state");
            ++deathQueries;
            return dead;
        }
        const Player* AsActorValueOwner() const
        {
            Check(initialized, "Startup accessed the uninitialized actor-value owner");
            ++ownerQueries;
            return hasValues ? this : nullptr;
        }
        float GetActorValue(Value value) const
        {
            Check(initialized && value == Value::Health, "Startup read health before the base actor existed");
            ++healthQueries;
            return health;
        }
    };
    using DietDrCamera::BleedoutCameraPolicy::RealDeath;
    Check(!RealDeath(static_cast<Player*>(nullptr), Value::Health), "Missing player counted as dead");
    Player player;
    for (int input = 0; input < 500; ++input)
        Check(!RealDeath(&player, Value::Health), "Partially constructed player counted as dead");
    Check(player.deathQueries == 0 && player.ownerQueries == 0 && player.healthQueries == 0,
        "Startup entered native actor queries");
    player.initialized = true;
    Check(!RealDeath(&player, Value::Health), "Loaded living player counted as dead");
    player.health = 0.0f;
    Check(RealDeath(&player, Value::Health), "Lethal hit before native death flag was missed");
    player.health = -10.0f;
    Check(RealDeath(&player, Value::Health), "Negative health was not classified as death");
    player.dead = true;
    const int healthQueries = player.healthQueries;
    Check(RealDeath(&player, Value::Health) && player.healthQueries == healthQueries,
        "Confirmed native death unnecessarily queried health");
    player.dead = false;
    player.hasValues = false;
    Check(!RealDeath(&player, Value::Health), "Missing actor-value owner was dereferenced");
    player.initialized = false;
    const int deathQueries = player.deathQueries;
    Check(!RealDeath(&player, Value::Health) && player.deathQueries == deathQueries,
        "Loading another save queried an unloaded player");
}

static void TargetLockControlChecks()
{
    using namespace DietDrCamera::GameplayCameraInput;
    int lookEvent{}, lockDown{}, lockUp{}, wheelLeft{}, wheelRight{}, attack{};
    Delivered packet;
    packet.Record(&lockDown, Route::TogglePOV);
    packet.Record(&lockDown, Route::POVHeld);
    packet.Record(&wheelLeft, Route::CameraButton);
    int toggles = 0, switches = 0, forbidden = 0;
    const auto replay = [&](const void* event, std::string_view name) {
        const auto route = ButtonRoute(name);
        if (route == Route::None || packet.Contains(event, route)) return;
        packet.Record(event, route);
        if (route == Route::TogglePOV) ++toggles;
        else if (route == Route::CameraButton) ++switches;
        else ++forbidden;
    };
    replay(&lockDown, "Toggle POV"); // native already dispatched this press
    replay(&lockUp, "Toggle POV");   // release must still reach TDM
    replay(&wheelLeft, "Zoom In");
    replay(&wheelRight, "Zoom Out");
    replay(&lockUp, "Toggle POV");
    replay(&wheelRight, "Zoom Out");
    for (const auto* name : {"Forward", "Jump", "Left Attack/Block", "Right Attack/Block", "Activate", "Shout", "Sneak"})
        replay(&attack, name);
    Check(toggles == 1 && switches == 1 && forbidden == 0,
        "Camera input replay duplicated native events, lost release, or forwarded gameplay actions");
    Check(packet.Contains(&lockDown, Route::POVHeld) && !packet.Contains(&lockUp, Route::POVHeld),
        "Held-button bookkeeping was confused with ProcessButton dispatch");
    Check(!packet.HasLook(), "Button input was mistaken for normalized look input");
    packet.Record(&lookEvent, Route::Look);
    Check(packet.HasLook(), "Native look would be normalized a second time");
    Delivered nextPacket;
    Check(!nextPacket.Contains(&lockDown, Route::TogglePOV), "A later press reused stale dispatch history");

    BleedoutFreeLook freeLook;
    Matrix camera = View(0.2, -0.2);
    freeLook.UpdateRadians(camera.m, 0, 0, 0, true);
    // Target tracking changes the camera every frame, including target switches
    // through yaw wrap. The follow path must leave the actual tracked pose intact.
    for (int frame = 1; frame <= 300; ++frame) {
        camera = View(frame * 0.035, std::sin(frame * 0.04) * 0.6, 0.15);
        const Matrix tracked = camera;
        freeLook.Follow(camera.m, frame / 60.0, true);
        Check(Same(camera, tracked), "Death free look overwrote target-lock framing");
        Check(std::abs(std::remainder(freeLook.Yaw() - frame * 0.035, 2 * pi)) < 1e-6,
            "Unlock retained the pre-death camera angle instead of the latest target");
    }
    const double yaw = freeLook.Yaw(), pitch = freeLook.Pitch();
    freeLook.UpdateRadians(camera.m, -0.08, 0.04, 301.0 / 60, true);
    Check(std::abs(std::remainder(freeLook.Yaw() - yaw + 0.08, 2 * pi)) < 1e-6 &&
        std::abs(freeLook.Pitch() - pitch - 0.04) < 1e-6,
        "Unlock/target loss did not resume free look from the tracked view");
    camera = View(-1.7, 0.3);
    freeLook.Follow(camera.m, 6.0, true); // re-lock on a new target
    Check(std::abs(freeLook.Yaw() + 1.7) < 1e-6, "Relock did not accept the new camera owner");
    const auto beforeMenu = camera;
    freeLook.Follow(camera.m, 6.1, false);
    freeLook.UpdateRadians(camera.m, 20, 20, 6.2, true);
    Check(Same(camera, beforeMenu), "Unlock replayed camera movement collected during a menu");
    freeLook.Reset();
    Check(!freeLook.IsActive(), "Recovery/load retained a target-lock free-look session");
}

static void GameplayControlChecks()
{
    using namespace DietDrCamera::NativeGameplayLook;
    // Feed the mouse values produced by Skyrim's native normalizer. Verify
    // physical mouse distance keeps the corrected gameplay angular distance.
    // Engine Fixes/Display Tweaks replace orbit pitch dt with 1/42.5;
    // putting dt back here made vertical look slower as FPS increased.
    for (bool orbit : {false, true}) for (bool inverted : {false, true})
        for (int fps : {30, 60, 120, 144, 240}) for (float sensitivity : {0.005f, 0.0125f, 0.045f}) {
            const float dt = 1.0f / fps;
            const float nativeX = (120.0f / fps) * sensitivity * 0.02f / dt;
            const float nativeY = (inverted ? -1.0f : 1.0f) * (20.0f / fps) * sensitivity * 0.85f;
            const float turnRate = 6.2831853f, orbitRate = 3.0f, lookingSpeed = 0.1f, fovScale = 0.75f;
            Matrix camera;
            BleedoutFreeLook look;
            look.UpdateRadians(camera.m, 0, 0, 0, true);
            for (int frame = 1; frame <= fps; ++frame) {
                const auto delta = Rotation(nativeX, nativeY, dt, orbit, orbitRate, turnRate, lookingSpeed, fovScale);
                look.UpdateRadians(camera.m, delta.yaw, delta.pitch, double(frame) / fps, true);
            }
            const double expectedHeading = 120.0 * sensitivity * 0.02 * (orbit ? orbitRate : turnRate);
            const double expectedPitch = (inverted ? -1.0 : 1.0) * 20.0 * sensitivity * 0.85 *
                (orbit ? orbitRate / 42.5 : lookingSpeed * fovScale);
            Check(std::abs(look.Yaw() + expectedHeading) < 1e-6, "Gameplay mouse horizontal sensitivity differs from native");
            Check(std::abs(look.Pitch() - expectedPitch) < 1e-6, "Gameplay mouse pitch bypassed the normal vertical sensitivity fix");
            CheckBasis(camera);
        }
    // Controller normalization already contains sensitivity, acceleration,
    // deadzone and axis snapping. Its output must not be reshaped again.
    for (float x : {-1.7f, -0.05f, 0.0f, 0.05f, 1.7f}) {
        const auto orbit = Rotation(x, 0.37f, 0.016f, true, 2.8f, 8.0f, 0.1f, 0.5f);
        Check(std::abs(orbit.yaw + double(x * 2.8f * 0.016f)) < 1e-8 &&
            std::abs(orbit.pitch - double(0.37f * 2.8f / 42.5f)) < 1e-8,
            "Normalized stick input was given another deadzone or sensitivity");
        const auto actor = Rotation(x, 0.37f, 0.016f, false, 2.8f, 8.0f, 0.1f, 0.5f);
        Check(std::abs(actor.yaw + double(x * 8.0f * 0.016f)) < 1e-8 &&
            std::abs(actor.pitch - double(0.37f * 0.1f * 0.5f)) < 1e-8,
            "Normal actor look rate or FOV pitch scaling changed");
    }
    // A steady controller input has already been time-normalized by Skyrim.
    // Compare one real second of looking up/down against the normal patched
    // camera; another dt on pitch would make 240 FPS eight times slower than 30.
    // Keep native acceleration/sensitivity output intact (including inversion).
    for (bool orbit : {false, true}) for (float direction : {-1.0f, 1.0f})
        for (int fps : {30, 60, 120, 144, 240}) {
            const float dt = 1.0f / fps;
            const float normalizedY = direction * 0.37f * dt * 30.0f;
            Matrix camera;
            BleedoutFreeLook look;
            look.UpdateRadians(camera.m, 0, 0, 0, true);
            for (int frame = 1; frame <= fps; ++frame) {
                const auto delta = Rotation(0.0f, normalizedY, dt, orbit, 2.8f, 8.0f, 0.1f, 0.5f);
                look.UpdateRadians(camera.m, delta.yaw, delta.pitch, double(frame) / fps, true);
            }
            const double expectedPitch = direction * (orbit ? 0.7312941176470588 : 0.555);
            Check(std::abs(look.Pitch() - expectedPitch) < 1e-6,
                "Controller vertical look changed with frame rate or was normalized twice");
            Check(std::abs(look.Yaw()) < 1e-8, "Vertical look changed horizontal heading");
            CheckBasis(camera);
        }
    for (float dt : {0.0f, -0.01f, 1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        const auto invalid = Rotation(1.0f, 1.0f, dt, true, 3.0f, 8.0f, 0.1f, 1.0f);
        Check(invalid.yaw == 0.0 && invalid.pitch == 0.0, "Invalid frame time generated camera motion");
    }
    // The compass recomputes target heading + state offset. It must agree
    // with the rendered view even as a tumbling body's heading changes.
    for (double yaw : {-3.13, -1.6, 0.0, 1.6, 3.13}) for (float body : {-6.2f, -1.0f, 0.0f, 1.0f, 6.2f}) {
        const float offset = HeadingOffset(yaw, body);
        Check(std::abs(std::remainder(double(body + offset) + yaw, 2 * pi)) < 1e-6,
            "Compass heading detached from the camera");
    }
    Matrix camera;
    BleedoutFreeLook look;
    look.UpdateRadians(camera.m, 0, 0, 0, true);
    look.UpdateRadians(camera.m, -0.3, 0.1, 0.01, true);
    const auto beforeMenu = camera;
    look.UpdateRadians(camera.m, 10, 10, 0.02, false);
    look.UpdateRadians(camera.m, 10, 10, 5.0, false);
    look.UpdateRadians(camera.m, 10, 10, 5.01, true);
    Check(Same(camera, beforeMenu), "Native look banked menu/focus input");
    look.UpdateRadians(camera.m, -0.1, 0.1, 5.02, true);
    Check(std::abs(look.Yaw() + 0.4) < 1e-6 && std::abs(look.Pitch() - 0.2) < 1e-6,
        "Native look cannot resume after a menu");
}

int main()
{
    CameraSuppressionChecks();
    StartupActorChecks();
    GameplayControlChecks();
    TargetLockControlChecks();
    for (const double yaw : {0.0, 0.7, pi/2 - 0.01, pi/2, pi/2 + 0.01, pi, -pi/2}) {
        for (const double pitch : {-pi/2, -1.55, -pi/6, 0.0, pi/6, 1.55, pi/2}) {
            for (const double roll : {-0.65, 0.0, 0.65, pi}) {
                const Matrix opening = View(yaw, pitch, roll);
                Matrix camera = opening;
                BleedoutFreeLook look;
                look.Update(camera.m, {32767, 32767, 4000, 4000}, 10.0, true);
                Check(look.IsActive() && Same(camera, opening), "Entry changed the opening orientation");
                look.Update(camera.m, {}, 10.01, true);
                Check(Same(camera, opening), "Idle clamped or drifted the opening orientation");
                CheckBasis(camera);
            }
        }
    }

    // Regression: at yaw 90 and pitch -30, the old Euler decomposition moved
    // pitch into its fixed heading. A horizontal turn then raised the view.
    for (const double yaw : {0.0, pi/2, -pi/2, pi}) {
        for (const double roll : {0.0, 0.65}) {
            Matrix camera = View(yaw, -pi/6, roll);
            BleedoutFreeLook look;
            look.Update(camera.m, {}, 0.0, true);
            for (int frame = 1; frame <= 2400; ++frame) {
                look.Update(camera.m, {32767, 0}, frame / 120.0, true);
                Check(std::abs(camera.m[2][1] + 0.5f) < 2e-6f, "Horizontal look changed elevation");
                Check(std::abs(look.Yaw()) <= pi, "Yaw accumulated without bound");
                CheckBasis(camera);
            }
            camera = View(yaw, 0.0, roll);
            look.Reset();
            look.Update(camera.m, {}, 0.0, true);
            for (int frame = 1; frame <= 120; ++frame) {
                look.Update(camera.m, {0, 32767}, frame / 120.0, true);
                Check(std::abs(camera.m[0][1]*std::cos(yaw) + camera.m[1][1]*std::sin(yaw)) < 2e-6,
                      "Vertical look changed compass direction");
                Check(look.Pitch() <= BleedoutFreeLook::kPitchLimit, "Vertical look flipped over the pole");
            }
            const double atLimit = look.Pitch();
            look.Update(camera.m, {0, -32768}, 1.01, true);
            Check(look.Pitch() < atLimit, "Pitch limit retained excess input instead of reversing immediately");
        }
    }

    const auto diagonal = BleedoutFreeLook::FilterStick(12000, 8000);
    Check(diagonal.y > 0 && std::abs(diagonal.y / diagonal.x - 2.0/3.0) < 1e-12,
          "Deadzone distorted a diagonal into horizontal movement");
    Check(BleedoutFreeLook::FilterStick(8689, 0).x == 0 &&
          BleedoutFreeLook::FilterStick(5000, 5000).y == 0, "Deadzone allowed centered stick drift");
    const auto fullDiagonal = BleedoutFreeLook::FilterStick(32767, 32767);
    Check(std::abs(std::hypot(fullDiagonal.x, fullDiagonal.y) - 1.0) < 1e-12, "Diagonal exceeded full speed");
    Check(BleedoutFreeLook::FilterStick(32767, 0).x == 1.0 &&
          BleedoutFreeLook::FilterStick(-32768, 0).x == -1.0, "Signed stick extremes have different speeds");
    const auto reference = BleedoutFreeLook::FilterStick(20000, 0);
    for (int angle = 0; angle < 360; angle += 10) {
        const auto stick = BleedoutFreeLook::FilterStick(
            std::int16_t(std::lround(20000 * std::cos(angle*pi/180))),
            std::int16_t(std::lround(20000 * std::sin(angle*pi/180))));
        Check(std::abs(std::hypot(stick.x, stick.y) - reference.x) < 4e-5, "Stick speed depends on direction");
    }

    Matrix expected;
    for (const int fps : {30, 60, 72, 120, 144, 240}) {
        Matrix camera;
        BleedoutFreeLook look;
        look.Update(camera.m, {}, 0.0, true);
        for (int frame = 1; frame <= fps; ++frame)
            look.Update(camera.m, {32767, 0}, double(frame)/fps, true);
        Check(std::abs(look.Yaw() - 2.7) < 1e-10, "Stick speed depends on FPS");
        if (fps == 30) expected = camera;
        else Check(Same(camera, expected), "Frame rate changed final orientation");
    }
    {
        BleedoutFreeLook look;
        Matrix camera;
        look.Update(camera.m, {}, 0.0, true);
        double time = 0;
        for (int frame = 0; frame < 200; ++frame) {
            time += 0.004 + (frame % 7) * 0.004;
            look.Update(camera.m, {32767, 0}, time, true);
        }
        Check(std::abs(std::remainder(look.Yaw() - 2.7*time, 2*pi)) < 1e-10,
              "Variable frame times changed stick speed");
    }
    for (const int frames : {1, 6, 24}) {
        BleedoutFreeLook look;
        Matrix camera;
        look.Update(camera.m, {}, 0.0, true);
        for (int frame = 1; frame <= frames; ++frame)
            look.Update(camera.m, {0, 0, 120.0/frames, 40.0/frames}, 0.1*frame/frames, true);
        Check(std::abs(look.Yaw() - 0.36) < 1e-10 && std::abs(look.Pitch() + 0.12) < 1e-10,
              "Mouse distance was time-scaled or its direction changed");
    }
    {
        BleedoutFreeLook look;
        Matrix camera = View(0.7, -0.3, 0.1);
        const Matrix opening = camera;
        look.Update(camera.m, {}, 0.0, true);
        look.Update(camera.m, {32767, 32767, 9000, 9000}, 1.0, false);
        look.Update(camera.m, {32767, 32767, 9000, 9000}, 20.0, false);
        look.Update(camera.m, {32767, 32767, 9000, 9000}, 20.01, true);
        Check(Same(camera, opening), "Menu/resume consumed blocked movement");
        look.Update(camera.m, {32767, 0}, 20.02, true);
        Check(std::abs(look.Yaw() - 0.727) < 1e-7, "Resume replayed menu time");
        const Matrix beforeGap = camera;
        look.Update(camera.m, {32767, 32767, 9000, 9000}, 40.0, true);
        Check(Same(camera, beforeGap), "Long engine pause accumulated input");
        look.Reset();
        Check(!look.IsActive(), "Exit retained an active free-look session");
        camera = View(-2.4, 0.6, -0.1);
        const Matrix nextKnockdown = camera;
        look.Update(camera.m, {32767, 32767, 9000, 9000}, 50.0, true);
        Check(Same(camera, nextKnockdown), "New knockdown reused previous orientation/input");
    }
    {
        BleedoutFreeLook look;
        Matrix camera;
        camera.m[0][0] = std::numeric_limits<float>::quiet_NaN();
        look.Update(camera.m, {}, 0.0, true);
        Check(!look.IsActive(), "Invalid camera orientation was captured");
    }
    std::cout << "Death/ragdoll free-look checks passed\n";
}
