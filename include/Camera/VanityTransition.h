#pragma once

#include "Settings/CameraProfile.h"

namespace DietDrCamera
{
    // Only selects transition SETTINGS. CameraController's existing channel
    // motion, speed curves, Weight and follow lag do all of the actual movement.
    class VanityTransition
    {
    public:
        void Reset()
        {
            wasActive = returning = false;
            returnDestination = nullptr;
        }

        void Update(bool active, const CameraProfile& vanity,
                    const CameraProfile& destination, bool settled)
        {
            if (active) {
                outgoing = vanity;  // Quick Tune edits apply live to both legs.
                returning = false;
                returnDestination = nullptr;
            } else {
                if (wasActive) {
                    returning = true;
                    returnDestination = &destination;
                }
                // The outgoing settings own this one return, not subsequent
                // sprint/cast/target-lock/environment transitions. Keep the
                // shared springs continuous; only release the settings source.
                if (settled || (returning && returnDestination != &destination)) {
                    returning = false;
                    returnDestination = nullptr;
                }
            }
            wasActive = active;
        }

        [[nodiscard]] bool IsReturning() const { return returning; }
        [[nodiscard]] const CameraProfile& Source(const CameraProfile& destination) const
        {
            return wasActive || returning ? outgoing : destination;
        }

    private:
        CameraProfile outgoing{};
        const CameraProfile* returnDestination = nullptr; // Identity only; never dereferenced.
        bool wasActive = false;
        bool returning = false;
    };
}
