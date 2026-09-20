#include "Hooks/RuntimePatchInspection.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
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
        SyntheticChecks();
        if (argc==2) EngineChecks(argv[1]);
        std::cout << "Runtime hook checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
