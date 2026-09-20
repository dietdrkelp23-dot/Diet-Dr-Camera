#pragma once

#include "Camera/SpellTrajectory.h"

namespace DietDrCamera::ProjectileFlight
{
    // A predicted arrival never ends a fired shot. Only an engine contact can
    // confirm an impact; losing the projectile simply removes its prediction.
    template <class Point>
    class Flight
    {
    public:
        void Observe(Point position, float time)
        {
            if (confirmed || missing || !SpellTrajectory::Finite(position) || !std::isfinite(time) || time < elapsed) return;
            elapsed = time;
            if (!history.empty() && history.back().time == time) history.back().position = position;
            else history.push_back({position,time});
            if (history.size() > 128) {
                std::size_t out = 1;
                for (std::size_t i = 2; i + 1 < history.size(); i += 2) history[out++] = history[i];
                history[out++] = history.back();
                history.resize(out);
            }
        }
        void Confirm(Point position, float time)
        {
            if (confirmed || missing || !SpellTrajectory::Finite(position) || !std::isfinite(time)) return;
            Observe(position, (std::max)(time, elapsed));
            confirmed = true;
        }
        void Lose() { missing = true; }
        void Advance(float dt)
        {
            if (confirmed && std::isfinite(dt) && dt > 0) settled += dt;
        }
        bool Confirmed() const { return confirmed; }
        bool Expired() const { return missing || settled >= 1.0f || elapsed > 12.0f; }
        float Elapsed() const { return elapsed; }
        float SettleTime() const { return settled; }

        // Combine observed travel with a fresh prediction FROM the projectile.
        // Uniform flight-time samples preserve the renderer's trailing fade.
        std::vector<Point> Compose(const SpellTrajectory::Path<Point>& future = {}) const
        {
            if (history.empty()) return {};
            const float duration = elapsed + (confirmed ? 0.0f : future.duration);
            const int count = int(std::clamp(std::ceil(duration / .025f), 8.0f, 128.0f));
            std::vector<Point> points;
            points.reserve(count + 1);
            std::size_t segment = 0;
            for (int i = 0; i <= count; ++i) {
                const float time = duration * (float(i)/count);
                if (time > elapsed && !confirmed && future.points.size() >= 2 && future.duration > 0) {
                    points.push_back(SpellTrajectory::AtFraction(future.points, (time-elapsed)/future.duration));
                } else {
                    while (segment + 1 < history.size() && history[segment+1].time < time) ++segment;
                    const auto& a = history[segment];
                    const auto& b = history[(std::min)(segment+1,history.size()-1)];
                    const float blend = b.time > a.time ? std::clamp((time-a.time)/(b.time-a.time),0.0f,1.0f) : 0;
                    points.push_back({a.position.x+(b.position.x-a.position.x)*blend,
                        a.position.y+(b.position.y-a.position.y)*blend, a.position.z+(b.position.z-a.position.z)*blend});
                }
            }
            points.back() = !confirmed && !future.points.empty() ? future.points.back() : history.back().position;
            return points;
        }
    private:
        struct Sample { Point position; float time; };
        std::vector<Sample> history;
        float elapsed = 0, settled = 0;
        bool confirmed = false, missing = false;
    };
}
