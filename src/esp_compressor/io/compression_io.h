#pragma once

#include "../types/compression_types.h"
#include "../util/memory_buffer.h"

#include <string>
#include <vector>

class CompressionSource {
  public:
	virtual ~CompressionSource() = default;

	virtual CompressionError open() noexcept = 0;
	virtual void close() noexcept = 0;
	virtual size_t read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept = 0;
	virtual bool eof() const noexcept = 0;
	virtual bool hasKnownSize() const noexcept = 0;
	virtual uint64_t totalSize() const noexcept = 0;
};

class CompressionSink {
  public:
	virtual ~CompressionSink() = default;

	virtual CompressionError open() noexcept = 0;
	virtual CompressionError write(const uint8_t *data, size_t size) noexcept = 0;
	virtual CompressionError commit() noexcept = 0;
	virtual void abort() noexcept = 0;
	virtual void close() noexcept = 0;
	virtual uint64_t bytesWritten() const noexcept = 0;
	virtual bool isTransactional() const noexcept = 0;
};

class BufferSource : public CompressionSource {
  public:
	BufferSource(const uint8_t *data, size_t size) : _data(data), _size(size) {
	}

	CompressionError open() noexcept override;
	void close() noexcept override;
	size_t read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept override;
	bool eof() const noexcept override;
	bool hasKnownSize() const noexcept override;
	uint64_t totalSize() const noexcept override;

  private:
	const uint8_t *_data = nullptr;
	size_t _size = 0;
	size_t _offset = 0;
	bool _open = false;
};

class StreamSource : public CompressionSource {
  public:
	explicit StreamSource(Stream &stream, uint64_t knownSize = 0, bool hasKnownSize = false)
	    : _stream(stream), _knownSize(knownSize), _hasKnownSize(hasKnownSize) {
	}

	CompressionError open() noexcept override;
	void close() noexcept override;
	size_t read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept override;
	bool eof() const noexcept override;
	bool hasKnownSize() const noexcept override;
	uint64_t totalSize() const noexcept override;

  private:
	Stream &_stream;
	uint64_t _knownSize = 0;
	bool _hasKnownSize = false;
	bool _eof = false;
};

class FileSource : public CompressionSource {
  public:
	FileSource(fs::FS &fs, const char *path);

	CompressionError open() noexcept override;
	void close() noexcept override;
	size_t read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept override;
	bool eof() const noexcept override;
	bool hasKnownSize() const noexcept override;
	uint64_t totalSize() const noexcept override;

  private:
	fs::FS *_fs = nullptr;
	std::string _path;
	File _file;
	bool _open = false;
	uint64_t _size = 0;
};

class DynamicBufferSink : public CompressionSink {
  public:
	explicit DynamicBufferSink(std::vector<uint8_t> &out, bool usePSRAMBuffers = true)
	    : _out(out), _staging(usePSRAMBuffers) {
	}

	CompressionError open() noexcept override;
	CompressionError write(const uint8_t *data, size_t size) noexcept override;
	CompressionError commit() noexcept override;
	void abort() noexcept override;
	void close() noexcept override;
	uint64_t bytesWritten() const noexcept override;
	bool isTransactional() const noexcept override;

  private:
	std::vector<uint8_t> &_out;
	MemoryBuffer _staging;
	bool _open = false;
};

class FixedBufferSink : public CompressionSink {
  public:
	FixedBufferSink(uint8_t *buffer, size_t capacity, bool usePSRAMBuffers = true)
	    : _buffer(buffer), _capacity(capacity), _staging(usePSRAMBuffers) {
	}

	CompressionError open() noexcept override;
	CompressionError write(const uint8_t *data, size_t size) noexcept override;
	CompressionError commit() noexcept override;
	void abort() noexcept override;
	void close() noexcept override;
	uint64_t bytesWritten() const noexcept override;
	bool isTransactional() const noexcept override;

  private:
	uint8_t *_buffer = nullptr;
	size_t _capacity = 0;
	MemoryBuffer _staging;
	bool _open = false;
};

class PrintSink : public CompressionSink {
  public:
	explicit PrintSink(Print &print) : _print(print) {
	}

	CompressionError open() noexcept override;
	CompressionError write(const uint8_t *data, size_t size) noexcept override;
	CompressionError commit() noexcept override;
	void abort() noexcept override;
	void close() noexcept override;
	uint64_t bytesWritten() const noexcept override;
	bool isTransactional() const noexcept override;

  private:
	Print &_print;
	uint64_t _written = 0;
	bool _open = false;
};

class FileSink : public CompressionSink {
  public:
	FileSink(fs::FS &fs, const char *path, bool truncate = true);

	CompressionError open() noexcept override;
	CompressionError write(const uint8_t *data, size_t size) noexcept override;
	CompressionError commit() noexcept override;
	void abort() noexcept override;
	void close() noexcept override;
	uint64_t bytesWritten() const noexcept override;
	bool isTransactional() const noexcept override;

  private:
	void cleanupTemp() noexcept;

	fs::FS *_fs = nullptr;
	std::string _path;
	std::string _tempPath;
	bool _truncate = true;
	File _file;
	bool _open = false;
	uint64_t _written = 0;
};
