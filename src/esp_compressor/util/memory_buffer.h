#pragma once

#include <ESPBufferManager.h>

#include <cstddef>
#include <cstdint>

class MemoryBuffer {
  public:
	MemoryBuffer() = default;
	explicit MemoryBuffer(bool usePSRAMBuffers) : _usePSRAMBuffers(usePSRAMBuffers) {
	}

	~MemoryBuffer();

	MemoryBuffer(const MemoryBuffer &) = delete;
	MemoryBuffer &operator=(const MemoryBuffer &) = delete;

	MemoryBuffer(MemoryBuffer &&other) noexcept;
	MemoryBuffer &operator=(MemoryBuffer &&other) noexcept;

	void setUsePSRAMBuffers(bool enabled) {
		_usePSRAMBuffers = enabled;
	}

	bool reserve(size_t capacity) noexcept;
	bool resize(size_t size) noexcept;
	bool append(const uint8_t *data, size_t size) noexcept;
	bool appendByte(uint8_t value) noexcept;
	void clear() noexcept {
		_size = 0;
	}
	void release() noexcept;

	uint8_t *data() noexcept {
		return _data;
	}
	const uint8_t *data() const noexcept {
		return _data;
	}
	size_t size() const noexcept {
		return _size;
	}
	size_t capacity() const noexcept {
		return _capacity;
	}
	bool empty() const noexcept {
		return _size == 0;
	}

  private:
	bool grow(size_t required) noexcept;

	uint8_t *_data = nullptr;
	size_t _size = 0;
	size_t _capacity = 0;
	bool _usePSRAMBuffers = true;
};
