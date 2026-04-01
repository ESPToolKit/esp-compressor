#include "memory_buffer.h"

#include <cstring>
#include <utility>

MemoryBuffer::~MemoryBuffer() {
	release();
}

MemoryBuffer::MemoryBuffer(MemoryBuffer &&other) noexcept
    : _data(other._data),
      _size(other._size),
      _capacity(other._capacity),
      _usePSRAMBuffers(other._usePSRAMBuffers) {
	other._data = nullptr;
	other._size = 0;
	other._capacity = 0;
}

MemoryBuffer &MemoryBuffer::operator=(MemoryBuffer &&other) noexcept {
	if (this == &other) {
		return *this;
	}

	release();
	_data = other._data;
	_size = other._size;
	_capacity = other._capacity;
	_usePSRAMBuffers = other._usePSRAMBuffers;

	other._data = nullptr;
	other._size = 0;
	other._capacity = 0;
	return *this;
}

bool MemoryBuffer::reserve(size_t capacity) noexcept {
	if (capacity <= _capacity) {
		return true;
	}
	return grow(capacity);
}

bool MemoryBuffer::resize(size_t size) noexcept {
	if (size > _capacity && !grow(size)) {
		return false;
	}
	_size = size;
	return true;
}

bool MemoryBuffer::append(const uint8_t *data, size_t size) noexcept {
	if (size == 0) {
		return true;
	}
	if (!data) {
		return false;
	}
	if (_size + size < _size) {
		return false;
	}
	if (!resize(_size + size)) {
		return false;
	}
	std::memcpy(_data + (_size - size), data, size);
	return true;
}

bool MemoryBuffer::appendByte(uint8_t value) noexcept {
	return append(&value, 1);
}

void MemoryBuffer::release() noexcept {
	if (_data) {
		ESPBufferManager::deallocate(_data);
		_data = nullptr;
	}
	_size = 0;
	_capacity = 0;
}

bool MemoryBuffer::grow(size_t required) noexcept {
	size_t newCapacity = _capacity == 0 ? 64 : _capacity;
	while (newCapacity < required) {
		if (newCapacity > (static_cast<size_t>(-1) / 2)) {
			newCapacity = required;
			break;
		}
		newCapacity *= 2;
	}

	void *memory = _data ? ESPBufferManager::reallocate(_data, newCapacity, _usePSRAMBuffers)
	                     : ESPBufferManager::allocate(newCapacity, _usePSRAMBuffers);
	if (!memory) {
		return false;
	}

	_data = static_cast<uint8_t *>(memory);
	_capacity = newCapacity;
	return true;
}
