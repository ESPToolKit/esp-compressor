#pragma once

#include "../types/compression_types.h"
#include "../util/memory_buffer.h"

#include <cstddef>
#include <cstdint>

CompressionError compressLzLiteBlock(
    const uint8_t *input,
    size_t inputSize,
    size_t windowSize,
    MemoryBuffer &output
) noexcept;

CompressionError decompressLzLiteBlock(
    const uint8_t *input,
    size_t inputSize,
    size_t expectedOutputSize,
    MemoryBuffer &output
) noexcept;
