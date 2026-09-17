// Implements deterministic 8-bit transformations for the bit-operation tool.
// No locale, window, or document state enters this module.

#include "BitOperations.h"

bool BitOperationRequiresOperand(BitOperation operation) noexcept {
    return operation != BitOperation::Not;
}

bool BitOperationUsesBitCount(BitOperation operation) noexcept {
    return operation == BitOperation::ShiftLeft || operation == BitOperation::ShiftRight ||
           operation == BitOperation::RotateLeft || operation == BitOperation::RotateRight;
}

bool ApplyBitOperation(std::uint8_t input, BitOperation operation,
                       std::uint8_t operand, std::uint8_t& output) noexcept {
    // Start with a safe identity result. Validation failures therefore never
    // expose an uninitialized or misleading preview value.
    output = input;
    if (BitOperationUsesBitCount(operation) && operand > 7) return false;

    switch (operation) {
    case BitOperation::And:
        output = static_cast<std::uint8_t>(input & operand);
        return true;
    case BitOperation::Or:
        output = static_cast<std::uint8_t>(input | operand);
        return true;
    case BitOperation::Xor:
        output = static_cast<std::uint8_t>(input ^ operand);
        return true;
    case BitOperation::Not:
        output = static_cast<std::uint8_t>(~input);
        return true;
    case BitOperation::ShiftLeft:
        output = static_cast<std::uint8_t>(static_cast<unsigned int>(input) << operand);
        return true;
    case BitOperation::ShiftRight:
        output = static_cast<std::uint8_t>(static_cast<unsigned int>(input) >> operand);
        return true;
    case BitOperation::RotateLeft:
        if (operand != 0) {
            output = static_cast<std::uint8_t>((static_cast<unsigned int>(input) << operand) |
                (static_cast<unsigned int>(input) >> (8u - operand)));
        }
        return true;
    case BitOperation::RotateRight:
        if (operand != 0) {
            output = static_cast<std::uint8_t>((static_cast<unsigned int>(input) >> operand) |
                (static_cast<unsigned int>(input) << (8u - operand)));
        }
        return true;
    case BitOperation::Add:
        output = static_cast<std::uint8_t>(static_cast<unsigned int>(input) + operand);
        return true;
    case BitOperation::Subtract:
        output = static_cast<std::uint8_t>(static_cast<unsigned int>(input) - operand);
        return true;
    case BitOperation::Multiply:
        output = static_cast<std::uint8_t>(static_cast<unsigned int>(input) * operand);
        return true;
    case BitOperation::Divide:
        if (operand == 0) return false;
        output = static_cast<std::uint8_t>(input / operand);
        return true;
    }

    // The switch is exhaustive for current enum values. Retain a defensive
    // failure path for corrupted or future values crossing a binary boundary.
    return false;
}
