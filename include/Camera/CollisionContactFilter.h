#pragma once

#include <bit>

namespace DietDrCamera
{
    // A forwarding collector, shared by swept contacts and initial overlaps.
    // Kept contacts go to the native collector unchanged; rejected contacts
    // cannot shorten its search. The policy owns object classification.
    template <class Base, class Contact, class Policy, class Body>
    class CollisionContactFilter final : public Base
    {
    public:
        CollisionContactFilter(Base* destination, Policy& policy, Body camera) :
            _destination(destination), _policy(policy), _camera(camera)
        {
            this->earlyOutDistance = destination ? destination->earlyOutDistance : std::bit_cast<float>(0x7F7FFFEEu);
        }

        void AddCdPoint(const Contact& contact) override
        {
            if (!_destination || !_policy.Keep(contact, _camera)) return;
            _destination->AddCdPoint(contact);
            this->earlyOutDistance = _destination->earlyOutDistance;
        }

        void Reset() override
        {
            if (_destination) {
                _destination->Reset();
                this->earlyOutDistance = _destination->earlyOutDistance;
            } else Base::Reset();
        }

    private:
        Base* _destination;
        Policy& _policy;
        Body _camera;
    };

    // World linear casts retain collectors in fields instead of passing them
    // through a phantom's virtual query. Replace those fields only while its
    // native broadphase callback runs, then restore the original ownership.
    template <class Base, class Contact, class Policy, class Body>
    class ScopedCollisionCollectors
    {
    public:
        ScopedCollisionCollectors(Base*& cast, Base*& start, Policy& policy, Body camera) :
            _castSlot(cast), _startSlot(start), _cast(cast), _start(start),
            _filteredCast(cast, policy, camera), _filteredStart(start, policy, camera)
        {
            _castSlot = _cast ? &_filteredCast : nullptr;
            _startSlot = _start ? (_start == _cast ? &_filteredCast : &_filteredStart) : nullptr;
        }

        ~ScopedCollisionCollectors() { _castSlot = _cast; _startSlot = _start; }
        ScopedCollisionCollectors(const ScopedCollisionCollectors&) = delete;
        ScopedCollisionCollectors& operator=(const ScopedCollisionCollectors&) = delete;

    private:
        Base*& _castSlot;
        Base*& _startSlot;
        Base* _cast;
        Base* _start;
        CollisionContactFilter<Base, Contact, Policy, Body> _filteredCast, _filteredStart;
    };
}
