#include "PCH.h"
#include "Hooks/RuntimeHooks.h"
#include "Hooks/RuntimeVersion.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include "RuntimeImageCheck.h"

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
            if (offset(&player->GetPlayerRuntimeData())!=layout.playerData ||
                offset(&player->GetPlayerFlags())!=layout.playerFlags ||
                offset(&graphics->GetLetterbox())!=layout.letterbox ||
                offset(&attack->GetRuntimeData())!=layout.attackData ||
                DietDrCamera::RuntimeHooks::InputSlot(4)!=layout.buttonSlot ||
                DietDrCamera::RuntimeHooks::InputSlot(5)!=layout.buttonSlot+1)
                throw std::runtime_error("wrong runtime layout for "+layout.version.string());
        }
        if (DietDrCamera::RuntimeVersion::IsKnown({1,7,105,0}) || DietDrCamera::RuntimeVersion::IsKnown({1,4,15,0}))
            throw std::runtime_error("unknown or VR runtime accepted");
        std::cout << "SE, early AE, post-629 AE, GOG and 1.7 layout checks passed\n";
        if (argc==4 && std::string_view(argv[1])=="--image") CheckRuntimeImage(argv[2],argv[3]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
