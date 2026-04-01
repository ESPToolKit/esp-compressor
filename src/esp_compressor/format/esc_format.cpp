#include "esc_format.h"

#include <cstring>

namespace {

void writeLe32(uint8_t *out, uint32_t value) {
	out[0] = static_cast<uint8_t>(value & 0xFF);
	out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
	out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
	out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void writeLe64(uint8_t *out, uint64_t value) {
	for (size_t i = 0; i < 8; ++i) {
		out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
	}
}

uint32_t readLe32(const uint8_t *in) {
	return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
	       (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t readLe64(const uint8_t *in) {
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i) {
		value |= static_cast<uint64_t>(in[i]) << (i * 8);
	}
	return value;
}

} // namespace

uint32_t espCompressorCrc32(const uint8_t *data, size_t size) noexcept {
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i) {
		crc ^= static_cast<uint32_t>(data[i]);
		for (int bit = 0; bit < 8; ++bit) {
			const bool lsb = (crc & 1u) != 0;
			crc >>= 1u;
			if (lsb) {
				crc ^= 0xEDB88320u;
			}
		}
	}
	return ~crc;
}

CompressionError encodeEscHeader(const EscHeader &header, MemoryBuffer &out) noexcept {
	out.clear();
	if (!out.resize(kEscEncodedHeaderSize)) {
		return CompressionError::NoMemory;
	}

	uint8_t *bytes = out.data();
	bytes[0] = 'E';
	bytes[1] = 'S';
	bytes[2] = 'C';
	bytes[3] = '1';
	bytes[4] = header.version;
	bytes[5] = header.flags;
	bytes[6] = header.algorithm;
	bytes[7] = static_cast<uint8_t>(kEscEncodedHeaderSize);
	writeLe64(bytes + 8, header.originalSize);
	writeLe32(bytes + 16, header.blockSize);
	writeLe32(bytes + 20, 0);
	writeLe32(bytes + 20, espCompressorCrc32(bytes, 20));
	return CompressionError::Ok;
}

CompressionError decodeEscHeader(const uint8_t *data, size_t size, EscHeader &header) noexcept {
	if (!data || size < kEscEncodedHeaderSize) {
		return CompressionError::CorruptData;
	}
	if (data[0] != 'E' || data[1] != 'S' || data[2] != 'C' || data[3] != '1') {
		return CompressionError::CorruptData;
	}
	if (data[7] != kEscEncodedHeaderSize) {
		return CompressionError::CorruptData;
	}
	const uint32_t expectedCrc = readLe32(data + 20);
	uint8_t scratch[kEscEncodedHeaderSize];
	std::memcpy(scratch, data, kEscEncodedHeaderSize);
	writeLe32(scratch + 20, 0);
	if (espCompressorCrc32(scratch, 20) != expectedCrc) {
		return CompressionError::CorruptData;
	}

	header.version = data[4];
	header.flags = data[5];
	header.algorithm = data[6];
	header.originalSize = readLe64(data + 8);
	header.blockSize = readLe32(data + 16);
	header.headerCrc = expectedCrc;
	return CompressionError::Ok;
}

CompressionError encodeEscBlockHeader(const EscBlockHeader &header, uint8_t *out, size_t size) noexcept {
	if (!out || size < kEscEncodedBlockHeaderSize) {
		return CompressionError::InvalidArgument;
	}
	out[0] = header.flags;
	writeLe32(out + 1, header.originalSize);
	writeLe32(out + 5, header.storedSize);
	writeLe32(out + 9, header.crc32);
	return CompressionError::Ok;
}

CompressionError decodeEscBlockHeader(const uint8_t *data, size_t size, EscBlockHeader &header) noexcept {
	if (!data || size < kEscEncodedBlockHeaderSize) {
		return CompressionError::CorruptData;
	}
	header.flags = data[0];
	header.originalSize = readLe32(data + 1);
	header.storedSize = readLe32(data + 5);
	header.crc32 = readLe32(data + 9);
	return CompressionError::Ok;
}
