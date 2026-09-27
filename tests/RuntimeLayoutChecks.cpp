#include "PCH.h"
#include "Hooks/RuntimeHooks.h"
#include "Hooks/RuntimeVersion.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include "RuntimeImageCheck.h"

namespace
{
    RE::TESAmmo* ReadFixtureAmmo(const RE::Actor*) { return reinterpret_cast<RE::TESAmmo*>(0x1234); }
    RE::TESAmmo* WrongFixtureAmmo(const RE::Actor*) { return nullptr; }

    void CheckAmmoDispatch(RE::PlayerCharacter* player)
    {
        // Upstream corrected the cross-VR wrapper's 0x9F ordinal. DDC builds
        // flat SE/AE virtual dispatch; verify its actual compiler-selected slot.
        std::array<std::uintptr_t, 0xA2> table{};
        table[0x9E] = reinterpret_cast<std::uintptr_t>(&ReadFixtureAmmo);
        table[0x9F] = reinterpret_cast<std::uintptr_t>(&WrongFixtureAmmo);
        auto* vptr = table.data();
        std::memcpy(player, &vptr, sizeof(vptr));
        if (player->GetCurrentAmmo() != reinterpret_cast<RE::TESAmmo*>(0x1234))
            throw std::runtime_error("ammo lookup dispatches to the wrong engine virtual slot");
    }

}

int main(int argc, char** argv)
{
    try {
        struct Layout
        {
            REL::Version version;
            std::size_t playerData, playerFlags, letterbox, attackData, buttonSlot;
        };
        const std::array versions{
            Layout{{1,5,97,0}, 0x3D8,0xBD8,0x51,0x18,4},
            Layout{{1,6,353,0},0x3D8,0xBD8,0x55,0x18,4},
            Layout{{1,6,640,0},0x3E0,0xBE0,0x55,0x18,4},
            Layout{{1,6,659,0},0x3E0,0xBE0,0x55,0x18,4},
            Layout{{1,6,1170,0},0x3E0,0xBE0,0x55,0x18,4},
            Layout{{1,6,1179,0},0x3E0,0xBE0,0x55,0x18,4},
            Layout{{1,7,99,0},0x3E8,0xBE8,0x61,0x58,6},
            Layout{{1,7,104,0},0x3E8,0xBE8,0x61,0x58,6}
        };
        alignas(16) std::array<std::byte,0x2000> storage{};
        const auto offset=[&](const auto* member) {
            return reinterpret_cast<const std::byte*>(member)-storage.data();
        };
        for (const auto& layout : versions) {
            if (!DietDrCamera::RuntimeVersion::IsKnown({layout.version[0],layout.version[1],layout.version[2],layout.version[3]}))
                throw std::runtime_error("known version rejected");
            if (!REL::Module::mock(layout.version)) throw std::runtime_error("mock runtime");
            auto* player=reinterpret_cast<RE::PlayerCharacter*>(storage.data());
            auto* graphics=reinterpret_cast<RE::BSGraphics::State*>(storage.data());
            auto* attack=reinterpret_cast<RE::AttackBlockHandler*>(storage.data());
            auto* thirdPerson=reinterpret_cast<RE::ThirdPersonState*>(storage.data());
            auto* orbitInput=static_cast<RE::PlayerInputHandler*>(thirdPerson);
            auto* firstPerson=reinterpret_cast<RE::FirstPersonState*>(storage.data());
            auto* firstPersonInput=static_cast<RE::PlayerInputHandler*>(firstPerson);
            if (offset(&player->GetPlayerRuntimeData())!=layout.playerData ||
                offset(&player->GetPlayerFlags())!=layout.playerFlags ||
                offset(&graphics->GetLetterbox())!=layout.letterbox ||
                offset(&attack->GetRuntimeData())!=layout.attackData ||
                DietDrCamera::RuntimeHooks::InputSlot(4)!=layout.buttonSlot ||
                DietDrCamera::RuntimeHooks::InputSlot(5)!=layout.buttonSlot+1 ||
                offset(orbitInput)!=0x20 || static_cast<RE::ThirdPersonState*>(orbitInput)!=thirdPerson ||
                offset(&thirdPerson->freeRotation)!=0xD4 ||
                offset(firstPersonInput)!=0x20 || static_cast<RE::FirstPersonState*>(firstPersonInput)!=firstPerson)
                throw std::runtime_error("wrong runtime layout for "+layout.version.string());
        }
        for (const std::uint16_t patch : {3, 16, 23, 39, 50, 53, 62, 73, 80, 96}) {
            if (DietDrCamera::RuntimeVersion::IsKnown({1,5,patch,0}))
                throw std::runtime_error("SE runtime below the 1.5.97 support floor accepted");
        }
        // Check the actual compiler-selected ammo vtable slot.
        CheckAmmoDispatch(reinterpret_cast<RE::PlayerCharacter*>(storage.data()));
        if (DietDrCamera::RuntimeVersion::IsKnown({1,7,105,0}) || DietDrCamera::RuntimeVersion::IsKnown({1,4,15,0}))
            throw std::runtime_error("unknown or VR runtime accepted");
        std::cout << "SE, early AE, post-629 AE, GOG and 1.7 layout checks passed\n";
        if ((argc==4 || argc==5) && std::string_view(argv[1])=="--image")
            CheckRuntimeImage(argv[2],argv[3],argc==5 ? argv[4] : "clean");
        else if (argc!=1) throw std::runtime_error("usage: RuntimeLayoutChecks [--image executable address-library [scenario]]");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
