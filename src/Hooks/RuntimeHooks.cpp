#include "PCH.h"
#include "Hooks/RuntimeHooks.h"
#include "Hooks/RuntimePatchInspection.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <span>

namespace DietDrCamera::RuntimeHooks
{
    namespace
    {
        Sites sites{};
        bool prepared = false;
        std::array<std::uint8_t, 6> uiBranchBytes{};
        std::string inspecting;

        [[noreturn]] void Fail(std::string_view detail)
        {
            const auto message = fmt::format("Diet Dr Camera cannot install its hooks on Skyrim {}: {}. "
                "No gameplay should be started with this combination. See DietDrCamera.log.",
                REL::Module::get().version().string(), inspecting.empty() ? std::string(detail) :
                    fmt::format("{} ({})", detail, inspecting));
            spdlog::critical("{}", message);
            SKSE::stl::report_and_fail(message);
        }

        std::span<const std::uint8_t> Function(std::uintptr_t address)
        {
            DWORD64 imageBase = 0;
            const auto* entry = RtlLookupFunctionEntry(address, &imageBase, nullptr);
            if (!entry || imageBase != REL::Module::get().base() ||
                imageBase + entry->BeginAddress > address || imageBase + entry->EndAddress <= address ||
                entry->EndAddress <= entry->BeginAddress ||
                entry->EndAddress - entry->BeginAddress > 0x20000) Fail("invalid engine function boundaries");
            const auto rootBegin = [imageBase](const RUNTIME_FUNCTION* region) {
                for (int depth = 0; depth < 16; ++depth) {
                    const auto* unwind = reinterpret_cast<const std::uint8_t*>(imageBase + region->UnwindData);
                    if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) return region->BeginAddress;
                    const auto alignedCodes = (static_cast<std::size_t>(unwind[2]) * 2 + 3) & ~std::size_t{3};
                    region = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + alignedCodes);
                }
                Fail("cyclic engine unwind metadata");
            };
            const auto root = rootBegin(entry);
            auto end = entry->EndAddress;
            // MSVC can split one function into adjacent regions with chained unwind data.
            // The dialogue update has five such regions on 1.6.1170.
            while (end - entry->BeginAddress < 0x20000) {
                DWORD64 nextBase = 0;
                auto nextBegin = end;
                const auto* next = RtlLookupFunctionEntry(imageBase + nextBegin, &nextBase, nullptr);
                // Chained regions may be separated by compiler alignment padding.
                while (!next && nextBegin - end < 15) {
                    const auto byte = *reinterpret_cast<const std::uint8_t*>(imageBase + nextBegin);
                    if (byte != 0xCC && byte != 0x90) break;
                    next = RtlLookupFunctionEntry(imageBase + ++nextBegin, &nextBase, nullptr);
                }
                if (!next || nextBase != imageBase || next->BeginAddress != nextBegin || rootBegin(next) != root) break;
                if (next->EndAddress <= end) Fail("invalid chained engine function boundaries");
                end = next->EndAddress;
            }
            if (end - entry->BeginAddress > 0x20000) Fail("engine function exceeds scan limit");
            return { reinterpret_cast<const std::uint8_t*>(address), imageBase + end - address };
        }

        bool IsExecutable(std::uintptr_t address)
        {
            MEMORY_BASIC_INFORMATION info{};
            return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) &&
                info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        }

        bool IsEngineData(std::uintptr_t address, std::size_t size)
        {
            MEMORY_BASIC_INFORMATION info{};
            return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) &&
                reinterpret_cast<std::uintptr_t>(info.AllocationBase) == REL::Module::get().base() &&
                info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                address - reinterpret_cast<std::uintptr_t>(info.BaseAddress) <= info.RegionSize &&
                size <= info.RegionSize - (address - reinterpret_cast<std::uintptr_t>(info.BaseAddress));
        }

        std::uintptr_t FindCall(std::string_view name, REL::RelocationID caller, REL::RelocationID callee,
            std::size_t legacySE = 0, std::size_t legacyAE = 0)
        {
            const auto address = caller.address();
            const auto target = callee.address();
            inspecting = fmt::format("{}, caller ID {}, callee ID {}", name, caller.id(), callee.id());
            const auto code = Function(address);
            spdlog::info("[Runtime] Inspecting {}: RVA 0x{:X}, {} bytes", inspecting,
                address - REL::Module::get().base(), code.size());
            std::optional<std::size_t> offset;
            // Established 1.5/1.6 sites may already be chained by another camera plugin.
            // Only accept an actual instruction boundary calling executable code outside Skyrim.
            if (REL::Module::get().version() < SKSE::RUNTIME_SSE_1_7_99) {
                const auto legacy = REL::Module::IsAE() ? legacyAE : legacySE;
                if (legacy) {
                    if (const auto instruction = RuntimePatchInspection::CallAt(code, address, legacy)) {
                        MEMORY_BASIC_INFORMATION info{};
                        if (instruction->target == target) {
                            offset = legacy;
                        } else if (IsExecutable(instruction->target) &&
                            VirtualQuery(reinterpret_cast<const void*>(instruction->target), &info, sizeof(info)) &&
                            reinterpret_cast<std::uintptr_t>(info.AllocationBase) != REL::Module::get().base()) {
                            offset = legacy;
                            spdlog::info("[Runtime] {} chains another plugin at +0x{:X}", name, legacy);
                        }
                    }
                }
            }
            if (!offset) {
                const auto decoded = RuntimePatchInspection::Decode(code, address);
                if (!decoded) Fail("cannot decode engine function");
                offset = RuntimePatchInspection::UniqueCall(*decoded, target);
            }
            if (!offset) Fail(fmt::format("missing or ambiguous {} call", name));
            spdlog::info("[Runtime] {}: ID {} +0x{:X}", name, caller.id(), *offset);
            return address + *offset;
        }
    }

    void Prepare()
    {
        if (prepared) return;
        Sites resolved{};
        resolved.cameraUpdate = FindCall("TESCamera::Update", REL::RelocationID{49852, 50784}, REL::RelocationID{32289, 33025}, 0x1A6, 0x1A6);
        resolved.enterFurniture = FindCall("EnterFurniture", REL::RelocationID{17034, 17420}, REL::RelocationID{49880, 50813}, 0x295, 0x2A2);
        resolved.mainUpdate = FindCall("ScrapHeap::KeepPages", REL::RelocationID{35565, 36564}, REL::RelocationID{66889, 68150});
        const auto dialogue = REL::RelocationID(36540, 37541).address();
        const auto timerTarget = REL::RelocationID(38327, 39302).address();
        inspecting = "dialogue close timer";
        const auto dialogueCode = Function(dialogue);
        if (REL::Module::get().version() < SKSE::RUNTIME_SSE_1_7_99) {
            // SkyrimSoulsRE's SE 2.2.2 / AE ports establish these decrement sites.
            // Older compilers need not use the same SSE instruction for negation.
            const auto legacyOffset = REL::Module::IsAE() ? 0x6E8u : 0x4F9u;
            if (const auto call = RuntimePatchInspection::CallAt(dialogueCode, dialogue, legacyOffset);
                call && call->target == timerTarget) resolved.dialogueTimer = dialogue + legacyOffset;
        }
        if (!resolved.dialogueTimer) {
            const auto timer = RuntimePatchInspection::DialogueTimerCall(dialogueCode, dialogue, timerTarget);
            if (timer && IsEngineData(timer->multiplier, sizeof(float)) && *reinterpret_cast<const float*>(timer->multiplier) == -1.0f)
                resolved.dialogueTimer = dialogue + timer->offset;
        }
        if (!resolved.dialogueTimer) Fail("unrecognized dialogue timer decrement");
        spdlog::info("[Runtime] dialogue close timer: +0x{:X}", resolved.dialogueTimer - dialogue);
        resolved.processMessages = REL::RelocationID(79945, 82082).address();
        resolved.advanceMovies = REL::RelocationID(79946, 82083).address();
        resolved.getConsole = REL::RelocationID(52063, 52950).address();
        const auto job = REL::RelocationID(38088, 39042).address();
        inspecting = "UI job pause guard and UI/console calls";
        auto uiJob = RuntimePatchInspection::InspectUIJob(Function(job), job,
            resolved.processMessages, resolved.advanceMovies, resolved.getConsole);
        if (!uiJob || !IsExecutable(uiJob->executeConsole)) Fail("unrecognized UI job control flow");
        resolved.uiJobBranch = job + uiJob->branchOffset;
        resolved.uiJobBranchLength = uiJob->branchLength;
        resolved.executeConsole = uiJob->executeConsole;
        std::memcpy(uiBranchBytes.data(), reinterpret_cast<const void*>(resolved.uiJobBranch), resolved.uiJobBranchLength);
        sites = resolved;
        prepared = true;
        inspecting.clear();
        spdlog::info("[Runtime] All instruction patch sites validated for Skyrim {}", REL::Module::get().version().string());
    }

    const Sites& Get()
    {
        if (!prepared) Fail("runtime hooks used before preflight");
        return sites;
    }

    void RequireCall(std::uintptr_t address)
    {
        if (*reinterpret_cast<const std::uint8_t*>(address) != 0xE8) Fail("a call site changed after preflight");
    }

    void DisableUIJob()
    {
        const auto& current = Get();
        if (std::memcmp(uiBranchBytes.data(), reinterpret_cast<const void*>(current.uiJobBranch), current.uiJobBranchLength))
            Fail("another plugin changed the UI job after preflight");
        if (current.uiJobBranchLength == 2) {
            REL::safe_write(current.uiJobBranch, std::uint8_t{0xEB});
        } else {
            // JNE rel32 -> NOP; JMP rel32 keeps the original destination and six-byte footprint.
            const std::array<std::uint8_t, 2> unconditional{0x90, 0xE9};
            REL::safe_write(current.uiJobBranch, unconditional.data(), unconditional.size());
        }
    }

    std::size_t InputSlot(std::size_t legacySlot)
    {
        return legacySlot + (REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? RE::PlayerInputHandler::kAE1799AddedVFuncCount : 0);
    }
}
