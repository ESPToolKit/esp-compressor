#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

class File {
  public:
	File() = default;
	explicit File(std::filesystem::path path, std::ios::openmode mode)
	    : _path(std::move(path)), _mode(mode), _stream(_path, mode | std::ios::binary) {
	}

	operator bool() const {
		return _stream.is_open();
	}

	size_t read(uint8_t *buffer, size_t size) {
		if (!_stream.is_open()) {
			return 0;
		}
		_stream.read(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(size));
		return static_cast<size_t>(_stream.gcount());
	}

	size_t write(const uint8_t *buffer, size_t size) {
		if (!_stream.is_open()) {
			return 0;
		}
		_stream.write(reinterpret_cast<const char *>(buffer), static_cast<std::streamsize>(size));
		return _stream ? size : 0;
	}

	size_t size() const {
		if (_path.empty() || !std::filesystem::exists(_path)) {
			return 0;
		}
		return static_cast<size_t>(std::filesystem::file_size(_path));
	}

	size_t position() const {
		if (!_stream.is_open()) {
			return 0;
		}
		if ((_mode & std::ios::in) != 0) {
			const auto pos = _stream.tellg();
			return pos < 0 ? 0 : static_cast<size_t>(pos);
		}
		const auto pos = _stream.tellp();
		return pos < 0 ? 0 : static_cast<size_t>(pos);
	}

	bool available() const {
		return position() < size();
	}

	void close() {
		if (_stream.is_open()) {
			_stream.close();
		}
	}

  private:
	std::filesystem::path _path;
	std::ios::openmode _mode{};
	mutable std::fstream _stream;
};

namespace fs {

class FS {
  public:
	FS() = default;
	explicit FS(std::filesystem::path root) : _root(std::move(root)) {
		std::filesystem::create_directories(_root);
	}

	File open(const char *path, const char *mode = "r") {
		const std::filesystem::path resolved = resolve(path);
		std::ios::openmode openMode = std::ios::binary;
		if (mode && mode[0] == 'w') {
			std::filesystem::create_directories(resolved.parent_path());
			openMode |= std::ios::out | std::ios::trunc;
		} else {
			openMode |= std::ios::in;
		}
		return File(resolved, openMode);
	}

	bool exists(const char *path) const {
		return std::filesystem::exists(resolve(path));
	}

	bool mkdir(const char *path) {
		return std::filesystem::create_directories(resolve(path)) || exists(path);
	}

	bool remove(const char *path) {
		return std::filesystem::remove(resolve(path));
	}

	bool rename(const char *from, const char *to) {
		std::error_code ec;
		std::filesystem::rename(resolve(from), resolve(to), ec);
		return !ec;
	}

	std::filesystem::path root() const {
		return _root;
	}

  private:
	std::filesystem::path resolve(const char *path) const {
		std::string value = path ? path : "";
		while (!value.empty() && value.front() == '/') {
			value.erase(value.begin());
		}
		return _root / value;
	}

	std::filesystem::path _root;
};

} // namespace fs
