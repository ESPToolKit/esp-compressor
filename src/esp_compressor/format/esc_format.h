#pragma once

#include "../types/compression_types.h"
#include "../util/memory_buffer.h"

#include <cstddef>
#include <cstdint>

constexpr uint8_t kEscFormatVersion = 1;
constexpr uint8_t kEscAlgorithmLzLite = 1;
constexpr uint8_t kEscHeaderFlagUnknownOriginalSize = 0x01;
constexpr uint8_t kEscBlockFlagRaw = 0x01;
constexpr uint8_t kEscBlockFlagFinal = 0x02;
constexpr uint64_t kEscUnknownOriginalSize = UINT64_MAX;
constexpr size_t kEscEncodedHeaderSize = 24;
constexpr size_t kEscEncodedBlockHeaderSize = 13;

struct EscHeader {
	uint8_t version = kEscFormatVersion;
	uint8_t flags = 0;
	uint8_t algorithm = kEscAlgorithmLzLite;
	uint64_t originalSize = kEscUnknownOriginalSize;
	uint32_t blockSize = 0;
	uint32_t headerCrc = 0;
};

struct EscBlockHeader {
	uint8_t flags = 0;
	uint32_t originalSize = 0;
	uint32_t storedSize = 0;
	uint32_t crc32 = 0;
};

uint32_t espCompressorCrc32(const uint8_t *data, size_t size) noexcept;
CompressionError encodeEscHeader(const EscHeader &header, MemoryBuffer &out) noexcept;
CompressionError decodeEscHeader(const uint8_t *data, size_t size, EscHeader &header) noexcept;
CompressionError encodeEscBlockHeader(const EscBlockHeader &header, uint8_t *out, size_t size) noexcept;
CompressionError decodeEscBlockHeader(const uint8_t *data, size_t size, EscBlockHeader &header) noexcept;
