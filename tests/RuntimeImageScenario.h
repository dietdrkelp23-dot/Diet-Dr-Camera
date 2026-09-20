#pragma once

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
        if (name == "camera-chain" || name == "furniture-chain" ||
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
        try { DietDrCamera::RuntimeHooks::Prepare(); }
        catch (const std::runtime_error& error) {
            if (expectedError_.empty() || std::string_view(error.what()).find(expectedError_) == std::string_view::npos) throw;
            rejected = true;
        }
        if (rejected != !expectedError_.empty()) throw std::runtime_error("unsafe scenario was accepted");
        for (const auto& snapshot : snapshots_) {
            if (std::memcmp(snapshot.bytes.data(), reinterpret_cast<const void*>(snapshot.address), snapshot.bytes.size()))
                throw std::runtime_error("preflight changed a patch site");
        }
        if (name_ == "ui-job-install" || name_ == "call-changed-after-preflight" ||
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
            if (name_ == "ui-job-install") {
                if (sites.uiJobBranchLength != 2 || snapshots_.back().bytes[0] != 0x75)
                    throw std::runtime_error("unexpected native UI branch");
                DietDrCamera::RuntimeHooks::DisableUIJob();
                // Only the opcode may change; its displacement/destination
                // and every other inspected byte must remain intact.
                snapshots_.back().bytes[0] = 0xEB;
            } else if (name_ == "call-changed-after-preflight") {
                REL::safe_write(sites.cameraUpdate, std::uint8_t{0x90});
                snapshots_.front().bytes[0] = 0x90;
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
    Allocation stub_;
    std::vector<Snapshot> snapshots_;
};
