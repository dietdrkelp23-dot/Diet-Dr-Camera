#pragma once

#include "Hooks/RuntimePatchInspection.h"
#include <array>
#include <limits>
#include <string_view>
#include <vector>

// Mutate only the private image mapped by RuntimeImageCheck. No engine code runs.
class RuntimeImageScenario
{
public:
    RuntimeImageScenario(std::uint8_t* image, std::size_t imageSize, REL::Version version, std::string_view name) :
        name_(name)
    {
        if (name == "clean") return;
        // Each version below was inspected with an independent disassembler
        // against a Steam-manifest-verified executable. Do not infer coverage
        // for GOG or a new release from the nearest Steam version.
        const std::array reviewed{
            REL::Version{1,5,3,0}, REL::Version{1,5,16,0}, REL::Version{1,5,23,0},
            REL::Version{1,5,39,0}, REL::Version{1,5,50,0}, REL::Version{1,5,53,0},
            REL::Version{1,5,62,0}, REL::Version{1,5,73,0}, REL::Version{1,5,80,0},
            REL::Version{1,5,97,0}, REL::Version{1,6,317,0}, REL::Version{1,6,318,0},
            REL::Version{1,6,323,0}, REL::Version{1,6,342,0}, REL::Version{1,6,353,0},
            REL::Version{1,6,629,0}, REL::Version{1,6,640,0}, REL::Version{1,6,1130,0},
            REL::Version{1,6,1170,0}, REL::Version{1,7,99,0}, REL::Version{1,7,104,0}
        };
        if (std::find(reviewed.begin(), reviewed.end(), version) == reviewed.end())
            throw std::runtime_error("scenario requires a reviewed executable");
        const auto camera = REL::RelocationID(49852,50784).address() + 0x1A6;
        const auto furniture = REL::RelocationID(17034,17420).address() + (REL::Module::IsAE() ? 0x2A2 : 0x295);
        const auto main = REL::RelocationID(35565,36564).address() +
            (version >= REL::Version{1,7,99,0} ? 0xAF1 : version >= REL::Version{1,6,1130,0} ?
                0xADF : (REL::Module::IsAE() ? 0xADA : 0x61A));
        const auto dialogue = REL::RelocationID(36540,37541).address() + (REL::Module::IsAE() ? 0x6E8 : 0x4F9);
        const auto ui = REL::RelocationID(38088,39042).address() + 0xB;
        for (const auto address : {camera, furniture, main, dialogue, ui}) {
            const auto base = reinterpret_cast<std::uintptr_t>(image);
            if (address < base || address - base > imageSize || imageSize - (address - base) < 16)
                throw std::runtime_error("scenario site outside image");
        }
        if (name == "caster-owned-entry" || name == "caster-entry-conflict") {
            caster_ = REL::RelocationID(32270,33007).address();
            Snapshot snapshot{caster_, {}};
            std::memcpy(snapshot.bytes.data(), reinterpret_cast<const void*>(caster_), snapshot.bytes.size());
            snapshots_.push_back(snapshot);
        } else if (name.starts_with("main-entry-detour")) {
            const auto entry = REL::RelocationID(35565,36564).address();
            const auto base = reinterpret_cast<std::uintptr_t>(image);
            if (entry < base || entry - base > imageSize || imageSize - (entry - base) < 32)
                throw std::runtime_error("entry detour fixture outside image");
            // Like an entry-hook library, replace whole instructions and pad
            // the remainder with NOPs. Never execute this synthetic callback.
            std::size_t overwritten = 14;
            for (; overwritten <= 32; ++overwritten)
                if (DietDrCamera::RuntimePatchInspection::Decode(
                    {reinterpret_cast<const std::uint8_t*>(entry), overwritten}, entry)) break;
            if (overwritten > 32) throw std::runtime_error("cannot decode entry detour fixture prologue");
            if (version == REL::Version{1,7,104,0} && overwritten != 19)
                throw std::runtime_error("entry detour fixture differs from the reported 1.7.104 prologue");
            stub_.address = VirtualAlloc(nullptr,0x1000,MEM_RESERVE | MEM_COMMIT,PAGE_READWRITE);
            if (!stub_.address) throw std::runtime_error("cannot allocate entry detour callback fixture");
            // 0x60 is not an x64 opcode. Put it in the pointer's low byte so
            // the old decoder fails deterministically, regardless of ASLR.
            auto* callback = static_cast<std::uint8_t*>(stub_.address)+0x60;
            *callback = 0xC3;
            auto target = reinterpret_cast<std::uintptr_t>(callback);
            if (name == "main-entry-detour-nonexec") {
                expectedError_ = "entry detour target is not executable code outside Skyrim";
            } else if (name == "main-entry-detour-engine-target") {
                target = REL::RelocationID(49880,50813).address();
                expectedError_ = "entry detour target is not executable code outside Skyrim";
            } else {
                DWORD protection{};
                if (!VirtualProtect(stub_.address,0x1000,PAGE_EXECUTE_READ,&protection))
                    throw std::runtime_error("cannot protect entry detour callback fixture");
                if (name == "main-entry-detour-call-conflict") {
                    RedirectCall(main, REL::RelocationID(49880,50813).address());
                    expectedError_ = "missing or ambiguous ScrapHeap::KeepPages call";
                } else if (name != "main-entry-detour") throw std::runtime_error("unknown entry detour scenario");
            }
            std::vector<std::uint8_t> patch(overwritten,0x90);
            const std::array<std::uint8_t,6> jump{0xFF,0x25,0,0,0,0};
            std::memcpy(patch.data(),jump.data(),jump.size());
            std::memcpy(patch.data()+jump.size(),&target,sizeof(target));
            REL::safe_write(entry,patch.data(),patch.size());
            for (const auto address : {entry,entry+16}) {
                Snapshot snapshot{address,{}};
                std::memcpy(snapshot.bytes.data(),reinterpret_cast<const void*>(address),snapshot.bytes.size());
                snapshots_.push_back(snapshot);
            }
        } else if (name == "camera-chain" || name == "furniture-chain" ||
            name == "main-call-conflict" || name == "dialogue-call-conflict") {
            const auto first = (reinterpret_cast<std::uintptr_t>(image) + imageSize + 0xFFFF) & ~std::uintptr_t{0xFFFF};
            for (std::uintptr_t offset = 0; offset < 0x10000000 && !stub_.address; offset += 0x10000)
                stub_.address = VirtualAlloc(reinterpret_cast<void*>(first + offset), 0x1000,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!stub_.address) throw std::runtime_error("cannot allocate a nearby test callback");
            *static_cast<std::uint8_t*>(stub_.address) = 0xC3;
            DWORD previousProtection{};
            if (!VirtualProtect(stub_.address, 0x1000, PAGE_EXECUTE_READ, &previousProtection))
                throw std::runtime_error("cannot protect test callback");
            const auto call = name == "camera-chain" ? camera : name == "furniture-chain" ? furniture :
                name == "main-call-conflict" ? main : dialogue;
            RedirectCall(call, reinterpret_cast<std::uintptr_t>(stub_.address));
            if (name == "main-call-conflict") expectedError_ = "missing or ambiguous ScrapHeap::KeepPages call";
            if (name == "dialogue-call-conflict") expectedError_ = "unrecognized dialogue timer decrement";
        } else if (name == "camera-wrong-target") {
            RedirectCall(camera, REL::RelocationID(49880,50813).address());
            expectedError_ = "missing or ambiguous TESCamera::Update call";
        } else if (name == "ui-driver-conflict") {
            if (*reinterpret_cast<const std::uint8_t*>(ui) != 0x75)
                throw std::runtime_error("unexpected UI branch in scenario fixture");
            const std::uint8_t jump = 0xEB;
            REL::safe_write(ui, &jump, 1);
            expectedError_ = "the UI job was already disabled by another patch";
        } else if (name != "ui-job-install" && name != "call-changed-after-preflight" &&
            name != "call-target-changed-after-preflight" &&
            name != "call-install-chain" && name != "call-install-nonexec" && name != "call-install-unvalidated" &&
            name != "ui-changed-after-preflight") throw std::runtime_error("unknown runtime image scenario");
        for (const auto address : {camera, furniture, main, dialogue, ui}) {
            Snapshot snapshot{address, {}};
            std::memcpy(snapshot.bytes.data(), reinterpret_cast<const void*>(address), snapshot.bytes.size());
            snapshots_.push_back(snapshot);
        }
    }

    RuntimeImageScenario(const RuntimeImageScenario&) = delete;
    RuntimeImageScenario& operator=(const RuntimeImageScenario&) = delete;

    bool Prepare()
    {
        bool rejected = false;
        try {
            DietDrCamera::RuntimeHooks::Prepare();
        }
        catch (const std::runtime_error& error) {
            if (expectedError_.empty() || std::string_view(error.what()).find(expectedError_) == std::string_view::npos) throw;
            rejected = true;
        }
        if (rejected != !expectedError_.empty()) throw std::runtime_error("unsafe scenario was accepted");
        for (const auto& snapshot : snapshots_) {
            if (std::memcmp(snapshot.bytes.data(), reinterpret_cast<const void*>(snapshot.address), snapshot.bytes.size()))
                throw std::runtime_error("preflight changed a patch site");
        }
        if (caster_) {
            using Bytes = std::array<std::uint8_t, 6>;
            Bytes original{};
            std::memcpy(original.data(), reinterpret_cast<const void*>(caster_), original.size());
            const Bytes disabled{0x31,0xC0,0xC3,0x90,0x90,0x90};
            const auto divert = DietDrCamera::RuntimePatchInspection::RelativeJump6(caster_, caster_ + 0x100);
            if (!divert) throw std::runtime_error("invalid caster diversion fixture");
            Bytes current = original;
            // Exercise normal toggles and the temporary restore/rearm around
            // a filtered cast. Never execute the modified engine instructions.
            for (const auto& next : {disabled, *divert, original, *divert, original}) {
                if (!DietDrCamera::RuntimeHooks::TryReplaceCode6(caster_, current, next))
                    throw std::runtime_error("owned caster transition was refused");
                current = next;
                if (std::memcmp(reinterpret_cast<const void*>(caster_), current.data(), current.size()) ||
                    std::memcmp(reinterpret_cast<const void*>(caster_+6), snapshots_.front().bytes.data()+6, 10))
                    throw std::runtime_error("caster transition changed the wrong bytes");
                if (name_ == "caster-entry-conflict") {
                    auto foreign = current;
                    foreign[4] ^= 1; // Retain the jump/return opcode; change its body.
                    REL::safe_write(caster_, foreign.data(), foreign.size());
                    if (DietDrCamera::RuntimeHooks::TryReplaceCode6(caster_, current, original) ||
                        std::memcmp(reinterpret_cast<const void*>(caster_), foreign.data(), foreign.size()))
                        throw std::runtime_error("a foreign caster patch was overwritten");
                    REL::safe_write(caster_, current.data(), current.size());
                }
            }
            for (const auto& snapshot : snapshots_)
                if (std::memcmp(snapshot.bytes.data(), reinterpret_cast<const void*>(snapshot.address), snapshot.bytes.size()))
                    throw std::runtime_error("caster ownership check did not restore its private fixture");
        }
        if (name_ == "ui-job-install" || name_ == "call-changed-after-preflight" ||
            name_ == "call-target-changed-after-preflight" ||
            name_ == "call-install-chain" || name_ == "call-install-nonexec" || name_ == "call-install-unvalidated" ||
            name_ == "ui-changed-after-preflight") {
            const auto& sites = DietDrCamera::RuntimeHooks::Get();
            const auto expectRejected = [](auto action, std::string_view expected) {
                try { action(); }
                catch (const std::runtime_error& error) {
                    if (std::string_view(error.what()).find(expected) != std::string_view::npos) return;
                    throw;
                }
                throw std::runtime_error("changed patch site was accepted");
            };
            if (name_ == "call-install-chain") {
                auto& trampoline = SKSE::GetTrampoline();
                trampoline.create(128, reinterpret_cast<void*>(sites.cameraUpdate));
                const auto target = [&] {
                    std::int32_t displacement{};
                    std::memcpy(&displacement, reinterpret_cast<void*>(sites.cameraUpdate+1), sizeof(displacement));
                    return sites.cameraUpdate+5+displacement;
                };
                const auto native = target();
                if (DietDrCamera::RuntimeHooks::InstallCall(sites.cameraUpdate, &FirstCallback) != native)
                    throw std::runtime_error("call installation lost the native callback");
                const auto first = target();
                if (DietDrCamera::RuntimeHooks::InstallCall(sites.cameraUpdate, &SecondCallback) != first)
                    throw std::runtime_error("second DDC camera hook lost the first callback");
                DietDrCamera::RuntimeHooks::RequireCall(sites.cameraUpdate);
                std::memcpy(snapshots_.front().bytes.data(), reinterpret_cast<void*>(sites.cameraUpdate), 5);
            } else if (name_ == "call-install-nonexec") {
                expectRejected([&] { DietDrCamera::RuntimeHooks::InstallCall(sites.cameraUpdate, std::uintptr_t{0}); },
                    "hook callback is not executable");
            } else if (name_ == "call-install-unvalidated") {
                expectRejected([&] { DietDrCamera::RuntimeHooks::InstallCall(sites.cameraUpdate+1, &FirstCallback); },
                    "call site was not validated");
            } else if (name_ == "ui-job-install") {
                if (sites.uiJobBranchLength != 2 || snapshots_.back().bytes[0] != 0x75)
                    throw std::runtime_error("unexpected native UI branch");
                DietDrCamera::RuntimeHooks::DisableUIJob();
                // Only the opcode may change; its displacement/destination
                // and every other inspected byte must remain intact.
                snapshots_.back().bytes[0] = 0xEB;
            } else if (name_ == "call-changed-after-preflight" || name_ == "call-target-changed-after-preflight") {
                if (name_ == "call-changed-after-preflight") {
                    REL::safe_write(sites.cameraUpdate, std::uint8_t{0x90});
                    snapshots_.front().bytes[0] = 0x90;
                } else {
                    RedirectCall(sites.cameraUpdate, REL::RelocationID(49880,50813).address());
                    std::memcpy(snapshots_.front().bytes.data(), reinterpret_cast<void*>(sites.cameraUpdate), 5);
                }
                expectRejected([&] { DietDrCamera::RuntimeHooks::RequireCall(sites.cameraUpdate); },
                    "a call site changed after preflight");
            } else {
                snapshots_.back().bytes[1] ^= 1;
                REL::safe_write(sites.uiJobBranch + 1, snapshots_.back().bytes[1]);
                expectRejected([] { DietDrCamera::RuntimeHooks::DisableUIJob(); },
                    "another plugin changed the UI job after preflight");
            }
            for (const auto& snapshot : snapshots_)
                if (std::memcmp(snapshot.bytes.data(), reinterpret_cast<const void*>(snapshot.address), snapshot.bytes.size()))
                    throw std::runtime_error("unexpected write during patch installation check");
        }
        if (name_ != "clean") std::cout << "Runtime scenario passed: " << name_ <<
            (rejected ? " (expected rejection; patch sites unchanged)\n" : " (checks passed)\n");
        return !rejected;
    }

private:
    static bool FirstCallback() { return false; }
    static bool SecondCallback() { return true; }

    static void RedirectCall(std::uintptr_t call, std::uintptr_t target)
    {
        if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8)
            throw std::runtime_error("unexpected call opcode in scenario fixture");
        const auto distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(call + 5);
        if (distance < std::numeric_limits<std::int32_t>::min() || distance > std::numeric_limits<std::int32_t>::max())
            throw std::runtime_error("test callback outside rel32 range");
        const auto displacement = static_cast<std::int32_t>(distance);
        REL::safe_write(call + 1, displacement);
    }

    struct Snapshot { std::uintptr_t address; std::array<std::uint8_t, 16> bytes; };
    struct Allocation {
        void* address{};
        ~Allocation() { if (address) VirtualFree(address, 0, MEM_RELEASE); }
    };
    std::string_view name_;
    std::string_view expectedError_;
    std::uintptr_t caster_{};
    Allocation stub_;
    std::vector<Snapshot> snapshots_;
};
