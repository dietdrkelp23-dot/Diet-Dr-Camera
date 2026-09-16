#include "Camera/TargetLockPreview.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace DietDrCamera;
using Point = TargetLockPreview::Point;

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

// Independent geometric reference for TDM's steady vertical camera framing:
// LookAtTarget softens the camera-to-focus elevation before writing freeRotation.
static float ReferencePitch(Point camera, Point focus)
{
    const double x = double(focus.x) - camera.x, y = double(focus.y) - camera.y;
    const double angle = std::atan2(double(focus.z) - camera.z, std::sqrt(x*x + y*y));
    return float(-angle * ((3.14159265358979323846 - std::abs(angle)) / 3.14159265358979323846));
}

int main()
{
    const Point target{70.0f, 850.0f, 220.0f};
    const Point opening{35.0f, -250.0f, 125.0f};
    for (const float offset : {-220.0f, -80.0f, 0.0f, 150.0f, 650.0f}) {
        const Point focus{target.x, target.y, target.z + offset};
        TargetLockPreview preview;
        const float before = ReferencePitch(opening, focus);
        Check(preview.Capture(opening, target, before), "Opening capture failed");
        Check(std::abs(*preview.Pitch(opening, target) - before) < 1e-6f, "Opening changed the view");
        for (const float height : {-500.0f, 0.0f, 500.0f, 1500.0f}) {
            for (const float zoom : {20.0f, 250.0f, 1000.0f, 3000.0f}) {
                for (const float side : {-600.0f, 0.0f, 600.0f}) {
                    const Point camera{side, -zoom, height};
                    const auto pitch = preview.Pitch(camera, target);
                    Check(pitch && std::abs(*pitch - ReferencePitch(camera, focus)) < 2e-6f,
                          "Height/zoom/side preview differs from the steady framing reference");
                }
            }
        }
        const Point shiftedTarget{target.x + 500, target.y - 40, target.z + 100};
        const Point shiftedCamera{opening.x + 500, opening.y - 40, opening.z + 100};
        Check(std::abs(*preview.Pitch(shiftedCamera, shiftedTarget) - before) < 1e-6f,
              "World translation changed the framing");

        // Exercise the feedback: camera elevation changes when pitch rotates
        // a long/high camera rig. It must converge without oscillation/NaNs.
        for (const int fps : {30, 60, 144, 240}) {
            float pitch = before;
            for (int i = 0; i < fps * 10; ++i) {
                Point camera{35, -2000.0f * std::cos(pitch), 650.0f + 2000.0f * std::sin(pitch)};
                const float desired = *preview.Pitch(camera, target);
                pitch += (1.0f - std::exp(-8.0f / fps)) * (desired - pitch);
                Check(std::isfinite(pitch) && std::abs(pitch) < 0.786f, "Preview feedback diverged");
            }
            Point camera{35, -2000.0f * std::cos(pitch), 650.0f + 2000.0f * std::sin(pitch)};
            Check(std::abs(pitch - ReferencePitch(camera, focus)) < 1e-5f,
                  "Closing onto the steady framing reference would jump");
        }
    }
    TargetLockPreview guard;
    Check(!guard.Pitch(opening, target), "Uninitialized preview returned a pitch");
    Check(!guard.Capture(opening, target, 0.9f), "Non-invertible pitch accepted");
    Check(!guard.Capture(opening, opening, 0.0f), "Degenerate target accepted");
    Point bad = opening;
    bad.z = std::numeric_limits<float>::quiet_NaN();
    Check(!guard.Capture(bad, target, 0.0f), "Invalid camera accepted");
    Check(guard.Capture(opening, target, 0.0f), "Horizontal capture failed");
    Check(!guard.Pitch(bad, target), "Invalid live geometry returned a pitch");
    Check(!guard.Capture(opening, target, 0.9f) && !guard.Pitch(opening, target),
          "Failed recapture retained stale focus");
    std::cout << "Target-lock preview checks passed\n";
}
