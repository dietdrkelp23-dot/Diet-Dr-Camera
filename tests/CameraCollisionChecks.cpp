#include "Camera/CollisionPolicy.h"
#include "Camera/CollisionContactFilter.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace DietDrCamera;
using namespace DietDrCamera::CollisionPolicy;

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

struct Hit { Object object; float normalZ, fraction; Category expected; };

// Emulates the native closest-contact collector. The actual forwarding adapter
// used by the DLL is instantiated below, including virtual dispatch and Reset.
struct Collector
{
    virtual ~Collector() = default;
    virtual void AddCdPoint(const Hit& hit)
    {
        accepted.push_back(hit.fraction);
        earlyOutDistance = (std::min)(earlyOutDistance, hit.fraction);
    }
    virtual void Reset() { earlyOutDistance = 1.0f; accepted.clear(); ++resets; }
    float earlyOutDistance = 1.0f;
    std::vector<float> accepted;
    unsigned resets = 0;
};

struct Policy
{
    Selection selection;
    bool Keep(const Hit& hit, int) const { return selection.Keeps(Classify(hit.object, hit.normalZ)); }
};

int main()
{
    const std::array fixtures{
        Hit{{Form::Tree, Layer::Static}, 1, .1f, Category::Trees},    // branch top, original Ground bug
        Hit{{Form::Tree, Layer::Static}, 0, .2f, Category::Trees},    // trunk side
        Hit{{Form::Static, Layer::Static, true}, 1, .3f, Category::Trees}, // static tree LOD
        Hit{{Form::Static, Layer::Static, false, true}, 1, .4f, Category::Trees}, // mod tree path
        Hit{{Form::Unknown, Layer::Trees}, -.3f, .2f, Category::Trees},
        Hit{{Form::Door, Layer::Static}, 1, .5f, Category::Doors},    // door top is not Ground
        Hit{{Form::Door, Layer::Transparent}, 0, .5f, Category::Doors},
        Hit{{Form::Unknown, Layer::DoorDetection}, 1, .6f, Category::Doors},
        Hit{{Form::MovableStatic, Layer::AnimatedStatic}, 0, .4f, Category::Walls}, // not every animated static is a door
        Hit{{Form::Static, Layer::Static}, 1, .8f, Category::Ground}, // room floor
        Hit{{Form::Static, Layer::Static}, 0, .2f, Category::Walls},  // same room, wall
        Hit{{Form::Static, Layer::Static}, -1, .4f, Category::Walls}, // same room, ceiling
        Hit{{Form::Static, Layer::Static}, .5f, .8f, Category::Ground}, // ramp threshold
        Hit{{Form::Static, Layer::Static}, .49f, .8f, Category::Walls},
        Hit{{Form::Unknown, Layer::StairHelper}, 1, .8f, Category::Ground},
        Hit{{Form::Unknown, Layer::Terrain}, .2f, .9f, Category::Ground},
        Hit{{Form::Unknown, Layer::Ground}, 1, .9f, Category::Ground},
        Hit{{Form::Unknown, Layer::InvisibleWall}, 0, .6f, Category::Walls},
        Hit{{Form::Other, Layer::Static}, 1, .01f, Category::Other},  // furniture/item/actor, even on static layer
        Hit{{Form::Other, Layer::AnimatedStatic}, 0, .01f, Category::Other},
        Hit{{Form::Unknown, Layer::Other}, 1, .01f, Category::Other},
        Hit{{Form::Static, Layer::Static}, std::numeric_limits<float>::quiet_NaN(), .01f, Category::Other}
    };
    for (const auto& fixture : fixtures)
        Check(Classify(fixture.object, fixture.normalZ) == fixture.expected, "collision category mismatch");

    Check(IsTreeModel("meshes\\landscape\\trees\\treepine01.nif"), "vanilla tree directory missed");
    Check(IsTreeModel("MyMod/TREES/AncientOak.nif"), "modded static tree directory missed");
    Check(IsTreeModel("tree/trunk.nif"), "singular tree directory missed");
    for (const auto path : {"Architecture/Whiterun/Streets/street01.nif", "treehousewall.nif",
                            "Architecture/treehouse/wall.nif", "clutter/treesap.nif", "", "treepine01.nif"})
        Check(!IsTreeModel(path), "loose name matching misclassified non-tree geometry");

    using Filter = CollisionContactFilter<Collector, Hit, Policy, int>;
    // All category combinations, in both near-to-far and far-to-near order.
    // A nearest ignored contact must never become the camera stop distance.
    for (unsigned mask = 0; mask < 16; ++mask) {
        Policy policy{{bool(mask&1), bool(mask&2), bool(mask&4), bool(mask&8)}};
        Check(policy.selection.Any() == (mask != 0), "exception activation mismatch");
        for (const bool reverse : {false, true}) {
            Collector native;
            Filter filtered(&native, policy, 0);
            Collector& dispatch = filtered;
            float expected = 1.0f;
            unsigned accepted = 0;
            for (unsigned n = 0; n < fixtures.size(); ++n) {
                const auto& hit = fixtures[reverse ? fixtures.size()-1-n : n];
                const float before = filtered.earlyOutDistance;
                dispatch.AddCdPoint(hit);
                if (policy.selection.Keeps(hit.expected)) {
                    expected = (std::min)(expected, hit.fraction);
                    ++accepted;
                } else Check(filtered.earlyOutDistance == before, "ignored hit shortened the native sweep");
                Check(native.earlyOutDistance == expected && filtered.earlyOutDistance == expected,
                    "camera stop came from the wrong object category");
            }
            Check(native.accepted.size() == accepted, "contact forwarding count changed");
            dispatch.Reset();
            Check(native.resets == 1 && native.accepted.empty() && filtered.earlyOutDistance == 1,
                "collector reset was not forwarded");
        }
    }

    // Starting overlapped: leave the engine's contact distance untouched.
    Policy groundOnly{{true, false, false, false}};
    Collector native;
    Filter filtered(&native, groundOnly, 0);
    filtered.AddCdPoint({{Form::Tree, Layer::Static}, 1, -.5f, Category::Trees});
    Check(native.earlyOutDistance == 1, "ignored starting overlap was kept");
    filtered.AddCdPoint({{Form::Static, Layer::Static}, 1, -.1f, Category::Ground});
    Check(native.earlyOutDistance == -.1f && filtered.earlyOutDistance == -.1f,
        "native penetration distance was changed");
    Filter absent(nullptr, groundOnly, 0);
    absent.AddCdPoint(fixtures[9]);
    absent.Reset();

    // Regression: world sweeps own collector pointers and bypass phantom query
    // methods. Route a nearer tree and farther floor through those actual slots.
    using Scope = ScopedCollisionCollectors<Collector, Hit, Policy, int>;
    native.Reset();
    Collector overlap;
    Collector* castSlot = &native;
    Collector* startSlot = &overlap;
    {
        Scope scope(castSlot, startSlot, groundOnly, 0);
        castSlot->AddCdPoint(fixtures[0]);
        Check(native.earlyOutDistance == 1, "world sweep kept an ignored nearer tree");
        castSlot->AddCdPoint(fixtures[9]);
        Check(native.earlyOutDistance == .8f, "world sweep missed the floor behind a tree");
        startSlot->AddCdPoint({{Form::Tree, Layer::Static}, 1, -.5f, Category::Trees});
        Check(overlap.earlyOutDistance == 1, "world starting overlap bypassed filtering");
    }
    Check(castSlot == &native && startSlot == &overlap, "world collector ownership was not restored");
    startSlot = castSlot;
    {
        Scope scope(castSlot, startSlot, groundOnly, 0);
        Check(castSlot == startSlot, "shared cast/overlap collector lost its alias");
    }
    startSlot = nullptr;
    {
        Scope scope(castSlot, startSlot, groundOnly, 0);
        Check(startSlot == nullptr, "missing world overlap collector was introduced");
    }
    Check(castSlot == &native && !startSlot, "optional collector was not restored");
    std::cout << "Camera collision checks passed: object identity, surfaces, all 16 selections, native forwarding and overlap handling\n";
}
