#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace DietDrCamera::RuntimePatchInspection
{
    struct Instruction
    {
        std::size_t offset;
        std::size_t length;
        std::uint8_t opcode;
        std::uint8_t opcode2;
        std::uintptr_t target;
    };

    using Instructions = std::vector<Instruction>;
    // Recognize only FF 25 00 00 00 00 followed by its inline 64-bit target
    // at the function entry. Decode excludes that pointer from instructions;
    // the caller must validate the destination before accepting live hooks.
    std::optional<std::uintptr_t> InlineEntryJumpTarget(std::span<const std::uint8_t> code);
    // E9 rel32 plus one NOP; reject destinations that cannot be represented.
    std::optional<std::array<std::uint8_t, 6>> RelativeJump6(std::uintptr_t address, std::uintptr_t target);
    std::optional<Instructions> Decode(std::span<const std::uint8_t> code, std::uintptr_t address);
    std::optional<std::size_t> UniqueCall(const Instructions& instructions, std::uintptr_t target);
    // Validate a known call without decoding unrelated code after the site.
    std::optional<Instruction> CallAt(std::span<const std::uint8_t> code, std::uintptr_t address, std::size_t offset);

    struct TimerCall { std::size_t offset; std::uintptr_t multiplier; };
    std::optional<TimerCall> DialogueTimerCall(std::span<const std::uint8_t> code,
        std::uintptr_t address, std::uintptr_t target);

    struct UIJob
    {
        std::size_t branchOffset;
        std::size_t branchLength;
        std::uintptr_t executeConsole;
        bool alreadyDisabled;
    };

    std::optional<UIJob> InspectUIJob(std::span<const std::uint8_t> code, std::uintptr_t address,
        std::uintptr_t processMessages, std::uintptr_t advanceMovies, std::uintptr_t getConsole);
}
