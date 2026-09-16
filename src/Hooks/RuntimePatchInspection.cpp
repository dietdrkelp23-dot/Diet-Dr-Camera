#include "Hooks/RuntimePatchInspection.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <hde64.h>

namespace DietDrCamera::RuntimePatchInspection
{
    std::optional<Instructions> Decode(std::span<const std::uint8_t> code, std::uintptr_t address)
    {
        Instructions result;
        for (std::size_t offset = 0; offset < code.size();) {
            // The decoder may read ahead. Padding keeps truncated instructions inside our buffer.
            std::array<std::uint8_t, 32> buffer{};
            std::memcpy(buffer.data(), code.data() + offset, std::min(buffer.size(), code.size() - offset));
            hde64s decoded{};
            const auto length = hde64_disasm(buffer.data(), &decoded);
            if (!length || (decoded.flags & F_ERROR) || length > code.size() - offset) return std::nullopt;
            std::uintptr_t target = 0;
            if (decoded.flags & F_RELATIVE) {
                const std::int64_t displacement = (decoded.flags & F_IMM8) ?
                    static_cast<std::int8_t>(decoded.imm.imm8) : static_cast<std::int32_t>(decoded.imm.imm32);
                target = static_cast<std::uintptr_t>(address + offset + length + displacement);
            }
            result.push_back({ offset, length, decoded.opcode, decoded.opcode2, target });
            offset += length;
        }
        return result;
    }

    std::optional<std::size_t> UniqueCall(const Instructions& instructions, std::uintptr_t target)
    {
        std::optional<std::size_t> result;
        for (const auto& instruction : instructions) {
            if (instruction.opcode != 0xE8 || instruction.length != 5 || instruction.target != target) continue;
            if (result) return std::nullopt;
            result = instruction.offset;
        }
        return result;
    }

    std::optional<TimerCall> DialogueTimerCall(std::span<const std::uint8_t> code,
        std::uintptr_t address, std::uintptr_t target)
    {
        const auto decoded = Decode(code, address);
        if (!decoded) return std::nullopt;
        std::optional<TimerCall> result;
        for (std::size_t i = 2; i < decoded->size(); ++i) {
            const auto& call = (*decoded)[i];
            const auto& loadThis = (*decoded)[i - 1];
            const auto& multiply = (*decoded)[i - 2];
            if (call.opcode != 0xE8 || call.length != 5 || call.target != target ||
                multiply.length != 8 || loadThis.length != 7) continue;
            const auto* mul = code.data() + multiply.offset;
            const auto* mov = code.data() + loadThis.offset;
            if (mul[0] != 0xF3 || mul[1] != 0x0F || mul[2] != 0x59 || mul[3] != 0x0D ||
                (mov[0] != 0x48 && mov[0] != 0x49) || mov[1] != 0x8B || (mov[2] & 0xF8) != 0x88) continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, mul + 4, sizeof(displacement));
            if (result) return std::nullopt;
            result = TimerCall{call.offset, address + multiply.offset + multiply.length + displacement};
        }
        return result;
    }

    std::optional<UIJob> InspectUIJob(std::span<const std::uint8_t> code, std::uintptr_t address,
        std::uintptr_t processMessages, std::uintptr_t advanceMovies, std::uintptr_t getConsole)
    {
        auto decoded = Decode(code, address);
        if (!decoded) return std::nullopt;
        const auto& ins = *decoded;
        std::optional<UIJob> result;
        for (std::size_t i = 1; i + 7 < ins.size(); ++i) {
            const auto& branch = ins[i];
            if (!((branch.opcode == 0x75 && branch.length == 2) ||
                (branch.opcode == 0x0F && branch.opcode2 == 0x85 && branch.length == 6))) continue;
            const auto& cmp = ins[i - 1];
            if (cmp.length != 7 || code[cmp.offset] != 0x80 || code[cmp.offset + 1] != 0x3D ||
                code[cmp.offset + 6] != 0) continue;
            // The guarded block must contain exactly the original UI/console tick sequence.
            const auto& msg = ins[i + 2];
            const auto& movie = ins[i + 4];
            const auto& console = ins[i + 5];
            const auto& moveThis = ins[i + 6];
            const auto& execute = ins[i + 7];
            if (ins[i + 1].opcode != 0x8B || ins[i + 3].opcode != 0x8B ||
                msg.opcode != 0xE8 || msg.target != processMessages ||
                movie.opcode != 0xE8 || movie.target != advanceMovies ||
                console.opcode != 0xE8 || console.target != getConsole ||
                moveThis.length != 3 || code[moveThis.offset] != 0x48 ||
                code[moveThis.offset + 1] != 0x8B || code[moveThis.offset + 2] != 0xC8 ||
                execute.opcode != 0xE8 || execute.length != 5 ||
                branch.target != address + execute.offset + execute.length) continue;
            if (result) return std::nullopt;
            result = UIJob{ branch.offset, branch.length, execute.target };
        }
        return result;
    }
}
