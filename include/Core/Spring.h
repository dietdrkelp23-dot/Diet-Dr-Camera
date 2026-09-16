#pragma once

#include <cmath>

namespace DietDrCamera
{
    // Critical-damped spring step (semi-implicit Euler).
    //
    // Used everywhere in the codebase that needs to smoothly chase a target:
    // profile transitions in CameraController, dialogue face-lock yaw/pitch,
    // lock-on yaw, adaptive-collision safe distance + tightness ratio, the
    // engine pitch-offset spring, etc. omega = 4 / settleTime so the value
    // settles to within 2% of target in ~settleTime seconds at constant dt.
    //
    // Critical damping (c = 2*omega) means no overshoot — the value
    // monotonically approaches the target. Re-targeting mid-flight is safe;
    // the existing velocity is preserved so transitions stay continuous.
    inline void CriticalDampedSpring(float& cur, float& vel, float target, float omega, float dt)
    {
        const float k     = omega * omega;
        const float c     = 2.0f * omega;
        const float accel = k * (target - cur) - c * vel;
        vel += accel * dt;
        cur += vel * dt;
    }

    // Exact (closed-form) critically-damped spring step.
    //
    // Same physics as CriticalDampedSpring (no overshoot, c = 2*omega) but
    // integrated analytically instead of with one explicit Euler step. The
    // explicit version is only stable while omega*dt < 2*(sqrt2 - 1) ~= 0.83;
    // past that its damping term sign-flips and the value jitters/diverges —
    // which is exactly what high Transition-speed sliders hit at low framerate.
    //
    // This form is UNCONDITIONALLY stable for any omega and dt, and the settle
    // time depends only on omega (~4/omega s), so a transition takes the same
    // wall-clock time at 30 FPS as at 144. Solution of x'' + 2w x' + w^2 x = 0
    // with x = cur - target: x(t) = (x0 + (v0 + w*x0)*t) * e^(-w*t).
    inline void CriticalDampedSpringExact(float& cur, float& vel, float target, float omega, float dt)
    {
        const float x = cur - target;
        const float b = vel + omega * x;          // = x'(0) + w*x0 coefficient
        const float e = std::exp(-omega * dt);
        cur = target + (x + b * dt) * e;
        vel = (vel - omega * b * dt) * e;
    }

    // Exact damped spring step with an arbitrary damping ratio zeta.
    // zeta == 1 is the critically-damped form above; zeta < 1 overshoots and
    // rings (underdamped); zeta > 1 is sluggish with a slow start
    // (overdamped). All three branches are closed-form solutions of
    // x'' + 2*zeta*w*x' + w^2*x = 0, so — like the critical form — this is
    // unconditionally stable at any omega/dt and framerate-independent.
    inline void DampedSpringExact(float& cur, float& vel, float target,
                                  float omega, float zeta, float dt)
    {
        const float x0 = cur - target;
        const float v0 = vel;
        if (std::abs(zeta - 1.0f) < 1.0e-3f) {
            CriticalDampedSpringExact(cur, vel, target, omega, dt);
            return;
        }
        if (zeta < 1.0f) {
            // Underdamped: x(t) = e^(-z*w*t) * (x0*cos(wd t) + B*sin(wd t)).
            const float wd = omega * std::sqrt(1.0f - zeta * zeta);
            const float B  = (v0 + zeta * omega * x0) / wd;
            const float e  = std::exp(-zeta * omega * dt);
            const float cw = std::cos(wd * dt);
            const float sw = std::sin(wd * dt);
            cur = target + e * (x0 * cw + B * sw);
            vel = e * ((v0 * cw) - (x0 * wd + zeta * omega * B) * sw);
            return;
        }
        // Overdamped: two real decay rates.
        const float sq = std::sqrt(zeta * zeta - 1.0f);
        const float r1 = -omega * (zeta - sq);   // slow pole
        const float r2 = -omega * (zeta + sq);   // fast pole
        const float A  = (v0 - r2 * x0) / (r1 - r2);
        const float Bc = x0 - A;
        const float e1 = std::exp(r1 * dt);
        const float e2 = std::exp(r2 * dt);
        cur = target + A * e1 + Bc * e2;
        vel = A * r1 * e1 + Bc * r2 * e2;
    }

    // TRANSITION PERSONALITIES — REMOVED 2026-08-17 (user cut).
    //
    // Bouncy and Elastic were cut on 2026-08-14; Heavy and Glide followed, and
    // with them the whole idea of a per-channel SHAPE toggle. Smooth was never
    // a personality — it is the critically-damped chase this file has always
    // been, and it is now simply what transitions do. Every channel steps
    // through CriticalDampedSpringExact and takes the tuned 0.8 retarget
    // front-load (kRetargetImpulse below).
    //
    // DampedSpringExact above is kept: it is the general form the removed
    // shapes were built from, and it is what any future zeta experiment starts
    // from. Do not re-add a personality selector without asking.

    // The retarget velocity kick applied at a DISCRETE retarget (see
    // CameraController's stepMotion) — an instant velocity boost so the camera
    // starts moving on the very first frame instead of easing out of rest.
    // Below 1.0 so the exact spring stays monotonic and never overshoots.
    inline constexpr float kRetargetImpulse = 0.8f;
}
