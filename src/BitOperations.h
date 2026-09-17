#pragma once

// Defines the UI-independent byte transformations exposed by the bit-operation
// tool. Keeping this contract independent from Win32 and D2D makes every numeric
// boundary rule directly testable by the console test project.

#include <cstdint>

// Each operation transforms one byte at a time. Arithmetic intentionally uses
// modulo-256 semantics, matching the storage width of the binary editor.
enum class BitOperation {
    And,
    Or,
    Xor,
    Not,
    ShiftLeft,
    ShiftRight,
    RotateLeft,
    RotateRight,
    Add,
    Subtract,
    Multiply,
    Divide
};

// NOT is the only unary operation; every other operation consumes the operand
// entered in the dialog.
[[nodiscard]] bool BitOperationRequiresOperand(BitOperation operation) noexcept;
// Shift and rotation counts are restricted to one byte's bit width (0 through 7).
[[nodiscard]] bool BitOperationUsesBitCount(BitOperation operation) noexcept;
// Applies one transformation. False reports an invalid bit count or division by
// zero and leaves output equal to input so callers cannot consume a partial value.
[[nodiscard]] bool ApplyBitOperation(std::uint8_t input, BitOperation operation,
                                     std::uint8_t operand, std::uint8_t& output) noexcept;
