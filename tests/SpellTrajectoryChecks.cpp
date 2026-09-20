#include "Camera/SpellTrajectory.h"
#include "Camera/RayHitFilter.h"
#include "Camera/ProjectileFlight.h"

#include <cstdlib>
#include <array>
#include <iostream>
#include <limits>

namespace Flight = DietDrCamera::SpellTrajectory;
struct Point { float x{}, y{}, z{}; };

struct RayBody { bool ignored{}; };
struct RayHit { float fraction{}; };
struct ClosestRayHit
{
    float earlyOutHitFraction = 1;
    virtual ~ClosestRayHit() = default;
    virtual void AddRayHit(const RayBody&, const RayHit& hit)
    {
        earlyOutHitFraction = (std::min)(earlyOutHitFraction, hit.fraction);
    }
};

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

static bool Near(Point a, Point b, float tolerance = 0.002f)
{
    return Flight::Length(Point{a.x-b.x, a.y-b.y, a.z-b.z}) <= tolerance;
}

// Independent frame-by-frame reference for Projectile's native update order.
// The predictor uses a closed-form sum; this deliberately executes each step.
static Point EnginePosition(Point origin, Point velocity, double gravity, float duration, float step)
{
    double x=origin.x, y=origin.y, z=origin.z, vz=velocity.z;
    double remaining=duration;
    while (remaining > 0) {
        const double dt=(std::min)(remaining, double(step));
        x+=velocity.x*dt; y+=velocity.y*dt; z+=vz*dt;
        vz-=gravity*step;
        remaining-=dt;
    }
    return {float(x), float(y), float(z)};
}

static std::pair<Point, double> EngineGroundImpact(Point origin, Point velocity, double gravity, float step)
{
    double z=origin.z, vz=velocity.z, elapsed=0;
    for (int i=0; i<10000; ++i) {
        const double next=z+vz*step;
        if (next<=0) {
            elapsed+=step*z/(z-next);
            return {{float(origin.x+velocity.x*elapsed), float(origin.y+velocity.y*elapsed), 0}, elapsed};
        }
        z=next; vz-=gravity*step; elapsed+=step;
    }
    Check(false, "Native reference exceeded its flight budget");
    return {};
}

int main()
{
    const auto clear = [](Point, Point, float&) { return false; };
    const auto ground = [](Point from, Point to, float& fraction) {
        if (from.z >= 0 && to.z <= 0) {
            fraction = from.z / (from.z-to.z);
            return true;
        }
        return false;
    };
    // The installed Mysticism Fireball (0010FBED) is speed 2500, gravity 0.15;
    // Firebolt is 2500/0.25. Native missile gravity is the fixed game-unit
    // constant at 1.6.1170 ID 210017, independent of cell Havok settings.
    constexpr double nativeGravity=686.378662109375;
    Check(Near(Flight::Acceleration<Point>(0.15f), Point{0, 0, -102.9567993f}), "Mysticism Fireball gravity mismatch");
    Check(Near(Flight::Acceleration<Point>(0.25f), Point{0, 0, -171.5946655f}), "Mysticism Firebolt gravity mismatch");
    Check(Near(Flight::Acceleration<Point>(0), Point{}), "Zero-gravity spell acquired drop");
    Check(Near(Flight::Acceleration<Point>(-0.25f), Point{0, 0, 171.5946655f}), "Upward gravity was clamped");
    // Compare impacts and every rendered sample with native frame recurrence
    // at low/high frame rates and a slow-motion-sized game-time delta.
    for (float step : {1.0f/30, 1.0f/60, 1.0f/120, 1.0f/240}) {
    for (float gravityScale : {0.15f, 0.25f, 1.0f}) {
        for (float speed : {250.0f, 1800.0f, 2500.0f, 3200.0f}) {
            const double g = nativeGravity * gravityScale;
            const Point origin{40, -30, 200}, velocity{0, speed, 0};
            const Point acceleration=Flight::Acceleration<Point>(gravityScale);
            const auto [impact, time] = EngineGroundImpact(origin, velocity, g, step);
            auto path = Flight::Trace(origin, velocity, acceleration, 8000.0f, ground, step);
            Check(path.hit && path.points.size() >= 2, "Gravity projectile missed the ground");
            Check(std::abs(path.duration-time) < 0.0003, "Gravity flight time differs from native integration");
            Check(Near(path.points.back(), impact, 0.8f), "Gravity impact differs from native integration");
            Check(Near(path.points.front(), origin), "Curve lost its casting-hand origin");
            for (std::size_t i = 0; i+1 < path.points.size(); ++i) {
                const float t = path.duration * (float(i)/(path.points.size()-1));
                const Point reference=EnginePosition(origin, velocity, g, t, step);
                Check(Near(path.points[i], reference), "Curve contains damping or incorrect gravity");
            }
            // The HUD must resample a curve, not replace it with a straight
            // hand-to-impact segment. Mid-flight height lies above that chord.
            auto rendered = Flight::Sample(origin, velocity, acceleration, path.duration, step);
            Check(rendered[rendered.size()/2].z > 125.0f, "Render preview straightened the gravity arc");
            Check(Near(rendered.back(), path.points.back(), 0.1f), "Tick/render curve endpoints disagree");
        }
    }
    }
    {
        const Point origin{0, 0, 200}, velocity{0, 2500, 0}, acceleration=Flight::Acceleration<Point>(0.15f);
        const float step=1.0f/30;
        Check(Near(Flight::Position(origin, velocity, acceleration, step, step), Point{0, 2500*step, 200}),
            "Gravity incorrectly lowered the initial flight segment");
        const auto afterOneSecond=Flight::Position(origin, velocity, acceleration, 1, step);
        const float oldParabolaHeight=200-float(nativeGravity*0.15*0.5);
        Check(afterOneSecond.z>oldParabolaHeight+1.7f, "Preview still drops before the engine");
        Check(Near(afterOneSecond, EnginePosition(origin, velocity, nativeGravity*0.15f, 1, step)),
            "Fireball regression does not match native flight");
    }
    {
        // Observed native 1.6.1170 + Mysticism Fireball positions, 2026-09-13.
        // These came from the live projectile and its visible scene node,
        // not from the predictor or the synthetic reference above. Cover both
        // hands, upward/downward shots and large outdoor world coordinates.
        struct Capture { Point origin, velocity; float step; std::array<Point, 3> positions; };
        const Capture captures[] = {
            {{172995.391f,-90442.703f,11106.359f}, {52.355f,2486.200f,-257.033f}, 0.01783f,
                {{{173001.859f,-90133.992f,11073.759f}, {173016.641f,-89424.133f,10992.781f}, {173038.766f,-88357.461f,10855.328f}}}},
            {{172963.578f,-90443.492f,11120.133f}, {68.419f,2485.271f,-262.191f}, 0.01783f,
                {{{172972.109f,-90131.562f,11086.529f}, {172991.547f,-89421.953f,11004.045f}, {173020.625f,-88359.406f,10864.842f}}}},
            {{173036.016f,-90197.344f,11049.772f}, {-1428.476f,2048.814f,108.721f}, 0.01917f,
                {{{172844.750f,-89923.125f,11063.530f}, {172403.422f,-89290.328f,11088.240f}, {171742.922f,-88343.336f,11106.876f}}}},
            {{173094.938f,-90167.594f,11034.529f}, {-1417.327f,2056.566f,108.257f}, 0.01917f,
                {{{172904.000f,-89890.625f,11048.305f}, {172465.594f,-89254.766f,11072.868f}, {171796.609f,-88284.305f,11091.393f}}}}
        };
        for (const auto& capture : captures) {
            for (const auto& observed : capture.positions) {
                // Compare height at the actual horizontal flight distance;
                // this is independent of the HUD's fade clock/frame phase.
                const auto& v=capture.velocity;
                const double time=((double(observed.x)-capture.origin.x)*v.x+
                    (double(observed.y)-capture.origin.y)*v.y)/(double(v.x)*v.x+double(v.y)*v.y);
                const auto predicted=Flight::Position(capture.origin,v,Flight::Acceleration<Point>(0.15f),float(time),capture.step);
                Check(std::abs(predicted.z-observed.z)<0.1f, "Prediction disagrees with observed Fireball flight");
            }
        }
    }

    const auto wall = [](Point from, Point to, float& fraction) {
        if (from.y < 1000 && to.y >= 1000) {
            fraction = (1000-from.y)/(to.y-from.y); return true;
        }
        return false;
    };
    for (float speed : {1000.0f, 2000.0f, 100000.0f}) {
        auto straight = Flight::Trace(Point{0, 0, 150}, Point{0, speed, 0}, Point{}, 8000.0f, wall);
        Check(straight.hit && Near(straight.points.back(), Point{0, 1000, 150}), "Zero-gravity path no longer flies straight");
        Check(std::abs(straight.duration - 1000/speed) < 0.00001f, "Projectile speed was ignored");
    }
    {
        // Collision must be tested along the curve, not the camera ray: the
        // wall hit is lower than the straight aim point and may precede ground.
        const auto firstSurface = [&](Point from, Point to, float& fraction) {
            float fw = 1, fg = 1;
            const bool hw = wall(from, to, fw), hg = ground(from, to, fg);
            fraction = (std::min)(fw, fg); return hw || hg;
        };
        auto curve = Flight::Trace(Point{0, 0, 200}, Point{0, 2000, 0}, Point{0, 0, -200}, 8000.0f, firstSurface);
        Check(curve.hit && Near(curve.points.back(), Point{0, 1000, 175.833333f}, 0.03f), "Curved collision used the camera-ray impact");
        int rays = 0;
        auto nearWall = Flight::Trace(Point{0, 0, 200}, Point{0, 2500, 0}, Point{0, 0, -100}, 8000.0f,
            [&](Point from, Point to, float& fraction) {
                ++rays;
                if (from.y < 2 && to.y >= 2) { fraction = (2-from.y)/(to.y-from.y); return true; }
                return false;
            });
        Check(nearWall.hit && rays == 1 && std::abs(nearWall.points.back().y-2) < 0.001f,
              "First segment skipped a nearby obstruction");
    }
    {
        // Third-person true aim converges from the hand; first-person and the
        // close-range fallback stay parallel to the camera direction.
        const Point hand{60, 30, 100}, aim{0, 1000, 150}, forward{0, 1, 0};
        auto trueAim = Flight::Velocity(hand, aim, forward, 2500.0f, false);
        auto parallel = Flight::Velocity(hand, aim, forward, 2500.0f, true);
        Check(trueAim.x < 0 && trueAim.z > 0 && std::abs(Flight::Length(trueAim)-2500) < 0.001f,
              "Hand-to-aim launch direction or speed changed");
        Check(Near(parallel, Point{0, 2500, 0}), "First-person/close-range launch is not parallel");
        const Point acceleration{0, 0, -171.5f};
        // Two hands/spells must retain their own origin, speed, and gravity.
        auto other = Flight::Sample(Point{-60, 30, 100}, Point{0, 1800, 0}, Point{0, 0, -102.9f}, 1.0f);
        auto first = Flight::Sample(hand, Point{0, 2500, 0}, acceleration, 1.0f);
        Check(Near(first.back(), Point{60, 2530, 15.679167f}) &&
              Near(other.back(), Point{-60, 1830, 49.4075f}), "Different spell parameters contaminated each other");
        const Point shift{1200, -500, 800};
        auto moved = Flight::Sample(Point{hand.x+shift.x, hand.y+shift.y, hand.z+shift.z},
            Point{0, 2500, 0}, acceleration, 1.0f);
        Check(Near(moved.back(), Point{first.back().x+shift.x, first.back().y+shift.y, first.back().z+shift.z}),
              "Live hand translation changed the curve shape");
        Check(Near(Flight::AtFraction(first, 0), first.front()) && Near(Flight::AtFraction(first, 1), first.back()),
              "Curve handoff lost its endpoints");
    }
    for (float speed : {1.0f, 50.0f, 2500.0f, 1000000.0f}) {
        int rays = 0;
        auto path = Flight::Trace(Point{0, 0, 200}, Point{0, speed, 0}, Point{0, 0, -100}, 1e9f,
            [&](Point, Point, float&) { ++rays; return false; });
        Check(rays <= 256 && path.duration <= 8.0f && path.points.size() <= 129, "Prediction exceeded its work budget");
        Check(path.points.size() >= 2 && Flight::Finite(path.points.back()), "Slow/fast projectile produced an invalid path");
    }
    {
        auto limited = Flight::Trace(Point{}, Point{0, 2000, 0}, Point{}, 300.0f, clear);
        Check(Near(limited.points.back(), Point{0, 300, 0}), "Projectile range ignored");
        auto capped = Flight::Trace(Point{}, Point{0, 2000, 0}, Point{}, 1e9f, clear);
        Check(Near(capped.points.back(), Point{0, 8000, 0}, 0.02f), "Trace display range exceeded");
        Check(Flight::Trace(Point{}, Point{}, Point{}, 100.0f, clear).points.empty(), "Zero-speed projectile accepted");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Check(Near(Flight::Acceleration<Point>(nan), Point{}), "Invalid projectile gravity escaped validation");
        for (float invalidStep : {nan, -1.0f, 0.0f})
            Check(Flight::IntegrationStep(invalidStep)==1.0f/60, "Invalid frame delta has no fallback");
        Check(Flight::Trace(Point{}, Point{0, 2000, 0}, Point{0, 0, nan}, 100.0f, clear).points.empty(),
              "Invalid gravity accepted");
    }
    {
        const auto ignore = [](const RayBody& body) { return body.ignored; };
        using Collector = DietDrCamera::RayHitFilter<ClosestRayHit, RayBody, RayHit, decltype(ignore)>;
        auto obstructed = Flight::Trace(Point{0, 0, 200}, Point{0, 2500, 0}, Point{0, 0, -100}, 8000.0f,
            [&](Point, Point, float& fraction) {
                Collector collector(ignore);
                // Self plus more projectiles than the old retry limit. An
                // ignored hit must leave the nearest-hit distance untouched.
                for (int n = 0; n < 8; ++n) collector.AddRayHit(RayBody{true}, RayHit{0.01f});
                Check(collector.earlyOutHitFraction == 1, "Ignored projectile clipped the ray");
                collector.AddRayHit(RayBody{false}, RayHit{0.7f});
                collector.AddRayHit(RayBody{false}, RayHit{0.011f});
                collector.AddRayHit(RayBody{true}, RayHit{0.001f});
                Check(collector.earlyOutHitFraction == 0.011f, "Wall beside an ignored body was skipped");
                fraction = collector.earlyOutHitFraction;
                return true;
            });
        Check(obstructed.hit && obstructed.points.back().y < 1.0f,
            "Nearby object disappeared behind overlapping self/projectile hits");
    }
    {
        // The enemy intersects the initial prediction at t=.4, then steps
        // aside. The real missile passes through that old point and hits a
        // wall at t=1.2. Neither the predicted arrival nor a clock confirms it.
        DietDrCamera::ProjectileFlight::Flight<Point> flight;
        flight.Observe({0,0,100},0);
        const auto enemy = [](Point from, Point to, float& fraction) {
            if (from.y <= 1000 && to.y >= 1000) { fraction = (1000-from.y)/(to.y-from.y); return true; }
            return false;
        };
        auto future = Flight::Trace(Point{0,0,100}, Point{0,2500,0}, Point{}, 8000, enemy);
        Check(future.hit && std::abs(future.duration-.4f) < .001f, "Moving-target fixture missed its initial prediction");
        for (int frame = 1; frame <= 60; ++frame) {
            const float time = frame/60.0f;
            flight.Observe({0,2500*time,100}, time);
            flight.Advance(1.0f/60);
        }
        Check(!flight.Confirmed() && !flight.Expired(), "Predicted arrival fabricated an impact or expired a live projectile");
        const auto wall = [](Point from, Point to, float& fraction) {
            if (from.y <= 3000 && to.y >= 3000) { fraction = (3000-from.y)/(to.y-from.y); return true; }
            return false;
        };
        future = Flight::Trace(Point{0,2500,100}, Point{0,2500,0}, Point{}, 5500, wall);
        const auto updated = flight.Compose(future);
        Check(Near(updated.front(),Point{0,0,100}) && Near(updated.back(),Point{0,3000,100}),
            "A moving enemy left the fired trace pinned to its old hit point");
        Check(Near(Flight::AtFraction(updated,1.0f/1.2f), Point{0,2500,100},.1f),
            "Observed travel and remaining prediction do not join at the native projectile");
        flight.Confirm({0,3000,100},1.2f);
        flight.Observe({0,3500,100},1.4f);
        Check(flight.Confirmed() && Near(flight.Compose().back(),Point{0,3000,100}), "Actual collision did not own the final impact");
        flight.Advance(.9f);
        Check(!flight.Expired(), "Confirmed impact disappeared before its settling tail");
        flight.Advance(.11f);
        Check(flight.Expired(), "Confirmed impact never faded");
    }
    {
        DietDrCamera::ProjectileFlight::Flight<Point> lost;
        lost.Observe({0,0,0},0); lost.Observe({0,50,0},.1f);
        lost.Lose();
        Check(lost.Expired() && !lost.Confirmed(), "An unloaded/deleted projectile became a false hit");
        DietDrCamera::ProjectileFlight::Flight<Point> instant;
        instant.Observe({0,0,0},0); instant.Confirm({0,2,0},0);
        Check(instant.Confirmed() && Near(instant.Compose().back(),Point{0,2,0}), "Same-frame contact lost its actual impact point");
        DietDrCamera::ProjectileFlight::Flight<Point> longFlight;
        for (int i=0; i<2000; ++i) longFlight.Observe({0,float(i),0},i*.005f);
        const auto path = longFlight.Compose();
        Check(path.size() <= 129 && Near(path.front(),Point{}) && Near(path.back(),Point{0,1999,0}),
            "Observed flight exceeded its render budget or lost its endpoints");
    }
    std::cout << "Spell trajectory checks passed (native flight, moving targets, confirmed impacts and bounded lifetime)\n";
}
