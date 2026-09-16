#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace DietDrCamera::SpellTrajectory
{
    template <class Point>
    bool Finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

    template <class Point>
    float Length(Point p) { return std::hypot(p.x, p.y, p.z); }

    // Projectile's missile integrator uses this game-unit constant, not the
    // cell's Havok gravity/world scale (which can be changed independently).
    // Verified in the unpacked 1.6.1170 executable: ID 44197 +0x88, constant
    // ID 210017. GetGravity supplies the loaded projectile record's multiplier.
    template <class Point>
    Point Acceleration(float gravity)
    {
        return std::isfinite(gravity) ? Point{0, 0, -686.378662109375f * gravity} : Point{};
    }

    inline float IntegrationStep(float step)
    {
        return std::isfinite(step) && step > 0
            ? std::clamp(step, 0.001f, 1.0f/15.0f) : 1.0f/60.0f;
    }

    template <class Point>
    Point Position(Point origin, Point velocity, Point acceleration, float time, float step = 1.0f/60.0f)
    {
        // Skyrim moves with the OLD velocity, then applies gravity to the
        // next frame. Sum that recurrence directly, interpolating the last
        // frame's straight movement. A continuous parabola falls too early.
        const double h = IntegrationStep(step);
        const double frames = std::floor(double(time)/h);
        const double remainder = double(time) - frames*h;
        const float gravityTime = float(h*h*frames*(frames-1)*0.5 + frames*h*remainder);
        return {origin.x + velocity.x*time + acceleration.x*gravityTime,
                origin.y + velocity.y*time + acceleration.y*gravityTime,
                origin.z + velocity.z*time + acceleration.z*gravityTime};
    }

    template <class Point>
    Point Velocity(Point origin, Point aim, Point forward, float speed, bool parallel)
    {
        Point direction = parallel ? forward : Point{aim.x-origin.x, aim.y-origin.y, aim.z-origin.z};
        float length = Length(direction);
        if (length < 0.001f) { direction = forward; length = Length(direction); }
        if (!Finite(direction) || !std::isfinite(speed) || length < 0.001f || speed <= 0) return {};
        return {direction.x/length*speed, direction.y/length*speed, direction.z/length*speed};
    }

    // Points are evenly spaced in flight time for the fade. The renderer uses
    // the complete collision-checked path; sampling alone cannot find a new hit.
    template <class Point>
    std::vector<Point> Sample(Point origin, Point velocity, Point acceleration, float duration,
                              float step = 1.0f/60.0f)
    {
        if (!Finite(origin) || !Finite(velocity) || !Finite(acceleration) ||
            !std::isfinite(duration) || duration <= 0) return {};
        const int count = int(std::clamp(std::ceil(duration / 0.05f), 8.0f, 128.0f));
        std::vector<Point> points;
        points.reserve(count + 1);
        for (int i = 0; i <= count; ++i)
            points.push_back(Position(origin, velocity, acceleration, duration * (float(i)/count), step));
        return points;
    }

    template <class Point>
    struct Path {
        std::vector<Point> points;
        float duration{};
        bool hit{};
    };

    // Standard missile flight has no iron-arrow damping. Raycast every chord
    // of the ballistic curve, including the first (the caller filters shooter
    // and projectile collisions). Cap distance, time, and ray count separately.
    template <class Point, class CastRay>
    Path<Point> Trace(Point origin, Point velocity, Point acceleration, float range, CastRay cast,
                     float step = 1.0f/60.0f)
    {
        Path<Point> path;
        if (!Finite(origin) || !Finite(velocity) || !Finite(acceleration) || Length(velocity) < 0.001f)
            return path;
        const float maxRange = std::isfinite(range) && range > 0 ? (std::min)(range, 8000.0f) : 8000.0f;
        Point previous = origin, end = origin;
        float distance = 0;
        for (int i = 0; i < 256 && path.duration < 8.0f && distance < maxRange; ++i) {
            const Point currentVelocity{velocity.x + acceleration.x*path.duration,
                                        velocity.y + acceleration.y*path.duration,
                                        velocity.z + acceleration.z*path.duration};
            float dt = (std::min)({1.0f/30.0f, 128.0f/(std::max)(Length(currentVelocity), 1.0f),
                                  8.0f-path.duration});
            end = Position(origin, velocity, acceleration, path.duration + dt, step);
            const Point span{end.x-previous.x, end.y-previous.y, end.z-previous.z};
            float segmentLength = Length(span);
            const bool rangeEnd = distance + segmentLength >= maxRange;
            if (rangeEnd && segmentLength > 0) {
                const float fraction = (maxRange-distance)/segmentLength;
                dt *= fraction;
                end = Position(origin, velocity, acceleration, path.duration + dt, step);
                segmentLength = maxRange-distance;
            }
            float fraction = 1.0f;
            if (cast(previous, end, fraction) && std::isfinite(fraction)) {
                fraction = std::clamp(fraction, 0.0f, 1.0f);
                end = Point{previous.x+(end.x-previous.x)*fraction,
                            previous.y+(end.y-previous.y)*fraction,
                            previous.z+(end.z-previous.z)*fraction};
                path.duration += dt*fraction;
                path.hit = true;
                break;
            }
            path.duration += dt;
            distance += segmentLength;
            previous = end;
            if (rangeEnd) break;
        }
        path.points = Sample(origin, velocity, acceleration, path.duration, step);
        if (path.points.empty() && path.hit) path.points = {origin, end};
        else if (!path.points.empty()) path.points.back() = end;  // exact collision point
        return path;
    }

    template <class Point>
    Point AtFraction(const std::vector<Point>& points, float fraction)
    {
        if (points.empty()) return {};
        const float index = std::clamp(fraction, 0.0f, 1.0f) * (points.size()-1);
        const auto first = (std::min)(std::size_t(index), points.size()-1);
        const auto second = (std::min)(first+1, points.size()-1);
        const float blend = index-float(first);
        return {points[first].x+(points[second].x-points[first].x)*blend,
                points[first].y+(points[second].y-points[first].y)*blend,
                points[first].z+(points[second].z-points[first].z)*blend};
    }
}
