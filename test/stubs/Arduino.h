#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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
};
