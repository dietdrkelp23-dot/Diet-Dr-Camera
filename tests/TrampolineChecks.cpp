#include "PCH.h"
#include "Hooks/HookTrampoline.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
    std::size_t poolCalls = 0;
    std::size_t poolRequest = 0;
    bool noInterface = false;
    bool exhaustedPool = false;
    void* poolMemory = nullptr;

    void Check(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    void* AllocatePool(SKSE::PluginHandle, std::size_t bytes)
    {
        ++poolCalls;
        poolRequest = bytes;
        if (exhaustedPool) return nullptr;
        poolMemory = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        return poolMemory;
    }

    SKSE::Impl::SKSETrampolineInterface pool{
        SKSE::TrampolineInterface::kVersion, &AllocatePool, nullptr
    };

    void* Query(std::uint32_t id)
    {
        return id == SKSE::LoadInterface::kTrampoline && !noInterface ? &pool : nullptr;
    }

    std::uintptr_t CallTarget(std::uintptr_t address)
    {
        std::int32_t displacement;
        std::memcpy(&displacement, reinterpret_cast<void*>(address + 1), sizeof(displacement));
        return address + 5 + displacement;
    }
}

int main(int argc, char** argv)
{
    try {
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        noInterface = mode == "--no-interface";
        exhaustedPool = mode == "--exhausted-pool";
        Check(REL::Module::mock({1, 6, 1170, 0}), "mock executable failed");
        const SKSE::Impl::SKSEInterface skse{
            0, 0, 0, 0, &Query,
            []() -> SKSE::PluginHandle { return 1; },
            []() -> std::uint32_t { return 0; },
            [](const char*) -> const void* { return nullptr; }
        };
        const auto logger = spdlog::default_logger();
        SKSE::Init(reinterpret_cast<const SKSE::LoadInterface*>(&skse), SKSE::InitInfo{ .log = false });
        Check(spdlog::default_logger() == logger, "SKSE replaced the plugin logger");
        auto& trampoline = SKSE::GetTrampoline();

        if (mode == "--old-sequence") {
            SKSE::AllocTrampoline(14);
            (void)trampoline.allocate(14);
            SKSE::AllocTrampoline(14);
            Check(trampoline.free_size() == 0 && poolCalls == 1,
                "The old allocation sequence no longer reproduces exhaustion");
            std::cout << "Old sequence reproduced: first hook uses all 14 bytes; next allocation has zero available\n";
            return 0;
        }

        DietDrCamera::HookTrampoline::Initialize();
        Check(trampoline.capacity() == DietDrCamera::HookTrampoline::capacity, "wrong shared reservation");
        Check(poolCalls == (noInterface ? 0 : 1), "wrong initial pool allocation count");
        if (!noInterface) Check(poolRequest == DietDrCamera::HookTrampoline::capacity, "pool got per-hook size");

        // Only patch owned synthetic code. This test executes no Skyrim code.
        auto* code = static_cast<std::uint8_t*>(trampoline.allocate(96));
        std::memset(code, 0x90, 96);
        const auto base = reinterpret_cast<std::uintptr_t>(code);
        for (const auto offset : {0, 8, 16, 24}) {
            code[offset] = 0xE8;
            std::memset(code + offset + 1, 0, 4);
        }
        const auto hookStart = trampoline.allocated_size();
        Check(trampoline.write_call<5>(base, base + 80) == base + 5, "furniture original lost");
        const auto firstStub = CallTarget(base);
        std::array<std::uint8_t, 14> originalStub{};
        std::memcpy(originalStub.data(), reinterpret_cast<void*>(firstStub), originalStub.size());

        DietDrCamera::HookTrampoline::Initialize();
        Check(trampoline.allocated_size() == hookStart + 14, "reinitialization reset the live trampoline");
        Check(trampoline.write_call<5>(base + 8, base + 81) == base + 13, "camera original lost");
        (void)trampoline.allocate(14);  // camera-collision diversion stub
        const auto cameraStub = CallTarget(base + 8);
        Check(trampoline.write_call<5>(base + 8, base + 82) == cameraStub, "noise hook broke camera chain");
        Check(trampoline.write_call<5>(base + 16, base + 83) == base + 21, "main-thread original lost");
        Check(trampoline.write_call<5>(base + 24, base + 84) == base + 29, "dialogue original lost");
        Check(trampoline.allocated_size() - hookStart == 84, "unexpected active hook storage");
        Check(std::memcmp(originalStub.data(), reinterpret_cast<void*>(firstStub), originalStub.size()) == 0,
            "later allocations overwrote an earlier hook");
        (void)trampoline.allocate(8);  // headroom for dormant menu hook, without enabling it
        Check(trampoline.free_size() > 0, "all hooks exhausted the shared reservation");
        Check(poolCalls == (noInterface ? 0 : 1), "hook installation reallocated the branch pool");
        std::cout << "Shared reservation passed: all camera/menu hook allocations, chained calls, stable earlier stubs";
        std::cout << (noInterface ? "; legacy SKSE without pool\n" : exhaustedPool ? "; exhausted pool fallback\n" : "; SKSE branch pool\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
