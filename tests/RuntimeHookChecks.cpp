#include "Hooks/CameraStateEntry.h"
#include "Hooks/RuntimePatchInspection.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using namespace DietDrCamera::RuntimePatchInspection;

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void Call(std::vector<std::uint8_t>& code, std::uintptr_t address, std::uintptr_t target)
    {
        const auto displacement = static_cast<std::int32_t>(target - (address + code.size() + 5));
        code.push_back(0xE8);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&displacement);
        code.insert(code.end(), bytes, bytes + 4);
    }

    std::vector<std::uint8_t> Job(bool nearBranch)
    {
        std::vector<std::uint8_t> code{0x48,0x83,0xEC,0x28, 0x80,0x3D,0,0,0,0,0};
        if (nearBranch) code.insert(code.end(), {0x0F,0x85,0x25,0,0,0});
        else code.insert(code.end(), {0x75,0x25});
        code.insert(code.end(), {0x48,0x8B,0x0D,0,0,0,0});
        Call(code, 0x1000, 0x2000);
        code.insert(code.end(), {0x48,0x8B,0x0D,0,0,0,0});
        Call(code, 0x1000, 0x3000);
        Call(code, 0x1000, 0x4000);
        code.insert(code.end(), {0x48,0x8B,0xC8});
        Call(code, 0x1000, 0x5000);
        code.insert(code.end(), {0x48,0x83,0xC4,0x28,0xC3});
        return code;
    }

    void SyntheticChecks()
    {
        std::vector<std::uint8_t> code{0x48,0xB8,0xE8,0,0,0,0,0,0,0};
        Call(code,0x1000,0x2000);
        Call(code,0x1000,0x0800);
        code.push_back(0xC3);
        auto decoded=Decode(code,0x1000);
        Check(decoded.has_value(), "decode complete instructions");
        Check(UniqueCall(*decoded,0x2000)==10, "ignore E8 inside an immediate");
        Check(UniqueCall(*decoded,0x800)==15, "resolve negative call displacement");
        Check(!UniqueCall(*decoded,0x9999), "reject absent call target");
        code.pop_back();
        Call(code,0x1000,0x2000);
        Check(!UniqueCall(*Decode(code,0x1000),0x2000), "reject ambiguous call targets");
        const std::array<std::uint8_t,4> truncated{0xE8,0,0,0};
        Check(!Decode(truncated,0x1000), "reject truncated call");
        const std::array<std::uint8_t,2> truncatedImmediate{0x48,0xB8};
        Check(!Decode(truncatedImmediate,0x1000), "reject truncated immediate");

        // Known SE/AE call sites must not depend on decoding unrelated trailing
        // compiler output. The call is still decoded from the function entry.
        std::vector<std::uint8_t> known{0x90};
        Call(known, 0x1000, 0x2000);
        known.insert(known.end(), {0x48, 0xB8}); // unrelated incomplete instruction
        Check(!Decode(known, 0x1000), "fixture must fail a whole-function scan");
        const auto established = CallAt(known, 0x1000, 1);
        Check(established && established->target == 0x2000, "validate established call without decoding its suffix");
        Check(!CallAt(code, 0x1000, 2), "known offset inside an immediate is not a call boundary");
        Check(!CallAt(known, 0x1000, known.size()), "reject out-of-range established site");

        std::vector<std::uint8_t> timerCode;
        Call(timerCode,0x1000,0x2000);
        timerCode.insert(timerCode.end(), {0xF3,0x0F,0x59,0x0D,0,0,0,0,0x49,0x8B,0x8E,0xF8,0,0,0});
        Call(timerCode,0x1000,0x2000);
        Call(timerCode,0x1000,0x2000);
        auto timer=DialogueTimerCall(timerCode,0x1000,0x2000);
        Check(timer && timer->offset==20 && timer->multiplier==0x100D, "select decrement among timer reset calls");
        timerCode[7]=0x58;
        Check(!DialogueTimerCall(timerCode,0x1000,0x2000), "reject different timer arithmetic");

        for (bool nearBranch : {false,true}) {
            auto job=Job(nearBranch);
            auto inspect=[&] {return InspectUIJob(job,0x1000,0x2000,0x3000,0x4000);};
            auto result=inspect();
            Check(result && result->branchOffset==11 && result->branchLength==(nearBranch?6:2) &&
                result->executeConsole==0x5000 && !result->alreadyDisabled, "recognize guarded UI and console sequence");
            if (nearBranch) {
                job[11]=0x90;
                job[12]=0xE9;
            } else job[11]=0xEB;
            const auto disabled=inspect();
            Check(disabled && disabled->alreadyDisabled && disabled->executeConsole==0x5000,
                "identify another UI driver's unconditional branch without accepting it as native");
            job=Job(nearBranch);
            job[nearBranch?12:11] ^= 1;
            Check(!inspect(), "reject inverted pause condition");
            job=Job(nearBranch);
            job[nearBranch?13:12] -= 1;
            Check(!inspect(), "reject branch landing inside an instruction");
            job=Job(nearBranch);
            Check(!InspectUIJob(job,0x1000,0x2001,0x3000,0x4000), "reject incorrect UI function");
            job=Job(nearBranch);
            job[job.size()-11]=0xD0;
            Check(!inspect(), "reject incorrect console this pointer");
            job=Job(nearBranch);
            // Equivalent MOV RCX,RAX encoding emitted by other compiler builds.
            job[job.size()-12]=0x89;
            job[job.size()-11]=0xC1;
            Check(inspect().has_value(), "accept equivalent console this-pointer encoding");
            job=Job(nearBranch);
            // One alignment NOP in the guarded block, with the branch adjusted.
            job.insert(job.begin() + (nearBranch ? 17 : 13), 0x90);
            job[nearBranch ? 13 : 12] += 1;
            // Inserting a byte also shifts each relative call's origin.
            for (std::size_t offset = nearBranch ? 25 : 21; offset < job.size(); ++offset) {
                if (job[offset] != 0xE8) continue;
                std::int32_t displacement{};
                std::memcpy(&displacement, job.data() + offset + 1, 4);
                --displacement;
                std::memcpy(job.data() + offset + 1, &displacement, 4);
                offset += 4;
            }
            Check(inspect().has_value(), "accept compiler alignment NOPs in UI job");
        }
    }

    void EntryJumpChecks()
    {
        // Exact CBPC entry detour/prologue from the 1.7.104 startup report.
        // The eight bytes at +6 are a pointer, not engine instructions.
        constexpr std::uintptr_t base = 0x7FF6ED740000 + 0x658870;
        constexpr std::uintptr_t target = 0x7FF6ED740000 + 0xCDFE00;
        std::vector<std::uint8_t> code{
            0xFF,0x25,0,0,0,0,0x10,0x42,0x92,0xDD,0xFE,0x7F,0,0,
            0x90,0x90,0x90,0x90,0x90,
            0x48,0xC7,0x84,0x24,0x28,0x01,0,0,0xFE,0xFF,0xFF,0xFF,
            0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,
            0x48,0x8D,0x6C,0x24,0x60,0x48,0x83,0xE5,0xC0,0x4C,0x8B,0xE1
        };
        const auto callOffset = code.size();
        Call(code, base, target);
        code.push_back(0xC3);
        const auto decoded = Decode(code, base);
        Check(decoded.has_value(), "decode the reported CBPC entry detour without interpreting its pointer as code");
        Check(InlineEntryJumpTarget(code) == 0x7FFEDD924210, "identify the reported CBPC callback address");
        Check(UniqueCall(*decoded, target) == callOffset, "find the native call after the CBPC entry detour");
        const auto call = CallAt(code, base, callOffset);
        Check(call && call->target == target, "validate an established call after an entry detour");

        std::uint64_t random = 0xDDC170104;
        for (unsigned trial = 0; trial < 4096; ++trial) {
            random = random * 6364136223846793005ULL + 1442695040888963407ULL;
            std::memcpy(code.data()+6, &random, sizeof(random));
            const auto varied = Decode(code, base);
            Check(varied && UniqueCall(*varied, target) == callOffset,
                "ASLR pointer bytes must not change native call discovery");
        }

        for (std::size_t size = 6; size < 14; ++size) {
            const auto truncated = std::span<const std::uint8_t>(code).first(size);
            Check(!InlineEntryJumpTarget(truncated) && !Decode(truncated, base), "reject a truncated inline jump pointer");
        }
        // A pointer that happens to encode a matching E8 call must never be
        // counted or accepted as a hook site, even if the real call is absent.
        code.resize(14);
        code[6] = 0xE8;
        const auto fakeDisplacement = static_cast<std::int32_t>(target - (base + 6 + 5));
        std::memcpy(code.data()+7, &fakeDisplacement, sizeof(fakeDisplacement));
        code[11] = 0x60;
        Check(!UniqueCall(*Decode(code, base), target), "ignore fake native calls inside the detour pointer");
        Check(!CallAt(code, base, 6), "an inline pointer cannot be an established call site");
        Call(code, base, target);
        Check(UniqueCall(*Decode(code, base), target) == 14, "retain a real call immediately after the detour pointer");
        Call(code, base, target);
        Check(!UniqueCall(*Decode(code, base), target), "reject ambiguous native calls after an entry detour");
        code[14] = 0x60;
        Check(!Decode(code, base), "do not skip invalid body instructions after an entry detour");

        code.resize(14);
        code[6] = 0x60;
        code[2] = 1;
        Check(!InlineEntryJumpTarget(code) && !Decode(code, base), "do not treat a different RIP displacement as inline data");
        code[2] = 0;
        code.insert(code.begin(), 0x90);
        Check(!InlineEntryJumpTarget(code) && !Decode(code, base), "limit inline detour recognition to the function entry");
    }

    void EngineChecks(const char* path)
    {
        std::ifstream file(path);
        auto root=nlohmann::json::parse(file);
        for (const auto& test : root.at("calls")) {
            const auto code=test.at("code").get<std::vector<std::uint8_t>>();
            const auto base=test.at("address").get<std::uintptr_t>();
            auto decoded=Decode(code,base);
            Check(decoded.has_value(), "decode actual engine function");
            std::optional<std::size_t> offset;
            if (test.at("name")=="dialogue timer") {
                auto timer=DialogueTimerCall(code,base,test.at("target"));
                if (timer) offset=timer->offset;
                Check(timer && timer->multiplier==test.at("multiplier").get<std::uintptr_t>() &&
                    test.at("multiplierValue").get<float>()==-1.0f, "validate timer decrement argument");
            } else {
                offset=UniqueCall(*decoded,test.at("target").get<std::uintptr_t>());
            }
            Check(offset && *offset==test.at("expectedOffset").get<std::size_t>(), "find actual engine call");
            std::cout << test.at("name").get<std::string>() << " +0x" << std::hex << *offset << '\n';
        }
        const auto& job=root.at("uiJob");
        auto result=InspectUIJob(job.at("code").get<std::vector<std::uint8_t>>(),job.at("address"),
            job.at("processMessages"),job.at("advanceMovies"),job.at("getConsole"));
        Check(result && result->branchOffset==job.at("expectedOffset").get<std::size_t>() &&
            result->executeConsole==job.at("executeConsole").get<std::uintptr_t>(), "validate actual UI job");
    }
}

int main(int argc, char** argv)
{
    try {
        {
            using namespace DietDrCamera::CameraStateEntry;
            constexpr std::uintptr_t resume = 0x7FF612345005;
            const auto gateway = Gateway(prologue, resume);
            Check(gateway.has_value(), "Accept reviewed camera SetState entry");
            const auto spill = Decode(std::span<const std::uint8_t>(*gateway).first(5), 0x1000);
            Check(spill && spill->size() == 1 && spill->front().length == 5,
                "Gateway must replay exactly one complete stack-home instruction");
            std::uintptr_t destination{};
            std::memcpy(&destination, gateway->data() + 11, sizeof(destination));
            Check(destination == resume && (*gateway)[5] == 0xFF && (*gateway)[6] == 0x25,
                "Gateway must jump to the full continuation address without clobbering registers");
            for (std::size_t i = 0; i < prologue.size(); ++i) {
                auto changed = prologue;
                changed[i] ^= 0x01;
                Check(!Gateway(changed, resume), "Changed camera entry was accepted for overwrite");
                Check(!Gateway(std::span<const std::uint8_t>(prologue).first(i), resume),
                    "Truncated camera entry was accepted");
            }
            Check(!Gateway(prologue, 0), "Null camera continuation was accepted");
        }
        SyntheticChecks();
        EntryJumpChecks();
        constexpr std::uintptr_t jump = 0x100000000;
        for (const auto displacement : {std::int64_t{-2147483648LL}, std::int64_t{-1}, std::int64_t{0}, std::int64_t{2147483647}}) {
            const auto target = static_cast<std::uintptr_t>(jump + 5 + displacement);
            const auto patch = RelativeJump6(jump, target);
            Check(patch && patch->back() == 0x90, "encode a reachable collision diversion");
            const auto decoded = Decode(*patch, jump);
            Check(decoded && decoded->front().target == target, "collision diversion retains the full target address");
        }
        Check(!RelativeJump6(jump, jump + 5 + 0x80000000ULL), "reject positive rel32 overflow");
        Check(!RelativeJump6(jump, jump + 5 - 0x80000001ULL), "reject negative rel32 overflow");
        Check(!RelativeJump6(std::numeric_limits<std::uintptr_t>::max(), 0), "reject instruction-address overflow");
        if (argc==2) EngineChecks(argv[1]);
        std::cout << "Runtime hook checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
