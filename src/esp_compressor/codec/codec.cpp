#include "codec.h"

#include <array>
#include <cstring>

namespace {

constexpr size_t kMinMatch = 3;
constexpr size_t kHashTableSize = 1024;

uint32_t hash3(const uint8_t *ptr) {
	const uint32_t value = static_cast<uint32_t>(ptr[0]) << 16 |
	                       static_cast<uint32_t>(ptr[1]) << 8 | static_cast<uint32_t>(ptr[2]);
	return (value * 2654435761u) >> 22;
}

bool appendLength(MemoryBuffer &buffer, size_t value) noexcept {
	while (value >= 255) {
		if (!buffer.appendByte(255)) {
			return false;
		}
		value -= 255;
	}
	return buffer.appendByte(static_cast<uint8_t>(value));
}

bool emitSequence(
    MemoryBuffer &output,
    const uint8_t *literalData,
    size_t literalLength,
    uint16_t matchOffset,
    size_t matchLength,
    bool hasMatch
) noexcept {
	const size_t encodedMatchLength = hasMatch ? (matchLength - kMinMatch) : 0;
	const uint8_t token = static_cast<uint8_t>(
	    ((literalLength < 15 ? literalLength : 15) << 4) |
	    (hasMatch ? (encodedMatchLength < 15 ? encodedMatchLength : 15) : 0)
	);
	if (!output.appendByte(token)) {
		return false;
	}
	if (literalLength >= 15 && !appendLength(output, literalLength - 15)) {
		return false;
	}
	if (literalLength != 0 && !output.append(literalData, literalLength)) {
		return false;
	}
	if (!hasMatch) {
		return true;
	}
	if (!output.appendByte(static_cast<uint8_t>(matchOffset & 0xFF)) ||
	    !output.appendByte(static_cast<uint8_t>((matchOffset >> 8) & 0xFF))) {
		return false;
	}
	if (encodedMatchLength >= 15 && !appendLength(output, encodedMatchLength - 15)) {
		return false;
	}
	return true;
}

size_t computeMatchLength(
    const uint8_t *input,
    size_t inputSize,
    size_t current,
    size_t candidate
) noexcept {
	size_t length = 0;
	while ((current + length) < inputSize && input[candidate + length] == input[current + length]) {
		++length;
	}
	return length;
}

CompressionError readExtendedLength(
    const uint8_t *input,
    size_t inputSize,
    size_t &inputOffset,
    size_t &value
) noexcept {
	uint8_t extra = 255;
	while (extra == 255) {
		if (inputOffset >= inputSize) {
			return CompressionError::CorruptData;
		}
		extra = input[inputOffset++];
		value += extra;
	}
	return CompressionError::Ok;
}

} // namespace

CompressionError compressLzLiteBlock(
    const uint8_t *input,
    size_t inputSize,
    size_t windowSize,
    MemoryBuffer &output
) noexcept {
	output.clear();
	if (!input && inputSize != 0) {
		return CompressionError::InvalidArgument;
	}

	if (inputSize == 0) {
		return CompressionError::Ok;
	}

	std::array<int32_t, kHashTableSize> table{};
	table.fill(-1);

	size_t anchor = 0;
	size_t index = 0;
	while (index + kMinMatch <= inputSize) {
		size_t matchLength = 0;
		size_t matchOffset = 0;

		if (index + kMinMatch <= inputSize) {
			const uint32_t slot = hash3(input + index) & (kHashTableSize - 1);
			const int32_t prev = table[slot];
			table[slot] = static_cast<int32_t>(index);
			if (prev >= 0) {
				const size_t candidate = static_cast<size_t>(prev);
				if (index > candidate && (index - candidate) <= windowSize &&
				    (index - candidate) <= 0xFFFFu &&
				    std::memcmp(input + candidate, input + index, kMinMatch) == 0) {
					matchLength = computeMatchLength(input, inputSize, index, candidate);
					matchOffset = index - candidate;
				}
			}
		}

		if (matchLength >= kMinMatch) {
			const size_t literalLength = index - anchor;
			if (!emitSequence(
			        output,
			        input + anchor,
			        literalLength,
			        static_cast<uint16_t>(matchOffset),
			        matchLength,
			        true
			    )) {
				return CompressionError::NoMemory;
			}

			for (size_t i = 1; i < matchLength; ++i) {
				if (index + i + kMinMatch <= inputSize) {
					const uint32_t slot = hash3(input + index + i) & (kHashTableSize - 1);
					table[slot] = static_cast<int32_t>(index + i);
				}
			}

			index += matchLength;
			anchor = index;
			continue;
		}

		++index;
	}

	const size_t literalLength = inputSize - anchor;
	if (!emitSequence(output, input + anchor, literalLength, 0, 0, false)) {
		return CompressionError::NoMemory;
	}

	return CompressionError::Ok;
}

CompressionError decompressLzLiteBlock(
    const uint8_t *input,
    size_t inputSize,
    size_t expectedOutputSize,
    MemoryBuffer &output
) noexcept {
	output.clear();
	if ((!input && inputSize != 0) || !output.resize(expectedOutputSize)) {
		return expectedOutputSize == 0 ? CompressionError::Ok : CompressionError::NoMemory;
	}

	size_t inputOffset = 0;
	size_t outputOffset = 0;
	while (inputOffset < inputSize) {
		const uint8_t token = input[inputOffset++];
		size_t literalLength = (token >> 4) & 0x0Fu;
		if (literalLength == 15) {
			CompressionError err = readExtendedLength(input, inputSize, inputOffset, literalLength);
			if (err != CompressionError::Ok) {
				return err;
			}
		}
		if (inputOffset + literalLength > inputSize || outputOffset + literalLength > expectedOutputSize) {
			return CompressionError::CorruptData;
		}
		if (literalLength != 0) {
			std::memcpy(output.data() + outputOffset, input + inputOffset, literalLength);
			inputOffset += literalLength;
			outputOffset += literalLength;
		}

		if (outputOffset == expectedOutputSize) {
			return inputOffset == inputSize ? CompressionError::Ok : CompressionError::CorruptData;
		}

		if (inputOffset + 2 > inputSize) {
			return CompressionError::CorruptData;
		}
		const size_t offset = static_cast<size_t>(input[inputOffset]) |
		                      (static_cast<size_t>(input[inputOffset + 1]) << 8);
		inputOffset += 2;
		if (offset == 0 || offset > outputOffset) {
			return CompressionError::CorruptData;
		}

		size_t matchLength = (token & 0x0Fu) + kMinMatch;
		if ((token & 0x0Fu) == 15) {
			CompressionError err = readExtendedLength(input, inputSize, inputOffset, matchLength);
			if (err != CompressionError::Ok) {
				return err;
			}
		}
		if (outputOffset + matchLength > expectedOutputSize) {
			return CompressionError::CorruptData;
		}

		for (size_t i = 0; i < matchLength; ++i) {
			output.data()[outputOffset + i] = output.data()[outputOffset - offset + i];
		}
		outputOffset += matchLength;
	}

	return outputOffset == expectedOutputSize ? CompressionError::Ok : CompressionError::CorruptData;
}
