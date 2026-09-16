#pragma once

#include <cstdint>
#include <optional>

namespace DietDrCamera
{
    // The options menu and runtime use the same open-country hold. An actual
    // location always wins; its own parent chain defines its containing areas.
    template <class Location, class Parent, class Visit>
    void VisitLocationAncestors(Location* exact, Location* openCountryHold,
                                Parent parent, Visit visit, unsigned limit = 8)
    {
        auto* location = exact ? exact : openCountryHold;
        for (unsigned depth = 0; location && depth < limit; ++depth) {
            visit(location);
            location = parent(location);
        }
    }

    struct LocationMatchContext
    {
        const void* location{};
        const void* worldspace{};
        const void* cell{};
        std::uint32_t cellID{};
        const void* room{};
        const void* region{};
        const void* openCountryHold{};
        bool operator==(const LocationMatchContext&) const = default;
    };

    class LocationMatchState
    {
    public:
        void Invalidate() { _last.reset(); }
        bool Refresh(const LocationMatchContext& context)
        {
            if (_last && *_last == context) return false;
            _last = context;
            return true;
        }

    private:
        std::optional<LocationMatchContext> _last;
    };
}
