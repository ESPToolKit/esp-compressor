#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#define ESPCOMPRESSOR_HAS_ARDUINO 1
#else
#define ESPCOMPRESSOR_HAS_ARDUINO 0
using String = std::string;

class Print {
  public:
	virtual ~Print() = default;

	virtual size_t write(uint8_t value) {
		return write(&value, 1);
	}

	virtual size_t write(const uint8_t *buffer, size_t size) = 0;
};

class Stream : public Print {
  public:
	virtual int available() = 0;
	virtual int read() = 0;
	virtual int peek() {
		return -1;
	}
	virtual void flush() {
	}

	size_t readBytes(uint8_t *buffer, size_t length) {
		size_t count = 0;
		while (count < length) {
			const int value = read();
			if (value < 0) {
				break;
			}
			buffer[count++] = static_cast<uint8_t>(value);
		}
		return count;
	}

	size_t write(const uint8_t *buffer, size_t size) override {
		size_t written = 0;
		for (size_t i = 0; i < size; ++i) {
			written += write(buffer[i]);
		}
		return written;
	}
};
#endif

#if defined(ARDUINO)
#include <FS.h>
#define ESPCOMPRESSOR_HAS_FS 1
#elif __has_include(<FS.h>)
#include <FS.h>
#define ESPCOMPRESSOR_HAS_FS 1
#else
#define ESPCOMPRESSOR_HAS_FS 0
namespace fs {
class FS;
}
class File;
#endif

#if __has_include(<freertos/FreeRTOS.h>)
extern "C" {
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
}
#define ESPCOMPRESSOR_HAS_FREERTOS 1
#else
#define ESPCOMPRESSOR_HAS_FREERTOS 0
using UBaseType_t = unsigned int;
using BaseType_t = int;
using StackType_t = std::uint32_t;
constexpr BaseType_t tskNO_AFFINITY = -1;
#endif
