#pragma once
#include <algorithm>
#include <string_view>
#include <vector>

namespace DietDrCamera::GameplayCameraInput
{
    enum class Route { None, Look, TogglePOV, POVHeld, CameraButton };

    // Only camera controls may bypass PlayerControls' death/down early return.
    // Dedicated TDM bindings already use its independent raw input sink.
    constexpr Route ButtonRoute(std::string_view event)
    {
        if (event == "Toggle POV") return Route::TogglePOV;
        if (event == "Zoom In" || event == "Zoom Out") return Route::CameraButton;
        return Route::None;
    }

    // Per input packet, record actual handler dispatches. A packet can contain
    // several buttons and devices; seeing one must not hide another, and a
    // control already handled by the original chain must never run twice.
    class Delivered
    {
    public:
        void Record(const void* event, Route route)
        {
            if (event && route != Route::None && !Contains(event, route)) _events.push_back({event, route});
        }
        [[nodiscard]] bool Contains(const void* event, Route route) const
        {
            return std::any_of(_events.begin(), _events.end(), [&](const Entry& item) {
                return item.event == event && item.route == route;
            });
        }
        [[nodiscard]] bool HasLook() const
        {
            return std::any_of(_events.begin(), _events.end(), [](const Entry& item) { return item.route == Route::Look; });
        }
    private:
        struct Entry { const void* event; Route route; };
        std::vector<Entry> _events;
    };
}
