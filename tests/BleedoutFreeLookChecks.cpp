#include "Camera/BleedoutFreeLook.h"

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

int main()
{
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
