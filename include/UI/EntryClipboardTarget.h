#pragma once

#include <cstdint>

namespace DietDrCamera
{
    // Extra clipboard context belongs to one rendered item, not every item
    // rendered in the same frame. ImGui IDs already include the window/ID scope.
    struct EntryClipboardTarget
    {
        int frame = -1000;
        std::uint32_t itemID = 0;

        template <class T>
        bool Attach(int a_frame, std::uint32_t a_itemID, T& a_destination, const T& a_context) const
        {
            if (frame != a_frame || itemID == 0 || itemID != a_itemID) return false;
            a_destination = a_context;
            return true;
        }
    };
}
