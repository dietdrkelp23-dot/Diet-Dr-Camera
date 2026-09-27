#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace DietDrCamera::SliderApply
{
    enum class Scope { Tab, TabWithOverrides, AllTabs, AllTabsWithOverrides,
                       CurrentEnvironment, CurrentEnvironmentWithOverrides };
    enum class Environment { Shared, Outdoor, Indoor };

    struct Request
    {
        std::string field;
        std::string tab;
        float value = 0.0f;
        Environment environment = Environment::Shared;
    };

    // Rebuilt at confirmation: a modal never retains pointers into removable
    // bindings, locations or preset storage. Missing state entries materialize
    // only when the selected scope actually writes to them.
    class Collector
    {
    public:
        explicit Collector(const float* source) : source_(source) {}
        explicit Collector(std::string_view field) : field_(field) {}

        void Add(std::string_view field, std::string_view tab, bool isOverride,
                 float* current, std::function<float*()> resolve,
                 float low, float high, std::function<void()> changed = {},
                 Environment environment = Environment::Shared)
        {
            if (source_) {
                if (current == source_ && !request_)
                    request_ = Request{std::string(field), std::string(tab), *source_, environment};
            } else if (field == field_) {
                targets_.push_back({std::string(tab), isOverride, std::move(resolve),
                                    low, high, std::move(changed), environment});
            }
        }

        void Add(std::string_view field, std::string_view tab, bool isOverride,
                 float& value, float low, float high, std::function<void()> changed = {},
                 Environment environment = Environment::Shared)
        {
            Add(field, tab, isOverride, &value, [&value] { return &value; },
                low, high, std::move(changed), environment);
        }

        [[nodiscard]] const std::optional<Request>& Identified() const { return request_; }

        [[nodiscard]] std::size_t Apply(const Request& request, Scope scope) const
        {
            if (!std::isfinite(request.value) || request.field != field_) return 0;
            const bool environmentOnly = scope == Scope::CurrentEnvironment || scope == Scope::CurrentEnvironmentWithOverrides;
            if (environmentOnly && request.environment == Environment::Shared) return 0;
            const bool allTabs = environmentOnly || scope == Scope::AllTabs || scope == Scope::AllTabsWithOverrides;
            const bool overrides = scope == Scope::TabWithOverrides || scope == Scope::AllTabsWithOverrides ||
                scope == Scope::CurrentEnvironmentWithOverrides;
            std::unordered_set<float*> written;
            for (const auto& target : targets_) {
                if ((!allTabs && target.tab != request.tab) || (!overrides && target.isOverride) ||
                    (environmentOnly && target.environment != request.environment)) continue;
                float* value = target.resolve();
                if (!value || !written.insert(value).second) continue;
                *value = std::clamp(request.value, target.low, target.high);
                if (target.changed) target.changed();
            }
            return written.size();
        }

    private:
        struct Target
        {
            std::string tab;
            bool isOverride;
            std::function<float*()> resolve;
            float low, high;
            std::function<void()> changed;
            Environment environment;
        };
        const float* source_ = nullptr;
        std::string field_;
        std::optional<Request> request_;
        std::vector<Target> targets_;
    };
}
