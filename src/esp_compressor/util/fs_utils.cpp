#include "fs_utils.h"

std::string espCompressorDirname(const std::string &path) {
	if (path.empty()) {
		return {};
	}

	const size_t pos = path.rfind('/');
	if (pos == std::string::npos || pos == 0) {
		return pos == 0 ? "/" : std::string{};
	}
	return path.substr(0, pos);
}

bool espCompressorEnsureDir(fs::FS &fs, const std::string &path) {
	if (path.empty() || path == "/") {
		return true;
	}
	if (fs.exists(path.c_str())) {
		return true;
	}

	const std::string parent = espCompressorDirname(path);
	if (!parent.empty() && parent != path && !espCompressorEnsureDir(fs, parent)) {
		return false;
	}

	return fs.mkdir(path.c_str());
}
