#pragma once

#include <cstdlib>

class ESPBufferManager {
  public:
	static void *allocate(size_t bytes, bool /*usePSRAMBuffers*/ = false) noexcept {
		return bytes == 0 ? nullptr : std::malloc(bytes);
	}

	static void *reallocate(void *ptr, size_t bytes, bool /*usePSRAMBuffers*/ = false) noexcept {
		if (bytes == 0) {
			std::free(ptr);
			return nullptr;
		}
		return std::realloc(ptr, bytes);
	}

	static void deallocate(void *ptr) noexcept {
		std::free(ptr);
	}

	static bool isPSRAMAvailable() noexcept {
		return false;
	}

	static bool shouldUsePSRAM(bool enabled) noexcept {
		return enabled && isPSRAMAvailable();
	}
};
