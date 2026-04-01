#include "compression_io.h"

#include "../util/fs_utils.h"

#include <algorithm>
#include <cstring>

CompressionError BufferSource::open() noexcept {
	_offset = 0;
	_open = true;
	return CompressionError::Ok;
}

void BufferSource::close() noexcept {
	_open = false;
}

size_t BufferSource::read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept {
	err = CompressionError::Ok;
	if (!_open || (!buffer && capacity != 0)) {
		err = CompressionError::ReadFailed;
		return 0;
	}

	const size_t remaining = _size - _offset;
	const size_t chunk = remaining < capacity ? remaining : capacity;
	if (chunk != 0) {
		std::memcpy(buffer, _data + _offset, chunk);
		_offset += chunk;
	}
	return chunk;
}

bool BufferSource::eof() const noexcept {
	return _offset >= _size;
}

bool BufferSource::hasKnownSize() const noexcept {
	return true;
}

uint64_t BufferSource::totalSize() const noexcept {
	return _size;
}

CompressionError StreamSource::open() noexcept {
	_eof = false;
	return CompressionError::Ok;
}

void StreamSource::close() noexcept {
	_eof = true;
}

size_t StreamSource::read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept {
	err = CompressionError::Ok;
	if (!buffer && capacity != 0) {
		err = CompressionError::ReadFailed;
		return 0;
	}
	if (capacity == 0) {
		return 0;
	}

	const size_t readBytes = _stream.readBytes(buffer, capacity);
	if (readBytes == 0) {
		_eof = true;
	}
	return readBytes;
}

bool StreamSource::eof() const noexcept {
	return _eof;
}

bool StreamSource::hasKnownSize() const noexcept {
	return _hasKnownSize;
}

uint64_t StreamSource::totalSize() const noexcept {
	return _knownSize;
}

FileSource::FileSource(fs::FS &fs, const char *path) : _fs(&fs), _path(path ? path : "") {
}

CompressionError FileSource::open() noexcept {
	if (!_fs || _path.empty()) {
		return CompressionError::InvalidArgument;
	}
	_file = _fs->open(_path.c_str(), "r");
	if (!_file) {
		return CompressionError::OpenFailed;
	}
	_size = _file.size();
	_open = true;
	return CompressionError::Ok;
}

void FileSource::close() noexcept {
	if (_file) {
		_file.close();
	}
	_open = false;
}

size_t FileSource::read(uint8_t *buffer, size_t capacity, CompressionError &err) noexcept {
	err = CompressionError::Ok;
	if (!_open || !_file || (!buffer && capacity != 0)) {
		err = CompressionError::ReadFailed;
		return 0;
	}

	return _file.read(buffer, capacity);
}

bool FileSource::eof() const noexcept {
	return !_file || _file.position() >= _file.size();
}

bool FileSource::hasKnownSize() const noexcept {
	return true;
}

uint64_t FileSource::totalSize() const noexcept {
	return _size;
}

CompressionError DynamicBufferSink::open() noexcept {
	_staging.clear();
	_open = true;
	return CompressionError::Ok;
}

CompressionError DynamicBufferSink::write(const uint8_t *data, size_t size) noexcept {
	if (!_open) {
		return CompressionError::WriteFailed;
	}
	return _staging.append(data, size) ? CompressionError::Ok : CompressionError::NoMemory;
}

CompressionError DynamicBufferSink::commit() noexcept {
	if (!_open) {
		return CompressionError::WriteFailed;
	}
	if (_staging.size() == 0) {
		_out.clear();
		_staging.release();
		return CompressionError::Ok;
	}
	_out.assign(_staging.data(), _staging.data() + _staging.size());
	_staging.release();
	return CompressionError::Ok;
}

void DynamicBufferSink::abort() noexcept {
	_staging.clear();
}

void DynamicBufferSink::close() noexcept {
	_open = false;
}

uint64_t DynamicBufferSink::bytesWritten() const noexcept {
	return _staging.size();
}

bool DynamicBufferSink::isTransactional() const noexcept {
	return true;
}

CompressionError FixedBufferSink::open() noexcept {
	_staging.clear();
	_open = true;
	return CompressionError::Ok;
}

CompressionError FixedBufferSink::write(const uint8_t *data, size_t size) noexcept {
	if (!_open) {
		return CompressionError::WriteFailed;
	}
	if (_staging.size() + size > _capacity) {
		return CompressionError::OutputOverflow;
	}
	return _staging.append(data, size) ? CompressionError::Ok : CompressionError::NoMemory;
}

CompressionError FixedBufferSink::commit() noexcept {
	if (!_open || !_buffer) {
		return CompressionError::WriteFailed;
	}
	if (_staging.size() > _capacity) {
		return CompressionError::OutputOverflow;
	}
	if (_staging.size() != 0) {
		std::memcpy(_buffer, _staging.data(), _staging.size());
	}
	_staging.release();
	return CompressionError::Ok;
}

void FixedBufferSink::abort() noexcept {
	_staging.clear();
}

void FixedBufferSink::close() noexcept {
	_open = false;
}

uint64_t FixedBufferSink::bytesWritten() const noexcept {
	return _staging.size();
}

bool FixedBufferSink::isTransactional() const noexcept {
	return true;
}

CompressionError PrintSink::open() noexcept {
	_written = 0;
	_open = true;
	return CompressionError::Ok;
}

CompressionError PrintSink::write(const uint8_t *data, size_t size) noexcept {
	if (!_open) {
		return CompressionError::WriteFailed;
	}
	if (size == 0) {
		return CompressionError::Ok;
	}
	const size_t written = _print.write(data, size);
	if (written != size) {
		return CompressionError::WriteFailed;
	}
	_written += written;
	return CompressionError::Ok;
}

CompressionError PrintSink::commit() noexcept {
	return CompressionError::Ok;
}

void PrintSink::abort() noexcept {
}

void PrintSink::close() noexcept {
	_open = false;
}

uint64_t PrintSink::bytesWritten() const noexcept {
	return _written;
}

bool PrintSink::isTransactional() const noexcept {
	return false;
}

FileSink::FileSink(fs::FS &fs, const char *path, bool truncate)
    : _fs(&fs), _path(path ? path : ""), _truncate(truncate) {
	_tempPath = _path + ".tmp";
	_backupPath = _path + ".bak";
}

CompressionError FileSink::open() noexcept {
	if (!_fs || _path.empty()) {
		return CompressionError::InvalidArgument;
	}
	if (!_truncate && _fs->exists(_path.c_str())) {
		return CompressionError::OpenFailed;
	}
	if (!espCompressorEnsureDir(*_fs, espCompressorDirname(_path))) {
		return CompressionError::OpenFailed;
	}
	cleanupTemp();
	cleanupBackup();
	_file = _fs->open(_tempPath.c_str(), "w");
	if (!_file) {
		return CompressionError::OpenFailed;
	}
	_written = 0;
	_open = true;
	return CompressionError::Ok;
}

CompressionError FileSink::write(const uint8_t *data, size_t size) noexcept {
	if (!_open || !_file) {
		return CompressionError::WriteFailed;
	}
	if (size == 0) {
		return CompressionError::Ok;
	}
	const size_t written = _file.write(data, size);
	if (written != size) {
		return CompressionError::WriteFailed;
	}
	_written += written;
	return CompressionError::Ok;
}

CompressionError FileSink::commit() noexcept {
	if (!_open || !_file) {
		return CompressionError::WriteFailed;
	}
	_file.close();
	const bool hadDestination = _fs->exists(_path.c_str());
	if (hadDestination) {
		cleanupBackup();
		if (!_fs->rename(_path.c_str(), _backupPath.c_str())) {
			return CompressionError::WriteFailed;
		}
	}
	if (!_fs->rename(_tempPath.c_str(), _path.c_str())) {
		if (hadDestination) {
			_fs->rename(_backupPath.c_str(), _path.c_str());
		}
		cleanupTemp();
		return CompressionError::WriteFailed;
	}
	cleanupBackup();
	return CompressionError::Ok;
}

void FileSink::abort() noexcept {
	if (_file) {
		_file.close();
	}
	cleanupTemp();
	cleanupBackup();
}

void FileSink::close() noexcept {
	if (_file) {
		_file.close();
	}
	_open = false;
}

uint64_t FileSink::bytesWritten() const noexcept {
	return _written;
}

bool FileSink::isTransactional() const noexcept {
	return true;
}

void FileSink::cleanupTemp() noexcept {
	if (_fs && !_tempPath.empty() && _fs->exists(_tempPath.c_str())) {
		_fs->remove(_tempPath.c_str());
	}
}

void FileSink::cleanupBackup() noexcept {
	if (_fs && !_backupPath.empty() && _fs->exists(_backupPath.c_str())) {
		_fs->remove(_backupPath.c_str());
	}
}
